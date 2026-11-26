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

#include "FsvmTable.h"

#include <errno.h>
#include <string.h>

#include <random>

namespace android {
namespace vold {
namespace fsvm {

/*
 * A small model of what sgdisk does for vold's two commands on a zapped disk (external/gptfdisk gpt.cc, gptcl.cc,
 * basicmbr.cc, mbr.cc, mbrpart.cc, support.cc): a fresh GPT of 128 entries, partitions made one after the other with
 * `--new=0:0:<end>` from the first sector of the largest free space, aligned to 1 MiB, and for a public disk the
 * conversion of partition 1 to an MBR. Only those steps, with sgdisk's own rules for each of them.
 */

namespace {

constexpr uint64_t kMiB = 1 << 20;
constexpr uint32_t kEntries = 128;
constexpr uint32_t kEntrySize = 128;
/* sgdisk's CHS geometry when the disk reports none (an image file): 255 heads, 63 sectors, 1024 cylinders */
constexpr uint64_t kHeads = 255;
constexpr uint64_t kSecsPerTrack = 63;
constexpr uint64_t kCylinders = 1024;

/* Partition type GUIDs as vold names them (Disk.cpp kGptBasicData, kGptAndroidMeta, kGptAndroidExpand). */
constexpr char kBasicData[] = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
constexpr char kAndroidMeta[] = "19A710A2-B3CA-11E4-B026-10604B889DCF";
constexpr char kAndroidExpand[] = "193D1EA4-B3CA-11E4-B075-10604B889DCF";

uint32_t Crc32(const uint8_t* p, size_t n) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

void Put16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = v >> 8;
}

void Put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

void Put64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (v >> (8 * i)) & 0xFF;
}

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* The GPT of a zapped disk while sgdisk makes partitions on it (GPTData). */
struct Gpt {
    uint64_t firstUsable = 0;
    uint64_t lastUsable = 0;
    uint64_t align = 0;
    std::vector<TablePart> parts;

    bool isFree(uint64_t s) const {
        for (const auto& p : parts) {
            if (s >= p.first && s <= p.last) return false;
        }
        return s >= firstUsable && s <= lastUsable;
    }

    /* GPTData::FindFirstAvailable: 0 when nothing is free from `start` on */
    uint64_t firstAvailable(uint64_t start) const {
        uint64_t first = start < firstUsable ? firstUsable : start;
        bool moved;
        do {
            moved = false;
            for (const auto& p : parts) {
                if (first >= p.first && first <= p.last) {
                    first = p.last + 1;
                    moved = true;
                }
            }
        } while (moved);
        return first > lastUsable ? 0 : first;
    }

    /* GPTData::FindLastInFree, without end alignment (sgdisk's default) */
    uint64_t lastInFree(uint64_t start) const {
        uint64_t nearest = lastUsable;
        for (const auto& p : parts) {
            if (nearest > p.first && p.first > start) nearest = p.first - 1;
        }
        return nearest;
    }

    /* GPTData::FindFirstInLargest */
    uint64_t firstInLargest() const {
        uint64_t start = 0, selectedSize = 0, selected = 0, first;
        do {
            first = firstAvailable(start);
            if (first != 0) {
                uint64_t last = lastInFree(first);
                uint64_t size = last - first + 1;
                if (size > selectedSize) {
                    selectedSize = size;
                    selected = first;
                }
                start = last + 1;
            }
        } while (first != 0);
        return selected;
    }

    /* GPTData::Align: to the alignment before `s` when everything from there is free, else after it */
    void alignSector(uint64_t* s) const {
        if (*s % align == 0) return;
        const uint64_t earlier = *s / align * align, later = earlier + align;
        bool ok = false;
        if (earlier >= firstUsable) {
            uint64_t t = earlier;
            do {
                ok = isFree(t++);
            } while (ok && t < *s);
            if (ok) {
                *s = earlier;
                return;
            }
        }
        if (later <= lastUsable) {
            uint64_t t = later;
            do {
                ok = isFree(t--);
            } while (ok && t > *s);
            if (ok) *s = later;
        }
    }

