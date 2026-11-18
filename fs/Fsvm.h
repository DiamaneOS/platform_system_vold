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
};

struct Layout {
    std::string table; /* "none", "mbr" or "gpt" */
    std::vector<Part> parts;
};

/* Has the filesystem VM read the partition table of the disk at `diskPath`. */
status_t Probe(const std::string& diskPath, uint64_t diskBytes, Layout* out);

/* Parses and checks fsvmd's probe output (exposed for vold_tests). */
status_t ParseLayout(const std::string& text, uint64_t diskBytes, Layout* out);

struct Volume {
    android::base::unique_fd fd; /* the whole disk, or the OBB file */
    bool obb = false;
    bool readOnly = false;
    uint64_t diskBytes = 0;
    Part part; /* unused for an OBB */
    gid_t gid = 0;
    std::string volumeId;
    std::string target;
};

/*
 * Serves `v` at `v.target` through FUSE from the filesystem VM. Returns OK once the VM serves it, with fsvmd's pid
 * in `pid`. On any failure nothing stays mounted: there is no fallback to Android's kernel.
 */
status_t Mount(Volume&& v, pid_t* pid);

/* Stops fsvmd after its FUSE mount is gone. */
void Stop(pid_t pid);

/* Counts a serving fsvmd that ended (Mount counts the ones that start). */
void Served(bool obb, bool starting);

/*
 * Kills fsvmd `pid` unless it is cancelled within `ms`: the steps of a mount after the VM served the volume go through
 * fsvmd, and vold holds its lock until they end.
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
