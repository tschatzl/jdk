/*
 * Copyright (c) 2011, 2025, Oracle and/or its affiliates. All rights reserved.
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

#include "memory/allocation.inline.hpp"
#include "runtime/os.hpp"
#include "utilities/quickSort.hpp"
#include "unittest.hpp"

static int test_comparator(int a, int b) {
  if (a == b) {
    return 0;
  }
  if (a < b) {
    return -1;
  }
  return 1;
}

static bool compare_arrays(int* actual, int* expected, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if (actual[i] != expected[i]) {
      return false;
    }
  }
  return true;
}

template <class C>
static bool sort_and_compare(int* arrayToSort, int* expectedResult, size_t length, C comparator) {
  QuickSort::sort(arrayToSort, length, comparator);
  return compare_arrays(arrayToSort, expectedResult, length);
}

extern "C" {
  static int test_stdlib_comparator(const void* a, const void* b) {
    int ai = *(int*)a;
    int bi = *(int*)b;
    if (ai == bi) {
      return 0;
    }
    if (ai < bi) {
      return -1;
    }
    return 1;
  }
}

TEST(QuickSort, quicksort) {
  {
    int* test_array = nullptr;
    int* expected_array = nullptr;
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 0, test_comparator));
  }
  {
    int test_array[] = {3};
    int expected_array[] = {3};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 1, test_comparator));
  }
  {
    int test_array[] = {3,2};
    int expected_array[] = {2,3};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 2, test_comparator));
  }
  {
    int test_array[] = {3,2,1};
    int expected_array[] = {1,2,3};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 3, test_comparator));
  }
  {
    int test_array[] = {4,3,2,1};
    int expected_array[] = {1,2,3,4};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 4, test_comparator));
  }
  {
    int test_array[] = {7,1,5,3,6,9,8,2,4,0};
    int expected_array[] = {0,1,2,3,4,5,6,7,8,9};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 10, test_comparator));
  }
  {
    int test_array[] = {4,4,1,4};
    int expected_array[] = {1,4,4,4};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 4, test_comparator));
  }
  {
    int test_array[] = {0,1,2,3,4,5,6,7,8,9};
    int expected_array[] = {0,1,2,3,4,5,6,7,8,9};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 10, test_comparator));
  }
  {
    // one of the random arrays that found an issue in the partition method.
    int test_array[] = {76,46,81,8,64,56,75,11,51,55,11,71,59,27,9,64,69,75,21,25,39,40,44,32,7,8,40,41,24,78,24,74,9,65,28,6,40,31,22,13,27,82};
    int expected_array[] = {6,7,8,8,9,9,11,11,13,21,22,24,24,25,27,27,28,31,32,39,40,40,40,41,44,46,51,55,56,59,64,64,65,69,71,74,75,75,76,78,81,82};
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, 42, test_comparator));
  }
}

TEST(QuickSort, random) {
  for (int i = 0; i < 1000; i++) {
    size_t length = os::random() % 100;
    int* test_array = NEW_C_HEAP_ARRAY(int, length, mtInternal);
    int* expected_array = NEW_C_HEAP_ARRAY(int, length, mtInternal);
    for (size_t j = 0; j < length; j++) {
        // Choose random values, but get a chance of getting duplicates
        test_array[j] = os::random() % (length * 2);
        expected_array[j] = test_array[j];
    }

    // Compare sorting to stdlib::qsort()
    qsort(expected_array, length, sizeof(int), test_stdlib_comparator);
    EXPECT_TRUE(sort_and_compare(test_array, expected_array, length, test_comparator));
    FREE_C_HEAP_ARRAY(int, test_array);
    FREE_C_HEAP_ARRAY(int, expected_array);
  }
}
