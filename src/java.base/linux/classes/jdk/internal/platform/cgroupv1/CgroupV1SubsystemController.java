/*
 * Copyright (c) 2018, 2025, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.  Oracle designates this
 * particular file as subject to the "Classpath" exception as provided
 * by Oracle in the LICENSE file that accompanied this code.
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

package jdk.internal.platform.cgroupv1;

import java.lang.System.Logger.Level;
import java.nio.file.Path;
import java.nio.file.Files;
import jdk.internal.platform.CgroupSubsystem;
import jdk.internal.platform.CgroupSubsystemController;

public class CgroupV1SubsystemController implements CgroupSubsystemController {

    private static final double DOUBLE_RETVAL_UNLIMITED = CgroupSubsystem.LONG_RETVAL_UNLIMITED;
    // Values returned larger than this number are unlimited.
    static long UNLIMITED_MIN = 0x7FFFFFFFFF000000L;
    String root;
    String mountPoint;
    String path;

    public CgroupV1SubsystemController(String root, String mountPoint) {
        this.root = root;
        this.mountPoint = mountPoint;
    }

    public void setPath(String cgroupPath) {
        if (root != null && cgroupPath != null) {
            String path = mountPoint;
            if (root.equals("/")) {
                // host processes and containers with cgroupns=private
                if (!cgroupPath.equals("/")) {
                    path += cgroupPath;
                }
            } else {
                // containers with cgroupns=host, default setting is _root==cgroup_path
                if (!cgroupPath.equals(root)) {
                    if (!cgroupPath.equals("") && !cgroupPath.equals("/")) {
                        // When moved to a subgroup, between subgroups, the path suffix will change.
                        Path cgp = Path.of(cgroupPath);
                        int nameCount = cgp.getNameCount();
                        for (int i=0; i < nameCount; i++) {
                            Path dir = Path.of(mountPoint, cgp.toString());
                            if (Files.isDirectory(dir)) {
                                path = dir.toString();
                                if (i > 0) {
                                    System.getLogger("jdk.internal.platform").log(Level.DEBUG, String.format(
                                            "Cgroup v1 path reduced to: %s.", cgp));
                                }
                                break;
                            }
                            int currentNameCount = cgp.getNameCount();
                            cgp = (currentNameCount > 1) ? cgp.subpath(1, currentNameCount) : Path.of("");
                        }
                    }
                }
            }
            this.path = path;
        }
    }

    @Override
    public String path() {
        return path;
    }

    public static long getLongEntry(CgroupSubsystemController controller, String param, String entryname) {
        return CgroupSubsystemController.getLongEntry(controller,
                                                      param,
                                                      entryname,
                                                      CgroupSubsystem.LONG_RETVAL_UNLIMITED /* retval on error */);
    }

    public static double getDoubleValue(CgroupSubsystemController controller, String param) {
        return CgroupSubsystemController.getDoubleValue(controller,
                                                        param,
                                                        DOUBLE_RETVAL_UNLIMITED /* retval on error */);
    }

    public static long convertStringToLong(String strval) {
        return CgroupSubsystemController.convertStringToLong(strval,
                                                             Long.MAX_VALUE /* overflow value */,
                                                             CgroupSubsystem.LONG_RETVAL_UNLIMITED /* retval on error */);
    }

    public static long longValOrUnlimited(long value) {
        return value > UNLIMITED_MIN ? CgroupSubsystem.LONG_RETVAL_UNLIMITED : value;
    }

    public static long getLongValueMatchingLine(CgroupSubsystemController controller,
                                                String param,
                                                String match) {
        return CgroupSubsystemController.getLongValueMatchingLine(controller,
                                                                  param,
                                                                  match,
                                                                  CgroupV1SubsystemController::convertHierachicalLimitLine,
                                                                  CgroupSubsystem.LONG_RETVAL_UNLIMITED);
    }

    public static long convertHierachicalLimitLine(String line) {
        String[] tokens = line.split("\\s");
        if (tokens.length == 2) {
            String strVal = tokens[1];
            return CgroupV1SubsystemController.convertStringToLong(strVal);
        }
        return CgroupV1SubsystemController.UNLIMITED_MIN + 1; // unlimited
    }

}
