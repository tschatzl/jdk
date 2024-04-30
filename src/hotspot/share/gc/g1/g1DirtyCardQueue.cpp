/*
 * Copyright (c) 2001, 2023, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "gc/g1/g1BarrierSet.inline.hpp"
#include "gc/g1/g1CardTable.inline.hpp"
#include "gc/g1/g1CardTableEntryClosure.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefineStats.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1DirtyCardQueue.hpp"
#include "gc/g1/g1FreeIdSet.hpp"
#include "gc/g1/g1HeapRegionRemSet.inline.hpp"
#include "gc/g1/g1RedirtyCardsQueue.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1ThreadLocalData.hpp"
#include "gc/shared/bufferNode.hpp"
#include "gc/shared/bufferNodeList.hpp"
#include "gc/shared/suspendibleThreadSet.hpp"
#include "memory/iterator.hpp"
#include "runtime/atomic.hpp"
#include "runtime/handshake.hpp"
#include "runtime/javaThread.hpp"
#include "runtime/mutex.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/os.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/threads.hpp"
#include "runtime/threadSMR.hpp"
#include "utilities/globalCounter.inline.hpp"
#include "utilities/macros.hpp"
#include "utilities/nonblockingQueue.inline.hpp"
#include "utilities/pair.hpp"
#include "utilities/quickSort.hpp"
#include "utilities/systemMemoryBarrier.hpp"
#include "utilities/ticks.hpp"

G1DirtyCardQueue::G1DirtyCardQueue(G1DirtyCardQueueSet* qset) :
  PtrQueue(qset),
  _refinement_stats(new G1ConcurrentRefineStats())
{ }

G1DirtyCardQueue::~G1DirtyCardQueue() {
  delete _refinement_stats;
}

// Assumed to be zero by concurrent threads.
static uint par_ids_start() { return 0; }

G1DirtyCardQueueSet::G1DirtyCardQueueSet(BufferNode::Allocator* allocator) :
  PtrQueueSet(allocator),
  _num_cards_completed(0),
  _mutator_refinement_threshold(SIZE_MAX),
  _completed(),
  _cleaning(),
  _num_cards_cleaning(0),
  _ready(),
  _num_cards_ready(0),
  _paused(),
  _free_ids(par_ids_start(), num_par_ids()),
  _detached_refinement_stats()
{}

G1DirtyCardQueueSet::~G1DirtyCardQueueSet() {
  abandon_completed_buffers();
}

// Determines how many mutator threads can process the buffers in parallel.
uint G1DirtyCardQueueSet::num_par_ids() {
  return (uint)os::initial_active_processor_count();
}

void G1DirtyCardQueueSet::flush_queue(G1DirtyCardQueue& queue) {
  if (queue.buffer() != nullptr) {
    G1ConcurrentRefineStats* stats = queue.refinement_stats();
    stats->inc_dirtied_cards(queue.size());
  }
  PtrQueueSet::flush_queue(queue);
}

void G1DirtyCardQueueSet::enqueue(G1DirtyCardQueue& queue,
                                  volatile CardValue* card_ptr) {
  CardValue* value = const_cast<CardValue*>(card_ptr);
  if (!try_enqueue(queue, value)) {
    handle_zero_index(queue);
    retry_enqueue(queue, value);
  }
}

void G1DirtyCardQueueSet::handle_zero_index(G1DirtyCardQueue& queue) {
  assert(queue.index() == 0, "precondition");
  BufferNode* old_node = exchange_buffer_with_new(queue);
  if (old_node != nullptr) {
    assert(old_node->index() == 0, "invariant");
    G1ConcurrentRefineStats* stats = queue.refinement_stats();
    stats->inc_dirtied_cards(old_node->capacity());
    handle_completed_buffer(old_node, stats);
  }
}

void G1DirtyCardQueueSet::handle_zero_index_for_thread(Thread* t) {
  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(t);
  G1BarrierSet::dirty_card_queue_set().handle_zero_index(queue);
}

size_t G1DirtyCardQueueSet::num_cards() const {
  return Atomic::load(&_num_cards_completed) + Atomic::load(&_num_cards_cleaning) + Atomic::load(&_num_cards_ready);
}

void G1DirtyCardQueueSet::enqueue_completed_buffer(BufferNode* cbn) {
  assert(cbn != nullptr, "precondition");
  // Increment _num_cards before adding to queue, so queue removal doesn't
  // need to deal with _num_cards possibly going negative.
  Atomic::add(&_num_cards_completed, cbn->size(), memory_order_relaxed);
  // Perform push in CS.  The old tail may be popped while the push is
  // observing it (attaching it to the new buffer).  We need to ensure it
  // can't be reused until the push completes, to avoid ABA problems.
  GlobalCounter::CriticalSection cs(Thread::current());
  _completed.push(*cbn);
}

// Thread-safe attempt to remove and return the first buffer from
// the _completed queue, using the NonblockingQueue::try_pop() underneath.
// It has a limitation that it may return null when there are objects
// in the queue if there is a concurrent push/append operation.
BufferNode* G1DirtyCardQueueSet::dequeue_completed_buffer() {
  Thread* current_thread = Thread::current();
  BufferNode* result = nullptr;
  while (true) {
    // Use GlobalCounter critical section to avoid ABA problem.
    // The release of a buffer to its allocator's free list uses
    // GlobalCounter::write_synchronize() to coordinate with this
    // dequeuing operation.
    // We use a CS per iteration, rather than over the whole loop,
    // because we're not guaranteed to make progress. Lingering in
    // one CS could defer releasing buffer to the free list for reuse,
    // leading to excessive allocations.
    GlobalCounter::CriticalSection cs(current_thread);
    if (_completed.try_pop(&result)) return result;
  }
}

BufferNode* G1DirtyCardQueueSet::get_completed_buffer() {
  BufferNode* result = dequeue_completed_buffer();
  if (result == nullptr) {         // Unlikely if no paused buffers.
    enqueue_previous_paused_buffers();
    result = dequeue_completed_buffer();
    if (result == nullptr) return nullptr;
  }
  Atomic::sub(&_num_cards_completed, result->size(), memory_order_relaxed);
  return result;
}

void G1DirtyCardQueueSet::enqueue_cleaning_buffer(BufferNode* cbn) {
  assert(cbn != nullptr, "precondition");
  // Increment _num_cards before adding to queue, so queue removal doesn't
  // need to deal with _num_cards possibly going negative.
  Atomic::add(&_num_cards_cleaning, cbn->size(), memory_order_relaxed);

  GlobalCounter::CriticalSection cs(Thread::current());
  _cleaning.push(*cbn);
}

// FIXME: Thread-safe attempt to remove and return the first buffer from
// the _completed queue, using the NonblockingQueue::try_pop() underneath.
// It has a limitation that it may return null when there are objects
// in the queue if there is a concurrent push/append operation.
BufferNode* G1DirtyCardQueueSet::dequeue_cleaning_buffer() {
  Thread* current_thread = Thread::current();
  BufferNode* result = nullptr;
  while (true) {
    // Use GlobalCounter critical section to avoid ABA problem.
    // The release of a buffer to its allocator's free list uses
    // GlobalCounter::write_synchronize() to coordinate with this
    // dequeuing operation.
    // We use a CS per iteration, rather than over the whole loop,
    // because we're not guaranteed to make progress. Lingering in
    // one CS could defer releasing buffer to the free list for reuse,
    // leading to excessive allocations.
    // Only a single thread will ever pop, so no synchronization required.
    GlobalCounter::CriticalSection cs(current_thread);
    if (_cleaning.try_pop(&result)) return result;
  }
}

BufferNode* G1DirtyCardQueueSet::get_cleaning_buffer() {
  BufferNode* result = dequeue_cleaning_buffer();
  if (result == nullptr) return nullptr;
  Atomic::sub(&_num_cards_cleaning, result->size(), memory_order_relaxed);
  return result;
}

void G1DirtyCardQueueSet::enqueue_ready_buffer(BufferNode* cbn) {
  assert(cbn != nullptr, "precondition");
  // Increment _num_cards before adding to queue, so queue removal doesn't
  // need to deal with _num_cards possibly going negative.
  Atomic::add(&_num_cards_ready, cbn->size(), memory_order_relaxed);
  // Perform push in CS.  The old tail may be popped while the push is
  // observing it (attaching it to the new buffer).  We need to ensure it
  // can't be reused until the push completes, to avoid ABA problems.
  GlobalCounter::CriticalSection cs(Thread::current());
  _ready.push(*cbn);
}

// FIXME: Thread-safe attempt to remove and return the first buffer from
// the _completed queue, using the NonblockingQueue::try_pop() underneath.
// It has a limitation that it may return null when there are objects
// in the queue if there is a concurrent push/append operation.
BufferNode* G1DirtyCardQueueSet::dequeue_ready_buffer() {
  Thread* current_thread = Thread::current();
  BufferNode* result = nullptr;
  while (true) {
    // Use GlobalCounter critical section to avoid ABA problem.
    // The release of a buffer to its allocator's free list uses
    // GlobalCounter::write_synchronize() to coordinate with this
    // dequeuing operation.
    // We use a CS per iteration, rather than over the whole loop,
    // because we're not guaranteed to make progress. Lingering in
    // one CS could defer releasing buffer to the free list for reuse,
    // leading to excessive allocations.
    GlobalCounter::CriticalSection cs(current_thread);
    if (_ready.try_pop(&result)) return result;
  }
}

BufferNode* G1DirtyCardQueueSet::get_ready_buffer() {
  BufferNode* result = dequeue_ready_buffer();
  if (result == nullptr) return nullptr;
  Atomic::sub(&_num_cards_ready, result->size(), memory_order_relaxed);
  return result;
}

#ifdef ASSERT
void G1DirtyCardQueueSet::verify_num_cards() const {
  size_t actual = 0;
  for (BufferNode* cur = _completed.first();
       !_completed.is_end(cur);
       cur = cur->next()) {
    actual += cur->size();
  }
  for (BufferNode* cur = _cleaning.first(); !_cleaning.is_end(cur); cur = cur->next()) {
    actual += cur->size();
  }
  for (BufferNode* cur = _ready.first(); !_ready.is_end(cur); cur = cur->next()) {
    actual += cur->size();
  }
  assert(actual == num_cards(),
         "Num entries in buffers should be " SIZE_FORMAT " but are " SIZE_FORMAT,
         num_cards(), actual);
}
#endif // ASSERT

G1DirtyCardQueueSet::PausedBuffers::PausedList::PausedList() :
  _head(nullptr), _tail(nullptr),
  _safepoint_id(SafepointSynchronize::safepoint_id())
{}

#ifdef ASSERT
G1DirtyCardQueueSet::PausedBuffers::PausedList::~PausedList() {
  assert(Atomic::load(&_head) == nullptr, "precondition");
  assert(_tail == nullptr, "precondition");
}
#endif // ASSERT

bool G1DirtyCardQueueSet::PausedBuffers::PausedList::is_next() const {
  assert_not_at_safepoint();
  return _safepoint_id == SafepointSynchronize::safepoint_id();
}

void G1DirtyCardQueueSet::PausedBuffers::PausedList::add(BufferNode* node) {
  assert_not_at_safepoint();
  assert(is_next(), "precondition");
  BufferNode* old_head = Atomic::xchg(&_head, node);
  if (old_head == nullptr) {
    assert(_tail == nullptr, "invariant");
    _tail = node;
  } else {
    node->set_next(old_head);
  }
}

G1DirtyCardQueueSet::HeadTail G1DirtyCardQueueSet::PausedBuffers::PausedList::take() {
  BufferNode* head = Atomic::load(&_head);
  BufferNode* tail = _tail;
  Atomic::store(&_head, (BufferNode*)nullptr);
  _tail = nullptr;
  return HeadTail(head, tail);
}

G1DirtyCardQueueSet::PausedBuffers::PausedBuffers() : _plist(nullptr) {}

#ifdef ASSERT
G1DirtyCardQueueSet::PausedBuffers::~PausedBuffers() {
  assert(Atomic::load(&_plist) == nullptr, "invariant");
}
#endif // ASSERT

void G1DirtyCardQueueSet::PausedBuffers::add(BufferNode* node) {
  assert_not_at_safepoint();
  PausedList* plist = Atomic::load_acquire(&_plist);
  if (plist == nullptr) {
    // Try to install a new next list.
    plist = new PausedList();
    PausedList* old_plist = Atomic::cmpxchg(&_plist, (PausedList*)nullptr, plist);
    if (old_plist != nullptr) {
      // Some other thread installed a new next list.  Use it instead.
      delete plist;
      plist = old_plist;
    }
  }
  assert(plist->is_next(), "invariant");
  plist->add(node);
}

G1DirtyCardQueueSet::HeadTail G1DirtyCardQueueSet::PausedBuffers::take_previous() {
  assert_not_at_safepoint();
  PausedList* previous;
  {
    // Deal with plist in a critical section, to prevent it from being
    // deleted out from under us by a concurrent take_previous().
    GlobalCounter::CriticalSection cs(Thread::current());
    previous = Atomic::load_acquire(&_plist);
    if ((previous == nullptr) ||   // Nothing to take.
        previous->is_next() ||  // Not from a previous safepoint.
        // Some other thread stole it.
        (Atomic::cmpxchg(&_plist, previous, (PausedList*)nullptr) != previous)) {
      return HeadTail();
    }
  }
  // We now own previous.
  HeadTail result = previous->take();
  // There might be other threads examining previous (in concurrent
  // take_previous()).  Synchronize to wait until any such threads are
  // done with such examination before deleting.
  GlobalCounter::write_synchronize();
  delete previous;
  return result;
}

G1DirtyCardQueueSet::HeadTail G1DirtyCardQueueSet::PausedBuffers::take_all() {
  assert_at_safepoint();
  HeadTail result;
  PausedList* plist = Atomic::load(&_plist);
  if (plist != nullptr) {
    Atomic::store(&_plist, (PausedList*)nullptr);
    result = plist->take();
    delete plist;
  }
  return result;
}

void G1DirtyCardQueueSet::record_paused_buffer(BufferNode* node) {
  assert_not_at_safepoint();
  assert(node->next() == nullptr, "precondition");
  // Ensure there aren't any paused buffers from a previous safepoint.
  enqueue_previous_paused_buffers();
  // Cards for paused buffers are included in count, to contribute to
  // notification checking after the coming safepoint if it doesn't GC.
  // Note that this means the queue's _num_cards differs from the number
  // of cards in the queued buffers when there are paused buffers.
  Atomic::add(&_num_cards_completed, node->size(), memory_order_relaxed);
  _paused.add(node);
}

void G1DirtyCardQueueSet::enqueue_paused_buffers_aux(const HeadTail& paused) {
  if (paused._head != nullptr) {
    assert(paused._tail != nullptr, "invariant");
    // Cards from paused buffers are already recorded in the queue count.
    _completed.append(*paused._head, *paused._tail);
  }
}

void G1DirtyCardQueueSet::enqueue_previous_paused_buffers() {
  assert_not_at_safepoint();
  enqueue_paused_buffers_aux(_paused.take_previous());
}

void G1DirtyCardQueueSet::enqueue_all_paused_buffers() {
  assert_at_safepoint();
  enqueue_paused_buffers_aux(_paused.take_all());
}

void G1DirtyCardQueueSet::abandon_completed_buffers() {
  BufferNodeList list = take_all_buffers();
  BufferNode* buffers_to_delete = list._head;
  while (buffers_to_delete != nullptr) {
    BufferNode* bn = buffers_to_delete;
    buffers_to_delete = bn->next();
    bn->set_next(nullptr);
    deallocate_buffer(bn);
  }
}

static void merge_into_queue(G1RedirtyCardsQueueSet* src, NonblockingQueue<BufferNode, &BufferNode::next_ptr>& queue, volatile size_t& counter) {
  const BufferNodeList from = src->take_all_completed_buffers();
  if (from._head != nullptr) {
    Atomic::add(&counter, from._entry_count, memory_order_relaxed);
    queue.append(*from._head, *from._tail);
  }
}

// Merge lists of buffers. The source queue set is emptied as a
// result. The queue sets must share the same allocator.
void G1DirtyCardQueueSet::merge_into_completed_queue(G1RedirtyCardsQueueSet* src) {
  assert(allocator() == src->allocator(), "precondition");
  merge_into_queue(src, _completed, _num_cards_completed);
}

void G1DirtyCardQueueSet::merge_into_ready_queue(G1RedirtyCardsQueueSet* src) {
  assert(allocator() == src->allocator(), "precondition");
  merge_into_queue(src, _ready, _num_cards_ready);
}

BufferNodeList G1DirtyCardQueueSet::take_all_buffers() {
  BufferNodeList completed = take_all_completed_buffers();
  BufferNodeList cleaning = take_all_cleaning_buffers();
  BufferNodeList ready = take_all_ready_buffers();

  completed.append(cleaning);
  completed.append(ready);

  return completed;
}

BufferNodeList G1DirtyCardQueueSet::take_all_completed_buffers() {
  enqueue_all_paused_buffers();
  verify_num_cards();
  Pair<BufferNode*, BufferNode*> completed = _completed.take_all();
  size_t cards_completed = Atomic::load(&_num_cards_completed);
  Atomic::store(&_num_cards_completed, size_t(0));
  return BufferNodeList(completed.first, completed.second, cards_completed);
}

BufferNodeList G1DirtyCardQueueSet::take_all_cleaning_buffers() {
  verify_num_cards();
  Pair<BufferNode*, BufferNode*> cleaning = _cleaning.take_all();
  size_t cards_cleaning = Atomic::load(&_num_cards_cleaning);
  Atomic::store(&_num_cards_cleaning, size_t(0));
  return BufferNodeList(cleaning.first, cleaning.second, cards_cleaning);
}

BufferNodeList G1DirtyCardQueueSet::take_all_ready_buffers() {
  verify_num_cards();
  Pair<BufferNode*, BufferNode*> ready = _ready.take_all();
  size_t cards_ready = Atomic::load(&_num_cards_ready);
  Atomic::store(&_num_cards_ready, size_t(0));
  return BufferNodeList(ready.first, ready.second, cards_ready);
}

void G1DirtyCardQueueSet::redirty_cleaning_and_ready_buffers() {
  class DirtyCardsClosure : public G1CardTableEntryClosure {
    G1CardTable* _ct;
  public:
    DirtyCardsClosure() : _ct(G1CollectedHeap::heap()->card_table()) { }
    void do_card_ptr(CardValue* card_ptr, uint worker_id) override {
      _ct->mark_clean_as_dirty(card_ptr);
    }
  } cl;

  for (BufferNode* node = _cleaning.pop(); node != nullptr; node = _cleaning.pop()) {
    cl.apply_to_buffer(node, 0);
    enqueue_completed_buffer(node);
  }
  Atomic::store(&_num_cards_cleaning, (size_t)0);

  for (BufferNode* node = _ready.pop(); node != nullptr; node = _ready.pop()) {
    cl.apply_to_buffer(node, 0);
    enqueue_completed_buffer(node);
  }
  Atomic::store(&_num_cards_ready, (size_t)0);
}

void G1DirtyCardQueueSet::print_buffers() {
  LogTarget(Trace, gc, refine) lt;
  LogStream ls(lt);

  if (!lt.is_enabled()) {
    return;
  }

  class DirtyCardsClosure : public G1CardTableEntryClosure {
    G1CardTable* _ct;
    outputStream* _s;
  public:
    DirtyCardsClosure(outputStream* s) : _ct(G1CollectedHeap::heap()->card_table()), _s(s) { }
    void do_card_ptr(CardValue* card_ptr, uint worker_id) override {
      _s->print(PTR_FORMAT " ", p2i(_ct->addr_for(card_ptr)));
    }
  } cl(&ls);

  ls.print_cr("complete");
  for (BufferNode* cur = _completed.first(); !_completed.is_end(cur); cur = cur->next()) {
    cl.apply_to_buffer(cur, 0);
    ls.cr();
  }
  ls.print_cr("cleaning");
  for (BufferNode* cur = _cleaning.first(); !_cleaning.is_end(cur); cur = cur->next()) {
    cl.apply_to_buffer(cur, 0);
    ls.cr();
  }
  ls.print_cr("ready");
  for (BufferNode* cur = _ready.first(); !_ready.is_end(cur); cur = cur->next()) {
    cl.apply_to_buffer(cur, 0);
    ls.cr();
  }
}

class G1RefineBufferedCards : public StackObj {
  BufferNode* const _node;
  CardTable::CardValue** const _node_buffer;
  const size_t _node_buffer_capacity;
  const uint _worker_id;
  G1ConcurrentRefineStats* _stats;
  G1RemSet* const _g1rs;

  static inline ptrdiff_t compare_cards(const CardTable::CardValue* p1,
                                        const CardTable::CardValue* p2) {
    return p2 - p1;
  }

  // Sorts the cards from start_index to _node_buffer_capacity in *decreasing*
  // address order. Tests showed that this order is preferable to not sorting
  // or increasing address order.
  void sort_cards() {
    const size_t start_index = _node->index();
    QuickSort::sort(&_node_buffer[start_index],
                    _node_buffer_capacity - start_index,
                    compare_cards,
                    false);
  }

  // Removes cards from the buffer, updating the buffer's index..
  void clean_cards() {
    const size_t start = _node->index();
    assert(start <= _node_buffer_capacity, "invariant");

    // Two-fingered compaction algorithm similar to the filtering mechanism in
    // SATBMarkQueue. The main difference is that clean_card_before_refine()
    // could change the buffer element in-place.
    // We don't check for SuspendibleThreadSet::should_yield(), because
    // cleaning and redirtying the cards is fast.
    CardTable::CardValue** src = &_node_buffer[start];
    CardTable::CardValue** dst = &_node_buffer[_node_buffer_capacity];
    assert(src <= dst, "invariant");
    for ( ; src < dst; ++src) {
      // Search low to high for a card to keep.
      if (_g1rs->clean_card_before_refine(src)) {
        // Found keeper.  Search high to low for a card to discard.
        while (src < --dst) {
          if (!_g1rs->clean_card_before_refine(dst)) {
            *dst = *src;         // Replace discard with keeper.
            break;
          }
        }
        // If discard search failed (src == dst), the outer loop will also end.
      }
    }

    // dst points to the first retained clean card, or the end of the buffer
    // if all the cards were discarded.
    const size_t first_clean = dst - _node_buffer;
    assert(first_clean >= start && first_clean <= _node_buffer_capacity, "invariant");
    // Discarded cards are considered as refined.
    const size_t num_cleaned = first_clean - start;
    _stats->inc_refined_cards(num_cleaned);
    _stats->inc_precleaned_cards(num_cleaned);
    _node->set_index(start + num_cleaned);
  }

  bool refine_cleaned_cards() {
    bool result = true;
    const size_t start_index = _node->index();
    size_t i = start_index;
    for ( ; i < _node_buffer_capacity; ++i) {
      if (SuspendibleThreadSet::should_yield()) {
        redirty_unrefined_cards(i);
        result = false;
        break;
      }
      _g1rs->refine_card_concurrently(_node_buffer[i], _worker_id);
    }
    _node->set_index(i);
    _stats->inc_refined_cards(i - start_index);
    return result;
  }

  void redirty_unrefined_cards(size_t start) {
    for ( ; start < _node_buffer_capacity; ++start) {
      *_node_buffer[start] = G1CardTable::dirty_card_val();
    }
  }

public:
  G1RefineBufferedCards(BufferNode* node,
                        uint worker_id,
                        G1ConcurrentRefineStats* stats) :
    _node(node),
    _node_buffer(reinterpret_cast<CardTable::CardValue**>(BufferNode::make_buffer_from_node(node))),
    _node_buffer_capacity(node->capacity()),
    _worker_id(worker_id),
    _stats(stats),
    _g1rs(G1CollectedHeap::heap()->rem_set()) {}

  bool clean() {
    clean_cards();
    return _node->is_empty();
  }

  bool refine() {
    sort_cards();
    return refine_cleaned_cards();
  }
};

bool G1DirtyCardQueueSet::refine_buffer(BufferNode* node,
                                        uint worker_id,
                                        G1ConcurrentRefineStats* stats) {
  Ticks start_time = Ticks::now();
  G1RefineBufferedCards buffered_cards(node, worker_id, stats);
  if (!G1UseAsyncDekkerSync) {
    if (buffered_cards.clean()) {
      return true;
    }
    // This fence serves two purposes. First, the cards must be cleaned
    // before processing the contents. Second, we can't proceed with
    // processing a region until after the read of the region's top in
    // collect_and_clean_cards(), for synchronization with possibly concurrent
    // humongous object allocation (see comment at the StoreStore fence before
    // setting the regions' tops in humongous allocation path).
    // It's okay that reading region's top and reading region's type were racy
    // wrto each other. We need both set, in any order, to proceed.
    OrderAccess::fence();
  }
  bool result = buffered_cards.refine();
  stats->inc_refinement_time(Ticks::now() - start_time);
  return result;
}

void G1DirtyCardQueueSet::handle_refined_buffer(BufferNode* node,
                                                bool fully_processed) {
  if (fully_processed) {
    assert(node->is_empty(), "Buffer not fully consumed: index: %zu, size: %zu",
           node->index(), node->capacity());
    deallocate_buffer(node);
  } else {
    assert(!node->is_empty(), "Buffer fully consumed.");
    // Buffer incompletely processed because there is a pending safepoint.
    // Record partially processed buffer, to be finished later.
    record_paused_buffer(node);
  }
}

void G1DirtyCardQueueSet::handle_completed_buffer(BufferNode* new_node,
                                                  G1ConcurrentRefineStats* stats) {
  enqueue_completed_buffer(new_node);

  // No need for mutator refinement if number of cards is below limit.
  if (num_cards() <= Atomic::load(&_mutator_refinement_threshold)) {
    return;
  }

  // Don't try to process a buffer that will just get immediately paused.
  // When going into a safepoint it's just a waste of effort.
  // When coming out of a safepoint, Java threads may be running before the
  // yield request (for non-Java threads) has been cleared.
  if (SuspendibleThreadSet::should_yield()) {
    return;
  }

  // Only Java threads perform mutator refinement.
  if (!Thread::current()->is_Java_thread()) {
    return;
  }

  BufferNode* node = get_ready_buffer();
  if (node == nullptr) return;     // Didn't get a buffer to process.

  // Refine cards in buffer.

  uint worker_id = _free_ids.claim_par_id(); // temporarily claim an id
  bool fully_processed = refine_buffer(node, worker_id, stats);
  _free_ids.release_par_id(worker_id); // release the id

  // Deal with buffer after releasing id, to let another thread use id.
  handle_refined_buffer(node, fully_processed);
}

bool G1DirtyCardQueueSet::refine_ready_buffer_concurrently(uint worker_id,
                                                           size_t stop_at,
                                                           G1ConcurrentRefineStats* stats) {
  // Not enough cards to trigger processing.
  if (num_cards() <= stop_at) return false;

  BufferNode* node = get_ready_buffer();
  if (node == nullptr) return false; // Didn't get a buffer to process.

  bool fully_processed = refine_buffer(node, worker_id, stats);
  handle_refined_buffer(node, fully_processed);
  return true;
}

#define ENABLE_PREDICTION 1

bool G1DirtyCardQueueSet::move_from_completed_to_ready_queue(uint worker_id, size_t stop_at, G1ConcurrentRefineStats* stats, size_t threads_active) {
#ifdef ENABLE_PREDICTION
  log_debug(gc, refine)("c %zu r %zu m %zu s %zu d %s", _num_cards_completed, _num_cards_ready, threads_active, stop_at, BOOL_TO_STR(num_cards() > stop_at));
#endif

  size_t min_ready_wanted = (size_t)threads_active * 1000 + 1;
  size_t num_ready_start = Atomic::load(&_num_cards_ready);
  if (num_ready_start > min_ready_wanted) { // FIXME: want to move every X ms, except just before GC
    return false;
  }
  size_t num_cards_start = num_cards();

  if (num_cards_start <= stop_at) {
    return false;
  }
  Ticks start = Ticks::now();

  size_t can_get_cards = num_cards_start - stop_at;
#ifdef ENABLE_PREDICTION
  double cards_move_rate = 30000.0; // "random" value; cards/ms
  double cards_refine_rate = 1500.0; // "random" value; cards/ms
#endif

  size_t capacity = 0;
  size_t to_get = MAX2(align_up(can_get_cards, 256) / 256, align_up(min_ready_wanted, 256) / 256);

  size_t num_moved = 0;

  guarantee(_cleaning.empty(), "must be at start");

  while (num_moved != to_get) {
#ifdef ENABLE_PREDICTION
    Ticks batch_start = Ticks::now();

    size_t num_cards_ready = _num_cards_ready;
    double available_time = (double)num_cards_ready / cards_refine_rate;
    size_t buffers_moved_per_ms = cards_move_rate / 256;
    size_t outer_batch_size = MAX2((size_t)((cards_move_rate * available_time) / 256 * 0.5 /* random percentage */), buffers_moved_per_ms /* at least spend one ms */);
    log_debug(gc, refine)("can_get_cards %zu to_get_bufs %zu move-rate %.0f available-time %.2fms batch-size %zu ready %zu", can_get_cards, to_get, cards_move_rate, available_time, outer_batch_size, num_cards_ready);

    size_t const batch_size = MIN2(outer_batch_size, to_get - num_moved);//MIN2(to_get, (size_t)100); // FIXME: smaller batch size?
