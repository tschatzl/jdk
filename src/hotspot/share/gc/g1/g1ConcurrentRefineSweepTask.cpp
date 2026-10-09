/*
 * Copyright (c) 2025, 2026, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/g1CardTableClaimTable.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1ConcurrentRefineStats.inline.hpp"
#include "gc/g1/g1ConcurrentRefineSweepTask.hpp"

static void check_card_ptr(CardTable::CardValue* card_ptr, G1CardTable* ct) {
#ifdef ASSERT
  G1CollectedHeap* g1h = G1CollectedHeap::heap();
  assert(g1h->is_in(ct->addr_for(card_ptr)),
         "Card at " PTR_FORMAT " index %zu representing heap at " PTR_FORMAT " (%u) must be in committed heap",
         p2i(card_ptr),
         ct->index_for(ct->addr_for(card_ptr)),
         p2i(ct->addr_for(card_ptr)),
         g1h->addr_to_region(ct->addr_for(card_ptr)));
#endif
}

class G1RefineRegionClosure : public G1HeapRegionClosure {
  using CardValue = G1CardTable::CardValue;

  G1CardTableClaimTable* _scan_state;

  uintx _seen_heap_region_alloc_epoch;

  bool has_work(G1HeapRegion* r) {
    return _scan_state->has_unclaimed_cards(r->hrm_index());
  }

  void verify_card_pair_refers_to_same_card(CardValue* source_card, CardValue* dest_card) {
#ifdef ASSERT
    G1CollectedHeap* g1h = G1CollectedHeap::heap();
    G1HeapRegion* refinement_r = g1h->heap_region_containing(g1h->refinement_table()->addr_for(source_card));
    G1HeapRegion* card_r = g1h->heap_region_containing(g1h->card_table()->addr_for(dest_card));
    size_t refinement_i = g1h->refinement_table()->index_for_cardvalue(source_card);
    size_t card_i = g1h->card_table()->index_for_cardvalue(dest_card);

    assert(refinement_r == card_r, "not same region source %u (%zu) dest %u (%zu) ", refinement_r->hrm_index(), refinement_i, card_r->hrm_index(), card_i);
    assert(refinement_i == card_i, "indexes are not same %zu %zu", refinement_i, card_i);
#endif
  }

  enum RefineResult {
      HasRefToCSet,          // The (dirty) card has a reference to the collection set.
      AlreadyToCSet,         // The card is already one marked as having a reference to the collection set.
      HasRefToOld,           // The dirty card contains references to other old regions (not the collection set).
      NoCrossRegion,         // There is no interesting reference in the card any more. The mutator changed all
                             // references to such after dirtying the card.
      CouldNotParse          // The card is unparsable, need to retry later.
  };
  // Refine the region corresponding to "card_ptr". Must be called after
  // being filtered by clean_card_before_refine(), and after proper
  // fence/synchronization.
  RefineResult refine_card_concurrently(CardValue* const card_ptr) {
    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    assert(!g1h->is_stw_gc_active(), "Only call concurrently");
    G1CardTable* ct = g1h->refinement_table();
    check_card_ptr(card_ptr, ct);

    // That card is already known to contain a reference to the collection set. Skip
    // further processing.
    if (*card_ptr == G1CardTable::g1_to_cset_card) {
      return AlreadyToCSet;
    }

    // Construct the MemRegion representing the card.
    HeapWord* start = ct->addr_for(card_ptr);
    // And find the region containing it.
    G1HeapRegion* r = g1h->heap_region_containing(start);
    // This reload of the top is safe even though it happens after the full
    // fence, because top is stable for old and unfiltered humongous
    // regions, so it must return the same value as the previous load when
    // cleaning the card. Also cleaning the card and refinement of the card
    // cannot span across safepoint, so we don't need to worry about top being
    // changed during safepoint.
    HeapWord* scan_limit = r->top();
    assert(scan_limit > start, "sanity region %u (%s) scan_limit " PTR_FORMAT " start " PTR_FORMAT, r->hrm_index(), r->get_short_type_str(), p2i(scan_limit), p2i(start));

    // Don't use addr_for(card_ptr + 1) which can ask for
    // a card beyond the heap.
    HeapWord* end = start + G1CardTable::card_size_in_words();
    MemRegion dirty_region(start, MIN2(scan_limit, end));
    assert(!dirty_region.is_empty(), "sanity");

    G1ConcurrentRefineOopClosure conc_refine_cl(g1h, _seen_heap_region_alloc_epoch);
    bool humongous_not_initialized =
      r->oops_on_memregion_seq_iterate_careful<false>(dirty_region, &conc_refine_cl) == nullptr;
    if (conc_refine_cl.has_ref_to_cset()) {
      return HasRefToCSet;
    } else if (humongous_not_initialized || conc_refine_cl.has_ref_unparsable()) {
      // If unable to process the card then we encountered an unparsable
      // part of the heap (e.g. a partially allocated object, so only
      // temporarily a problem) while processing a stale card.  Despite
      // the card being stale, we can't simply ignore it, because we've
      // already marked the card as cleaned, so taken responsibility for
      // ensuring the card gets scanned.
      return CouldNotParse;
    } else if (conc_refine_cl.has_ref_to_old()) {
      return HasRefToOld;
    } else {
      return NoCrossRegion;
    }
  }

  void do_dirty_card(CardValue* source_card, CardValue* dest_card) {
    verify_card_pair_refers_to_same_card(source_card, dest_card);

    RefineResult res = refine_card_concurrently(source_card);
    // Gather statistics based on the result.
    switch (res) {
      case HasRefToCSet: {
        *dest_card = G1CardTable::g1_to_cset_card;
        _per_worker_refine_data._cards_refer_to_cset++;
        break;
      }
      case AlreadyToCSet: {
        *dest_card = G1CardTable::g1_to_cset_card;
        _per_worker_refine_data._cards_already_refer_to_cset++;
        break;
      }
      case NoCrossRegion: {
        _per_worker_refine_data._cards_no_cross_region++;
        break;
      }
      case CouldNotParse: {
        // Could not refine - redirty with the original value.
        *dest_card = *source_card;
        _per_worker_refine_data._cards_not_parsable++;
        break;
      }
      case HasRefToOld : break; // Nothing special to do.
    }
    // Clean card on source card table.
    *source_card = G1CardTable::clean_card_val();
  }

  void do_claimed_block(CardValue* dirty_l, CardValue* dirty_r, CardValue* dest_card) {
    for (CardValue* source = dirty_l; source < dirty_r; ++source, ++dest_card) {
      do_dirty_card(source, dest_card);
    }
  }

public:
  bool _completed;
  G1LocalRefineStats _per_worker_refine_data;

  G1RefineRegionClosure(G1CardTableClaimTable* scan_state,
                        uintx seen_heap_region_alloc_epoch) :
    G1HeapRegionClosure(),
    _scan_state(scan_state),
    _seen_heap_region_alloc_epoch(seen_heap_region_alloc_epoch),
    _completed(true),
    _per_worker_refine_data() { }

  bool do_heap_region(G1HeapRegion* r) override {

    if (!has_work(r)) {
      return false;
    }

    G1CollectedHeap* g1h = G1CollectedHeap::heap();

    if (r->is_young()) {
      if (_scan_state->claim_all_cards(r->hrm_index()) == 0) {
        // Clear the pre-dirtying information.
        r->clear_refinement_table();
      }
      return false;
    }

    G1CardTable* card_table = g1h->card_table();
    G1CardTable* refinement_table = g1h->refinement_table();

    G1CardTableChunkClaimer claim(_scan_state, r->hrm_index());

    size_t const region_card_base_idx = (size_t)r->hrm_index() << G1HeapRegion::LogCardsPerRegion;

    while (claim.has_next()) {
      size_t const start_idx = region_card_base_idx + claim.value();
      CardValue* const start_card = refinement_table->byte_for_index(start_idx);
      CardValue* const end_card = start_card + claim.size();

      CardValue* dest_card = card_table->byte_for_index(start_idx);

      G1ChunkScanner scanner{start_card, end_card};

      size_t num_dirty_cards = 0;
      scanner.on_dirty_cards([&] (CardValue* dirty_l, CardValue* dirty_r) {
                               jlong refine_start = os::elapsed_counter();

                               do_claimed_block(dirty_l, dirty_r, dest_card + pointer_delta(dirty_l, start_card, sizeof(CardValue)));
                               num_dirty_cards += pointer_delta(dirty_r, dirty_l, sizeof(CardValue));

                               _per_worker_refine_data._refine_duration += os::elapsed_counter() - refine_start;
                             });

      if (VerifyDuringGC) {
        for (CardValue* i = start_card; i < end_card; ++i) {
          guarantee(*i == G1CardTable::clean_card_val(), "must be");
        }
      }

      _per_worker_refine_data._cards_scanned += claim.size();
      _per_worker_refine_data._cards_clean += claim.size() - num_dirty_cards;

      if (SuspendibleThreadSet::should_yield()) {
        _completed = false;
        break;
      }
    }

    return !_completed;
  }
};

G1ConcurrentRefineSweepTask::G1ConcurrentRefineSweepTask(G1CardTableClaimTable* scan_state,
                                                         G1ConcurrentRefineStats* stats,
                                                         uint max_workers) :
  WorkerTask("G1 Refine Task"),
  _scan_state(scan_state),
  _stats(stats),
  _max_workers(max_workers),
  _sweep_completed(true),
  _seen_heap_region_alloc_epoch(G1CollectedHeap::heap()->cur_heap_region_alloc_epoch_acquire())
{ }

void G1ConcurrentRefineSweepTask::work(uint worker_id) {
  jlong start = os::elapsed_counter();

  G1RefineRegionClosure sweep_cl(_scan_state, _seen_heap_region_alloc_epoch);
  _scan_state->heap_region_iterate_from_worker_offset(&sweep_cl, worker_id, _max_workers);

  if (!sweep_cl._completed) {
    _sweep_completed.store_relaxed(false);
  }

  _stats->inc_sweep_duration(os::elapsed_counter() - start);
  _stats->add_atomic(&sweep_cl._per_worker_refine_data);
}

bool G1ConcurrentRefineSweepTask::sweep_completed() const { return _sweep_completed.load_relaxed(); }
