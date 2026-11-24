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

/*
 * Fuzz target: vold's reading of fsvmd's probe report (fs/Fsvm.cpp ParseLayout and its helpers, compiled from the
 * file itself through the vold_fsvm_layout_parser rule). vold adds kernel partitions from these numbers, so whatever it
 * accepts must stay inside the disk and clear of the partition tables, without overlaps, with at most one adoptable
 * storage pair and printable labels. An input "FSVMD-OUTPUT <disk bytes> <block size> <hash>\n" followed by the
 * report, the hash the FNV-1a of "<disk bytes> <block size>\n" and the report, is one fsvmd wrote from a layout it
 * accepted (vendor/diamaneos fsvm/fuzz): vold must accept it, with the same partitions (a mutated report no longer
 * matches its hash). Any other input is eight bytes of disk size, a byte for the block size, then the report.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>

#include <android-base/parseint.h>
#include <android-base/strings.h>

#include "fs/Fsvm.h"

namespace android {
namespace vold {
namespace fsvm {
using android::base::ParseUint;
using android::base::Split;
#include "FsvmLayoutParser.inc"
}  // namespace fsvm
}  // namespace vold
}  // namespace android

using android::vold::fsvm::Layout;
using android::vold::fsvm::ParseLayout;
using android::vold::fsvm::Part;

/* A broken property: aborts in the fuzzer, recorded by the corpus test. */
void (*gFsvmLayoutBroken)(const char* what) = [](const char* what) {
    fprintf(stderr, "vold_fsvm_layout_fuzzer: %s\n", what);
    abort();
};

static void Check(bool ok, const char* what) {
    if (!ok) gFsvmLayoutBroken(what);
}

static bool Hex(const std::string& s, size_t from, size_t n) {
    for (size_t i = from; i < from + n; i++) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'A' && s[i] <= 'F'))) return false;
    }
    return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string in(reinterpret_cast<const char*>(data), size);
    uint64_t diskBytes = 0;
    uint32_t blockSize = 512;
    std::string text;
    bool fromFsvmd = false;
    if (in.rfind("FSVMD-OUTPUT ", 0) == 0) {
        size_t nl = in.find('\n');
        uint64_t hash = 0;
        if (nl == std::string::npos || sscanf(in.substr(0, nl).c_str(), "FSVMD-OUTPUT %" SCNu64 " %" SCNu32 " %" SCNx64,
                                              &diskBytes, &blockSize, &hash) != 3) {
            return 0;
        }
        text = in.substr(nl + 1);
        uint64_t h = 0xcbf29ce484222325ULL;
        for (unsigned char c : std::to_string(diskBytes) + " " + std::to_string(blockSize) + "\n" + text) {
            h = (h ^ c) * 0x100000001b3ULL;
        }
        fromFsvmd = h == hash;
    } else {
        if (size < 9) return 0;
        memcpy(&diskBytes, data, 8);
        if (data[8] & 2) diskBytes &= ~uint64_t{511};
        if (data[8] & 4) diskBytes %= uint64_t{1} << 44;
        blockSize = (data[8] & 1) ? 4096 : 512;
        text = in.substr(9);
    }
    Layout l;
    bool ok = ParseLayout(text, diskBytes, &l, blockSize) == android::OK;
    if (fromFsvmd) Check(ok, "a report fsvmd wrote from a layout it accepted was refused");
    if (!ok) return 0;

    Check(diskBytes % 512 == 0, "a disk size that is not whole sectors");
    const uint64_t sectors = diskBytes / 512;
    const uint64_t entryBlocks = (128 * 128 + blockSize - 1) / blockSize;
    const uint64_t head = (2 + entryBlocks) * (blockSize / 512), tail = (1 + entryBlocks) * (blockSize / 512);
    Check(l.table == "none" || l.table == "mbr" || l.table == "gpt", "table");
    Check(l.parts.size() <= 16, "more than 16 partitions");
    int meta = 0, expand = 0;
    for (size_t i = 0; i < l.parts.size(); i++) {
        const Part& p = l.parts[i];
        Check(p.length >= 1 && p.start <= sectors && p.length <= sectors - p.start, "a range past the disk");
        if (l.table == "none") {
            Check(p.index == 0, "a numbered entry without a table");
        } else {
            Check(p.index >= 1, "entry 0 in a table");
        }
        if (l.table == "mbr") Check(p.index <= 36, "an MBR entry above 36");
        if (l.table == "gpt") {
            Check(p.index <= 128, "a GPT entry above 128");
            Check(!p.partGuid.empty() && sectors >= head + tail && p.start >= head &&
                          p.start + p.length <= sectors - tail,
                  "a GPT entry over the tables or without its GUID");
        } else {
            Check(p.cls == Part::Class::kPublic && p.partGuid.empty(), "GPT fields without a GPT");
        }
        Check(p.fsType.empty() || p.fsType == "vfat" || p.fsType == "exfat", "filesystem type");
        Check(!p.damaged || p.fsType.empty(), "damaged with a filesystem");
        Check(p.cls == Part::Class::kPublic || (p.fsType.empty() && !p.damaged), "a filesystem on adoptable storage");
        Check(p.index != 0 || !p.fsType.empty() || p.damaged, "a whole-disk entry without a filesystem");
        meta += p.cls == Part::Class::kMeta;
        expand += p.cls == Part::Class::kExpand;
        Check(p.label.size() <= 47, "label length");
        for (unsigned char c : p.label) Check(c >= 0x20 && c != 0x7f, "a control character in a label");
        Check(p.fsUuid.empty() || (p.fsUuid.size() == 9 && Hex(p.fsUuid, 0, 4) && p.fsUuid[4] == '-' &&
                                   Hex(p.fsUuid, 5, 4)),
              "filesystem UUID");
        Check(p.partGuid.empty() || p.partGuid.size() == 36, "partition GUID");
        for (size_t k = 0; k < i; k++) {
            const Part& q = l.parts[k];
            Check(p.index != q.index, "two entries with one number");
            Check(p.start + p.length <= q.start || q.start + q.length <= p.start, "overlapping entries");
        }
    }
    Check(meta <= 1 && expand <= 1, "more than one adoptable storage pair");
    if (fromFsvmd) {
        size_t count = 0;
        Check(sscanf(text.c_str(), "LAYOUT 1 %*s %zu", &count) == 1 && count == l.parts.size(),
              "partitions lost between fsvmd and vold");
    }
    return 0;
}