#else
    size_t const batch_size = to_get;
#endif
    size_t cur_size = 0;

    while (cur_size != batch_size) {
      BufferNode* node = get_completed_buffer();
      if (node == nullptr) {
        break; // Didn't get a buffer to move. Means that everything is in the ready buffers anyway?
      }

      if (G1UseAsyncDekkerSync) {
        G1RefineBufferedCards buffered_cards(node, worker_id, stats);
        bool all_cleaned = buffered_cards.clean();
        // Nothing to do with the buffer counter wrt to cleaned cards. This buffer
        // is completely independent of those now.
        if (all_cleaned) {
          log_trace(gc, refine)("dropped " PTR_FORMAT, p2i(node));
          handle_refined_buffer(node, true /* fully_processed */);
          continue;
        }
      }

      capacity = node->capacity();
      cur_size++;

      enqueue_cleaning_buffer(node);

      // If we yielded due to a safepoint, one of two situations can be met:
      // * this was a GC safepoint. Completed buffers are empty, so we will exit the
      // loop next time we want to get a buffer (or shortly after).
      // The safepoint will handle the cleaning buffers correctly.
      // * not a GC safepoint. Continue as normal.
      if (SuspendibleThreadSet::should_yield()) {
        log_debug(gc, refine)("yield while cleaning");
        SuspendibleThreadSet::yield();
      }
    }

    if (cur_size == 0) {
      // Did not get any completed buffers in this batch, nothing to do, so exit.
      break;
    }

    // Do the memory synchronization.
    if (G1UseAsyncDekkerSync) {
      Ticks start = Ticks::now();
      if (!UseNewCode) {
        SystemMemoryBarrier::emit();
      } else {
        class RendezvousClosure : public HandshakeClosure {
        public:
          RendezvousClosure() : HandshakeClosure("G1 Async Dekker Rendezvous") { }
          virtual void do_thread(Thread* thread) { /* nothing to do */ }
        } cl;
        log_debug(gc, refine)("Enter rendezvous with %zu cleaning buffers (%zu buffers)", cur_size, _cleaning.length());
        {
          SuspendibleThreadSetLeaver sts_leave;
          Handshake::execute(&cl);
        }
        log_debug(gc, refine)("Exit rendezvous with %zu cleaning buffers (%zu buffers)", cur_size, _cleaning.length());
      }
      stats->inc_sysmembarrier_time(Ticks::now() - start);
      stats->inc_sysmembarrier_executed();
    }

    if (_cleaning.empty()) {
      // While rendezvousing there has been a safepoint/GC that processed the cleaning list.
      // The whole request to move items can be cancelled. Exit.
      // FIXME: may be subsumed by below loop condition, but for the message...
      log_debug(gc, refine)("Rendezvous interrupted by GC.");
      break;
    }
    // Enqueue into ready buffer.
    while (true) {
      BufferNode* node = get_cleaning_buffer();
      if (node == nullptr) {
        break;
      }
      enqueue_ready_buffer(node);
      num_moved++;
      if (SuspendibleThreadSet::should_yield()) {
        log_debug(gc, refine)("yield while enqueuing ready");
        SuspendibleThreadSet::yield();
      }
    }
