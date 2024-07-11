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

#ifndef SHARE_GC_G1_G1CARDTABLECLAIMTABLE_HPP
#define SHARE_GC_G1_G1CARDTABLECLAIMTABLE_HPP

#include "gc/g1/g1CardTable.hpp"
#include "memory/allocation.hpp"

class G1HeapRegionClosure;

class G1CardTableClaimTable : public CHeapObj<mtGC> {
  size_t _max_reserved_regions;

  // Card table iteration claim for each heap region, from 0 (completely unscanned)
  // to (>=) G1HeapRegion::CardsPerRegion (completely scanned).
  uint volatile* _card_table_scan_state;

  uint8_t _log_scan_chunks_per_region;  // Log of number of chunks per region.
  uint8_t _scan_chunks_shift;           // For conversion between card index and chunk index.

public:
  G1CardTableClaimTable(uint chunks_per_region);
  virtual ~G1CardTableClaimTable();

  virtual void initialize(size_t max_reserved_regions);
  void reset_card_table_unclaimed();
  void reset_card_table_claimed();

  inline bool has_cards_to_scan(uint region);
  inline uint claim_cards(uint region, uint increment);
  inline void reset_to_unclaimed(uint region);
  inline uint claim_all_cards(uint region);

  inline uint scan_chunk_size_in_cards() const;

  size_t max_reserved_regions() { return _max_reserved_regions; }

  void heap_region_iterate_from_worker_offset(G1HeapRegionClosure* cl, uint worker_id, uint max_workers);
};

// Helper class to claim dirty chunks within the card table.
class G1CardTableChunkClaimer {
  G1CardTableClaimTable* _scan_state;
  uint _region_idx;
  uint _cur_claim;

public:
  G1CardTableChunkClaimer(G1CardTableClaimTable* scan_state, uint region_idx);

  inline bool has_next();

  inline uint value() const;
  inline uint size() const;
};

// To locate consecutive dirty cards inside a chunk.
class G1ChunkScanner {
  using Word = size_t;
  using CardValue = G1CardTable::CardValue;

  CardValue* const _start_card;
  CardValue* const _end_card;

  static const size_t ExpandedToScanMask = G1CardTable::WordAlreadyScanned;
  static const size_t ToScanMask = G1CardTable::g1_card_already_scanned;

  inline bool is_card_dirty(const CardValue* const card) const;

  inline bool is_word_aligned(const void* const addr) const;

  inline CardValue* find_first_dirty_card(CardValue* i_card) const;
  inline CardValue* find_first_non_dirty_card(CardValue* i_card) const;

public:
  G1ChunkScanner(CardValue* const start_card, CardValue* const end_card);

  template<typename Func>
  void on_dirty_cards(Func&& f) {
    for (CardValue* cur_card = _start_card; cur_card < _end_card; /* empty */) {
      CardValue* dirty_l = find_first_dirty_card(cur_card);
      CardValue* dirty_r = find_first_non_dirty_card(dirty_l);

      assert(dirty_l <= dirty_r, "inv");

      if (dirty_l == dirty_r) {
        assert(dirty_r == _end_card, "finished the entire chunk");
        return;
      }

      f(dirty_l, dirty_r);

      cur_card = dirty_r + 1;
    }
  }
};

#endif // SHARE_GC_G1_G1CARDTABLECLAIMTABLE_HPP
