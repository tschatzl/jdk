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
#include "utilities/debug.hpp"
#include "utilities/systemMemoryBarrier.hpp"

#ifdef __APPLE__
  #include <mach/task.h>
extern "C" {
  #include <mach/thread_state.h>
}
  #include <mach/vm_map.h>
#endif

ReservedSpace _mprotect_page;

bool BSDSystemMemoryBarrier::initialize() {
#ifdef __APPLE__
  return true;
#else
  return false;
#endif
}

void BSDSystemMemoryBarrier::emit() {
#ifdef __APPLE__
  // The idea about the OSX implementation is to execute some function in the context
  // of every thread that serializes memory, in this case
  // thread_get_register_pointer_values().

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
    // FIXME: investigate why this does not return KERN_SUCCESS on success but some "random" numbers
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
#endif
}
