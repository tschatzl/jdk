/*
 * Copyright (c) 2001, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/g1Analytics.hpp"
#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CardTableClaimTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSet.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1ConcurrentRefineSweepTask.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "gc/g1/g1HeapRegionRemSet.inline.hpp"
#include "gc/g1/g1Policy.hpp"
#include "gc/shared/gc_globals.hpp"
#include "gc/shared/workerThread.hpp"
#include "logging/log.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "runtime/java.hpp"
#include "runtime/mutexLocker.hpp"
#include "utilities/debug.hpp"
#include "utilities/globalDefinitions.hpp"
#include "utilities/ticks.hpp"
#include <math.h>

G1ConcurrentRefineThread* G1ConcurrentRefineThreadControl::create_refinement_thread() {
  G1ConcurrentRefineThread* result = nullptr;
  result = G1ConcurrentRefineThread::create(_cr);
  if (result == nullptr || result->osthread() == nullptr) {
    log_warning(gc)("Failed to create refinement control thread, no more %s",
                    result == nullptr ? "memory" : "OS threads");
    if (result != nullptr) {
      delete result;
      result = nullptr;
    }
  }
  return result;
}

G1ConcurrentRefineThreadControl::G1ConcurrentRefineThreadControl(uint max_num_threads) :
  _cr(nullptr),
  _control_thread(nullptr),
  _workers(nullptr),
  _max_num_threads(max_num_threads)
{}

G1ConcurrentRefineThreadControl::~G1ConcurrentRefineThreadControl() {
  delete _control_thread;
  delete _workers;
}

jint G1ConcurrentRefineThreadControl::initialize(G1ConcurrentRefine* cr) {
  assert(cr != nullptr, "G1ConcurrentRefine must not be null");
  _cr = cr;

  if (is_refinement_enabled()) {
    _control_thread = create_refinement_thread();
    if (_control_thread == nullptr) {
      vm_shutdown_during_initialization("Could not allocate refinement control thread");
      return JNI_ENOMEM;
    }
    _workers = new WorkerThreads("G1 Refinement Workers", max_num_threads());
    _workers->initialize_workers();
  }
  return JNI_OK;
}

#ifdef ASSERT
void G1ConcurrentRefineThreadControl::assert_current_thread_is_control_refinement_thread() const {
  assert(Thread::current() == _control_thread, "Not refinement control thread");
}
#endif // ASSERT

void G1ConcurrentRefineThreadControl::activate() {
  _control_thread->activate();
}

void G1ConcurrentRefineThreadControl::run_task(WorkerTask* task, uint num_workers) {
  assert(num_workers >= 1, "must be");

  WithActiveWorkers w(_workers, num_workers);
  _workers->run_task(task);
}

void G1ConcurrentRefineThreadControl::control_thread_do(ThreadClosure* tc) {
  if (is_refinement_enabled()) {
    tc->do_thread(_control_thread);
  }
}

void G1ConcurrentRefineThreadControl::worker_threads_do(ThreadClosure* tc) {
  if (is_refinement_enabled()) {
    _workers->threads_do(tc);
  }
}

void G1ConcurrentRefineThreadControl::stop() {
  if (is_refinement_enabled()) {
    _control_thread->stop();
  }
}

G1ConcurrentRefineSweepState::G1ConcurrentRefineSweepState(uint max_reserved_regions) :
  _state(State::Idle),
  _sweep_start_epoch(0),
  _sweep_table(new G1CardTableClaimTable(G1CollectedHeap::get_chunks_per_region_for_merge())),
  _stats()
{
  _sweep_table->initialize(max_reserved_regions);
}

G1ConcurrentRefineSweepState::~G1ConcurrentRefineSweepState() {
  delete _sweep_table;
}

void G1ConcurrentRefineSweepState::set_state_start_time() {
  _state_start[static_cast<uint>(_state)] = Ticks::now();
}

Tickspan G1ConcurrentRefineSweepState::get_duration(State start, State end) {
  return _state_start[static_cast<uint>(end)] - _state_start[static_cast<uint>(start)];
}

void G1ConcurrentRefineSweepState::reset_stats() {
  stats()->reset();
}

void G1ConcurrentRefineSweepState::add_yield_duration(jlong duration) {
  stats()->inc_yield_duration(duration);
}

size_t G1ConcurrentRefineSweepState::refinement_epoch() {
  return G1CollectedHeap::heap()->refinement_epoch();
}

bool G1ConcurrentRefineSweepState::in_sweep_epoch() const {
  return _sweep_start_epoch == refinement_epoch();
}

bool G1ConcurrentRefineSweepState::advance_state(State next_state) {
  bool result = in_sweep_epoch();
  if (result) {
    _state = next_state;
  } else {
    _state = State::Idle;
  }
  return result;
}

void G1ConcurrentRefineSweepState::assert_state(State expected) {
  assert(_state == expected, "must be %s but is %s", state_name(expected), state_name(_state));
}

void G1ConcurrentRefineSweepState::start_work() {
  assert_state(State::Idle);

  set_state_start_time();

  _sweep_start_epoch = refinement_epoch();

  _stats.reset();

  advance_state(State::SwapGlobalCT);
}

bool G1ConcurrentRefineSweepState::swap_global_card_table() {
  assert_state(State::SwapGlobalCT);

  set_state_start_time();

  {
    // We can't have any new threads being in the process of created while we
    // swap the card table because we read the current card table state during
    // initialization.
    // A safepoint may occur during that time, so leave the STS temporarily.
    SuspendibleThreadSetLeaver sts_leave;

    size_t epoch = refinement_epoch();
    MutexLocker mu(Threads_lock);
    // A GC that advanced the epoch might have happened, which already switched
    // The global card table. Do nothing.
    log_debug(gc,refine)("swap global %zu == %zu?", epoch, _sweep_start_epoch);
    if (in_sweep_epoch()) {
      G1BarrierSet::g1_barrier_set()->swap_global_card_table();
    } else {
      G1CollectedHeap::heap()->verifier()->verify_card_tables_clean(true /* refinement_table_only */);
    }
  }

  return advance_state(State::SwapJavaThreadsCT);
}

