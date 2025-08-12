/*
 * Copyright (c) 2019, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "gc/g1/g1CardRemSet.inline.hpp"
#include "gc/g1/g1CollectionSetCandidates.inline.hpp"
#include "gc/g1/g1CollectionSetChooser.hpp"
#include "gc/g1/g1HeapRegion.inline.hpp"
#include "utilities/growableArray.hpp"

G1CollectionSetRegionInfo::G1CollectionSetRegionInfo() :
  G1CollectionSetRegionInfo(nullptr) { }

G1CollectionSetRegionInfo::G1CollectionSetRegionInfo(G1HeapRegion* r) :
  _r(r), _num_unreclaimed(0) { }

G1CSetCandidateGroup::G1CSetCandidateGroup(G1CardSetConfiguration* config,
                                           G1MonotonicArenaFreePool* card_set_freelist_pool,
                                           bool rem_set_is_complete,
                                           uint id) :
  _candidate_infos(4, mtGCCardSet),
  _card_rem_set(this, config, card_set_freelist_pool, rem_set_is_complete),
  _reclaimable_bytes(size_t(0)),
  _gc_efficiency(0.0),
  _id(id)
{ }

int G1CollectionSetRegionInfo::compare_region_gc_efficiency(G1HeapRegion** rr1, G1HeapRegion** rr2) {
  G1HeapRegion* r1 = *rr1;
  G1HeapRegion* r2 = *rr2;

  // Make sure that null entries are moved to the end.
  if (r1 == nullptr) {
    if (r2 == nullptr) {
      return 0;
    } else {
      return 1;
    }
  } else if (r2 == nullptr) {
    return -1;
  }

  G1Policy* p = G1CollectedHeap::heap()->policy();
  double gc_efficiency1 = p->predict_gc_efficiency(r1);
  double gc_efficiency2 = p->predict_gc_efficiency(r2);

  if (gc_efficiency1 > gc_efficiency2) {
    return -1;
  } else if (gc_efficiency1 < gc_efficiency2) {
    return 1;
  } else {
    return 0;
  }
}

G1CSetCandidateGroup::G1CSetCandidateGroup(bool rem_set_is_complete) :
  G1CSetCandidateGroup(G1CollectedHeap::heap()->card_set_config(),
                       G1CollectedHeap::heap()->card_set_freelist_pool(),
                       rem_set_is_complete,
                       next_id())
{ }

G1CSetCandidateGroup::~G1CSetCandidateGroup() {
  assert(length() == 0, "post condition!");
}

void G1CSetCandidateGroup::add(G1HeapRegion* hr) {
  G1CollectionSetRegionInfo c(hr);
  _candidate_infos.append(c);

  hr->install_card_rem_set(&_card_rem_set);
}

void G1CSetCandidateGroup::calculate_efficiency() {
  _reclaimable_bytes = 0;
  for (G1CollectionSetRegionInfo ci : _candidate_infos) {
    _reclaimable_bytes += ci.r()->reclaimable_bytes();
  }
  _gc_efficiency = _reclaimable_bytes / predict_group_total_time_ms();
}

double G1CSetCandidateGroup::liveness_percent() const {
  size_t capacity = length() * G1HeapRegion::GrainBytes;
  return ((capacity - _reclaimable_bytes) * 100.0) / capacity;
}

void G1CSetCandidateGroup::clear(bool uninstall_group_cardset) {
  if (uninstall_group_cardset) {
    for (G1CollectionSetRegionInfo ci : _candidate_infos) {
      G1HeapRegion* r = ci.r();
      r->uninstall_card_rem_set();
      r->rem_set()->clear(true /* only_cardset */);
    }
  }
  _card_rem_set.clear();
  _candidate_infos.clear();
}

