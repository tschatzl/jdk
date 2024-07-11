/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CARDTABLECLAIMTABLE_INLINE_HPP
#define SHARE_GC_G1_G1CARDTABLECLAIMTABLE_INLINE_HPP

#include "gc/g1/g1CardTableClaimTable.hpp"

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "runtime/atomic.hpp"

bool G1CardTableClaimTable::has_cards_to_scan(uint region) {
  assert(region < _max_reserved_regions, "Tried to access invalid region %u", region);
  return Atomic::load(&_card_table_scan_state[region]) < G1HeapRegion::CardsPerRegion;
}

void G1CardTableClaimTable::reset_to_unclaimed(uint region) {
  assert(region < _max_reserved_regions, "Tried to access invalid region %u", region);
  Atomic::store(&_card_table_scan_state[region], 0u);
}

uint G1CardTableClaimTable::claim_cards(uint region, uint increment) {
  assert(region < _max_reserved_regions, "Tried to access invalid region %u", region);
  return Atomic::fetch_then_add(&_card_table_scan_state[region], increment, memory_order_relaxed);
}

uint G1CardTableClaimTable::claim_all_cards(uint region) {
  return claim_cards(region, (uint)G1HeapRegion::CardsPerRegion);
}

uint G1CardTableClaimTable::scan_chunk_size_in_cards() const { return (uint)1 << _scan_chunks_shift; }

bool G1CardTableChunkClaimer::has_next() {
  // FIXME: maybe not do the indexing every time here, but store a reference to
  // the claim value directly.
  _cur_claim = _scan_state->claim_cards(_region_idx, size());
  return (_cur_claim < G1HeapRegion::CardsPerRegion);
}

uint G1CardTableChunkClaimer::value() const { return _cur_claim; }
uint G1CardTableChunkClaimer::size() const { return _scan_state->scan_chunk_size_in_cards(); }

bool G1ChunkScanner::is_card_dirty(const CardValue* const card) const {
  return (*card & ToScanMask) == 0;
}

bool G1ChunkScanner::is_word_aligned(const void* const addr) const {
  return ((uintptr_t)addr) % sizeof(Word) == 0;
}

G1CardTable::CardValue* G1ChunkScanner::find_first_dirty_card(CardValue* i_card) const {
  while (!is_word_aligned(i_card)) {
    if (is_card_dirty(i_card)) {
      return i_card;
    }
    i_card++;
  }

  for (/* empty */; i_card < _end_card; i_card += sizeof(Word)) {
    Word word_value = *reinterpret_cast<Word*>(i_card);
    bool has_dirty_cards_in_word = (~word_value & ExpandedToScanMask) != 0;

    if (has_dirty_cards_in_word) {
      for (uint i = 0; i < sizeof(Word); ++i) {
        if (is_card_dirty(i_card)) {
          return i_card;
        }
        i_card++;
      }
      assert(false, "should have early-returned");
    }
  }

  return _end_card;
}

G1CardTable::CardValue* G1ChunkScanner::find_first_non_dirty_card(CardValue* i_card) const {
  while (!is_word_aligned(i_card)) {
    if (!is_card_dirty(i_card)) {
      return i_card;
    }
    i_card++;
  }

  for (/* empty */; i_card < _end_card; i_card += sizeof(Word)) {
    Word word_value = *reinterpret_cast<Word*>(i_card);
    bool all_cards_dirty = (word_value & ExpandedToScanMask) == 0;

    if (!all_cards_dirty) {
      for (uint i = 0; i < sizeof(Word); ++i) {
        if (!is_card_dirty(i_card)) {
          return i_card;
        }
        i_card++;
      }
      assert(false, "should have early-returned");
    }
  }

  return _end_card;
}

#endif // SHARE_GC_G1_G1CARDTABLECLAIMTABLE_INLINE_HPP
