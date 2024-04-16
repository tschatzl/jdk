/*
 * Copyright (c) 2019, 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "precompiled.hpp"
#include "gc/shared/bufferNode.hpp"
#include "gc/shared/bufferNodeList.hpp"
#include "utilities/debug.hpp"

BufferNodeList::BufferNodeList() :
  _head(nullptr), _tail(nullptr), _entry_count(0) {}

BufferNodeList::BufferNodeList(BufferNode* head,
                               BufferNode* tail,
                               size_t entry_count) :
  _head(head), _tail(tail), _entry_count(entry_count)
{
  assert((_head == nullptr) == (_tail == nullptr), "invariant " PTR_FORMAT " " PTR_FORMAT, p2i(_head), p2i(_tail));
  assert((_head == nullptr) == (_entry_count == 0), "invariant");
#ifdef ASSERT
  size_t actual = 0;
  for (BufferNode* cur = head; cur != nullptr; cur = cur->next()) {
    actual += cur->size();
  }
  assert(_entry_count == actual, "Expected %zu and actual %zu entry counts differ", _entry_count, actual);
#endif
}

BufferNodeList BufferNodeList::append(BufferNodeList& other) {
  if (_entry_count == 0) {
    return other;
  } else if (other._entry_count == 0) {
    return *this;
  }
  _tail->set_next(other._head);
  return BufferNodeList(_head, other._tail, _entry_count + other._entry_count);
}