#ifdef ENABLE_PREDICTION
    log_debug(gc, refine)("Move-size %zu moved %zu to-get %zu ready %zu took %.2fms", cur_size, num_moved, to_get, _num_cards_ready, (Ticks::now() - batch_start).seconds() * MILLIUNITS);
#endif
  }
  guarantee(_cleaning.empty(), "must be at end");
  if (num_moved > 0) {
    log_debug(gc, refine)("Wanted %zu moved %zu buffers from completed (%zu) to ready (%zu, initial %zu) stop_at %zu (time: %.2fms)",
                          to_get, num_moved, align_up(Atomic::load(&_num_cards_completed), capacity) / capacity, align_up(Atomic::load(&_num_cards_ready), capacity) / capacity, align_up(num_ready_start, capacity) / capacity, stop_at,
                          (Ticks::now() - start).seconds() * MILLIUNITS);
  }
  return true;
}


void G1DirtyCardQueueSet::abandon_logs_and_stats() {
  assert_at_safepoint();

  // Disable mutator refinement until concurrent refinement decides otherwise.
  set_mutator_refinement_threshold(SIZE_MAX);

  // Iterate over all the threads, resetting per-thread queues and stats.
  struct AbandonThreadLogClosure : public ThreadClosure {
    G1DirtyCardQueueSet& _qset;
    AbandonThreadLogClosure(G1DirtyCardQueueSet& qset) : _qset(qset) {}
    virtual void do_thread(Thread* t) {
      G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(t);
      _qset.reset_queue(queue);
      queue.refinement_stats()->reset();
    }
  } closure(*this);
  Threads::threads_do(&closure);

  enqueue_all_paused_buffers();
  abandon_completed_buffers();

  // Reset stats from detached threads.
  MutexLocker ml(G1DetachedRefinementStats_lock, Mutex::_no_safepoint_check_flag);
  _detached_refinement_stats.reset();
}

