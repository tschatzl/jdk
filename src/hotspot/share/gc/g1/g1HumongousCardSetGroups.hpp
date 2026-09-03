/*
 * Copyright (c) 2026, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_G1_HUMONGOUSCARDSETGROUPS_HPP
#define SHARE_GC_G1_HUMONGOUSCARDSETGROUPS_HPP

#include "gc/g1/g1CollectionSetCandidates.hpp"

class G1HumongousCardSetGroups {
  G1CardSetGroupList _updating;
  G1CardSetGroupList _complete;

  G1CardSetGroup* new_group(G1HeapRegion* starts_humongous, G1CardSetGroup::State state);
public:
  G1CardSetGroupList* updating() { return &_updating; }
  G1CardSetGroupList* complete() { return &_complete; }

  G1HumongousCardSetGroups() : _updating(), _complete() {
  }

  void clear();

  void remove_group(G1CardSetGroup* gr);

  void add_complete_group(G1HeapRegion* starts_humongous);
  void set_updating_groups(GrowableArrayCHeap<G1HeapRegion*, mtGC>* regions);

  void after_rebuild();
};

#endif // SHARE_GC_G1_HUMONGOUSCARDSETGROUPS_HPP
