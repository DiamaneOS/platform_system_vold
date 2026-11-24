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

#ifndef ANDROID_VOLD_FSVM_TABLE_H
#define ANDROID_VOLD_FSVM_TABLE_H

#include <stdint.h>
#include <utils/Errors.h>

#include <array>
#include <string>
#include <vector>

namespace android {
namespace vold {
namespace fsvm {

/*
 * The partition tables vold writes on an SD card or USB drive the filesystem VM serves, built here from the user's
 * request alone: no partition tool runs on Android and nothing is read from the medium. The bytes are the ones
 * sgdisk wrote for Disk::partitionPublic and Disk::partitionMixed after its --zap-all, on a disk that reports no CHS
 * geometry (255 heads, 63 sectors, as on an image file): the same types, GUIDs, names, alignment and sizes
 * (tests/FsvmTable_test.cpp compares them with sgdisk's output for the same requests).
 */

/* A GUID in the byte order GPT stores it. */
using Guid = std::array<uint8_t, 16>;

/* A GUID from its text: 32 hex digits, with or without the four dashes, read as sgdisk reads it. */
bool ParseGuid(const std::string& text, Guid* out);
/* A random (version 4) GUID, as sgdisk makes one for the disk and each new partition. */
Guid RandomGuid();

enum class TableKind {
    kPublic,  /* one FAT partition over the disk, MBR (sgdisk --new=0:0:-0 --typecode=0:0c00 --gpttombr=1) */
    kPrivate, /* GPT: android_meta of 16 MiB, android_expand over the rest */
    kMixed,   /* GPT: shared (basic data) of `ratio` percent, then as private */
};

struct TableRequest {
    TableKind kind = TableKind::kPublic;
    uint64_t diskBytes = 0;
    uint32_t blockSize = 512; /* the disk's logical block size, 512 or 4096 */
    int ratio = 0;            /* kMixed: 10 to 90 */
    Guid diskGuid = {};
    /* The partitions' unique GUIDs, in the order they are made: kPublic its one partition's (kept only on a disk an
     * MBR cannot describe, which keeps sgdisk's GPT), kMixed shared, then android_meta and android_expand (the
     * expand one is the GUID the volume key is saved under) */
    std::vector<Guid> partGuids;
};

/* One partition of a table built (sectors of the disk's logical block size). */
struct TablePart {
    uint64_t first = 0;
    uint64_t last = 0;
};

/*
 * The table for `r`: the sectors to write (`head`: the disk's first MiB, `tail`: its last MiB from `tailOffset`, both
 * whole, zeros where nothing else goes, as the old tables are zeroed first) and the partitions made. -EINVAL for a
 * request sgdisk refused (a ratio out of range, a disk too small or too large for the layout, a block size other than
 * 512 or 4096, a disk under 2 MiB).
 */
struct Table {
    std::vector<uint8_t> head;
    std::vector<uint8_t> tail;
    uint64_t tailOffset = 0;
    std::vector<TablePart> parts;
};
status_t BuildTable(const TableRequest& r, Table* out);

}  // namespace fsvm
}  // namespace vold
}  // namespace android

#endif