double G1CSetCandidateGroup::predict_group_total_time_ms() const {
  G1Policy* p = G1CollectedHeap::heap()->policy();

  double predicted_copy_time_ms = 0.0;
  double predict_code_root_scan_time_ms = 0.0;
  size_t predict_bytes_to_copy = 0.0;

  for (G1CollectionSetRegionInfo ci : _candidate_infos) {
    G1HeapRegion* r = ci.r();
    assert(r->rem_set()->card_rem_set() == card_rem_set(), "Must be!");

    predict_bytes_to_copy += p->predict_bytes_to_copy(r);
    predicted_copy_time_ms += p->predict_region_copy_time_ms(r, false /* for_young_only_phase */);
    predict_code_root_scan_time_ms += p->predict_region_code_root_scan_time(r, false /* for_young_only_phase */);
  }

  size_t card_rs_length = _card_rem_set.occupied();

  double merge_scan_time_ms = p->predict_merge_scan_time(card_rs_length);
  double non_young_other_time_ms = p->predict_non_young_other_time_ms(length());

  double total_time_ms = merge_scan_time_ms +
                         predict_code_root_scan_time_ms +
                         predicted_copy_time_ms +
                         non_young_other_time_ms;

  log_trace(gc, ergo, cset) ("Prediction for group %u (%u regions): total_time %.2fms card_rs_length %zu merge_scan_time %.2fms code_root_scan_time_ms %.2fms evac_time_ms %.2fms other_time %.2fms bytes_to_copy %zu",
                             id(),
                             length(),
                             total_time_ms,
                             card_rs_length,
                             merge_scan_time_ms,
                             predict_code_root_scan_time_ms,
                             predicted_copy_time_ms,
                             non_young_other_time_ms,
                             predict_bytes_to_copy);

  return total_time_ms;
}

int G1CSetCandidateGroup::compare_gc_efficiency(G1CSetCandidateGroup** gr1, G1CSetCandidateGroup** gr2) {
  double gc_eff1 = (*gr1)->gc_efficiency();
  double gc_eff2 = (*gr2)->gc_efficiency();

  if (gc_eff1 > gc_eff2) {
    return -1;
  } else if (gc_eff1 < gc_eff2) {
    return 1;
  } else {
    return 0;
  }
}

uint G1CSetCandidateGroup::_next_id = G1CSetCandidateGroup::InitialId;

uint G1CSetCandidateGroup::next_id() {
  return _next_id++;
}

void G1CSetCandidateGroup::reset_next_id() {
  _next_id = G1CSetCandidateGroup::InitialId;
}

G1CSetCandidateGroupList::G1CSetCandidateGroupList() : _groups(8, mtGC), _num_regions(0) { }

void G1CSetCandidateGroupList::append(G1CSetCandidateGroup* candidate) {
  assert(candidate->length() > 0, "Do not add empty groups");
  assert(!_groups.contains(candidate), "Already added to list");
  _groups.append(candidate);
  _num_regions += candidate->length();
}

G1CSetCandidateGroup* G1CSetCandidateGroupList::at(uint index) {
  return _groups.at(index);
}

void G1CSetCandidateGroupList::clear(bool uninstall_group_cardset) {
  for (G1CSetCandidateGroup* gr : _groups) {
    gr->clear(uninstall_group_cardset);
    delete gr;
  }
  _groups.clear();
  _num_regions = 0;
}

void G1CSetCandidateGroupList::prepare_for_scan() {
  for (G1CSetCandidateGroup* gr : _groups) {
    gr->card_rem_set()->reset_table_scanner_for_groups();
  }
}

void G1CSetCandidateGroupList::remove_selected(uint count, uint num_regions) {
  _groups.remove_till(count);
  _num_regions -= num_regions;
}

