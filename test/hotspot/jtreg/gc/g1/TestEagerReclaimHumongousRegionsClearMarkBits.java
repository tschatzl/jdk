/*
 * Copyright (c) 2014, 2025, Oracle and/or its affiliates. All rights reserved.
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
 */

package gc.g1;

/*
 * @test TestEagerReclaimHumongousRegionsClearMarkBits
 * @bug 8051973
 * @summary Test to make sure that eager reclaim of humongous objects correctly clears
 * mark bitmaps at reclaim.
 * @requires vm.gc.G1
 * @requires vm.debug
 * @library /test/lib /testlibrary /
 * @modules java.base/jdk.internal.misc
 *          java.management
 * @build jdk.test.whitebox.WhiteBox
 * @run driver jdk.test.lib.helpers.ClassFileInstaller jdk.test.whitebox.WhiteBox
 * @run main/othervm -XX:+UnlockDiagnosticVMOptions -Xbootclasspath/a:. -XX:+WhiteBoxAPI gc.g1.TestEagerReclaimHumongousRegionsClearMarkBits
 */

import java.util.regex.Matcher;
import java.util.regex.Pattern;

import jdk.test.lib.Asserts;
import jdk.test.lib.process.OutputAnalyzer;
import jdk.test.lib.process.ProcessTools;
import jdk.test.whitebox.WhiteBox;

public class TestEagerReclaimHumongousRegionsClearMarkBits {
    private static final WhiteBox WB = WhiteBox.getWhiteBox();

    private static String runHelperVM(boolean useTypeArray, boolean keepReference, String phase) throws Exception {
        OutputAnalyzer output = ProcessTools.executeLimitedTestJava("-XX:+UseG1GC",
                                                                    "-Xmx20M",
                                                                    "-Xms20m",
                                                                    "-XX:+UnlockDiagnosticVMOptions",
                                                                    "-XX:+VerifyAfterGC",
                                                                    "-Xbootclasspath/a:.",
                                                                    "-Xlog:gc=debug,gc+humongous=debug",
                                                                    "-XX:+UnlockDiagnosticVMOptions",
                                                                    "-XX:+WhiteBoxAPI",
                                                                    TestEagerReclaimHumongousRegionsClearMarkBitsRunner.class.getName(),
                                                                    String.valueOf(useTypeArray),
                                                                    String.valueOf(keepReference),
                                                                    phase);

        String log = output.getStdout();
        System.out.println(log);
        output.shouldHaveExitValue(0);
        return log;
    }

    private static String boolToInt(boolean value) {
        return value ? "1" : "0";
    }

    private static void runTest(boolean useTypeArray, boolean keepReference, String phase, boolean expectedMarked, boolean expectedCandidate, boolean expectedReclaim) throws Exception {
        String log = runHelperVM(useTypeArray, keepReference, phase);
        // Find the log output indicating that the humongous object has been reclaimed, and marked.
        // Example output:
        // [0.351s][debug][gc,humongous] GC(3) Humongous region 2 (object size 4194320 @ 0x00000000fee00000) remset 0 code roots 0 marked 1 pinned count 0 reclaim candidate 1 type tA
        String patternString = "Humongous region .* marked " + boolToInt(expectedMarked) + " .* reclaim candidate " + boolToInt(expectedCandidate) + " type " + (useTypeArray ? "tA" : "oA");
        Pattern pattern = Pattern.compile(patternString);
        Asserts.assertTrue(pattern.matcher(log).find(), "Could not find log output matching marked humongous region with pattern \"" + patternString + "\"");

        pattern = Pattern.compile("Reclaimed humongous region .*");
        Asserts.assertTrue(expectedReclaim == pattern.matcher(log).find(), "Wrong log output reclaiming humongous region");
    }

    public static void main(String[] args) throws Exception {
        runTest(true /* useTypeArray */, false /* keepReference */, WB.BEFORE_MARKING_COMPLETED, true /* expectedMarked */, true /* expectedCandidate */, true /* expectedReclaim */);
        runTest(true /* useTypeArray */, false /* keepReference */, WB.G1_BEFORE_REBUILD_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, true /* expectedReclaim */);
        runTest(true /* useTypeArray */, false /* keepReference */, WB.G1_BEFORE_CLEANUP_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, true /* expectedReclaim */);

        runTest(true /* useTypeArray */, true /* keepReference */, WB.BEFORE_MARKING_COMPLETED, true /* expectedMarked */, true /* expectedCandidate */, false /* expectedReclaim */);
        runTest(true /* useTypeArray */, true /* keepReference */, WB.G1_BEFORE_REBUILD_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, false /* expectedReclaim */);
        runTest(true /* useTypeArray */, true /* keepReference */, WB.G1_BEFORE_CLEANUP_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, false /* expectedReclaim */);

        runTest(false /* useTypeArray */, false /* keepReference */, WB.BEFORE_MARKING_COMPLETED, true /* expectedMarked */, false /* expectedCandidate */, false /* expectedReclaim */);
        runTest(false /* useTypeArray */, false /* keepReference */, WB.G1_BEFORE_REBUILD_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, true /* expectedReclaim */);
        runTest(false /* useTypeArray */, false /* keepReference */, WB.G1_BEFORE_CLEANUP_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, true /* expectedReclaim */);

        runTest(false /* useTypeArray */, true /* keepReference */, WB.BEFORE_MARKING_COMPLETED, true /* expectedMarked */, false /* expectedCandidate */, false /* expectedReclaim */);
        runTest(false /* useTypeArray */, true /* keepReference */, WB.G1_BEFORE_REBUILD_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, false /* expectedReclaim */);
        runTest(false /* useTypeArray */, true /* keepReference */, WB.G1_BEFORE_CLEANUP_COMPLETED, false /* expectedMarked */, true /* expectedCandidate */, false /* expectedReclaim */);
    }
}

class TestEagerReclaimHumongousRegionsClearMarkBitsRunner {
    private static final WhiteBox WB = WhiteBox.getWhiteBox();
    private static final int M = 1024 * 1024;

    public static void main(String[] args) throws Exception {
        if (args.length != 3) {
            throw new Exception("Invalid number of arguments " + args.length);
        }
        boolean useTypeArray = Boolean.parseBoolean(args[0]);
        boolean keepReference = Boolean.parseBoolean(args[1]);
        String phase = args[2];

        System.out.println("useTypeArray: " + useTypeArray + " keepReference: " + keepReference + " phase: " + phase);
        WB.fullGC();

        Object largeObj = useTypeArray ? new int[M] : new Object[M]; // Humongous object.

        WB.concurrentGCAcquireControl();
        WB.concurrentGCRunTo(phase);

        if (!keepReference) {
          largeObj = null;
        }
        WB.youngGC(); // Should reclaim marked humongous object.

        WB.concurrentGCRunToIdle();
        System.out.println("Large object at " + largeObj);
    }
}

