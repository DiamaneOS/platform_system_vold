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
#include <sys/sysmacros.h>

#include "../fs/Fsvm.h"

// vold's main.cpp defines it, and CreateRange refers to it through CreateDeviceNode. The tests never get that far.
struct selabel_handle* sehandle;

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

TEST(FsvmTest, LogicalPartitionsAndBlockSizes) {
    Layout l;
    // Logical partitions of an extended MBR partition are numbered from 5, up to 36.
    EXPECT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 5 public 4160 2048 vfat - - 0 -\nEND\n", kDisk, &l));
    EXPECT_NE(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 37 public 4160 2048 vfat - - 0 -\nEND\n", kDisk, &l));
    // A GPT takes 48 sectors at the start and 40 at the end on a disk with 4096-byte blocks, 34 and 33 with 512.
    const std::string sectors = std::to_string(kDisk / 512);
    const char* guid = "01010101-0101-0101-0101-010101010101";
    auto gpt = [&](uint64_t start, uint64_t length) {
        return "LAYOUT 1 gpt 1\nPART 1 public " + std::to_string(start) + " " + std::to_string(length) +
               " vfat - " + guid + " 0 -\nEND\n";
    };
    EXPECT_EQ(OK, ParseLayout(gpt(34, 100), kDisk, &l, 512));
    EXPECT_NE(OK, ParseLayout(gpt(34, 100), kDisk, &l, 4096));
    EXPECT_EQ(OK, ParseLayout(gpt(48, 100), kDisk, &l, 4096));
    EXPECT_EQ(OK, ParseLayout(gpt(kDisk / 512 - 133, 100), kDisk, &l, 512));
    EXPECT_NE(OK, ParseLayout(gpt(kDisk / 512 - 133, 100), kDisk, &l, 4096));
    EXPECT_NE(OK, ParseLayout(gpt(48, 100), kDisk, &l, 1024));
}

TEST(FsvmTest, DamagedAndUnreadable) {
    Layout l;
    // A damaged FAT or exFAT boot sector, on a partition or on the whole disk.
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4096 bad - - 0 -\nEND\n", kDisk, &l));
    EXPECT_TRUE(l.parts[0].damaged);
    EXPECT_TRUE(l.parts[0].fsType.empty());
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 none 1\nPART 0 public 0 1000 bad - - 0 -\nEND\n", kDisk, &l));
    EXPECT_TRUE(l.parts[0].damaged);
    // A whole-disk entry without a filesystem, and "bad" on adoptable storage, are refused.
    EXPECT_NE(OK, ParseLayout("LAYOUT 1 none 1\nPART 0 public 0 1000 - - - 0 -\nEND\n", kDisk, &l));
    EXPECT_NE(OK, ParseLayout("LAYOUT 1 gpt 1\nPART 2 expand 2048 10 bad - 01010101-0101-0101-0101-010101010101 0 "
                              "-\nEND\n",
                              kDisk, &l));
}

TEST(FsvmTest, OnlyTwoEqualAnswersOfferAFormat) {
    Layout blank, ntfs, fat, mixed, damaged, adopted, other;
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 none 0\nEND\n", kDisk, &blank));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4096 - - - 0 -\nEND\n", kDisk, &ntfs));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4096 vfat - - 0 -\nEND\n", kDisk, &fat));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 2\nPART 1 public 2048 4096 vfat - - 0 -\n"
                              "PART 2 public 8192 4096 - - - 0 -\nEND\n",
                              kDisk, &mixed));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4096 bad - - 0 -\nEND\n", kDisk, &damaged));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 gpt 2\nPART 1 meta 2048 2048 - - 02020202-0202-0202-0202-020202020202 0 -\n"
                              "PART 2 expand 4096 100000 - - 03030303-0303-0303-0303-030303030303 0 -\nEND\n",
                              kDisk, &adopted));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4097 - - - 0 -\nEND\n", kDisk, &other));
    // Nothing usable, or a public partition without a filesystem: a format offer, once confirmed.
    EXPECT_TRUE(OffersFormat(blank));
    EXPECT_TRUE(OffersFormat(ntfs));
    EXPECT_TRUE(OffersFormat(mixed));
    // A readable card, a damaged FAT and adoptable storage never offer one.
    EXPECT_FALSE(OffersFormat(fat));
    EXPECT_FALSE(OffersFormat(damaged));
    EXPECT_FALSE(OffersFormat(adopted));
    // Two answers count only when they are the same in every field.
    EXPECT_TRUE(SameLayout(ntfs, ntfs));
    EXPECT_TRUE(SameLayout(blank, blank));
    EXPECT_FALSE(SameLayout(ntfs, other));
    EXPECT_FALSE(SameLayout(ntfs, blank));
    EXPECT_FALSE(SameLayout(ntfs, fat));
    Layout unread = ntfs;
    unread.parts[0].unread = true;
    EXPECT_FALSE(SameLayout(unread, unread));
    // The same answer from another medium (a blank card swapped for another between the two probes) or size.
    Layout first = blank, second = blank;
    first.diskSeq = "41";
    second.diskSeq = "42";
    EXPECT_FALSE(SameLayout(first, second));
    second.diskSeq = "41";
    EXPECT_TRUE(SameLayout(first, second));
    second.diskBytes = kDisk / 2;
    EXPECT_FALSE(SameLayout(first, second));
}

