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

#ifndef ANDROID_VOLD_FSVM_H
#define ANDROID_VOLD_FSVM_H

#include <android-base/unique_fd.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/Errors.h>

#include <memory>
#include <string>
#include <vector>

namespace android {
namespace vold {
namespace fsvm {

/*
 * SD cards, USB drives and OBB images served from a protected VM (fsvmd, vendor/diamaneos/fsvm). Android's kernel
 * reads neither their partition tables nor their filesystems.
 */

/* A partition the filesystem VM read from a disk, as fsvmd checked it. Sectors are 512 bytes. */
struct Part {
    /* Only the placeholder of a disk the VM could not read: it says so, and is never mounted or formatted */
    bool unread = false;
    enum class Class { kPublic, kMeta, kExpand };
    uint32_t index = 0; /* 0: a filesystem on the whole disk */
    Class cls = Class::kPublic;
    uint64_t start = 0;
    uint64_t length = 0;
    std::string fsType;   /* "vfat", "exfat", or empty when the VM found neither */
    std::string fsUuid;   /* as blkid printed it, "6FFF-FAB8", or empty */
    std::string partGuid; /* GPT unique partition GUID as sgdisk printed it, or empty */
    std::string label;
    uint64_t metaBytes = 0; /* metadata the filesystem keeps in memory, sizes the VM */
    /*
     * The disk also holds adoptable storage (a mixed card): this public partition is served through a partition
     * device of its own, so the VM gets only its range and never the adoptable storage next to it.
     */
    bool ownRange = false;
};

struct Layout {
    std::string table; /* "none", "mbr" or "gpt" */
    std::vector<Part> parts;
};

/* Has the filesystem VM read the partition table of the disk at `diskPath`. A `timeoutMs` shorter than the default
 * (the check after a format) ends the probe early. */
status_t Probe(const std::string& diskPath, uint64_t diskBytes, Layout* out, int timeoutMs = 0);

/*
 * Parses and checks fsvmd's probe output (exposed for vold_tests). `blockSize`: the disk's logical block size, which
 * sets how much room a GPT takes at either end (34 and 33 sectors for 512-byte blocks, 48 and 40 for 4096).
 */
status_t ParseLayout(const std::string& text, uint64_t diskBytes, Layout* out, uint32_t blockSize = 512);

/* The logical block size of a block device (512 or 4096), or 0. */
uint32_t BlockSize(int fd);

struct Volume {
    android::base::unique_fd fd; /* the whole disk, or the OBB file */
    bool obb = false;
    bool readOnly = false;
    uint64_t diskBytes = 0;
    uint32_t blockSize = 512;
    Part part; /* unused for an OBB */
    gid_t gid = 0;
    std::string volumeId;
    std::string target;
};

/*
 * Serves `v` at `v.target` through FUSE from the filesystem VM. Returns OK once the VM serves it, with fsvmd's pid
 * in `pid`, and in `servedReadOnly` whether the VM served a volume asked for writing read-only (its check did not
 * finish in time, or its driver found an error). On any failure nothing stays mounted: there is no fallback to Android's kernel.
 */
status_t Mount(Volume&& v, pid_t* pid, bool* servedReadOnly = nullptr);

/*
 * Stops fsvmd. `orderly`: its FUSE mount is gone, and fsvmd writes the volume back and unmounts it in the VM before
 * it ends (up to kSyncMs, as long as the write back makes progress). Otherwise (a mount that failed, or a read-only
 * volume with nothing to write) it is stopped at once. Either way it is gone after kSyncMs plus 3 s at most. Returns
 * fsvmd's exit status (kExitNotWrittenBack when the write back failed), or -1 when it had to be killed.
 */
int Stop(pid_t pid, bool orderly);

/* fsvmd's exit status when it could not write the volume back before it ended (fsvmd main.rs EXIT_NOT_WRITTEN_BACK) */
static constexpr int kExitNotWrittenBack = 3;

/* Asks fsvmd to stop now, without waiting: Stop then reaps it. For stopping several at once. */
void RequestStop(pid_t pid);

/* Asks fsvmd to write its volume back and end, with the full time for it, without waiting: several volumes of a disk
 * that goes write back together, and each Stop then finds its fsvmd done. */
void RequestWriteBack(pid_t pid);

/* Takes one of the probes all disks share (4 at once, then one per 5 s). When none is left, says in `waitMs` when the
 * next one comes. vold's lock must be held. */
bool TakeProbe(int64_t nowMs, int64_t* waitMs);
/* For tests: all probes free again. */
void ResetProbes();

/* Counts a serving fsvmd that ended (Mount counts the ones that start). */
void Served(bool obb, bool starting);

/*
 * Stops fsvmd `pid` (a stop, then a kill 2 s later) unless it is cancelled within `ms`: the steps of a mount after the
 * VM served the volume, and of an unmount before fsvmd stops, go through fsvmd, and vold holds its lock until they
 * end.
 */
class Guard {
  public:
    Guard(pid_t pid, int ms);
    ~Guard();

  private:
    struct State;
    std::shared_ptr<State> mState;
};

/*
 * Adds partition `p` of a disk whose table Android's kernel does not read, at the range the VM read. Returns its
 * device in `dev`, and `added` true once the kernel has the partition.
 */
status_t AddPartition(const std::string& diskPath, const std::string& diskSysPath, const Part& p, dev_t* dev,
                      bool* added);
status_t DeletePartition(const std::string& diskPath, uint32_t index);
/*
 * Removes every partition Android's kernel has for the disk: ones vold added earlier, and on a kernel without
 * CONFIG_BLK_NO_EXTERNAL_PART_SCAN the ones it read itself. Fails while one is in use.
 */
status_t DeleteAllPartitions(const std::string& diskPath, const std::string& diskSysPath);

/* Zeroes the first and last MiB of a disk, where MBR and GPT tables live, before new ones are written. */
status_t ZeroTables(const std::string& diskPath, uint64_t diskBytes);

}  // namespace fsvm
}  // namespace vold
}  // namespace android

#endif