void G1CSetCandidateGroupList::remove(G1CSetCandidateGroupList* other) {
  guarantee((uint)_groups.length() >= other->length(), "Other should be a subset of this list");

  if (other->length() == 0) {
    // Nothing to remove or nothing in the original set.
    return;
  }

  // Create a list from scratch, copying over the elements from the candidate
  // list not in the other list. Finally deallocate and overwrite the old list.
  int new_length = _groups.length() - other->length();
  _num_regions = num_regions() - other->num_regions();
  GrowableArray<G1CSetCandidateGroup*> new_list(new_length, mtGC);

  uint other_idx = 0;
  for (G1CSetCandidateGroup* gr : _groups) {
    if (other_idx == other->length() || gr != other->at(other_idx)) {
      new_list.append(gr);
    } else {
      other_idx++;
    }
  }
  _groups.swap(&new_list);

  verify();
  assert(_groups.length() == new_length, "Must be");
}

void G1CSetCandidateGroupList::sort_by_efficiency() {
  _groups.sort(G1CSetCandidateGroup::compare_gc_efficiency);
}

#ifndef PRODUCT
void G1CSetCandidateGroupList::verify() const {
  G1CSetCandidateGroup* prev = nullptr;

  for (G1CSetCandidateGroup* gr : _groups) {
    assert(prev == nullptr || prev->gc_efficiency() >= gr->gc_efficiency(),
           "Stored gc efficiency must be descending");
    prev = gr;
  }
}
#endif

G1CollectionSetCandidates::G1CollectionSetCandidates() :
  _contains_map(nullptr),
  _max_regions(0),
  _from_marking_groups(),
  _retained_groups(),
  _humongous_groups(),
  _last_marking_candidates_length(0)
{ }

G1CollectionSetCandidates::~G1CollectionSetCandidates() {
  FREE_C_HEAP_ARRAY(CandidateOrigin, _contains_map);
  clear();
}

bool G1CollectionSetCandidates::is_from_marking(G1HeapRegion* r) const {
  assert(contains(r), "must be");
  return _contains_map[r->hrm_index()] == CandidateOrigin::Marking;
}

void G1CollectionSetCandidates::initialize(uint max_regions) {
  assert(_contains_map == nullptr, "already initialized");
  _max_regions = max_regions;
  _contains_map = NEW_C_HEAP_ARRAY(CandidateOrigin, max_regions, mtGC);
  clear();
}

void G1CollectionSetCandidates::clear() {
  _retained_groups.clear(true /* uninstall_group_cardset */);
  _from_marking_groups.clear(true /* uninstall_group_cardset */);
  _humongous_groups.clear(true /* uninstall_group_cardset */);
  for (uint i = 0; i < _max_regions; i++) {
    _contains_map[i] = CandidateOrigin::Invalid;
  }
  _last_marking_candidates_length = 0;
}

void G1CollectionSetCandidates::sort_marking_by_efficiency() {
  for (G1CSetCandidateGroup* gr : _from_marking_groups) {
    gr->calculate_efficiency();
  }
  _from_marking_groups.sort_by_efficiency();

  _from_marking_groups.verify();
}

void G1CollectionSetCandidates::add_old_candidates_from_marking(G1HeapRegion** candidates,
                                                                uint num_candidates) {
  if (num_candidates == 0) {
    log_debug(gc, ergo, cset) ("No regions selected from marking.");
    return;
  }

  assert(_from_marking_groups.length() == 0, "must be empty at the start of a cycle");
  verify();

  G1Policy* p = G1CollectedHeap::heap()->policy();
  // During each Mixed GC, we must collect at least G1Policy::calc_min_old_cset_length regions to meet
  // the G1MixedGCCountTarget. For the first collection in a Mixed GC cycle, we can add all regions
  // required to meet this threshold to the same remset group. We are certain these will be collected in
  // the same MixedGC.
  uint group_limit = p->calc_min_old_cset_length(num_candidates);

  uint num_added_to_group = 0;

  G1CSetCandidateGroup::reset_next_id();
  G1CSetCandidateGroup* current = nullptr;

  current = new G1CSetCandidateGroup(false /* rem_set_is_complete */);

  for (uint i = 0; i < num_candidates; i++) {
    G1HeapRegion* r = candidates[i];
    assert(!contains(r), "must not contain region %u", r->hrm_index());
    _contains_map[r->hrm_index()] = CandidateOrigin::Marking;

    if (num_added_to_group == group_limit) {
      if (group_limit != G1OldCSetGroupSize) {
        group_limit = G1OldCSetGroupSize;
      }

      _from_marking_groups.append(current);

      current = new G1CSetCandidateGroup(false /* rem_set_is_complete */);
      num_added_to_group = 0;
    }
    current->add(candidates[i]);
    num_added_to_group++;
  }

  _from_marking_groups.append(current);

  assert(_from_marking_groups.num_regions() == num_candidates, "Must be!");

  log_debug(gc, ergo, cset) ("Finished creating %u collection groups from %u regions", _from_marking_groups.length(), num_candidates);
  _last_marking_candidates_length = num_candidates;

  verify();
}

