/*
 * Copyright (c) 2019, 2024, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP
#define SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP

#include "gc/g1/g1CardRemSet.hpp"
#include "gc/g1/g1CardSetMemory.hpp"
#include "gc/g1/g1CollectionSetCandidates.hpp"
#include "gc/shared/gc_globals.hpp"
#include "memory/allocation.hpp"
#include "runtime/globals.hpp"
#include "utilities/growableArray.hpp"

class G1CSetCandidateGroup;
class G1CollectionSetCandidates;
class G1CSetCandidateGroupList;
class G1HeapRegion;
class G1HeapRegionClosure;

// Information about a region in the collection set candidates.
class G1CollectionSetRegionInfo {
  G1HeapRegion* _r;
  // The number of attempts this region has not been put in the collection set because it has been pinned.
  uint _num_unreclaimed;

public:
  G1CollectionSetRegionInfo();
  G1CollectionSetRegionInfo(G1HeapRegion* r);

  inline bool update_num_unreclaimed();

  inline G1HeapRegion* r() const;

  static int compare_region_gc_efficiency(G1HeapRegion** rr1, G1HeapRegion** rr2);
};

using G1CollectionSetCandidateIterator = GrowableArrayIterator<G1CollectionSetRegionInfo>;

// G1CollectionSetCandidate groups candidate regions that will be selected for evacuation at the same time.
// Grouping occurs both for candidates from marking or regions retained during evacuation failure, but a group
// can not contain regions from more than one type of regions.
//
// All regions in the group share a G1CardRemSet instance, which tracks remembered set entries for the
// regions in the group. We do not have track to cross-region references for regions that are in the
// same group saving memory.
class G1CSetCandidateGroup : public CHeapObj<mtGCCardSet> {
  friend class G1CollectionSetCandidates;

  GrowableArray<G1CollectionSetRegionInfo> _candidate_infos;

  G1CardRemSet _card_rem_set;

  size_t _reclaimable_bytes;
  double _gc_efficiency;

  // The id is primarily used when printing out per-region liveness information,
  // making it easier to associate regions with their assigned G1CollectionSetCandidate, if any.
  const uint _id;

  static uint _next_id;

  G1CSetCandidateGroup(bool rem_set_is_complete);

public:
  G1CSetCandidateGroup(G1CardSetConfiguration* config, G1MonotonicArenaFreePool* card_set_freelist_pool, bool rem_set_is_complete, uint id);
  ~G1CSetCandidateGroup();

  // Special values for the id:
  // * id 0 is reserved for regions that do not have a remembered set.
  // * id 1 is reserved for the G1CollectionSetCandidate that contains all young regions.
  // * other ids are handed out incrementally, starting from InitialId.
  static const uint NoRemSetId = 0;
  static const uint YoungRegionId = 1;
  static const uint InitialId = 2;

  // Comparison function to order regions in decreasing GC efficiency order. This
  // will cause regions with a lot of live objects and large remembered sets to end
  // up at the end of the list.
  static int compare_gc_efficiency(G1CSetCandidateGroup** gr1, G1CSetCandidateGroup** gr2);

  void add(G1HeapRegion* hr);

  // Number of regions in this candidate.
  inline uint length() const;

  inline G1CardRemSet* card_rem_set();
  inline const G1CardRemSet* card_rem_set() const;

  G1CollectionSetRegionInfo first() const;

  void calculate_efficiency();

  double gc_efficiency() const { return _gc_efficiency; }
  double predict_group_total_time_ms() const;
  double liveness_percent() const;

  G1MonotonicArenaMemoryStats card_set_memory_stats() const;

  inline size_t cards_occupied() const;

  void clear(bool uninstall_group_cardset);

  G1CollectionSetCandidateIterator begin() const {
    return _candidate_infos.begin();
  }

  G1CollectionSetCandidateIterator end() const {
    return _candidate_infos.end();
  }

  inline uint id() const;

  static uint next_id();
  static void reset_next_id();
};

using G1CSetCandidateGroupListIterator = GrowableArrayIterator<G1CSetCandidateGroup*>;

class G1CSetCandidateGroupList {
  GrowableArray<G1CSetCandidateGroup*> _groups;
  volatile uint _num_regions;

public:
  G1CSetCandidateGroupList();
  void append(G1CSetCandidateGroup* candidate);

  // Delete all groups from the list. The cardset cleanup for regions within the
  // groups could have been done elsewhere (e.g. when adding groups to the
  // collection set or to retained regions). The uninstall_group_cardset is set to
  // true if cleanup needs to happen as we clear the groups from the list.
  void clear(bool uninstall_group_cardset = false); /* FIXME: This param should always be true */

  G1CSetCandidateGroup* at(uint index);

  uint length() const { return (uint)_groups.length(); }

  uint num_regions() const { return _num_regions; }

  void remove_selected(uint count, uint num_regions);

  // Removes any candidate groups stored in this list and also in the other list. The other
  // list may only contain candidate groups in this list, sorted by gc efficiency. It need
  // not be a prefix of this list.
  // E.g. if this list is "A B G H", the other list may be "A G H", but not "F" (not in
  // this list) or "A H G" (wrong order).
  void remove(G1CSetCandidateGroupList* other);

  void prepare_for_scan();

  void sort_by_efficiency();

  void verify() const PRODUCT_RETURN;

  G1CSetCandidateGroupListIterator begin() const {
    return _groups.begin();
  }

  G1CSetCandidateGroupListIterator end() const {
    return _groups.end();
  }

  template<typename Func>
  void iterate(Func&& f) const;
};

