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

#ifndef SHARE_GC_SHARED_CLASSUNLOADINGCONTEXT_HPP
#define SHARE_GC_SHARED_CLASSUNLOADINGCONTEXT_HPP

class ClassLoaderData;
class CodeBlob;
class Klass;

class ClassUnloadingContext {
  static ClassUnloadingContext* _context;

public:
  static ClassUnloadingContext* context() { return _context; }

  virtual bool has_unloaded_classes() const = 0;

  virtual void register_unloading_class_loader(ClassLoaderData* cld) = 0;
  virtual void classloaderdata_unloading_do(void f(ClassLoaderData* const)) = 0;
  virtual void classes_unloading_do(void f(Klass* const)) = 0;

  virtual void register_free_nmethod(CodeBlob* cb) = 0;
};

class DefaultClassUnloadingContext : public ClassUnloadingContext {
  ClassLoaderData* volatile _unloading_head;

public:
  DefaultClassUnloadingContext();
  ~DefaultClassUnloadingContext();

  bool has_unloaded_classes() const override;

  void register_unloading_class_loader(ClassLoaderData* cld) override;
  void classloaderdata_unloading_do(void f(ClassLoaderData* const)) override;
  void classes_unloading_do(void f(Klass* const)) override;

  void register_free_nmethod(CodeBlob* cb) override;
};

#endif // SHARE_GC_SHARED_CLASSUNLOADINGCONTEXT_HPP
