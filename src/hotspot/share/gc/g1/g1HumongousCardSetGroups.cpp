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

#include "gc/g1/g1CollectedHeap.inline.hpp"
#include "gc/g1/g1HumongousCardSetGroups.hpp"

void G1HumongousCardSetGroups::clear() {
  _updating.clear(true /* uninstall_card_set_group */);
  _complete.clear(true /* uninstall_card_set_group */);
}

G1CardSetGroup* G1HumongousCardSetGroups::new_group(G1HeapRegion* starts_humongous, G1CardSetGroup::State state) {
  precond(starts_humongous->is_starts_humongous());

  G1CardSetGroup* gr = new G1CardSetGroup(state);
  G1CollectedHeap::heap()->humongous_obj_regions_iterate(starts_humongous, [&] (G1HeapRegion* r) { gr->add(r); });
  return gr;
}

void G1HumongousCardSetGroups::add_complete_group(G1HeapRegion* starts_humongous) {
  G1CardSetGroup* gr = new_group(starts_humongous, G1CardSetGroup::State::Complete);
  _complete.append(gr);
  log_debug(gc)("new complete group %u", gr->group_id());
}

void G1HumongousCardSetGroups::set_updating_groups(GrowableArrayCHeap<G1HeapRegion*, mtGC>* regions) {
  assert_at_safepoint_on_vm_thread();
  assert(_updating.num_regions() == 0, "should be empty");

  for (G1HeapRegion* starts_humongous : *regions) {
    G1CardSetGroup* gr = new_group(starts_humongous, G1CardSetGroup::State::Updating);
    _updating.append(gr);
    log_debug(gc)("new updating group %u", gr->group_id());
  }
}

void G1HumongousCardSetGroups::remove_group(G1CardSetGroup* gr) {
  assert_at_safepoint();
  precond(gr != nullptr);

  _updating.remove(gr);
  _complete.remove(gr);

  log_debug(gc)("removed group %u", gr->group_id());
  gr->clear(true /* uninstall_card_set_group */);
  delete gr;
}

void G1HumongousCardSetGroups::after_rebuild() {
  assert_at_safepoint_on_vm_thread();

  for (G1CardSetGroup* gr : _updating) {
    bool should_keep = G1CollectedHeap::heap()->policy()->remset_tracker()->update_after_rebuild(gr);

    if (should_keep) {
      gr->set_complete();
      _complete.append(gr);
    } else {
      gr->clear(true /* uninstall_card_set_group */);
      delete gr;
    }
  }
  // Empty the updating list. Ownership has transferred or the element deleted.
  // FIXME: add detach_all() or something
  _updating.remove_selected(_updating.length(), _updating.num_regions());
}
