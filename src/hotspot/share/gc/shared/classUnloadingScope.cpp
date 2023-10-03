/*
 * Copyright (c) 2023, Oracle and/or its affiliates. All rights reserved.
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

#include "classfile/classLoaderData.hpp"
#include "gc/shared/classUnloadingScope.hpp"

ClassUnloadingContext* ClassUnloadingContext::_context = nullptr;

DefaultClassUnloadingContext::DefaultClassUnloadingContext() : _unloading_head(nullptr) {
  _context = this;
}

~DefaultClassUnloadingContext::DefaultClassUnloadingContext() {
  assert(_context == this, "must be");
  _context = nullptr;
}


bool DefaultClassUnloadingContext::has_unloaded_classes() const {
  return _unloading_head != nullptr;
}

void DefaultClassUnloadingContext::register_unloading_class_loader(ClassLoaderData* cld) {
  cld->unload();

  cld->set_unloading_next(_unloading_head);
  _unloading_head = cld;
}

void DefaultClassUnloadingContext::classloaderdata_unloading_do(void f(ClassLoaderData* const)) {
  assert_locked_or_safepoint(ClassLoaderDataGraph_lock);
  for (ClassLoaderData* cld = _unloading_head; cld != nullptr; cld = cld->unloading_next()) {
    assert(cld->is_unloading(), "invariant");
    f(cld);
  }  
}

void DefaultClassUnloadingContext::classes_unloading_do(void f(Klass* const)) {
  assert_locked_or_safepoint(ClassLoaderDataGraph_lock);
  for (ClassLoaderData* cld = _unloading_head; cld != nullptr; cld = cld->unloading_next()) {
    assert(cld->is_unloading(), "invariant");
    cld->classes_do(f);
  }
}

void DefaultClassUnloadingContext::register_free_nmethod(CodeBlob* cb) {
  assert(nm->unlinked_next() == nullptr, "Only register for unloading once");
  for (;;) {
    // Only need acquire when reading the head, when the next
    // pointer is walked, which it is not here.
    nmethod* head = Atomic::load(&_unlinked_head);
    nmethod* next = head != nullptr ? head : nm; // Self looped means end of list
    nm->set_unlinked_next(next);
    if (Atomic::cmpxchg(&_unlinked_head, head, nm) == head) {
      break;
    }
  }
}
