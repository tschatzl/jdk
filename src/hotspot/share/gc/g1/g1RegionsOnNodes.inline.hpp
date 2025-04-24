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

#ifndef SHARE_VM_GC_G1_G1REGIONS_INLINE_HPP
#define SHARE_VM_GC_G1_G1REGIONS_INLINE_HPP

#include "gc/g1/g1RegionsOnNodes.hpp"

#include "gc/g1/g1HeapRegion.hpp"
#include "gc/g1/g1NUMA.inline.hpp"


uint G1RegionsOnNodes::add(G1HeapRegion* hr) {
  uint node_index = hr->node_index();

  // Update only if the node index is valid.
  if (node_index < _numa->num_active_nodes()) {
    *(_count_per_node + node_index) += 1;
    return node_index;
  }

  return G1NUMA::UnknownNodeIndex;
}

void G1RegionsOnNodes::clear() {
  for (uint i = 0; i < _numa->num_active_nodes(); i++) {
    _count_per_node[i] = 0;
  }
}

uint G1RegionsOnNodes::count(uint node_index) const {
  return _count_per_node[node_index];
}


#endif // SHARE_VM_GC_G1_G1REGIONS_INLINE_HPP