// Tracks all collection set candidates, i.e. region groups that could/should be evacuated soon.
//
// These candidate groups are tracked in two list of region groups, sorted by decreasing
// "gc efficiency".
//
// * from_marking_groups: the set of region groups selected by concurrent marking to be
//                        evacuated to keep overall heap occupancy stable.
//                        They are guaranteed to be evacuated and cleared out during
//                        the mixed phase.
//
// * retained_groups: set of region groups selected for evacuation during evacuation
//                    failure.
//                    Any young collection will try to evacuate them.
//
class G1CollectionSetCandidates : public CHeapObj<mtGC> {

  enum class CandidateOrigin : uint8_t {
    Invalid,
    Marking,                   // This region has been determined as candidate by concurrent marking.
    Retained,                  // This region has been added because it has been retained after evacuation.
    Humongous,                 // This region is a humongous candidate.
    Verify                     // Special value for verification.
  };

  CandidateOrigin* _contains_map;
  uint _max_regions;

  G1CSetCandidateGroupList _from_marking_groups; // Set of regions selected by concurrent marking.
  // Set of regions retained due to evacuation failure. Groups added to this list
  // should contain only one region each, making it easier to evacuate retained regions
  // in any young collection.
  G1CSetCandidateGroupList _retained_groups;

  // Candidates containing humongous regions.
  G1CSetCandidateGroupList _humongous_groups;

  // The number of regions from the last merge of candidates from the marking.
  uint _last_marking_candidates_length;

  bool is_from_marking(G1HeapRegion* r) const;

  uint marking_regions_length() const;
  uint retained_regions_length() const;
  uint humongous_regions_length() const;

  uint length() const {
    return marking_regions_length() + retained_regions_length() + humongous_regions_length();
  }

public:
  G1CollectionSetCandidates();
  ~G1CollectionSetCandidates();

  void initialize(uint max_regions);
  void clear();

  G1CSetCandidateGroupList& from_marking_groups() { return _from_marking_groups; }
  G1CSetCandidateGroupList& retained_groups() { return _retained_groups; }
  G1CSetCandidateGroupList& humongous_groups() { return _humongous_groups; }

  // Merge collection set candidates from marking into the current marking candidates
  // (which needs to be empty).
  void add_old_candidates_from_marking(G1HeapRegion** candidates,
                                       uint num_candidates);
  // The most recent length of the list that had been merged last via
  // add_old_candidates_from_marking(). Used for calculating minimum collection set
  // regions.
  uint last_marking_candidates_length() const { return _last_marking_candidates_length; }

  void sort_by_efficiency();

  void sort_marking_by_efficiency();

  // Add the given region to the set of retained regions without regards to the
  // gc efficiency sorting. The retained regions must be re-sorted manually later.
  void add_retained_region_unsorted(G1HeapRegion* r);

  // Adds the given humongous regions (each represented by the starts-humongous
  // region) to the candidate list.
  void add_humongous_candidates(G1HeapRegion** regions,
                                uint num_candidates,
                                bool rem_sets_are_complete);
  // Remove the given groups from the candidates. All given regions must be part
  // of the candidates.
  void remove(G1CSetCandidateGroupList* other);

  bool contains(const G1HeapRegion* r) const;
  bool is_humongous(const G1HeapRegion* r) const;

  const char* get_short_type_str(const G1HeapRegion* r) const;

  bool is_empty() const;

  bool has_more_marking_candidates() const;

private:
  void verify_helper(G1CSetCandidateGroupList* list, uint& from_marking, CandidateOrigin* verify_map) PRODUCT_RETURN;

public:
  void verify() PRODUCT_RETURN;

  template<typename Func>
  void iterate_regions(Func&& f) const;
};

#endif /* SHARE_GC_G1_G1COLLECTIONSETCANDIDATES_HPP */
