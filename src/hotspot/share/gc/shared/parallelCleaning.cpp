/*
 * Copyright (c) 2018, 2023, Oracle and/or its affiliates. All rights reserved.
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
#include "classfile/symbolTable.hpp"
#include "classfile/stringTable.hpp"
#include "code/codeCache.hpp"
#include "gc/shared/parallelCleaning.hpp"
#include "logging/log.hpp"
#include "memory/resourceArea.hpp"
#include "logging/log.hpp"
#include "runtime/atomic.hpp"

#include "gc/shared/gc_globals.hpp"

#include "logging/log.hpp"
#include "utilities/ticks.hpp"

#include "code/dependencyContext.hpp"

CodeCacheUnloadingTask::CodeCacheUnloadingTask(uint num_workers, CodeCacheUnloadingTaskScopeProvider* scope_provider) :
  _scope_provider(scope_provider),
  _num_workers(num_workers),
  _first_nmethod(nullptr),
  _claimed_nmethod(nullptr),
  _num_unloaded(nullptr) {
  // Get first alive nmethod
  CompiledMethodIterator iter(CompiledMethodIterator::all_blobs);
  if(iter.next()) {
    _first_nmethod = iter.method();
  }
  _claimed_nmethod = _first_nmethod;
  _num_unloaded = NEW_C_HEAP_ARRAY(size_t, num_workers, mtGC);
  for (uint i = 0; i < num_workers; i++) {
    _num_unloaded[i] = 0;
  }
}

CodeCacheUnloadingTask::~CodeCacheUnloadingTask() {
  CodeCache::verify_clean_inline_caches();
  CodeCache::verify_icholder_relocations();
  FREE_C_HEAP_ARRAY(size_t, _num_unloaded);
}

void CodeCacheUnloadingTask::claim_nmethods(CompiledMethod** claimed_nmethods, int *num_claimed_nmethods) {
  CompiledMethod* first;
  CompiledMethodIterator last(CompiledMethodIterator::all_blobs);

  do {
    *num_claimed_nmethods = 0;

    first = _claimed_nmethod;
    last = CompiledMethodIterator(CompiledMethodIterator::all_blobs, first);

    if (first != nullptr) {

      for (int i = 0; i < MaxClaimNmethods; i++) {
        if (!last.next()) {
          break;
        }
        claimed_nmethods[i] = last.method();
        (*num_claimed_nmethods)++;
      }
    }

  } while (Atomic::cmpxchg(&_claimed_nmethod, first, last.method()) != first);
}

void CodeCacheUnloadingTask::do_work(CompiledMethod::UnloadingScope* scope, CompiledMethod* method, uint worker_id) {
  if (method->do_unloading(scope)) {
    _num_unloaded[worker_id]++;
  }
}

void CodeCacheUnloadingTask::work(uint worker_id) {
    jlong unlink_time = 0;
  Ticks start = Ticks::now();
  CompiledMethod::UnloadingScope* scope = _scope_provider->get_scope(worker_id);
  // The first nmethods is claimed by the first worker.
  if (worker_id == 0 && _first_nmethod != nullptr) {
    do_work(scope, _first_nmethod, worker_id);
    _first_nmethod = nullptr;
  }

  if (worker_id >= ParallelUnlinkWorkers) {
    log_debug(gc)("CodeCachUnloading::do_work %u skipped", worker_id);
    return;
  }
  size_t total_claimed_nmethods = 0;
  int num_claimed_nmethods;
  CompiledMethod* claimed_nmethods[MaxClaimNmethods];

  while (true) {
    claim_nmethods(claimed_nmethods, &num_claimed_nmethods);

    total_claimed_nmethods += num_claimed_nmethods;
    if (num_claimed_nmethods == 0) {
      break;
    }
// nmethod::do_unloading->unlink->unlink_from_method
//                                invalidate_osr_method->remove_osr_method
// take CompiledMethod_lock...
    jlong unlink_start = os::elapsed_counter();
    for (int i = 0; i < num_claimed_nmethods; i++) {
      do_work(scope, claimed_nmethods[i], worker_id);
    }
    unlink_time += (os::elapsed_counter() - unlink_start);
  }
  double duration = (Ticks::now() - start).seconds() * 1000.0;
  log_debug(gc)("CodeCachUnloading::do_work %u total %1.2f unlink %1.2f claimed %zu unloaded %zu", worker_id, duration,
               ((double)unlink_time / os::elapsed_frequency()) * 1000.0, total_claimed_nmethods, _num_unloaded[worker_id]);
}

size_t CodeCacheUnloadingTask::num_unloaded() const {
  size_t result = 0;
  for (uint i = 0; i < _num_workers; i++) {
    result += _num_unloaded[i];
  }
  return result;
}

KlassCleaningTask::KlassCleaningTask() :
  _clean_klass_tree_claimed(0),
  _klass_iterator() {
}

bool KlassCleaningTask::claim_clean_klass_tree_task() {
  if (_clean_klass_tree_claimed) {
    return false;
  }

  return Atomic::cmpxchg(&_clean_klass_tree_claimed, 0, 1) == 0;
}

InstanceKlass* KlassCleaningTask::claim_next_klass() {
  Klass* klass;
  do {
    klass =_klass_iterator.next_klass();
  } while (klass != nullptr && !klass->is_instance_klass());

  // this can be null so don't call InstanceKlass::cast
  return static_cast<InstanceKlass*>(klass);
}

void KlassCleaningTask::clean_klass(InstanceKlass* ik) {
  ik->clean_weak_instanceklass_links();
//  if (UseNewCode && UseG1GC)
//      ik->dependencies().remove_all_dependents();
}

void KlassCleaningTask::work() {
  Ticks start = Ticks::now();
  ResourceMark rm;

  bool claimed_klass_tree_task = false;
  // One worker will clean the subklass/sibling klass tree.
  if (claim_clean_klass_tree_task()) {
    claimed_klass_tree_task = true;
    Klass::clean_subklass_tree();
  }

  // All workers will help cleaning the classes,
  size_t cleaned_klasses = 0;
  InstanceKlass* klass;
  while ((klass = claim_next_klass()) != nullptr) {
    cleaned_klasses++;
    clean_klass(klass);
  }
  double duration = (Ticks::now() - start).seconds() * 1000.0;
  log_debug(gc)("KlassCleaningTask::work(): %1.2f cleaned_klasses %zu claimed_klass_tree_task %s", duration, cleaned_klasses, BOOL_TO_STR(claimed_klass_tree_task));
}