bool G1ConcurrentRefineSweepState::swap_java_threads_ct() {
  assert_state(State::SwapJavaThreadsCT);

  set_state_start_time();

  {
    SuspendibleThreadSetLeaver sts_leave;

    class SwapThreadCardTableClosure : public HandshakeClosure {
    public:
      SwapThreadCardTableClosure() : HandshakeClosure("G1 Swap JT card table") { }

      virtual void do_thread(Thread* t) {
        G1BarrierSet* bs = G1BarrierSet::g1_barrier_set();
        {
          ResourceMark rm;
          G1CardTable::CardValue* table = bs->card_table()->byte_map_base();
          log_debug(gc, refine)("set ct base2 " PTR_FORMAT " thread " PTR_FORMAT " %s", p2i(table), p2i(t), t->name());
        }
        bs->update_card_table_base(t);
      }
    } cl;
    Handshake::execute(&cl);
  }

  return advance_state(State::SynchronizeGCThreads);
}

bool G1ConcurrentRefineSweepState::swap_gc_threads_ct() {
  assert_state(State::SynchronizeGCThreads);

  set_state_start_time();

  {
    class RendezvousGCThreads: public VM_Operation {
    public:
      VMOp_Type type() const { return VMOp_G1RendezvousGCThreads; }

      virtual bool evaluate_at_safepoint() const {
        // We only care about synchronizing the GC threads.
        // Leave the Java threads running.
        return false;
      }

      virtual bool skip_thread_oop_barriers() const {
        fatal("Concurrent VMOps should not call this");
        return true;
      }

      void doit() {
        // Light weight "handshake" of the GC threads for memory synchronization.
        SuspendibleThreadSet::synchronize();
        SuspendibleThreadSet::desynchronize();
      };
    } op;

    SuspendibleThreadSetLeaver sts_leave;
    VMThread::execute(&op);
  }

  return advance_state(State::SnapshotHeap);
}