    /*
     * `--new=0:0:+<mib>M` (mib > 0), or `--new=0:0:-0` (mib 0): from the first sector of the largest free space,
     * aligned, to the given size or to the end of that space (gptcl.cc 'n', support.cc IeeeToInt,
     * GPTData::CreatePartition). False when sgdisk refuses it.
     */
    bool add(uint64_t mib, uint32_t blockSize) {
        uint64_t low = firstInLargest();
        alignSector(&low);
        const uint64_t high = lastInFree(low);
        uint64_t start = low, end = high;
        if (mib > 0) {
            const uint64_t mult = kMiB / blockSize;
            if (mib > UINT64_MAX / mult) return false;
            const uint64_t sectors = mib * mult;
            // IeeeToInt: the end's default is `high` itself, so "+" counts from the start, one sector less.
            if (sectors - 1 > UINT64_MAX - start) return false;
            end = start + sectors - 1;
        }
        alignSector(&start);
        if (!isFree(start) || start > end || lastInFree(start) < end) return false;
        parts.push_back({start, end});
        return true;
    }
};

/* MBRPart::LBAtoCHS with sgdisk's default geometry. */
void Chs(uint32_t lba, uint8_t* chs) {
    if (lba == 0) {
        chs[0] = chs[1] = chs[2] = 0;
        return;
    }
    if (lba >= kHeads * kSecsPerTrack * kCylinders) {
        chs[0] = 254;
        chs[1] = chs[2] = 255;
        return;
    }
    const uint64_t cylinder = lba / (kHeads * kSecsPerTrack);
    const uint64_t rest = lba - cylinder * kHeads * kSecsPerTrack;
    const uint64_t head = rest / kSecsPerTrack, sector = rest - head * kSecsPerTrack;
    chs[0] = static_cast<uint8_t>(head);
    chs[1] = static_cast<uint8_t>((sector + 1) + (cylinder >> 8) * 64);
    chs[2] = static_cast<uint8_t>(cylinder & 0xFF);
}

bool ChsValid(uint32_t lba) {
    return lba < kHeads * kSecsPerTrack * kCylinders;
}

/* One MBR entry (MBRPart::SetLocation, StoreInStruct): status 0. */
void MbrEntry(uint8_t* e, uint8_t type, uint32_t first, uint32_t length, bool protective, uint64_t diskSectors) {
    e[0] = 0;
    Chs(first, e + 1);
    e[4] = type;
    Chs(first + length - 1, e + 5);
    Put32(e + 8, first);
    Put32(e + 12, length);
    // A whole-disk 0xEE entry past the CHS range ends at 0xFFFFFF, as the GPT spec asks (MBRPart::SetLocation).
    if (protective && !(ChsValid(first) && ChsValid(first + length - 1)) && first == 1 &&
        (length == diskSectors - 1 || length == UINT32_MAX)) {
        e[5] = e[6] = e[7] = 0xFF;
    }
}

void PutName(uint8_t* p, const char* name) {
    for (size_t i = 0; name[i] && i < 36; i++) Put16(p + 2 * i, static_cast<uint8_t>(name[i]));
}

struct Entry {
    const char* type;
    const char* name;
};

}  // namespace

bool ParseGuid(const std::string& text, Guid* out) {
    std::string hex;
    if (text.size() == 36) {
        for (size_t i = 0; i < 36; i++) {
            const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
            if (dash != (text[i] == '-')) return false;
            if (!dash) hex.push_back(text[i]);
        }
    } else if (text.size() == 32) {
        hex = text;
    } else {
        return false;
    }
    uint8_t b[16];
    for (int i = 0; i < 16; i++) {
        const int hi = Hex(hex[2 * i]), lo = Hex(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        b[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    // The first three fields are stored little-endian (guid.cc).
    static constexpr int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    for (int i = 0; i < 16; i++) (*out)[i] = b[kOrder[i]];
    return true;
}

Guid RandomGuid() {
    // libc++ draws it from the kernel's random source (getrandom), as libuuid's uuid_generate does for sgdisk.
    std::random_device rd;
    uint8_t b[16];
    for (int i = 0; i < 16; i += 4) {
        const uint32_t v = rd();
        for (int k = 0; k < 4; k++) b[i + k] = static_cast<uint8_t>(v >> (8 * k));
    }
    // In text order: version 4, variant 1, then stored as GPT stores it.
    b[6] = (b[6] & 0x0F) | 0x40;
    b[8] = (b[8] & 0x3F) | 0x80;
    static constexpr int kOrder[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    Guid g;
    for (int i = 0; i < 16; i++) g[i] = b[kOrder[i]];
    return g;
}

status_t BuildTable(const TableRequest& r, Table* out) {
    *out = Table();
    const uint32_t bs = r.blockSize;
    if ((bs != 512 && bs != 4096) || r.diskBytes < 2 * kMiB || r.diskBytes % bs) return -EINVAL;
    const uint64_t n = r.diskBytes / bs;
    const uint64_t tableSectors = (kEntries * kEntrySize + bs - 1) / bs;
    if (n <= tableSectors * 2 + 3) return -EINVAL;
    Gpt gpt;
    gpt.firstUsable = 2 + tableSectors;
    gpt.lastUsable = n - gpt.firstUsable;
    gpt.align = 2048 * 512 / bs;

    std::vector<Entry> entries;
    switch (r.kind) {
        case TableKind::kPublic:
            if (!gpt.add(0, bs)) return -EINVAL;
            break;
        case TableKind::kMixed:
        case TableKind::kPrivate: {
            if (r.kind == TableKind::kMixed) {
                if (r.ratio < 10 || r.ratio > 90) return -EINVAL;
                // Disk::partitionMixed: whole MiB of the share.
                const uint64_t splitMib = ((r.diskBytes / 100) * r.ratio) / 1024 / 1024;
                if (!gpt.add(splitMib, bs)) return -EINVAL;
                entries.push_back({kBasicData, "shared"});
            }
            if (!gpt.add(16, bs) || !gpt.add(0, bs)) return -EINVAL;
            entries.push_back({kAndroidMeta, "android_meta"});
            entries.push_back({kAndroidExpand, "android_expand"});
            if (r.partGuids.size() != entries.size()) return -EINVAL;
            break;
        }
    }

    out->head.assign(kMiB, 0);
    out->tail.assign(kMiB, 0);
    out->tailOffset = r.diskBytes - kMiB;
    out->parts = gpt.parts;
    uint8_t* mbr = out->head.data();
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    if (r.kind == TableKind::kPublic) {
        // --gpttombr=1: partition 1 as the MBR's first entry, type 0x0C (from --typecode=0:0c00), and the GPT gone.
        // A partition an MBR cannot hold (past 2^32 sectors) fails the conversion, and sgdisk keeps the GPT with it
        // as a basic data partition without a name.
        const TablePart& p = gpt.parts[0];
        if (p.last <= UINT32_MAX) {
            MbrEntry(mbr + 446, 0x0C, static_cast<uint32_t>(p.first), static_cast<uint32_t>(p.last - p.first + 1),
                     false, n);
            return OK;
        }
        entries.push_back({kBasicData, ""});
        if (r.partGuids.size() != entries.size()) return -EINVAL;
    }

    // The protective MBR (MBRData::MakeProtectiveMBR).
    MbrEntry(mbr + 446, 0xEE, 1, n - 1 < UINT32_MAX ? static_cast<uint32_t>(n - 1) : UINT32_MAX, true, n);

    // The entries, the same at both ends.
    std::vector<uint8_t> table(kEntries * kEntrySize, 0);
    for (size_t i = 0; i < entries.size(); i++) {
        uint8_t* e = table.data() + i * kEntrySize;
        Guid type;
        ParseGuid(entries[i].type, &type);
        memcpy(e, type.data(), 16);
        memcpy(e + 16, r.partGuids[i].data(), 16);
        Put64(e + 32, gpt.parts[i].first);
        Put64(e + 40, gpt.parts[i].last);
        PutName(e + 56, entries[i].name);
    }
    const uint32_t tableCrc = Crc32(table.data(), table.size());

    // The headers (GPTData::ClearGPTData, RebuildSecondHeader, RecomputeCRCs): the backup's entries follow the last
    // usable sector.
    auto header = [&](uint64_t current, uint64_t backup, uint64_t entriesLba) {
        std::vector<uint8_t> h(512, 0);
        memcpy(h.data(), "EFI PART", 8);
        Put32(&h[8], 0x00010000);
        Put32(&h[12], 92);
        Put64(&h[24], current);
        Put64(&h[32], backup);
        Put64(&h[40], gpt.firstUsable);
        Put64(&h[48], gpt.lastUsable);
        memcpy(&h[56], r.diskGuid.data(), 16);
        Put64(&h[72], entriesLba);
        Put32(&h[80], kEntries);
        Put32(&h[84], kEntrySize);
        Put32(&h[88], tableCrc);
        Put32(&h[16], Crc32(h.data(), 92));
        return h;
    };
    auto place = [&](uint64_t lba, const uint8_t* data, size_t size) {
        const uint64_t at = lba * bs;
        if (at + size <= kMiB) {
            memcpy(out->head.data() + at, data, size);
        } else {
            memcpy(out->tail.data() + (at - out->tailOffset), data, size);
        }
    };
    const uint64_t backupEntries = gpt.lastUsable + 1;
    // Every place lies in the first or the last MiB: the entries take 16 KiB, the disk at least 2 MiB.
    if ((2 + tableSectors) * bs > kMiB || backupEntries * bs < out->tailOffset) return -EINVAL;
    auto mainHeader = header(1, n - 1, 2);
    auto backupHeader = header(n - 1, 1, backupEntries);
    place(1, mainHeader.data(), mainHeader.size());
    place(2, table.data(), table.size());
    place(backupEntries, table.data(), table.size());
    place(n - 1, backupHeader.data(), backupHeader.size());
    return OK;
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
