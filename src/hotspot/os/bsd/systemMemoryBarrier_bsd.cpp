/*
 * Copyright (c) 2024, Oracle and/or its affiliates. All rights reserved.
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
#include "logging/log.hpp"
#include "os_bsd.hpp"
#include "runtime/atomic.hpp"
#include "utilities/debug.hpp"
#include "utilities/systemMemoryBarrier.hpp"

#ifdef __APPLE__
  #include <mach/task.h>
extern "C" {
  #include <mach/thread_state.h>
}
  #include <mach/vm_map.h>
#endif

ReservedSpace BSDSystemMemoryBarrier::_mprotect_page;

bool BSDSystemMemoryBarrier::initialize_mprotect_page() {
  _mprotect_page = ReservedSpace(os::vm_page_size(), mtInternal); // FIXME
  if (!_mprotect_page.is_reserved()) {
    return false;
  }
  if (!os::commit_memory(_mprotect_page.base(), _mprotect_page.size(), false, mtInternal)) {
    log_error(os)("Failed to commit memory barrier page."); // FIXME: or just bail out?
    return false;
  }
  if (!os::protect_memory(_mprotect_page.base(), _mprotect_page.size(), os::MEM_PROT_NONE, _mprotect_page.special())) {
    log_error(os)("Failed to mprotect memory barrier page."); // FIXME: or just bail out?
    return false;
  }
  if (_mprotect_page.is_reserved()) {
    log_info(os)("Using MPROTECT");
    return true;
  } else {
    return false;
  }
}

bool BSDSystemMemoryBarrier::initialize() {
#ifdef __APPLE__
  if (UseNewCode2) {
    return initialize_mprotect_page();
  }
  return true;
#else
  return initialize_mprotect_page();
#endif
}

void BSDSystemMemoryBarrier::emit_mprotect() {
  if (!os::protect_memory(_mprotect_page.base(), _mprotect_page.size(), os::MEM_PROT_RW, _mprotect_page.special())) {
    log_error(os)("Failed to mprotect memory barrier page."); // FIXME: or just bail out?
  }
  Atomic::store(_mprotect_page.base(), '1');
  if (!os::protect_memory(_mprotect_page.base(), _mprotect_page.size(), os::MEM_PROT_NONE, _mprotect_page.special())) {
    log_error(os)("Failed to mprotect memory barrier page."); // FIXME: or just bail out?
  }
}

void BSDSystemMemoryBarrier::emit() {
#ifdef __APPLE__
  if (UseNewCode2) {
    emit_mprotect();
    return;
  }
  // The idea about the OSX implementation is to execute some function in the context
  // of every thread that serializes memory, in this case thread_get_register_pointer_values().

  mach_msg_type_number_t num_threads;
  thread_act_t* threads;
  kern_return_t kr;

  kr = task_threads(mach_task_self(), &threads, &num_threads);
  if (kr != KERN_SUCCESS) {
    fatal("task_threads() failed with %d", kr);
  }

  const size_t num_regs = 256;

  for (mach_msg_type_number_t i = 0; i < num_threads; i++) {
    uintptr_t values[num_regs];
    size_t actual_num_regs = num_regs;

    kr = thread_get_register_pointer_values(threads[i], nullptr, &actual_num_regs, values);
    // Some threads return garbage for above call, so only check KERN_INSUFFICIENT_BUFFER_SIZE.
    if (kr == KERN_INSUFFICIENT_BUFFER_SIZE) {
      fatal("thread_get_register_pointer_values() failed with %d", kr);
    }

    kr = mach_port_deallocate(mach_task_self(), threads[i]);
    if (kr != KERN_SUCCESS) {
      fatal("mach_port_deallocate() failed with %d", kr);
    }
  }

  kr = vm_deallocate(mach_task_self(), (vm_address_t)threads, num_threads * sizeof(thread_act_t));
  if (kr != KERN_SUCCESS) {
    fatal("vm_deallocate() failed with %d", kr);
  }
#else
  emit_mprotect();
#endif
}