void G1ConcurrentRefineSweepState::snapshot_heap(bool concurrent) {
  if (concurrent) {
    assert_state(State::SnapshotHeap);

    set_state_start_time();
  } else {
    assert_state(State::Idle);
    assert_at_safepoint();
  }

  snapshot_heap_into(_sweep_table);

  if (concurrent) {
    advance_state(State::SweepRT);
  }
}

void G1ConcurrentRefineSweepState::sweep_refinement_table_start() {
  assert_state(State::SweepRT);

  set_state_start_time();
}

bool G1ConcurrentRefineSweepState::sweep_refinement_table_step() {
  assert_state(State::SweepRT);

  G1ConcurrentRefine* cr = G1CollectedHeap::heap()->concurrent_refine();

  G1ConcurrentRefineSweepTask task(_sweep_table, &_stats, cr->num_threads_wanted());
  cr->run_with_refinement_workers(&task);

  if (task.sweep_completed()) {
    _state = State::CompleteRefineWork;
    return true;
  } else {
    return false;
  }
}

bool G1ConcurrentRefineSweepState::complete_work(bool concurrent, bool print_log) {
  if (concurrent) {
    assert_state(State::CompleteRefineWork);
  } else {
    // May have been forced to complete at any other time.
    assert(is_in_progress() && _state != State::CompleteRefineWork, "must be but is %s", state_name(_state));
  }

  set_state_start_time();

  if (print_log) {
    G1ConcurrentRefineStats* s = &_stats;

    log_debug(gc, refine)("Refinement took %.2fms (pre-sweep %.2fms card refine %.2f) "
                          "(scanned %zu clean %zu (%.2f%%) not_clean %zu (%.2f%%) not_parsable %zu "
                          "refers_to_cset %zu (%.2f%%) still_refers_to_cset %zu (%.2f%%) clean_again %zu pending %zu)",
                          get_duration(State::Idle, _state).seconds() * 1000.0,
                          get_duration(State::Idle, State::SweepRT).seconds() * 1000.0,
                          TimeHelper::counter_to_millis(s->refine_duration()),
                          s->cards_scanned(),
                          s->cards_clean(),
                          percent_of(s->cards_clean(), s->cards_scanned()),
                          s->cards_not_clean(),
                          percent_of(s->cards_not_clean(), s->cards_scanned()),
                          s->cards_not_parsable(),
                          s->cards_refer_to_cset(),
                          percent_of(s->cards_refer_to_cset(), s->cards_not_clean()),
                          s->cards_already_refer_to_cset(),
                          percent_of(s->cards_already_refer_to_cset(), s->cards_not_clean()),
                          s->cards_clean_again(),
                          s->cards_pending()
                         );
  }

  bool has_sweep_rt_work = is_in_progress() && _state == State::SweepRT;

  _state = State::Idle;
  return has_sweep_rt_work;
}

void G1ConcurrentRefineSweepState::snapshot_heap_into(G1CardTableClaimTable* sweep_table) {
  // G1CollectedHeap::heap_region_iterate() below will only visit currently committed
  // regions. Initialize all entries in the state table here and later in this method
  // selectively enable regions that we are interested. This way regions committed
  // later will be automatically excluded from iteration.
  // Their refinement table must be completely empty anyway.
  sweep_table->reset_all_to_claimed();

  class SnapshotRegionsClosure : public G1HeapRegionClosure {
    G1CardTableClaimTable* _sweep_table;

  public:
    SnapshotRegionsClosure(G1CardTableClaimTable* sweep_table) : G1HeapRegionClosure(), _sweep_table(sweep_table) { }

    bool do_heap_region(G1HeapRegion* r) override {
      if (!r->is_free()) {
        // Need to scan all parts of non-free regions, so reset the claim.
        // No need for synchronization: we are only interested in regions
        // that were allocated before the handshake; the handshake makes such
        // regions' metadata visible to all threads, and we do not care about
        // humongous regions that were allocated afterwards.
        _sweep_table->reset_to_unclaimed(r->hrm_index());
      }
      return false;
    }
  } cl(sweep_table);
  G1CollectedHeap::heap()->heap_region_iterate(&cl);
}

