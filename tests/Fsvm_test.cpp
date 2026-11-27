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

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <gtest/gtest.h>
#include <selinux/android.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <thread>

#include "../fs/Fsvm.h"
#include "../fs/FsvmDrive.h"
#include "../model/Disk.h"
#include "../model/PublicVolume.h"

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

TEST(FsvmTest, AdoptableStorageWithoutItsKeyIsNeverOfferedForFormatting) {
    Layout blank, adopted, mixed, fat;
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 none 0\nEND\n", kDisk, &blank));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 gpt 2\nPART 1 meta 2048 2048 - - 02020202-0202-0202-0202-020202020202 0 -\n"
                              "PART 2 expand 4096 100000 - - 03030303-0303-0303-0303-030303030303 0 -\nEND\n",
                              kDisk, &adopted));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 gpt 3\nPART 1 public 2048 4096 - - 01010101-0101-0101-0101-010101010101 0 -\n"
                              "PART 2 meta 6144 2048 - - 02020202-0202-0202-0202-020202020202 0 -\n"
                              "PART 3 expand 8192 100000 - - 03030303-0303-0303-0303-030303030303 0 -\nEND\n",
                              kDisk, &mixed));
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 1\nPART 1 public 2048 4096 vfat - - 0 -\nEND\n", kDisk, &fat));
    // Only a blank disk without any volume, read by a probe that worked, gets the offer.
    EXPECT_TRUE(OffersFormatOnScan(false, blank, false));
    EXPECT_FALSE(OffersFormatOnScan(true, blank, false));
    EXPECT_FALSE(OffersFormatOnScan(false, blank, true));
    // An adopted card whose key is missing: its volume could not be made, and still no offer, with or without the
    // volume that stands in for it.
    EXPECT_FALSE(OffersFormatOnScan(false, adopted, false));
    EXPECT_FALSE(OffersFormatOnScan(false, adopted, true));
    EXPECT_FALSE(OffersFormatOnScan(false, mixed, false));
    EXPECT_FALSE(OffersFormatOnScan(false, fat, false));
}

