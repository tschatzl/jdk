/*
 * Copyright (c) 2018, 2021, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_GC_SHARED_PARALLELCLEANING_HPP
#define SHARE_GC_SHARED_PARALLELCLEANING_HPP

#include "classfile/classLoaderDataGraph.hpp"
#include "code/codeCache.hpp"
#include "code/compiledMethod.hpp"
#include "gc/shared/oopStorageParState.hpp"
#include "gc/shared/workerThread.hpp"

class CodeCacheUnloadingTaskScopeProvider {
public:
  virtual CompiledMethod::UnloadingScope* get_scope(uint worker_id) = 0;
};

class DefaultCodeCacheUnloadingTaskDefaultScopeProvider : public CodeCacheUnloadingTaskScopeProvider {
  CodeCache::DefaultCompiledMethodUnloadingScope _default_cm_scope;

public:
  DefaultCodeCacheUnloadingTaskDefaultScopeProvider(bool unloading_occurred) : _default_cm_scope(unloading_occurred) { }
  CompiledMethod::UnloadingScope* get_scope(uint worker_id) override { return &_default_cm_scope; }
};

class CodeCacheUnloadingTask {
  CodeCacheUnloadingTaskScopeProvider* _scope_provider;
  const uint _num_workers;

  // Variables used to claim nmethods.
  CompiledMethod* _first_nmethod;
  CompiledMethod* volatile _claimed_nmethod;

  size_t volatile* _num_unloaded;

public:
  CodeCacheUnloadingTask(uint num_workers, CodeCacheUnloadingTaskScopeProvider* scope_provider);
  ~CodeCacheUnloadingTask();

private:
  static const int MaxClaimNmethods = 16;
  void claim_nmethods(CompiledMethod** claimed_nmethods, int *num_claimed_nmethods);

  void do_work(CompiledMethod::UnloadingScope* scope, CompiledMethod* nmethod, uint worker_id);

public:
  // Cleaning and unloading of nmethods.
  void work(uint worker_id);

  size_t num_unloaded() const;
};


class KlassCleaningTask : public StackObj {
  volatile int                            _clean_klass_tree_claimed;
  ClassLoaderDataGraphKlassIteratorAtomic _klass_iterator;

public:
  KlassCleaningTask();

private:
  bool claim_clean_klass_tree_task();
  InstanceKlass* claim_next_klass();

public:

  void clean_klass(InstanceKlass* ik);
  void work();
};

#endif // SHARE_GC_SHARED_PARALLELCLEANING_HPP
