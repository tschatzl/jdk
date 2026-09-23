/*
 * Copyright (c) 2013, 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/g1BarrierSet.hpp"
#include "gc/g1/g1CardSetGroup.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefine.hpp"
#include "gc/g1/g1ConcurrentRefineThread.hpp"
#include "gc/g1/g1HeapRegion.hpp"
#include "gc/g1/g1HeapRegionRemSet.inline.hpp"
#include "gc/g1/g1RemSet.hpp"
#include "gc/g1/g1RemSetSummary.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/iterator.hpp"
#include "runtime/javaThread.hpp"

void G1RemSetSummary::update() {
  G1ConcurrentRefine* refine = G1CollectedHeap::heap()->concurrent_refine();

  class CollectWorkerData : public ThreadClosure {
    G1RemSetSummary* _summary;
    uint _counter;
  public:
    CollectWorkerData(G1RemSetSummary* summary) : _summary(summary),  _counter(0) {}
    virtual void do_thread(Thread* t) {
      G1ConcurrentRefineThread* crt = static_cast<G1ConcurrentRefineThread*>(t);
      _summary->set_worker_thread_cpu_time(_counter, crt->cpu_time());
      _counter++;
    }
  } collector(this);

  refine->worker_threads_do(&collector);

  class CollectControlData : public ThreadClosure {
    G1RemSetSummary* _summary;
  public:
    CollectControlData(G1RemSetSummary* summary) : _summary(summary) {}
    virtual void do_thread(Thread* t) {
      G1ConcurrentRefineThread* crt = static_cast<G1ConcurrentRefineThread*>(t);
      _summary->set_control_thread_cpu_time(crt->cpu_time());
    }
  } control(this);

  refine->control_thread_do(&control);
}

void G1RemSetSummary::set_worker_thread_cpu_time(uint thread, jlong value) {
  assert(_worker_threads_cpu_times != nullptr, "just checking");
  assert(thread < _num_worker_threads, "just checking");
  _worker_threads_cpu_times[thread] = value;
}

void G1RemSetSummary::set_control_thread_cpu_time(jlong value) {
  _control_thread_cpu_time = value;
}

jlong G1RemSetSummary::worker_thread_cpu_time(uint thread) const {
  assert(_worker_threads_cpu_times != nullptr, "just checking");
  assert(thread < _num_worker_threads, "just checking");
  return _worker_threads_cpu_times[thread];
}

jlong G1RemSetSummary::control_thread_cpu_time() const {
  return _control_thread_cpu_time;
}

G1RemSetSummary::G1RemSetSummary(bool should_update) :
  _num_worker_threads(G1ConcRefinementThreads),
  _worker_threads_cpu_times(NEW_C_HEAP_ARRAY(jlong, _num_worker_threads, mtGC)),
  _control_thread_cpu_time(0) {

  memset(_worker_threads_cpu_times, 0, sizeof(jlong) * _num_worker_threads);

  if (should_update) {
    update();
  }
}

G1RemSetSummary::~G1RemSetSummary() {
  FREE_C_HEAP_ARRAY(_worker_threads_cpu_times);
}

void G1RemSetSummary::set(G1RemSetSummary* other) {
  assert(other != nullptr, "just checking");
  assert(_num_worker_threads == other->_num_worker_threads, "just checking");

  memcpy(_worker_threads_cpu_times, other->_worker_threads_cpu_times, sizeof(jlong) * _num_worker_threads);
  _control_thread_cpu_time = other->_control_thread_cpu_time;
}

void G1RemSetSummary::subtract_from(G1RemSetSummary* other) {
  assert(other != nullptr, "just checking");
  assert(_num_worker_threads == other->_num_worker_threads, "just checking");

  for (uint i = 0; i < _num_worker_threads; i++) {
    set_worker_thread_cpu_time(i, other->worker_thread_cpu_time(i) - worker_thread_cpu_time(i));
  }
  _control_thread_cpu_time = other->_control_thread_cpu_time - _control_thread_cpu_time;
}

class G1PerRegionTypeRemSetCounters {
  const char* _name;

  size_t _card_set_group_unused_bytes;
  size_t _card_set_group_bytes;
  size_t _card_set_group_num_cards;
  size_t _num_regions;
  size_t _num_tracked_regions;

  size_t _hrrs_bytes;
  size_t _code_root_length;

  double card_set_group_bytes_percent_of(size_t total) {
    return percent_of(_card_set_group_bytes, total);
  }

  double cards_occupied_percent_of(size_t total) {
    return percent_of(_card_set_group_num_cards, total);
  }

  double hrrs_bytes_percent_of(size_t total) {
    return percent_of(_hrrs_bytes, total);
  }

  double code_root_length_percent_of(size_t total) {
    return percent_of(_code_root_length, total);
  }

  size_t num_regions() const { return _num_regions; }
  size_t num_tracked_regions() const { return _num_tracked_regions; }

public:
  G1PerRegionTypeRemSetCounters(const char* name) : _name(name), _card_set_group_unused_bytes(0), _card_set_group_bytes(0), _card_set_group_num_cards(0),
    _num_regions(0), _num_tracked_regions(0), _hrrs_bytes(0), _code_root_length(0) { }

  void add(size_t card_set_group_unused_bytes, size_t card_set_group_bytes, size_t card_set_group_num_cards,
           size_t hrrs_bytes, size_t code_root_length, bool tracked) {
    _card_set_group_unused_bytes += card_set_group_unused_bytes;
    _card_set_group_bytes += card_set_group_bytes;
    _card_set_group_num_cards += card_set_group_num_cards;
    _hrrs_bytes += hrrs_bytes;
    _code_root_length += code_root_length;
    _num_regions++;
    _num_tracked_regions += tracked ? 1 : 0;
  }

  size_t card_set_group_unused_bytes() const { return _card_set_group_unused_bytes; }
  size_t card_set_group_bytes() const { return _card_set_group_bytes; }
  size_t cards_occupied() const { return _card_set_group_num_cards; }

  size_t hrrs_bytes() const { return _hrrs_bytes; }
  size_t code_root_elems() const { return _code_root_length; }

  void print_card_set_group_memory_usage_on(outputStream * out, size_t total) {
    out->print_cr("    %8zu (%5.1f%%) by %zu "
                  "(%zu) %s regions unused %zu",
                  card_set_group_bytes(), card_set_group_bytes_percent_of(total),
                  num_tracked_regions(), num_regions(),
                  _name, card_set_group_unused_bytes());
  }

  void print_card_set_group_cards_occupied_on(outputStream * out, size_t total) {
    out->print_cr("     %8zu (%5.1f%%) entries by %zu "
                  "(%zu) %s regions",
                  cards_occupied(), cards_occupied_percent_of(total),
                  num_tracked_regions(), num_regions(), _name);
  }

  void print_hrrs_memory_usage_on(outputStream * out, size_t total) {
    out->print_cr("    %8zu%s (%5.1f%%) by %zu %s regions",
        byte_size_in_proper_unit(hrrs_bytes()),
        proper_unit_for_byte_size(hrrs_bytes()),
        hrrs_bytes_percent_of(total), num_regions(), _name);
  }

  void print_code_root_length_on(outputStream * out, size_t total) {
    out->print_cr("     %8zu (%5.1f%%) elements by %zu %s regions",
        code_root_elems(), code_root_length_percent_of(total), num_regions(), _name);
  }
};


class G1HeapRegionStatsClosure: public G1HeapRegionClosure {
  G1PerRegionTypeRemSetCounters _young;
  G1PerRegionTypeRemSetCounters _humongous;
  G1PerRegionTypeRemSetCounters _free;
  G1PerRegionTypeRemSetCounters _old;
  G1PerRegionTypeRemSetCounters _all;

  size_t _max_hrrs_bytes;
  G1HeapRegion* _max_hrrs_bytes_region;

  size_t _max_card_set_group_used_bytes;
  G1CardSetGroup* _max_card_set_group_used_bytes_group;

  size_t total_card_set_group_unused_bytes() const     { return _all.card_set_group_unused_bytes(); }
  size_t total_card_set_group_bytes() const            { return _all.card_set_group_bytes(); }
  size_t total_cards_occupied() const       { return _all.cards_occupied(); }

  size_t max_card_set_group_used_bytes() const                 { return _max_card_set_group_used_bytes; }
  G1CardSetGroup* max_card_set_group_used_bytes_group() const  { return _max_card_set_group_used_bytes_group; }

  size_t total_hrrs_bytes() const     { return _all.hrrs_bytes(); }
  size_t total_code_root_elems() const      { return _all.code_root_elems(); }

  size_t max_hrrs_bytes() const       { return _max_hrrs_bytes; }
  G1HeapRegion* max_hrrs_bytes_region() const { return _max_hrrs_bytes_region; }

public:
  G1HeapRegionStatsClosure() : _young("Young"), _humongous("Humongous"),
    _free("Free"), _old("Old"), _all("All"), _max_hrrs_bytes(0),
    _max_hrrs_bytes_region(nullptr),
    _max_card_set_group_used_bytes(0), _max_card_set_group_used_bytes_group(nullptr)
  {}

  bool do_heap_region(G1HeapRegion* r) {
    G1HeapRegionRemSet* hrrs = r->rem_set();

    size_t code_root_length = hrrs->code_roots_length();
    size_t hrrs_bytes = hrrs->mem_size();

    size_t card_set_group_num_cards = 0;
    size_t card_set_group_used_bytes = 0;
    size_t card_set_group_unused_bytes = 0;

    // Accumulate card set details for regions. Avoid duplicate accounting by using the
    // first element of the card set group as representative of that card set group.
    // G1HeapRegionRemSet::mem_size() includes the size of the code roots
    if (hrrs->has_card_set_group() && (r->hrm_index() == hrrs->card_set_group()->region_at(0)->hrm_index())) {
      G1CardSet* card_set = hrrs->card_set_group()->card_set();
      card_set_group_used_bytes = card_set->mem_size();
      card_set_group_unused_bytes = card_set->unused_mem_size();
      card_set_group_num_cards = card_set->occupied();

      if (card_set_group_used_bytes > _max_card_set_group_used_bytes) {
        _max_card_set_group_used_bytes = card_set_group_used_bytes;
        _max_card_set_group_used_bytes_group = hrrs->card_set_group();
      }
    }

    if (hrrs_bytes > _max_hrrs_bytes) {
      _max_hrrs_bytes = hrrs_bytes;
      _max_hrrs_bytes_region = r;
    }

    G1PerRegionTypeRemSetCounters* current = nullptr;
    if (r->is_free()) {
      current = &_free;
    } else if (r->is_young()) {
      current = &_young;
    } else if (r->is_humongous()) {
      current = &_humongous;
    } else if (r->is_old()) {
      current = &_old;
    } else {
      ShouldNotReachHere();
    }
    current->add(card_set_group_unused_bytes, card_set_group_used_bytes, card_set_group_num_cards,
                 hrrs_bytes, code_root_length, r->rem_set()->is_tracked());
    _all.add(card_set_group_unused_bytes, card_set_group_used_bytes, card_set_group_num_cards,
             hrrs_bytes, code_root_length, r->rem_set()->is_tracked());

    return false;
  }

  void print_summary_on(outputStream* out) {
    G1PerRegionTypeRemSetCounters* counters[] = { &_young, &_humongous, &_free, &_old, nullptr };

    out->print_cr(" Current heap region remembered set statistics");
    out->print_cr("  Total card set group memory usage = %zu"
                  " unused = %zu Max individual = %zu (%u)",
                  total_card_set_group_bytes(),
                  total_card_set_group_unused_bytes(),
                  max_card_set_group_used_bytes(),
                  max_card_set_group_used_bytes_group() != nullptr ? max_card_set_group_used_bytes_group()->group_id() : G1CardSetGroup::NoGroupId
                  );
    for (G1PerRegionTypeRemSetCounters** current = &counters[0]; *current != nullptr; current++) {
      (*current)->print_card_set_group_memory_usage_on(out, total_card_set_group_bytes());
    }

    out->print_cr("    %zu occupied cards.",
                  total_cards_occupied());
    for (G1PerRegionTypeRemSetCounters** current = &counters[0]; *current != nullptr; current++) {
      (*current)->print_card_set_group_cards_occupied_on(out, total_cards_occupied());
    }

    // Largest sized single region HRRS statistics
    if (max_hrrs_bytes_region() != nullptr) {
      G1HeapRegionRemSet* rem_set = max_hrrs_bytes_region()->rem_set();
      out->print_cr("    Region with largest memory usage = " HR_FORMAT ", "
                    "size = %zu code roots = %zu occupied = %zu",
                    HR_FORMAT_PARAMS(max_hrrs_bytes_region()),
                    max_hrrs_bytes(),
                    rem_set->code_roots_length(),
                    rem_set->occupied());
    }

    if (max_card_set_group_used_bytes_group() != nullptr) {
      G1CardSetGroup* card_set_group = max_card_set_group_used_bytes_group();
      out->print_cr("    Card Set Group with largest card set = %u:(%u regions), "
                    "size = %zu occupied = %zu",
                    card_set_group->group_id(), card_set_group->num_regions(),
                    max_card_set_group_used_bytes(),
                    card_set_group->card_set()->occupied());
    }

    G1HeapRegionRemSet::print_static_mem_size(out);
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    g1h->card_set_freelist_pool()->print_on(out);

    // Code root statistics
    G1HeapRegionRemSet* max_hrrs_bytes_region_rem_set = max_hrrs_bytes_region()->rem_set();
    out->print_cr("  Total heap region rem set sizes = %zu%s."
                  "  Max = %zu%s.",
                  byte_size_in_proper_unit(total_hrrs_bytes()),
                  proper_unit_for_byte_size(total_hrrs_bytes()),
                  byte_size_in_proper_unit(max_hrrs_bytes_region_rem_set->mem_size()),
                  proper_unit_for_byte_size(max_hrrs_bytes_region_rem_set->mem_size()));
    for (G1PerRegionTypeRemSetCounters** current = &counters[0]; *current != nullptr; current++) {
      (*current)->print_hrrs_memory_usage_on(out, total_hrrs_bytes());
    }

    out->print_cr("    %zu code roots represented.",
                  total_code_root_elems());
    for (G1PerRegionTypeRemSetCounters** current = &counters[0]; *current != nullptr; current++) {
      (*current)->print_code_root_length_on(out, total_code_root_elems());
    }

    out->print_cr("    Region with largest amount of code roots = " HR_FORMAT ", "
                  "size = %zu%s, code roots = %zu.",
                  HR_FORMAT_PARAMS(max_hrrs_bytes_region()),
                  byte_size_in_proper_unit(max_hrrs_bytes_region_rem_set->code_roots_mem_size()),
                  proper_unit_for_byte_size(max_hrrs_bytes_region_rem_set->code_roots_mem_size()),
                  max_hrrs_bytes_region()->rem_set()->code_roots_length());
  }
};

void G1RemSetSummary::print_on(outputStream* out, bool show_thread_times) {
  if (show_thread_times) {
    out->print_cr(" Concurrent refinement threads times (s)");
    out->print_cr(" Control %5.2f Workers", (double)control_thread_cpu_time() / NANOSECS_PER_SEC);
    out->print("     ");
    for (uint i = 0; i < _num_worker_threads; i++) {
      out->print("    %5.2f", (double)worker_thread_cpu_time(i) / NANOSECS_PER_SEC);
    }
    out->cr();
  }
  G1HeapRegionStatsClosure blk;
  G1CollectedHeap::heap()->heap_region_iterate(&blk);
  blk.print_summary_on(out);
}