TEST(FsvmTest, MediumBinding) {
    // No sequence known (a kernel without diskseq): nothing to compare. A disk that does not exist has no medium.
    EXPECT_TRUE(SameMedium(makedev(179, 0), ""));
    EXPECT_FALSE(SameMedium(makedev(4095, 1048575), "7"));
    // A range is refused before anything is set up when the medium differs.
    std::string node;
    EXPECT_EQ(-ENODEV, CreateRange("fsvm-test-0", makedev(4095, 1048575), 2048, 8, false, &node, nullptr, "7"));
    EXPECT_TRUE(node.empty());
}

TEST(FsvmTest, RangeNames) {
    EXPECT_EQ("fsvm-179-0-1", RangeName(makedev(179, 0), 1));
    EXPECT_EQ("fsvm-8-16-36", RangeName(makedev(8, 16), 36));
}

TEST(FsvmTest, RangesRefuseHostileNumbers) {
    // Ranges come only from a layout ParseLayout accepted (inside the disk, no overlaps). CreateRange itself still
    // refuses an empty range and one whose end wraps, before device-mapper sees anything. The kernel's dm-linear
    // refuses a range past the end of the disk.
    std::string node = "x";
    EXPECT_EQ(-EINVAL, CreateRange("fsvm-test-0", makedev(179, 0), 2048, 0, false, &node));
    EXPECT_TRUE(node.empty());
    EXPECT_EQ(-EINVAL, CreateRange("fsvm-test-0", makedev(179, 0), UINT64_MAX / 512, 2, false, &node));
    EXPECT_EQ(-EINVAL, CreateRange("fsvm-test-0", makedev(179, 0), UINT64_MAX - 1, 2, true, &node));
}

TEST(FsvmTest, ProbesShareALimit) {
    // 4 at once, then one per 5 s, for all disks together (a device that comes and goes in a loop).
    ResetProbes();
    int64_t wait = 0;
    for (int i = 0; i < 4; i++) EXPECT_TRUE(TakeProbe(1000, &wait)) << i;
    EXPECT_FALSE(TakeProbe(1000, &wait));
    EXPECT_EQ(5000, wait);
    EXPECT_FALSE(TakeProbe(5999, &wait));
    EXPECT_EQ(1, wait);
    EXPECT_TRUE(TakeProbe(6000, &wait));
    EXPECT_FALSE(TakeProbe(6000, &wait));
    // Two periods later two more, never more than 4 after a long pause.
    EXPECT_TRUE(TakeProbe(16000, &wait));
    EXPECT_TRUE(TakeProbe(16000, &wait));
    EXPECT_FALSE(TakeProbe(16000, &wait));
    for (int i = 0; i < 4; i++) EXPECT_TRUE(TakeProbe(100000, &wait)) << i;
    EXPECT_FALSE(TakeProbe(100000, &wait));
    ResetProbes();
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