void G1CollectionSetCandidates::sort_by_efficiency() {
  // From marking regions must always be sorted so no reason to actually sort
  // them.
  _from_marking_groups.verify();
  _retained_groups.sort_by_efficiency();
  _retained_groups.verify();
  // No need to sort humongous groups.
  _humongous_groups.verify();
}

void G1CollectionSetCandidates::remove(G1CSetCandidateGroupList* other) {
  // During removal, we exploit the fact that elements in the different lists are
  // sorted, and the "other" lists retains the order. Furthermore, all regions
  // in the passed other list are in one the lists.
  //
  // Split other list into elements to remove in the different lists.
  G1CSetCandidateGroupList other_marking_groups;
  G1CSetCandidateGroupList other_retained_groups;
  G1CSetCandidateGroupList other_humongous_groups;

  for (G1CSetCandidateGroup* group : *other) {
    assert(group->length() > 0, "Should not have empty groups");
    // Regions in the same group have the same source (i.e from_marking or retained).
    G1HeapRegion* r = group->first().r();

    switch (_contains_map[r->hrm_index()]) {
      case CandidateOrigin::Marking:
        other_marking_groups.append(group);
        break;
      case CandidateOrigin::Retained:
        other_retained_groups.append(group);
        break;
      case CandidateOrigin::Humongous:
        other_humongous_groups.append(group);
        break;
      default: ShouldNotReachHere();
    }
  }

  _from_marking_groups.remove(&other_marking_groups);
  _retained_groups.remove(&other_retained_groups);
  _humongous_groups.remove(&other_humongous_groups);

  other->iterate([&] (G1HeapRegion* r) {
    assert(contains(r), "Must contain region %u", r->hrm_index());
    _contains_map[r->hrm_index()] = CandidateOrigin::Invalid;
  });

  verify();
}

void G1CollectionSetCandidates::add_retained_region_unsorted(G1HeapRegion* r) {
  assert(!contains(r), "Must not already contain region %u", r->hrm_index());
  _contains_map[r->hrm_index()] = CandidateOrigin::Retained;

  G1CSetCandidateGroup* gr = new G1CSetCandidateGroup(true /* rem_set_is_complete */);
  gr->add(r);

  _retained_groups.append(gr);
}

void G1CollectionSetCandidates::add_humongous_candidates(G1HeapRegion** candidates,
                                                         uint num_candidates,
                                                         bool rem_sets_are_complete) {
  assert(num_candidates > 0, "must be");

  for (uint i = 0; i < num_candidates; i++) {
    G1HeapRegion* hr = *candidates++;
    assert(hr->is_starts_humongous(), "must be");

    G1CSetCandidateGroup* gr = new G1CSetCandidateGroup(rem_sets_are_complete);

    auto on_humongous_add = [&] (G1HeapRegion* r) {
      assert(!contains(r), "Must not already contain region %u", r->hrm_index());
      assert(r->is_humongous(), "must be");

      _contains_map[r->hrm_index()] = CandidateOrigin::Humongous;
      gr->add(r);
    };
    
    G1CollectedHeap::heap()->humongous_obj_regions_iterate(hr, on_humongous_add);

    _humongous_groups.append(gr);
  }
}

