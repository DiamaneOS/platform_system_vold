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

#include <iterator>
#include <string>
#include <vector>

#include "../fs/FsvmTable.h"

namespace android {
namespace vold {
namespace fsvm {

namespace {

struct RefChunk {
    unsigned long long offset;
    const char* hex;
};

struct RefCase {
    TableKind kind;
    uint32_t blockSize;
    unsigned long long diskBytes;
    int ratio;
    bool ok;
    const RefChunk* chunks;
    size_t count;
};

// What sgdisk wrote for each request on a scratch image file (tests/fsvm_table_ref.py).
#include "FsvmTableRef.inc"

Guid G(const char* text) {
    Guid g;
    EXPECT_TRUE(ParseGuid(text, &g)) << text;
    return g;
}

TableRequest RequestOf(const RefCase& c) {
    TableRequest r;
    r.kind = c.kind;
    r.blockSize = c.blockSize;
    r.diskBytes = c.diskBytes;
    r.ratio = c.ratio;
    r.diskGuid = G(kRefDiskGuid);
    if (c.kind == TableKind::kPublic) r.partGuids = {G(kRefSharedGuid)};
    if (c.kind == TableKind::kMixed) r.partGuids.push_back(G(kRefSharedGuid));
    if (c.kind != TableKind::kPublic) {
        r.partGuids.push_back(G(kRefMetaGuid));
        r.partGuids.push_back(G(kRefExpandGuid));
    }
    return r;
}

std::string Name(const RefCase& c) {
    const char* k = c.kind == TableKind::kPublic ? "public" : c.kind == TableKind::kPrivate ? "private" : "mixed";
    return std::string(k) + " " + std::to_string(c.blockSize) + " " + std::to_string(c.diskBytes) + " " +
           std::to_string(c.ratio);
}

/* sgdisk's two ends of the disk: zeros with what it wrote. */
void Expected(const RefCase& c, std::vector<uint8_t>* head, std::vector<uint8_t>* tail, uint64_t tailOffset) {
    head->assign(1 << 20, 0);
    tail->assign(1 << 20, 0);
    for (size_t k = 0; k < c.count; k++) {
        const RefChunk& ch = c.chunks[k];
        const std::string hex = ch.hex;
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            const uint8_t b = static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16));
            const uint64_t at = ch.offset + i / 2;
            if (at < (1u << 20)) (*head)[at] = b;
            if (at >= tailOffset) (*tail)[at - tailOffset] = b;
        }
    }
}

/* The first differing byte of two ends, for the failure message. */
std::string FirstDifference(const std::vector<uint8_t>& want, const std::vector<uint8_t>& got, uint64_t base) {
    for (size_t i = 0; i < want.size() && i < got.size(); i++) {
        if (want[i] != got[i]) {
            return "byte " + std::to_string(base + i) + ": sgdisk " + std::to_string(want[i]) + ", vold " +
                   std::to_string(got[i]);
        }
    }
    return want.size() == got.size() ? "" : "sizes differ";
}

}  // namespace

// Every request vold may make gives the very bytes sgdisk wrote for it, and a request sgdisk refused is refused.
TEST(FsvmTableTest, SameBytesAsSgdisk) {
    ASSERT_GT(std::size(kRefCases), 20u);
    for (const auto& c : kRefCases) {
        Table t;
        status_t res = BuildTable(RequestOf(c), &t);
        if (!c.ok) {
            EXPECT_NE(OK, res) << Name(c) << ": sgdisk refused it";
            continue;
        }
        ASSERT_EQ(OK, res) << Name(c);
        EXPECT_EQ(c.diskBytes - (1 << 20), t.tailOffset) << Name(c);
        std::vector<uint8_t> head, tail;
        Expected(c, &head, &tail, t.tailOffset);
        EXPECT_EQ("", FirstDifference(head, t.head, 0)) << Name(c) << ", the first MiB";
        EXPECT_EQ("", FirstDifference(tail, t.tail, t.tailOffset)) << Name(c) << ", the last MiB";
    }
}

TEST(FsvmTableTest, RefusesWhatVoldRefuses) {
    TableRequest r;
    r.diskBytes = 64ULL << 30;
    r.kind = TableKind::kMixed;
    r.partGuids = {RandomGuid(), RandomGuid(), RandomGuid()};
    Table t;
    for (int ratio : {0, 9, 91, 100, -1}) {
        r.ratio = ratio;
        EXPECT_NE(OK, BuildTable(r, &t)) << ratio;
    }
    r.ratio = 50;
    EXPECT_EQ(OK, BuildTable(r, &t));
    r.blockSize = 1024;
    EXPECT_NE(OK, BuildTable(r, &t));
    r.blockSize = 4096;
    r.diskBytes = (64ULL << 30) + 512;
    EXPECT_NE(OK, BuildTable(r, &t));
    r.diskBytes = (1 << 20) + 4096;
    EXPECT_NE(OK, BuildTable(r, &t));
    // The GUIDs must be given, one per partition.
    r.diskBytes = 64ULL << 30;
    r.partGuids.pop_back();
    EXPECT_NE(OK, BuildTable(r, &t));
}

TEST(FsvmTableTest, Guids) {
    Guid a, b;
    ASSERT_TRUE(ParseGuid("0123456789ABCDEF0FEDCBA987654321", &a));
    ASSERT_TRUE(ParseGuid("01234567-89ab-cdef-0fed-cba987654321", &b));
    EXPECT_EQ(a, b);
    const Guid want = {0x67, 0x45, 0x23, 0x01, 0xab, 0x89, 0xef, 0xcd,
                       0x0f, 0xed, 0xcb, 0xa9, 0x87, 0x65, 0x43, 0x21};
    EXPECT_EQ(want, a);
    EXPECT_FALSE(ParseGuid("0123456789ABCDEF0FEDCBA98765432", &a));
    EXPECT_FALSE(ParseGuid("0123456789ABCDEF0FEDCBA98765432G", &a));
    EXPECT_FALSE(ParseGuid("01234567-89ab-cdef-0fed+cba987654321", &a));
    const Guid r1 = RandomGuid(), r2 = RandomGuid();
    EXPECT_NE(r1, r2);
    // Version 4, variant 1, as sgdisk's random GUIDs.
    EXPECT_EQ(0x40, r1[7] & 0xF0);
    EXPECT_EQ(0x80, r1[8] & 0xC0);
}

TEST(FsvmTableTest, ProtectiveLengthCountsOnlySectorsAfterLbaZero) {
    for (uint64_t sectors : {uint64_t(UINT32_MAX) - 1, uint64_t(UINT32_MAX), uint64_t(UINT32_MAX) + 1}) {
        TableRequest r;
        r.kind = TableKind::kPrivate;
        r.diskBytes = sectors * 512;
        r.partGuids = {RandomGuid(), RandomGuid()};
        Table table;
        ASSERT_EQ(OK, BuildTable(r, &table));
        const uint8_t* p = table.head.data() + 458;
        uint32_t length = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        EXPECT_EQ(std::min<uint64_t>(sectors - 1, UINT32_MAX), length);
    }
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