bool G1ConcurrentRefineSweepState::is_in_progress() const {
  return _state != State::Idle;
}

bool G1ConcurrentRefineSweepState::are_java_threads_synched() const {
  return _state > State::SwapJavaThreadsCT || !is_in_progress();
}

uint64_t G1ConcurrentRefine::adjust_threads_period_ms() const {
  // Instead of a fixed value, this could be a command line option.  But then
  // we might also want to allow configuration of adjust_threads_wait_ms().
  return 50;
}

static size_t minimum_pending_cards_target() {
  // One buffer per thread.
  return ParallelGCThreads * G1UpdateBufferSize;
}

G1ConcurrentRefine::G1ConcurrentRefine(G1CollectedHeap* g1h) :
  _policy(g1h->policy()),
  _num_threads_wanted(0),
  _pending_cards_target(PendingCardsTargetUninitialized),
  _last_adjust(),
  _needs_adjust(false),
  _heap_was_locked(false),
  _threads_needed(g1h->policy(), adjust_threads_period_ms()),
  _thread_control(G1ConcRefinementThreads),
  _sweep_state(g1h->max_reserved_regions())
{ }

jint G1ConcurrentRefine::initialize() {
  return _thread_control.initialize(this);
}

G1ConcurrentRefineSweepState& G1ConcurrentRefine::sweep_state_for_merge() {
  bool has_sweep_claims = sweep_state().complete_work(false /* concurrent */);
  if (has_sweep_claims) {
    log_debug(gc, refine)("Continue existing work");
  } else {
    // Refinement has been interrupted without having a snapshot. There may
    // be a mix of already swapped and not-swapped card tables assigned to threads,
    // so they might have already dirtied the swapped card tables.
    // Conservatively scan all (non-free, non-committed) region's card tables,
    // creating the snapshot right now.
    log_debug(gc, refine)("Create work from scratch");

    sweep_state().snapshot_heap(false /* concurrent */);
  }
  return sweep_state();
}

void G1ConcurrentRefine::run_with_refinement_workers(WorkerTask* task) {
  _thread_control.run_task(task, num_threads_wanted());
}

void G1ConcurrentRefine::notify_region_reclaimed(G1HeapRegion* r) {
  assert_at_safepoint();
  if (_sweep_state.is_in_progress()) {
    _sweep_state.sweep_table()->claim_all_cards(r->hrm_index());
  }
}

G1ConcurrentRefine* G1ConcurrentRefine::create(G1CollectedHeap* g1h, jint* ecode) {
  G1ConcurrentRefine* cr = new G1ConcurrentRefine(g1h);
  *ecode = cr->initialize();
  if (*ecode != 0) {
    delete cr;
    cr = nullptr;
  }
  return cr;
}

void G1ConcurrentRefine::stop() {
  _thread_control.stop();
}

G1ConcurrentRefine::~G1ConcurrentRefine() {
}

void G1ConcurrentRefine::threads_do(ThreadClosure *tc) {
  worker_threads_do(tc);
  control_thread_do(tc);
}

void G1ConcurrentRefine::worker_threads_do(ThreadClosure *tc) {
  _thread_control.worker_threads_do(tc);
}

void G1ConcurrentRefine::control_thread_do(ThreadClosure *tc) {
  _thread_control.control_thread_do(tc);
}