bool G1CollectionSetCandidates::is_empty() const {
  return length() == 0;
}

bool G1CollectionSetCandidates::has_more_marking_candidates() const {
  return marking_regions_length() != 0;
}

uint G1CollectionSetCandidates::marking_regions_length() const {
  return _from_marking_groups.num_regions();
}

uint G1CollectionSetCandidates::retained_regions_length() const {
  return _retained_groups.num_regions();
}

uint G1CollectionSetCandidates::humongous_regions_length() const {
  return _humongous_groups.num_regions();
}

#ifndef PRODUCT
void G1CollectionSetCandidates::verify_helper(G1CSetCandidateGroupList* list, uint& from_marking, CandidateOrigin* verify_map) {
  list->verify();

  for (G1CSetCandidateGroup* gr : *list) {
    for (G1CollectionSetRegionInfo ci : *gr) {
      G1HeapRegion* r = ci.r();

      if (is_from_marking(r)) {
        from_marking++;
      }
      const uint hrm_index = r->hrm_index();
      assert(_contains_map[hrm_index] == CandidateOrigin::Marking || _contains_map[hrm_index] == CandidateOrigin::Retained,
             "must be %u is %u", hrm_index, (uint)_contains_map[hrm_index]);
      assert(verify_map[hrm_index] == CandidateOrigin::Invalid, "already added");

      verify_map[hrm_index] = CandidateOrigin::Verify;
    }
  }
}

void G1CollectionSetCandidates::verify() {
  uint from_marking = 0;

  CandidateOrigin* verify_map = NEW_C_HEAP_ARRAY(CandidateOrigin, _max_regions, mtGC);
  for (uint i = 0; i < _max_regions; i++) {
    verify_map[i] = CandidateOrigin::Invalid;
  }

  verify_helper(&_from_marking_groups, from_marking, verify_map);
  assert(from_marking == marking_regions_length(), "must be");

  uint from_marking_retained = 0;
  verify_helper(&_retained_groups, from_marking_retained, verify_map);
  assert(from_marking_retained == 0, "must be");

  uint temp = 0;
  verify_helper(&_humongous_groups, temp, verify_map);
  assert(temp == 0, "must be");

  assert(length() >= marking_regions_length(), "must be");

  // Check whether the _contains_map is consistent with the list.
  for (uint i = 0; i < _max_regions; i++) {
    assert(_contains_map[i] == verify_map[i] ||
           (_contains_map[i] != CandidateOrigin::Invalid && verify_map[i] == CandidateOrigin::Verify),
           "Candidate origin does not match for region %u, is %u but should be %u",
           i,
           static_cast<std::underlying_type<CandidateOrigin>::type>(_contains_map[i]),
           static_cast<std::underlying_type<CandidateOrigin>::type>(verify_map[i]));
  }

  FREE_C_HEAP_ARRAY(CandidateOrigin, verify_map);
}
#endif

bool G1CollectionSetCandidates::contains(const G1HeapRegion* r) const {
  const uint index = r->hrm_index();
  assert(index < _max_regions, "must be");
  return _contains_map[index] != CandidateOrigin::Invalid;
}

bool G1CollectionSetCandidates::is_humongous(const G1HeapRegion* r) const {
  assert(contains(r), "must be");
  return _contains_map[r->hrm_index()] == CandidateOrigin::Humongous;
}

const char* G1CollectionSetCandidates::get_short_type_str(const G1HeapRegion* r) const {
  static const char* type_strings[] = {
    "Ci",  // Invalid
    "Cm",  // Marking
    "Cr",  // Retained
    "Ch",  // Humongous
    "Cv"   // Verification
  };

  uint8_t kind = static_cast<std::underlying_type<CandidateOrigin>::type>(_contains_map[r->hrm_index()]);
  return type_strings[kind];
}
