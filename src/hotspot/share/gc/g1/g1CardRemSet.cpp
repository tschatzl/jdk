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

#include "gc/g1/g1CardRemSet.inline.hpp"

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1CollectionSetCandidates.inline.hpp"

const char* G1CardRemSet::_state_strings[] =  {"Untracked", "Updating", "Complete"};
const char* G1CardRemSet::_short_state_strings[] =  {"UNTRA", "UPDAT", "CMPLT"};

const char* G1CardRemSet::get_state_str(G1CardRemSet* rem_set) {
  if (rem_set == nullptr) {
    return "None";
  } else {
    return _state_strings[rem_set->state()];
  }
}

const char* G1CardRemSet::get_short_state_str(G1CardRemSet* rem_set) {
  if (rem_set == nullptr) {
    return "NONE ";
  } else {
    return _short_state_strings[rem_set->state()];
  }
}

uint G1CardRemSet::id(G1CardRemSet* rem_set) {
  if (rem_set == nullptr) {
    return G1CSetCandidateGroup::NoRemSetId;
  } else {
    return rem_set->_candidate->id();
  }
}

G1CardRemSet::G1CardRemSet(G1CSetCandidateGroup* candidate, G1CardSetConfiguration* config, G1MonotonicArenaFreePool* card_set_freelist_pool, bool is_complete) :
  _candidate(candidate),
  _card_set_mm(config, card_set_freelist_pool),
  _card_set(config, &_card_set_mm),
  _state(is_complete ? Complete : Updating)
{ }
