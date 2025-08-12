/*
 * Copyright (c) 1997, 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_GC_G1_G1HEAPREGIONREMSET_INLINE_HPP
#define SHARE_VM_GC_G1_G1HEAPREGIONREMSET_INLINE_HPP

#include "gc/g1/g1HeapRegionRemSet.hpp"

#include "gc/g1/g1CardRemSet.inline.hpp"
#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "runtime/atomic.hpp"
#include "utilities/bitMap.inline.hpp"

inline bool G1HeapRegionRemSet::card_rem_set_is_empty() const {
  return !has_card_rem_set() || card_rem_set()->is_empty();
}

inline bool G1HeapRegionRemSet::has_single_region_card_rem_set() const {
  return has_card_rem_set() && (card_rem_set()->length() == 1);
}

inline bool G1HeapRegionRemSet::has_card_rem_set() const {
  return _card_rem_set != nullptr;
}

inline G1CardRemSet* G1HeapRegionRemSet::card_rem_set() {
  return _card_rem_set;
}

inline const G1CardRemSet* G1HeapRegionRemSet::card_rem_set() const {
  return _card_rem_set;
}

inline bool G1HeapRegionRemSet::is_empty() const {
  return (code_roots_list_length() == 0) && card_rem_set_is_empty();
}

inline bool G1HeapRegionRemSet::occupancy_less_or_equal_than(size_t occ) const {
  return (code_roots_list_length() == 0) && card_rem_set()->occupancy_less_or_equal_to(occ);
}

inline size_t G1HeapRegionRemSet::occupied() {
  assert(has_card_rem_set(), "pre-condition");
  return card_rem_set()->occupied();
}

bool G1HeapRegionRemSet::is_tracked() const {
  return has_card_rem_set() && (card_rem_set()->state() != G1CardRemSet::Untracked);
}

bool G1HeapRegionRemSet::is_updating() const {
  return has_card_rem_set() && (card_rem_set()->state() == G1CardRemSet::Updating);
}

bool G1HeapRegionRemSet::is_complete() const {
  return has_card_rem_set() && (card_rem_set()->state() == G1CardRemSet::Complete);
}

void G1HeapRegionRemSet::set_state_untracked() {
  guarantee(SafepointSynchronize::is_at_safepoint() || !is_tracked(),
            "Should only set to Untracked during safepoint but is %s.",
            G1CardRemSet::get_state_str(card_rem_set()));
  if (!is_tracked()) {
    return;
  }
  clear_fcc();
  card_rem_set()->set_state(G1CardRemSet::Untracked);
}

void G1HeapRegionRemSet::set_state_updating() {
  assert(SafepointSynchronize::is_at_safepoint(), "Should only set during safepoint");
  assert(has_card_rem_set(), "must have card rem set");
  assert(!is_tracked(), "must only set from Untracked to Updating");
  clear_fcc();
  card_rem_set()->set_state(G1CardRemSet::Updating);
}

void G1HeapRegionRemSet::set_state_complete() {
  assert(has_card_rem_set(), "must have card rem set");
  clear_fcc();
  card_rem_set()->set_state(G1CardRemSet::Complete);
}

template <class CardOrRangeVisitor>
inline void G1HeapRegionRemSet::iterate_for_merge(CardOrRangeVisitor& cl) {
  iterate_for_merge(card_rem_set(), cl);
}

template <class CardOrRangeVisitor>
void G1HeapRegionRemSet::iterate_for_merge(G1CardRemSet* card_rem_set, CardOrRangeVisitor& cl) {
  card_rem_set->iterate_for_merge(cl);
}

uintptr_t G1HeapRegionRemSet::to_card(OopOrNarrowOopStar from) const {
  return pointer_delta(from, _heap_base_address, 1) >> CardTable::card_shift();
}

void G1HeapRegionRemSet::add_reference(OopOrNarrowOopStar from, uint tid) {
  assert(is_tracked(), "must be");

  uint cur_idx = _hr->hrm_index();
  uintptr_t from_card = uintptr_t(from) >> CardTable::card_shift();

  if (G1FromCardCache::contains_or_replace(tid, cur_idx, from_card)) {
    // We can't check whether the card is in the remembered set - the card container
    // may be coarsened just now.
    //assert(contains_reference(from), "We just found " PTR_FORMAT " in the FromCardCache", p2i(from));
    return;
  }

#ifdef ASSERT
  {
    G1HeapRegion* from_region = G1CollectedHeap::heap()->heap_region_containing(from);
    assert(!from_region->rem_set()->has_card_rem_set() ||
           from_region->rem_set()->card_rem_set() != card_rem_set(),
           "Should not add reference within the same card remembered set");
  }
#endif

  card_rem_set()->add_card(to_card(from));
}

bool G1HeapRegionRemSet::contains_reference(OopOrNarrowOopStar from) {
  return card_rem_set()->contains_card(to_card(from));
}

/*
void G1HeapRegionRemSet::print_info(outputStream* st, OopOrNarrowOopStar from) {
  card_rem_set()->print_info(st, to_card(from));
}
 */

#endif // SHARE_VM_GC_G1_G1HEAPREGIONREMSET_INLINE_HPP
