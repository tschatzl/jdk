/*
 *  Copyright (c) 2022, Oracle and/or its affiliates. All rights reserved.
 *  DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  This code is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License version 2 only, as
 *  published by the Free Software Foundation.
 *
 *  This code is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 *  version 2 for more details (a copy is included in the LICENSE file that
 *  accompanied this code).
 *
 *  You should have received a copy of the GNU General Public License version
 *  2 along with this work; if not, write to the Free Software Foundation,
 *  Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 *  Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 *  or visit www.oracle.com if you need additional information or have any
 *  questions.
 */

/*
 * @test id=shenandoah
 * @requires vm.gc.Shenandoah
 * @enablePreview
 * @modules java.base/jdk.internal.foreign
 * @run testng/othervm -XX:+UseShenandoahGC TestNativeView
 */

/*
 * @test id=g1
 * @requires vm.gc.G1
 * @enablePreview
 * @modules java.base/jdk.internal.foreign
 * @run testng/othervm -XX:+UseG1GC TestNativeView
 */

/*
 * @test id=serial
 * @requires vm.gc.Serial
 * @enablePreview
 * @modules java.base/jdk.internal.foreign
 * @run testng/othervm -XX:+UseSerialGC TestNativeView
 */

import jdk.internal.foreign.ForeignGlobals;
import org.testng.annotations.Test;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;

import static java.lang.foreign.ValueLayout.JAVA_INT;
import static org.testng.Assert.assertEquals;
import static org.testng.Assert.assertFalse;

public class TestNativeView {

    @Test
    public void testNativeView() {
        int[] arr = { 0, 1, 2, 3, 4 };
        MemorySegment arrSeg = MemorySegment.ofArray(arr);
        MemorySegment nativeView;
        int[] copy;
        try (Arena arena = Arena.ofConfined()) {
            nativeView = arrSeg.asNativeView(arena);
            for (int i = 0; i < arr.length; i++) {
                int e = nativeView.getAtIndex(JAVA_INT, i);
                assertEquals(e, arr[i]);
                nativeView.setAtIndex(JAVA_INT, i, e + 1);
            }
            copy = nativeView.toArray(JAVA_INT);
            if (ForeignGlobals.PINNING_SUPPORTED) {
                // before the arena is closed, writes should be visible in the arr
                assertEquals(copy, arr);
            }
        }
        assertFalse(nativeView.scope().isAlive());
        assertEquals(copy, arr);
    }

}
