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

#ifndef SHARE_VM_GC_G1_NUMA_INLINE_HPP
#define SHARE_VM_GC_G1_NUMA_INLINE_HPP

#include "gc/g1/g1NUMA.hpp"

inline size_t G1NUMA::region_size() const {
  assert(_region_size > 0, "Heap region size is not yet set");
  return _region_size;
}

inline size_t G1NUMA::page_size() const {
  assert(_page_size > 0, "Page size not is yet set");
  return _page_size;
}

inline uint G1NUMA::index_of_node_id(uint node_id) const {
  assert(node_id < _len_node_id_to_index_map, "invalid node id %d", node_id);
  uint node_index = _node_id_to_index_map[node_id];
  assert(node_index != G1NUMA::UnknownNodeIndex,
         "invalid node id %d", node_id);
  return node_index;
}

inline bool G1NUMA::is_enabled() const { return num_active_nodes() > 1; }

inline uint G1NUMA::numa_id(uint index) const {
  assert(index < _len_node_id_to_index_map, "Index %d out of range: [0,%d)",
         index, _len_node_id_to_index_map);
  return _node_ids[index];
}

inline const uint* G1NUMA::node_ids() const {
  return _node_ids;
}

inline void G1NUMA::set_region_info(size_t region_size, size_t page_size) {
  _region_size = region_size;
  _page_size = page_size;
}

inline uint G1NUMA::num_active_nodes() const {
  assert(_num_active_node_ids > 0, "just checking");
  return _num_active_node_ids;
}

inline uint G1NUMA::index_of_current_thread() const {
  if (!is_enabled()) {
    return 0;
  }
  return index_of_node_id(os::numa_get_group_id());
}

inline uint G1NUMA::max_search_depth() const {
  // Multiple of 3 is just random number to limit iterations.
  // There would be some cases that 1 page may be consisted of multiple heap regions.
  return 3 * MAX2((uint)(page_size() / region_size()), (uint)1) * num_active_nodes();
}

#endif // SHARE_VM_GC_G1_NUMA_INLINE_HPP
