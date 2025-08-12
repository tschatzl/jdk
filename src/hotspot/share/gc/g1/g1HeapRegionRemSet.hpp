/*
 * Copyright (c) 2001, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1HEAPREGIONREMSET_HPP
#define SHARE_GC_G1_G1HEAPREGIONREMSET_HPP

#include "gc/g1/g1CardSet.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "gc/g1/g1CodeRootSet.hpp"
#include "gc/g1/g1CollectionSetCandidates.hpp"
#include "gc/g1/g1FromCardCache.hpp"
#include "runtime/atomic.hpp"
#include "runtime/mutexLocker.hpp"
#include "runtime/safepoint.hpp"
#include "utilities/bitMap.hpp"

class G1CardSetMemoryManager;
class G1CSetCandidateGroup;
class outputStream;

class G1HeapRegionRemSet : public CHeapObj<mtGC> {
  friend class VMStructs;

  // A set of nmethods whose code contains pointers into
  // the region that owns this RSet.
  G1CodeRootSet _code_roots;

  // The card based remembered set for this region.
  G1CardRemSet* _card_rem_set;

  G1HeapRegion* _hr;

  // Cached value of heap base address.
  static HeapWord* _heap_base_address;

  void clear_fcc();

  inline bool card_rem_set_is_empty() const;
  
public:
  G1HeapRegionRemSet(G1HeapRegion* hr);
  ~G1HeapRegionRemSet();

  inline bool has_card_rem_set() const;

  inline G1CardRemSet* card_rem_set();
  inline const G1CardRemSet* card_rem_set() const;

  inline bool has_single_region_card_rem_set() const;

  inline bool is_tracked() const; // FIXME: renames?
  inline bool is_updating() const;
  inline bool is_complete() const;

  inline void set_state_untracked();
  inline void set_state_updating();
  inline void set_state_complete(); // FIXME: renames until here

  void install_card_rem_set(G1CardRemSet* card_rem_set);
  void uninstall_card_rem_set();

  inline bool is_empty() const;
  inline bool occupancy_less_or_equal_than(size_t occ) const;

  // Iterate the card based remembered set for merging them into the card table.
  // The passed closure must be a CardOrRangeVisitor; we use a template parameter
  // to pass it in to facilitate inlining as much as possible.
  template <class CardOrRangeVisitor>
  inline void iterate_for_merge(CardOrRangeVisitor& cl);

  template <class CardOrRangeVisitor>
  inline static void iterate_for_merge(G1CardRemSet* card_rem_set, CardOrRangeVisitor& cl);

  inline size_t occupied();

  static void initialize(MemRegion reserved);

  // Coarsening statistics since VM start.
  static G1CardSetCoarsenStats coarsen_stats() { return G1CardSet::coarsen_stats(); }

  inline uintptr_t to_card(OopOrNarrowOopStar from) const;

  inline void add_reference(OopOrNarrowOopStar from, uint tid);

  // The region is being reclaimed; clear its remset, and any mention of
  // entries for this region in other remsets.
  void clear(bool only_cardset = false);

  void reset_table_scanner();

  G1MonotonicArenaMemoryStats card_set_memory_stats() const;

  // The actual # of bytes this hr_remset takes up. Also includes the code
  // root set.
  size_t mem_size() {
    return sizeof(G1HeapRegionRemSet) + code_roots_mem_size();
  }

  // Returns the memory occupancy of all static data structures associated
  // with remembered sets.
  static size_t static_mem_size() {
    return G1CardSet::static_mem_size();
  }

  static void print_static_mem_size(outputStream* out);

  inline bool contains_reference(OopOrNarrowOopStar from);

  //inline void print_info(outputStream* st, OopOrNarrowOopStar from);

  // Routines for managing the list of code roots that point into
  // the heap region that owns this RSet.
  void add_code_root(nmethod* nm);
  void remove_code_root(nmethod* nm);
  void bulk_remove_code_roots();

  // Applies blk->do_nmethod() to each of the entries in _code_roots
  void code_roots_do(NMethodClosure* blk) const;
  // Clean out code roots not having an oop pointing into this region any more.
  void clean_code_roots(G1HeapRegion* hr);

  // Returns the number of elements in _code_roots
  size_t code_roots_list_length() const {
    return _code_roots.length();
  }

  // Returns true if the code roots contains the given
  // nmethod.
  bool code_roots_list_contains(nmethod* nm) {
    return _code_roots.contains(nm);
  }

  // Returns the amount of memory, in bytes, currently
  // consumed by the code roots.
  size_t code_roots_mem_size();

  static void invalidate_from_card_cache(uint start_idx, size_t num_regions) {
    G1FromCardCache::invalidate(start_idx, num_regions);
  }

#ifndef PRODUCT
  static void print_from_card_cache() {
    G1FromCardCache::print();
  }

  static void test();
#endif
};

#endif // SHARE_GC_G1_G1HEAPREGIONREMSET_HPP
