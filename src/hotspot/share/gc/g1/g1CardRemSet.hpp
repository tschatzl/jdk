/*
 * Copyright (c) 2025, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1CARDREMSET_HPP
#define SHARE_GC_G1_G1CARDREMSET_HPP

#include "gc/g1/g1CardSet.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "memory/allocation.hpp"
#include "utilities/growableArray.hpp"

class G1CardSetConfiguration;
class G1CSetCandidateGroup;
class G1HeapRegion;
class G1MonotonicArenaFreePool;

class G1CardRemSet : public CHeapObj<mtGC> {
  G1CSetCandidateGroup* _candidate;

  G1CardSetMemoryManager _card_set_mm;
  G1CardSet _card_set;

public:
  enum State : uint {
    Untracked, // Initializing
    Updating,
    Complete
  };

private:
  State _state;

  static const char* _state_strings[];
  static const char* _short_state_strings[];

public:
  G1CardRemSet(G1CSetCandidateGroup* candidate, G1CardSetConfiguration* config, G1MonotonicArenaFreePool* card_set_freelist_pool, bool is_complete);

  static const char* get_state_str(G1CardRemSet* rem_set);
  static const char* get_short_state_str(G1CardRemSet* rem_set);
  static uint id(G1CardRemSet* rem_set);

  // Card set state tracking.
  void set_state(State state) { _state = state; }
  State state() const { return _state; }

  // Empty out the cards in the card rem set.
  inline void clear_rem_set_contents();
  inline void clear();

  template <class CardOrRangeVisitor>
  void iterate_for_merge(CardOrRangeVisitor& cl);

  inline void reset_table_scanner_for_groups();
  // Number of regions this card remembered set covers.
  inline uint length() const;

  inline void add_card(uintptr_t card);
  inline bool contains_card(uintptr_t card);

  inline bool is_empty() const;
  inline bool occupancy_less_or_equal_to(size_t occ) const;
  inline size_t occupied() const;

  inline size_t mem_size() const;
  inline size_t unused_mem_size() const;
  inline G1MonotonicArenaMemoryStats memory_stats() const;
};

#endif /* SHARE_GC_G1_G1CARDREMSET_HPP */