TEST(FsvmTest, NewAdoptableStorageIsRefusedBeforeAnythingIsWritten) {
    // No such device: whatever got past the refusal would fail on it, with another error than EPERM. A Disk makes its
    // device node, labelled as vold's main does.
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    ASSERT_NE(nullptr, sehandle);
    Disk disk("/devices/virtual/block/fsvm-test-none", makedev(179, 1048575), "test", Disk::Flags::kAdoptable);
    EXPECT_EQ(-EPERM, disk.partitionPrivate());
    EXPECT_EQ(-EPERM, disk.partitionMixed(50));
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

TEST(FsvmTest, ProbeMemory) {
    using Next = ProbeMemory::Next;
    ProbeMemory m;
    EXPECT_TRUE(m.holds("5", kDisk));
    EXPECT_EQ(Next::kRead, m.next("5", kDisk, 1000, false, false, false));
    EXPECT_FALSE(m.took("5", kDisk, 1000));
    // Change events without a change read nothing, soon or late.
    EXPECT_EQ(Next::kSkip, m.next("5", kDisk, 1500, false, false, false));
    EXPECT_EQ(Next::kSkip, m.next("5", kDisk, 60000, false, false, false));
    // Another medium or size: one read 3 s after the last, at once after that.
    EXPECT_EQ(Next::kLater, m.next("6", kDisk, 2000, false, false, false));
    EXPECT_EQ(Next::kLater, m.next("5", kDisk / 2, 2000, false, false, false));
    EXPECT_EQ(Next::kRead, m.next("6", kDisk, 4000, false, false, false));
    // A failed read, a new table and a retry vold scheduled read again.
    EXPECT_EQ(Next::kRead, m.next("5", kDisk, 60000, false, false, true));
    EXPECT_EQ(Next::kRead, m.next("5", kDisk, 1500, false, true, false));
    EXPECT_EQ(Next::kRead, m.next("5", kDisk, 1500, true, false, false));
    EXPECT_TRUE(m.took("6", kDisk, 4000));
    EXPECT_FALSE(m.holds("5", kDisk));
    // vold reset: the disk's volumes went with the same medium in place. Its next read makes them again, also right
    // after the last read.
    m.forget();
    EXPECT_EQ(Next::kRead, m.next("6", kDisk, 4100, false, false, false));
    EXPECT_FALSE(m.took("6", kDisk, 4100));
    EXPECT_EQ(Next::kSkip, m.next("6", kDisk, 4200, false, false, false));
}

static Part DrivePart(uint32_t index, const char* fs, uint64_t metaBytes) {
    Part p;
    p.index = index;
    p.start = 2048 * index;
    p.length = 2048;
    p.fsType = fs;
    p.metaBytes = metaBytes;
    return p;
}

// The same values as fsvm proto layout.rs drive_memory: fsvmd sizes the drive's VM, vold places it by this.
TEST(FsvmTest, DriveMemoryAsFsvmdSizesIt) {
    EXPECT_EQ(96u, DriveMemoryMib({DrivePart(1, "vfat", 1 << 20)}));
    EXPECT_EQ(144u, DriveMemoryMib({DrivePart(1, "vfat", 64 << 20)}));
    EXPECT_EQ(96u, DriveMemoryMib({}));
    EXPECT_EQ(113u, DriveMemoryMib({DrivePart(1, "vfat", 1 << 20), DrivePart(2, "vfat", 1 << 20)}));
    EXPECT_EQ(177u, DriveMemoryMib({DrivePart(1, "exfat", 1 << 20), DrivePart(2, "vfat", 64 << 20)}));
    EXPECT_EQ(113u, DriveMemoryMib({DrivePart(1, "vfat", 1 << 20), DrivePart(2, "", 0)}));
    EXPECT_EQ(112u, DriveMemoryMib({DrivePart(1, "", 0), DrivePart(2, "", 0)}));
    std::vector<Part> large(4, DrivePart(1, "vfat", 256ULL << 20));
    EXPECT_EQ(204u, DriveMemoryMib(large));
    std::vector<Part> huge(4, DrivePart(1, "exfat", UINT64_MAX));
    EXPECT_EQ(204u, DriveMemoryMib(huge));
}

// The drive's VM gets the public partitions Disk makes volumes of, never a damaged one, never more than its cap.
TEST(FsvmTest, DriveParts) {
    Layout l;
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 mbr 6\n"
                              "PART 1 public 2048 2048 vfat 1111-1111 - 0 -\n"
                              "PART 2 public 4096 2048 bad - - 0 -\n"
                              "PART 3 public 6144 2048 - - - 0 -\n"
                              "PART 5 public 8192 2048 exfat 2222-2222 - 0 -\n"
                              "PART 6 public 10240 2048 vfat 3333-3333 - 0 -\n"
                              "PART 7 public 12288 2048 vfat 4444-4444 - 0 -\n"
                              "END\n",
                              kDisk, &l));
    std::vector<Part> d = DriveParts(l, 64, 4);
    ASSERT_EQ(3u, d.size());
    EXPECT_EQ(1u, d[0].index);
    EXPECT_EQ(3u, d[1].index);
    EXPECT_EQ(5u, d[2].index);
    d = DriveParts(l, 3, 4);
    ASSERT_EQ(2u, d.size());
    EXPECT_EQ(3u, d[1].index);
    ASSERT_EQ(OK, ParseLayout("LAYOUT 1 none 1\nPART 0 public 0 2048 vfat 1111-1111 - 0 -\nEND\n", kDisk, &l));
    d = DriveParts(l, 64, 4);
    ASSERT_EQ(1u, d.size());
    EXPECT_EQ(0u, d[0].index);
}