void G1ConcurrentRefine::update_pending_cards_target(double logged_cards_time_ms,
                                                     size_t processed_logged_cards,
                                                     double goal_ms) {
  size_t minimum = minimum_pending_cards_target();
  if ((processed_logged_cards < minimum) || (logged_cards_time_ms == 0.0)) {
    log_debug(gc, ergo, refine)("Unchanged pending cards target: %zu (processed %zu minimum %zu time %1.2f)",
                                _pending_cards_target, processed_logged_cards, minimum, logged_cards_time_ms);
    return;
  }

  // Base the pending cards budget on the measured rate.
  double rate = processed_logged_cards / logged_cards_time_ms;
  size_t new_target = static_cast<size_t>(goal_ms * rate);
  // Add some hysteresis with previous values.
  if (is_pending_cards_target_initialized()) {
    new_target = (new_target + _pending_cards_target) / 2;
  }
  // Apply minimum target.
  new_target = MAX2(new_target, minimum_pending_cards_target());
  _pending_cards_target = new_target;
  log_debug(gc, ergo, refine)("New pending cards target: %zu", new_target);
}

void G1ConcurrentRefine::adjust_after_gc(double logged_cards_time_ms,
                                         size_t processed_logged_cards,
                                         double goal_ms) {
  if (!G1UseConcRefinement) {
    return;
  }

  update_pending_cards_target(logged_cards_time_ms,
                              processed_logged_cards,
                              goal_ms);
  if (_thread_control.is_refinement_enabled()) {
    _needs_adjust = true;
    if (is_pending_cards_target_initialized()) {
      _thread_control.activate();
    }
  }
}

// Wake up the control thread less frequently when the time available until
// the next GC is longer.  But don't increase the wait time too rapidly.
// This reduces the number of control thread wakeups that just immediately
// go back to waiting, while still being responsive to behavior changes.
static uint64_t compute_adjust_wait_time_ms(double available_ms) {
  return static_cast<uint64_t>(sqrt(available_ms) * 4.0);
}

uint64_t G1ConcurrentRefine::adjust_threads_wait_ms() const {
  assert_current_thread_is_control_refinement_thread();
  if (is_pending_cards_target_initialized()) {
    // Retry asap when the cause for not getting a prediction was that we temporarily
    // did not get the heap lock. Otherwise we might wait for too long until we get
    // back here.
    if (_heap_was_locked) {
      return 1;
    }
    double available_ms = _threads_needed.predicted_time_until_next_gc_ms();
    uint64_t wait_time_ms = compute_adjust_wait_time_ms(available_ms);
    return MAX2(wait_time_ms, adjust_threads_period_ms());
  } else {
    // If target not yet initialized then wait forever (until explicitly
    // activated).  This happens during startup, when we don't bother with
    // refinement.
    return 0;
  }
}

class G1ConcurrentRefine::RemSetSamplingClosure : public G1HeapRegionClosure {
  size_t _sampled_code_root_rs_length;

public:
  RemSetSamplingClosure() :
    _sampled_code_root_rs_length(0) {}

  bool do_heap_region(G1HeapRegion* r) override {
    G1HeapRegionRemSet* rem_set = r->rem_set();
    _sampled_code_root_rs_length += rem_set->code_roots_list_length();
    return false;
  }

  size_t sampled_code_root_rs_length() const { return _sampled_code_root_rs_length; }
};

// Adjust the target length (in regions) of the young gen, based on the
// current length of the remembered sets.
//
// At the end of the GC G1 determines the length of the young gen based on
// how much time the next GC can take, and when the next GC may occur
// according to the MMU.
//
// The assumption is that a significant part of the GC is spent on scanning
// the remembered sets (and many other components), so this thread constantly
// reevaluates the prediction for the remembered set scanning costs, and potentially
// resizes the young gen. This may do a premature GC or even increase the young
// gen size to keep pause time length goal.
void G1ConcurrentRefine::adjust_young_list_target_length() {
  if (_policy->use_adaptive_young_list_length()) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1CollectionSet* cset = g1h->collection_set();
    RemSetSamplingClosure cl;
    cset->iterate(&cl);

    size_t pending_cards;
    size_t current_to_collection_set_cards;
    {
      MutexLocker x(G1RareEvent_lock, Mutex::_no_safepoint_check_flag);
      G1Policy* p = g1h->policy();
      pending_cards = p->current_pending_cards();
      current_to_collection_set_cards = p->current_to_collection_set_cards();
    }
    _policy->revise_young_list_target_length(pending_cards,
                                             current_to_collection_set_cards,
                                             cl.sampled_code_root_rs_length());
  }
}