void G1DirtyCardQueueSet::update_refinement_stats(G1ConcurrentRefineStats& stats) {
  assert_at_safepoint();

  _concatenated_refinement_stats = stats;

  enqueue_all_paused_buffers();
  verify_num_cards();

  // Collect and reset stats from detached threads.
  MutexLocker ml(G1DetachedRefinementStats_lock, Mutex::_no_safepoint_check_flag);
  _concatenated_refinement_stats += _detached_refinement_stats;
  _detached_refinement_stats.reset();
}

G1ConcurrentRefineStats G1DirtyCardQueueSet::concatenate_log_and_stats(Thread* thread) {
  assert_at_safepoint();

  G1DirtyCardQueue& queue = G1ThreadLocalData::dirty_card_queue(thread);
  // Flush the buffer if non-empty.  Flush before accumulating and
  // resetting stats, since flushing may modify the stats.
  if (!queue.is_empty()) {
    flush_queue(queue);
  }

  G1ConcurrentRefineStats result = *queue.refinement_stats();
  queue.refinement_stats()->reset();
  return result;
}

G1ConcurrentRefineStats G1DirtyCardQueueSet::concatenated_refinement_stats() const {
  assert_at_safepoint();
  return _concatenated_refinement_stats;
}

void G1DirtyCardQueueSet::record_detached_refinement_stats(G1ConcurrentRefineStats* stats) {
  MutexLocker ml(G1DetachedRefinementStats_lock, Mutex::_no_safepoint_check_flag);
  _detached_refinement_stats += *stats;
  stats->reset();
}

size_t G1DirtyCardQueueSet::mutator_refinement_threshold() const {
  return Atomic::load(&_mutator_refinement_threshold);
}

void G1DirtyCardQueueSet::set_mutator_refinement_threshold(size_t value) {
  Atomic::store(&_mutator_refinement_threshold, value);
}
