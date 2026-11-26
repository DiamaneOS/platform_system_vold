#!/usr/bin/env python3
# Copyright (C) 2026 DiamaneOS
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""What sgdisk writes for vold's partition requests, as tests/FsvmTableRef.inc (fs/FsvmTable.cpp's reference).

Runs sgdisk (the host build of external/gptfdisk) on sparse scratch image files only, never on a device: per case
--zap-all, then the command Disk::partitionPublic or Disk::partitionMixed runs, with fixed GUIDs (--disk-guid,
--partition-guid) in place of random ones. Records every sector sgdisk left non-zero, and checks it wrote nothing
outside the first and the last MiB. A 4096-byte logical block size comes from a preloaded library that answers the
block size ioctls for the image (fsvm_table_ref_bs.c, Linux) or from FSVM_TABLE_BS4096 for an sgdisk built to read it.

    fsvm_table_ref.py SGDISK SCRATCH_DIR OUT.inc [PRELOAD.so]
"""

import os
import subprocess
import sys

MIB = 1 << 20
DISK_GUID = "01020304-0506-4708-890A-0B0C0D0E0F10"
SHARED_GUID = "11111111-2222-4333-8444-555555555555"
META_GUID = "AAAAAAAA-BBBB-4CCC-8DDD-EEEEEEEEEEEE"
# The expand partition's GUID as vold passes it: StrToHex of 16 random bytes, lower case, no dashes.
EXPAND_GUID = "0123456789abcdef0fedcba987654321"
BASIC = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7"
META = "19A710A2-B3CA-11E4-B026-10604B889DCF"
EXPAND = "193D1EA4-B3CA-11E4-B075-10604B889DCF"

# (kind, logical block size, disk size in those blocks, ratio)
CASES = [
    ("public", 512, 4096, 0),
    ("public", 512, 8192, 0),
    ("public", 512, 131072, 0),
    ("public", 512, 1000001, 0),
    ("public", 512, 7744512, 0),
    ("public", 512, 16450560, 0),
    ("public", 512, 16450561, 0),
    ("public", 512, 62333952, 0),
    ("public", 512, 124735488, 0),
    ("public", 512, 4294967296, 0),
    ("public", 512, 4294969344, 0),
    ("private", 512, 34000, 0),
    ("private", 512, 36864, 0),
    ("private", 512, 131072, 0),
    ("private", 512, 1000001, 0),
    ("private", 512, 62333952, 0),
    ("private", 512, 4294969344, 0),
    ("mixed", 512, 8192, 10),
    ("mixed", 512, 131072, 10),
    ("mixed", 512, 131072, 50),
    ("mixed", 512, 1000001, 33),
    ("mixed", 512, 62333952, 10),
    ("mixed", 512, 62333952, 50),
    ("mixed", 512, 62333952, 90),
    ("mixed", 512, 124735488, 75),
    ("public", 4096, 512, 0),
    ("public", 4096, 32768, 0),
    ("public", 4096, 250001, 0),
    ("public", 4096, 7629312, 0),
    ("public", 4096, 1073741825, 0),
    ("private", 4096, 32768, 0),
    ("private", 4096, 7629312, 0),
    ("mixed", 4096, 7629312, 50),
    ("mixed", 4096, 250001, 20),
]


def command(kind, bs, sectors, ratio):
    if kind == "public":
        # The GUIDs before --gpttombr: after it they would ask sgdisk to save a GPT on what is an MBR disk by then.
        # Only a disk too large for an MBR keeps the GPT, and with it these GUIDs.
        return ["--new=0:0:-0", "--typecode=0:0c00", f"--partition-guid=0:{SHARED_GUID}", f"--disk-guid={DISK_GUID}",
                "--gpttombr=1"]
    cmd = []
    if kind == "mixed":
        split_mb = (((sectors * bs) // 100) * ratio) // 1024 // 1024
        cmd += [f"--new=0:0:+{split_mb}M", f"--typecode=0:{BASIC}", "--change-name=0:shared",
                f"--partition-guid=0:{SHARED_GUID}"]
    cmd += ["--new=0:0:+16M", f"--typecode=0:{META}", "--change-name=0:android_meta",
            f"--partition-guid=0:{META_GUID}"]
    cmd += ["--new=0:0:-0", f"--typecode=0:{EXPAND}", f"--partition-guid=0:{EXPAND_GUID}",
            "--change-name=0:android_expand"]
    return cmd + [f"--disk-guid={DISK_GUID}"]


def data_extents(f, size):
    """(start, end) of every stretch of the sparse file that holds data."""
    out, at = [], 0
    while at < size:
        try:
            start = os.lseek(f, at, os.SEEK_DATA)
        except OSError:
            break
        end = os.lseek(f, start, os.SEEK_HOLE)
        out.append((start, end))
        at = end
    return out


def run_case(sgdisk, scratch, preload, kind, bs, sectors, ratio):
    size = sectors * bs
    img = os.path.join(scratch, f"{kind}-{bs}-{sectors}-{ratio}.img")
    with open(img, "wb") as f:
        f.truncate(size)
    env = dict(os.environ)
    if bs == 4096:
        env["FSVM_TABLE_BS4096"] = img
        if preload:
            env["LD_PRELOAD"] = preload
    q = dict(stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    subprocess.run([sgdisk, "--zap-all", img], **q)
    rc = subprocess.run([sgdisk] + command(kind, bs, sectors, ratio) + [img], **q).returncode
    chunks = []
    if rc == 0:
        fd = os.open(img, os.O_RDONLY)
        try:
            # Data the file holds between the first and the last MiB must be zeros (holes are whole file system
            # blocks, so a write at either end can make a little of it data).
            for start, end in data_extents(fd, size):
                at, hi = max(start, MIB), min(end, size - MIB)
                while at < hi:
                    n = min(MIB, hi - at)
                    if any(os.pread(fd, n, at)):
                        sys.exit(f"{img}: sgdisk wrote outside the first and last MiB at {at}")
                    at += n
            for base in sorted({0, size - MIB}):
                block = os.pread(fd, MIB, base)
                for off in range(0, MIB, 512):
                    piece = block[off:off + 512].rstrip(b"\0")
                    if piece:
                        chunks.append((base + off, piece.hex()))
        finally:
            os.close(fd)
    os.unlink(img)
    return rc == 0, chunks


def main():
    sgdisk, scratch, out = sys.argv[1:4]
    preload = sys.argv[4] if len(sys.argv) > 4 else ""
    os.makedirs(scratch, exist_ok=True)
    lines = ["// Generated by tests/fsvm_table_ref.py from sgdisk's output on scratch image files. Do not edit.",
             f'static constexpr char kRefDiskGuid[] = "{DISK_GUID}";',
             f'static constexpr char kRefSharedGuid[] = "{SHARED_GUID}";',
             f'static constexpr char kRefMetaGuid[] = "{META_GUID}";',
             f'static constexpr char kRefExpandGuid[] = "{EXPAND_GUID}";']
    cases = []
    for n, (kind, bs, sectors, ratio) in enumerate(CASES):
        ok, chunks = run_case(sgdisk, scratch, preload, kind, bs, sectors, ratio)
        kname = {"public": "kPublic", "private": "kPrivate", "mixed": "kMixed"}[kind]
        if chunks:
            lines.append(f"static constexpr RefChunk kRef{n}[] = {{")
            for at, hexs in chunks:
                lines.append(f'    {{{at}ULL, "{hexs}"}},')
            lines.append("};")
            where = f"kRef{n}, {len(chunks)}"
        else:
            where = "nullptr, 0"
        cases.append(f"    {{TableKind::{kname}, {bs}, {sectors * bs}ULL, {ratio}, {'true' if ok else 'false'}, {where}}},")
        print(f"{kind} {bs} {sectors} {ratio}: {'ok' if ok else 'refused'}, {len(chunks)} sectors")
    lines.append("static constexpr RefCase kRefCases[] = {")
    lines += cases
    lines.append("};")
    with open(out, "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