bool G1ConcurrentRefine::adjust_num_threads_periodically() {
  assert_current_thread_is_control_refinement_thread();

  _heap_was_locked = false;
  // Check whether it's time to do a periodic adjustment if there is no explicit
  // request pending. We might have spuriously woken up.
  if (!_needs_adjust) {
    Tickspan since_adjust = Ticks::now() - _last_adjust;
    if (since_adjust.milliseconds() < adjust_threads_period_ms()) {
      _num_threads_wanted = 0;
      return false;
    }
  }

  // Reset pending request.
  _needs_adjust = false;
  // Getting used young bytes requires holding Heap_lock.  But we can't use
  // normal lock and block until available.  Blocking on the lock could
  // deadlock with a GC VMOp that is holding the lock and requesting a
  // safepoint.  Instead try to lock, and if fail then skip adjustment for
  // this iteration and retry the adjustment later.
  if (Heap_lock->try_lock()) {
    size_t used_bytes = _policy->estimate_used_young_bytes_locked();
    Heap_lock->unlock();

    adjust_young_list_target_length();
    size_t young_bytes = _policy->young_list_target_length() * G1HeapRegion::GrainBytes;
    size_t available_bytes = young_bytes - MIN2(young_bytes, used_bytes);
    adjust_threads_wanted(available_bytes);
    _last_adjust = Ticks::now();
  } else {
    _heap_was_locked = true;
    // Defer adjustment to next time.
    _needs_adjust = true;
  }

  return (_num_threads_wanted > 0) && !wait_for_heap_lock();
}

void G1ConcurrentRefine::adjust_threads_wanted(size_t available_bytes) {
  assert_current_thread_is_control_refinement_thread();

  G1Policy* policy = G1CollectedHeap::heap()->policy();
  const G1Analytics* analytics = policy->analytics();

  size_t num_cards = policy->current_pending_cards();

  _threads_needed.update(_num_threads_wanted,
                         available_bytes,
                         num_cards,
                         _pending_cards_target);
  uint new_wanted = _threads_needed.threads_needed();
  if (new_wanted > _thread_control.max_num_threads()) {
    // Bound the wanted threads by maximum available.
    new_wanted = _thread_control.max_num_threads();
  }

  _num_threads_wanted = MAX2(G1StressBarriers ? 1u : 0u, new_wanted);

  log_debug(gc, refine)("Concurrent refinement: wanted %u, pending cards: %zu (pending-from-gc %zu), "
                        "predicted: %zu, goal %zu, time-until-next-gc: %1.2fms pred-refine-rate %1.2fc/ms log-rate %1.2fc/ms",
                        new_wanted,
                        num_cards,
                        G1CollectedHeap::heap()->policy()->pending_cards_from_gc(),
                        _threads_needed.predicted_cards_at_next_gc(),
                        _pending_cards_target,
                        _threads_needed.predicted_time_until_next_gc_ms(),
                        analytics->predict_concurrent_refine_rate_ms(),
                        analytics->predict_dirtied_cards_rate_ms()
                        );
}

bool G1ConcurrentRefine::is_thread_adjustment_needed() const {
  assert_current_thread_is_control_refinement_thread();
  return _needs_adjust;
}

void G1ConcurrentRefine::record_thread_adjustment_needed() {
  assert_current_thread_is_control_refinement_thread();
  _needs_adjust = true;
}
