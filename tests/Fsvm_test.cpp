/*
 * Copyright (C) 2026 DiamaneOS
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include "../fs/Fsvm.h"

namespace android {
namespace vold {
namespace fsvm {

static constexpr uint64_t kDisk = 64ULL << 30;

TEST(FsvmTest, ParsesTheProbeOutput) {
    Layout l;
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 gpt 3\n"
                              "PART 1 public 2048 4096 exfat 6FFF-FAB8 01010101-0101-0101-0101-010101010101 125 "
                              "4b61727465\n"
                              "PART 2 meta 6144 2048 - - 02020202-0202-0202-0202-020202020202 0 -\n"
                              "PART 3 expand 8192 100000 - - 03030303-0303-0303-0303-030303030303 0 -\n"
                              "END\n",
                              kDisk, &l));
    ASSERT_EQ(3u, l.parts.size());
    EXPECT_EQ("gpt", l.table);
    EXPECT_EQ("Karte", l.parts[0].label);
    EXPECT_EQ("6FFF-FAB8", l.parts[0].fsUuid);
    EXPECT_EQ(Part::Class::kExpand, l.parts[2].cls);
    EXPECT_EQ("03030303-0303-0303-0303-030303030303", l.parts[2].partGuid);
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 none 0\nEND\n", kDisk, &l));
    EXPECT_TRUE(l.parts.empty());
}

TEST(FsvmTest, RefusesAnythingElse) {
    Layout l;
    const char* bad[] = {
            "",
            "LAYOUT 1 gpt 0\nEND",                                                 // no final newline
            "LAYOUT 2 gpt 0\nEND\n",                                               // other version
            "LAYOUT 1 apm 0\nEND\n",                                               // other table
            "LAYOUT 1 mbr 1\nEND\n",                                               // fewer lines than counted
            "LAYOUT 1 mbr 17\nEND\n",                                              // too many
            "LAYOUT 1 mbr 0\nEND\nEND\n",                                          // trailing line
            "LAYOUT 1 mbr 1\nPART 1 public 2048 0 vfat - - 0 -\nEND\n",            // empty
            "LAYOUT 1 mbr 1\nPART 1 public 2048 134217728 vfat - - 0 -\nEND\n",    // past the end
            "LAYOUT 1 mbr 1\nPART 1 public 18446744073709551615 2 vfat - - 0 -\nEND\n",  // wraps
            "LAYOUT 1 mbr 2\nPART 1 public 2048 4096 vfat - - 0 -\n"
            "PART 2 public 4096 4096 vfat - - 0 -\nEND\n",                         // overlap
            "LAYOUT 1 mbr 2\nPART 1 public 2048 10 vfat - - 0 -\n"
            "PART 1 public 9000 10 vfat - - 0 -\nEND\n",                           // same number
            "LAYOUT 1 mbr 1\nPART 1 public 2048 10 ntfs - - 0 -\nEND\n",           // other filesystem
            "LAYOUT 1 mbr 1\nPART 1 public 2048 10 vfat 6fff-fab8 - 0 -\nEND\n",   // lower case UUID
            "LAYOUT 1 mbr 1\nPART 1 public 2048 10 vfat ../x - 0 -\nEND\n",        // path in the UUID
            "LAYOUT 1 mbr 1\nPART 1 public 2048 10 vfat - - 0 0a41\nEND\n",        // control character in the label
            "LAYOUT 1 mbr 1\nPART 1 public 2048 10 vfat - - 0 4\nEND\n",           // odd label hex
            "LAYOUT 1 mbr 1\nPART 1 private 2048 10 - - - 0 -\nEND\n",             // other class
            "LAYOUT 1 gpt 1\nPART 2 expand 2048 10 - - - 0 -\nEND\n",              // adoptable without a GUID
            "LAYOUT 1 gpt 1\nPART 2 expand 2048 10 vfat - 01010101-0101-0101-0101-010101010101 0 -\nEND\n",
            "LAYOUT 1 none 1\nPART 1 public 0 10 vfat - - 0 -\nEND\n",             // numbered whole disk
            "LAYOUT 1 mbr 1\nPART 129 public 2048 10 vfat - - 0 -\nEND\n",         // number too high
            "LAYOUT 1 mbr 1\nPART 1  public 2048 10 vfat - - 0 -\nEND\n",          // double space
            "LAYOUT 1 mbr 1\nPART +1 public 2048 10 vfat - - 0 -\nEND\n",         // sign
            "LAYOUT 1 mbr 1\nPART 1 public 0x800 10 vfat - - 0 -\nEND\n",        // hex
            "LAYOUT 1 mbr  1\nEND\n",                                            // spaces in the count
    };
    for (const char* t : bad) {
        EXPECT_NE(OK, ParseLayout(t, kDisk, &l)) << t;
    }
    EXPECT_NE(OK, ParseLayout("LAYOUT 1 none 0\nEND\n", kDisk + 1, &l));
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