TEST(FsvmTest, DriveAnswers) {
    uint32_t id = 0;
    char k = 0;
    int disk = -1, value = -1;
    std::string rest;
    EXPECT_TRUE(ParseDriveAnswer("1 K 0", &id, &k, &disk, &value, &rest));
    EXPECT_EQ(1u, id);
    EXPECT_EQ('K', k);
    EXPECT_EQ(0, value);
    EXPECT_TRUE(ParseDriveAnswer("4294967295 A 1 11", &id, &k, &disk, &value, &rest));
    EXPECT_EQ(UINT32_MAX, id);
    EXPECT_EQ('A', k);
    EXPECT_EQ(1, disk);
    EXPECT_EQ(11, value);
    EXPECT_TRUE(ParseDriveAnswer("7 D 0 3", &id, &k, &disk, &value, &rest));
    EXPECT_EQ(3, value);
    EXPECT_TRUE(ParseDriveAnswer("8 F 2 0\nLAYOUT 1 none 1\nEND\n", &id, &k, &disk, &value, &rest));
    EXPECT_EQ("LAYOUT 1 none 1\nEND\n", rest);
    EXPECT_TRUE(ParseDriveAnswer("9 F 2 4002", &id, &k, &disk, &value, &rest));
    EXPECT_EQ(kFormatUncertain, value);
    EXPECT_FALSE(ParseDriveAnswer("7 D 0 3\nmore", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 X 0 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 A 8 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 A -1 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 A 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 A 0 0 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 K 0 0", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("7 AA 0 0", &id, &k, &disk, &value, &rest));
    // Every answer names its request.
    EXPECT_FALSE(ParseDriveAnswer("A 1 11", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("0 A 1 11", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("01 A 1 11", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("4294967296 A 1 11", &id, &k, &disk, &value, &rest));
    EXPECT_FALSE(ParseDriveAnswer("+1 A 1 11", &id, &k, &disk, &value, &rest));
}

// M9: an answer that comes after vold stopped waiting for it never passes for the answer to a later request, and the
// partition the late request changes takes no new change until that answer came.
TEST(FsvmTest, LateDriveAnswersAreDropped) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    android::base::unique_fd peer(sv[1]);
    DriveControl c;
    c.reset(android::base::unique_fd(sv[0]));
    // A fake drive fsvmd: the first FORMAT is answered only when the second one comes, before the second's answer.
    std::thread fsvmd([&peer] {
        char buf[256];
        std::string first;
        for (;;) {
            ssize_t n = recv(peer.get(), buf, sizeof(buf), 0);
            if (n <= 0) return;
            std::string req(buf, n);
            std::string id = req.substr(0, req.find(' '));
            if (first.empty()) {
                first = id;
                continue;
            }
            std::string late = first + " F 0 0\nLAYOUT 1 none 1\nPART 0 public 0 2048 vfat 1111-1111 - 0 -\nEND\n";
            std::string now = id + " F 0 " + std::to_string(kFormatUncertain);
            send(peer.get(), late.data(), late.size(), 0);
            send(peer.get(), now.data(), now.size(), 0);
        }
    });
    int value = -1;
    std::string text;
    EXPECT_EQ(-ETIMEDOUT, c.call("FORMAT 0 vfat", {}, 'F', 0, true, 200, &value, &text));
    EXPECT_TRUE(c.pending(0));
    EXPECT_FALSE(c.pending(1));
    EXPECT_EQ(OK, c.call("FORMAT 0 vfat", {}, 'F', 0, true, 5000, &value, &text));
    // The second request's own answer, not the first one's late "formatted".
    EXPECT_EQ(kFormatUncertain, value);
    EXPECT_TRUE(text.empty());
    // The late answer settled the first request.
    EXPECT_FALSE(c.pending(0));
    c.reset();
    shutdown(peer.get(), SHUT_RDWR);
    fsvmd.join();
}

// A format's answer reads as the partition it formatted, which keeps its place and its medium.
TEST(FsvmTest, FormattedPart) {
    Part p = DrivePart(2, "", 0);
    p.diskSeq = "17";
    p.noFsConfirmed = true;
    Part out;
    ASSERT_EQ(OK, FormattedPart("LAYOUT 1 none 1\nPART 0 public 0 2048 vfat 6FFF-FAB8 - 4096 4b61727465\nEND\n", p,
                                &out));
    EXPECT_EQ(2u, out.index);
    EXPECT_EQ(4096u, out.start);
    EXPECT_EQ(2048u, out.length);
    EXPECT_EQ("vfat", out.fsType);
    EXPECT_EQ("6FFF-FAB8", out.fsUuid);
    EXPECT_EQ("Karte", out.label);
    EXPECT_EQ(4096u, out.metaBytes);
    EXPECT_EQ("17", out.diskSeq);
    EXPECT_FALSE(out.noFsConfirmed);
    // Another size, a table, nothing found, or a damaged filesystem: not the format's answer.
    EXPECT_NE(OK, FormattedPart("LAYOUT 1 none 1\nPART 0 public 0 1024 vfat 6FFF-FAB8 - 0 -\nEND\n", p, &out));
    EXPECT_NE(OK, FormattedPart("LAYOUT 1 mbr 1\nPART 1 public 0 2048 vfat 6FFF-FAB8 - 0 -\nEND\n", p, &out));
    EXPECT_NE(OK, FormattedPart("LAYOUT 1 none 0\nEND\n", p, &out));
    EXPECT_NE(OK, FormattedPart("LAYOUT 1 none 1\nPART 0 public 0 2048 bad - - 0 -\nEND\n", p, &out));
}

// A drive refuses what it does not serve, before any VM.
TEST(FsvmTest, DriveServesOnlyItsParts) {
    Drive d(makedev(4095, 1048575), "/dev/block/vold/fsvm-test-none", "disk:4095,1048575");
    d.setParts({DrivePart(1, "vfat", 0), DrivePart(2, "exfat", 0)});
    EXPECT_TRUE(d.serves(DrivePart(1, "vfat", 0)));
    EXPECT_TRUE(d.serves(DrivePart(2, "", 0)));
    EXPECT_FALSE(d.serves(DrivePart(3, "vfat", 0)));
    Part moved = DrivePart(1, "vfat", 0);
    moved.length++;
    EXPECT_FALSE(d.serves(moved));
    pid_t serving = 0;
    EXPECT_EQ(-EIO,
              d.attach(DrivePart(3, "vfat", 0), 1023, 0, "public:4095,3", "/nonexistent", false, &serving, nullptr));
    EXPECT_EQ(0, serving);
    EXPECT_EQ(0, d.detach(DrivePart(1, "vfat", 0), true));
    EXPECT_EQ(-1, d.startRepair(DrivePart(1, "vfat", 0)));
    EXPECT_FALSE(static_cast<bool>(d.stuck(DrivePart(1, "vfat", 0))));
}

// L3: a format whose outcome is not known for sure has the disk read again, and only a definite answer counts.
TEST(FsvmTest, FormatOutcomes) {
    Part p = DrivePart(1, "", 0);
    p.diskSeq = "5";
    p.noFsConfirmed = true;
    Part out;
    bool recheck = true;
    const std::string read = "LAYOUT 1 none 1\nPART 0 public 0 2048 vfat 6FFF-FAB8 - 4096 -\nEND\n";
    EXPECT_EQ(OK, FormatOutcome(OK, 0, read, p, "vfat", &out, &recheck));
    EXPECT_FALSE(recheck);
    EXPECT_EQ("6FFF-FAB8", out.fsUuid);
    // Formatted, not read back: done, with the type asked for and no UUID, and read again.
    EXPECT_EQ(OK, FormatOutcome(OK, kFormatUnverified, "", p, "exfat", &out, &recheck));
    EXPECT_TRUE(recheck);
    EXPECT_EQ("exfat", out.fsType);
    EXPECT_TRUE(out.fsUuid.empty());
    EXPECT_FALSE(out.noFsConfirmed);
    // Perhaps formatted: a failure, and read again.
    EXPECT_EQ(-EIO, FormatOutcome(OK, kFormatUncertain, "", p, "vfat", &out, &recheck));
    EXPECT_TRUE(recheck);
    EXPECT_EQ(-EIO, FormatOutcome(-ETIMEDOUT, -1, "", p, "vfat", &out, &recheck));
    EXPECT_TRUE(recheck);
    EXPECT_EQ(-EIO, FormatOutcome(-EPIPE, -1, "", p, "vfat", &out, &recheck));
    EXPECT_TRUE(recheck);
    // An answer that contradicts the request (another filesystem) is not a format that worked.
    EXPECT_EQ(-EIO, FormatOutcome(OK, 0, read, p, "exfat", &out, &recheck));
    EXPECT_TRUE(recheck);
    // Not formatted: no room, or the format tool failed.
    EXPECT_EQ(-EAGAIN, FormatOutcome(OK, EAGAIN, "", p, "vfat", &out, &recheck));
    EXPECT_FALSE(recheck);
    EXPECT_EQ(-EIO, FormatOutcome(OK, EIO, "", p, "vfat", &out, &recheck));
    EXPECT_TRUE(recheck);
}

// B2: a format refuses a medium it cannot name as the one it read, before anything opens the disk (no discard, no VM
// start), also with no sequence known.
TEST(FsvmTest, FormatTouchesNothingOnAnUnknownMedium) {
    EXPECT_FALSE(IsMedium(makedev(179, 0), ""));
    EXPECT_FALSE(IsMedium(makedev(4095, 1048575), "7"));
    TemporaryDir dir;
    const std::string disk = std::string(dir.path) + "/disk";
    ASSERT_TRUE(android::base::WriteStringToFile("x", disk));
    android::base::unique_fd watch(inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    ASSERT_GE(watch.get(), 0);
    ASSERT_GE(inotify_add_watch(watch.get(), disk.c_str(), IN_OPEN | IN_MODIFY), 0);
    for (const char* seq : {"", "7"}) {
        Drive d(makedev(4095, 1048575), disk, "disk:4095,1048575", 64ULL << 30);
        Part p = DrivePart(0, "vfat", 0);
        p.start = 0;
        p.diskSeq = seq;
        d.setParts({p});
        Part out;
        bool recheck = true;
        bool writerStopped = true;
        EXPECT_EQ(-ENODEV, d.format(p, "vfat", &out, &recheck, &writerStopped))
                << "sequence '" << seq << "'";
        EXPECT_FALSE(recheck);
        EXPECT_FALSE(writerStopped);
    }
    char buf[4096];
    EXPECT_LE(read(watch.get(), buf, sizeof(buf)), 0) << "the disk was opened";
}

// M8: vold admits each VM by the size fsvmd gives it (fsvm proto layout.rs, fsvmd main.rs): a drive by the smallest
// serving VM fsvmd falls back to, an OBB image by its size, a repair by its first VM.
TEST(FsvmTest, AdmissionSizesAsFsvmd) {
    EXPECT_EQ(std::vector<uint64_t>({96}), DriveVmMib({DrivePart(1, "vfat", 1 << 20)}));
    EXPECT_EQ(std::vector<uint64_t>({144, 96}), DriveVmMib({DrivePart(1, "vfat", 64 << 20)}));
    std::vector<Part> large(4, DrivePart(1, "vfat", 256ULL << 20));
    EXPECT_EQ(std::vector<uint64_t>({204, 96}), DriveVmMib(large));
    EXPECT_EQ(64u, ObbVmMib(2ULL << 30));
    EXPECT_EQ(96u, ObbVmMib((2ULL << 30) + 512));
    // Every VM of vold's fits beside the decode service's VM: 512 MiB pool, 298 MiB of it the decode VM's, 10 for
    // this VM's firmware region and bounce buffers.
    EXPECT_EQ(96u, RepairVmMib("vfat", 0));
    EXPECT_EQ(204u, RepairVmMib("exfat", 0));
    EXPECT_EQ(204u, RepairVmMib("", 0));
    EXPECT_EQ(144u, RepairVmMib("vfat", 64 << 20));
    EXPECT_EQ(96u, RepairVmMib("vfat", 1 << 20));
    EXPECT_EQ(145u, RepairVmMib("exfat", 65 << 20));
    EXPECT_EQ(204u, RepairVmMib("exfat", UINT64_MAX));
    EXPECT_EQ(204u, RepairVmMib("vfat", UINT64_MAX));
}

// H4: a partition's ID and node come only from the disk's own minors, and a node left for another device goes.
TEST(FsvmTest, PartitionIdsStayOnTheirDisk) {
    dev_t d = 0;
    EXPECT_TRUE(PartitionDevice(makedev(8, 96), 15, 15, &d));
    EXPECT_EQ(makedev(8, 111), d);
    EXPECT_FALSE(PartitionDevice(makedev(8, 96), 16, 15, &d));
    EXPECT_FALSE(PartitionDevice(makedev(8, 96), 20, 15, &d));
    EXPECT_FALSE(PartitionDevice(makedev(8, 96), 0, 15, &d));
    EXPECT_FALSE(PartitionDevice(makedev(179, 0), 1, 0, &d));
    EXPECT_FALSE(PartitionDevice(makedev(8, 0xFFFFF), 1, 15, &d));
    TemporaryDir dir;
    const std::string node = std::string(dir.path) + "/private:8,97";
    EXPECT_EQ(OK, ClaimNode(node, makedev(8, 97)));
    ASSERT_TRUE(android::base::WriteStringToFile("x", node));
    EXPECT_EQ(OK, ClaimNode(node, makedev(8, 97)));
    EXPECT_NE(0, access(node.c_str(), F_OK));
}

// H2: vold never mounts an app-visible volume itself: without the framework's callback MediaProvider would never get
// its FUSE session. Such a mount is refused before any mount point is made, and the volume is not opened (no format
// offer).
TEST(FsvmTest, NoVisibleMountWithoutTheFrameworkCallback) {
    Part p = DrivePart(1, "vfat", 0);
    p.fsUuid = "1B2C-3D4E";
    PublicVolume vol(makedev(4095, 1048575), "/dev/block/vold/disk:4095,1048575", "/sys/nonexistent", 64ULL << 30, p,
                     nullptr);
    vol.setSilent(true);
    ASSERT_EQ(OK, vol.setMountFlags(VolumeBase::MountFlags::kVisibleForWrite));
    EXPECT_NE(OK, vol.mount());
    EXPECT_EQ(VolumeBase::State::kUnopenable, vol.getState());
    EXPECT_TRUE(vol.getInternalPath().empty()) << vol.getInternalPath();
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
