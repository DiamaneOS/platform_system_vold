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
#include <cutils/multiuser.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/Errors.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

/*
 * DiamaneOS: a hook in an upstream vold file. Its code is compiled only with the build switch VOLD_PUBLIC_FS_IN_VM
 * (Android.bp, the device's fs_in_vm), so that each hook stays one marked line.
 */
#ifdef VOLD_PUBLIC_FS_IN_VM
#define DIAMANEOS_FS_IN_VM(...) __VA_ARGS__
#else
#define DIAMANEOS_FS_IN_VM(...)
#endif

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
    /* Only the placeholder of a disk whose probe waits for a protected VM place: it says so, and is never mounted */
    bool waitingForPlace = false;
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
    /* The VM found a FAT or exFAT boot sector that fails its checks: could not be opened, never formatted */
    bool damaged = false;
    /*
     * Two probes in a row, each from its own VM, answered the same that this partition holds no filesystem the VM
     * can read (a new card, ext4, NTFS). Only such a partition is offered for formatting.
     */
    bool noFsConfirmed = false;
    /* The disk's medium sequence (/sys/block/<disk>/diskseq) the probe that found this saw, empty when the kernel has
     * none: a range, a mount or a format of it is refused on another medium (SameMedium) */
    std::string diskSeq;
};

struct Layout {
    std::string table; /* "none", "mbr" or "gpt" */
    std::vector<Part> parts;
    /* The medium sequence and size of the disk the probe read (set by Disk): two answers are the same only for the
     * same medium */
    std::string diskSeq;
    uint64_t diskBytes = 0;
};

/* Has the filesystem VM read the partition table of the disk at `diskPath`. A `timeoutMs` shorter than the default
 * (the check after a format) ends the probe early. */
status_t Probe(const std::string& diskPath, uint64_t diskBytes, Layout* out, int timeoutMs = 0);

/* Whether `disk` still holds the medium whose sequence number (diskseq) a probe saw as `seq`. An empty `seq` (a
 * kernel without diskseq) always matches. */
bool SameMedium(dev_t disk, const std::string& seq);

/*
 * Parses and checks fsvmd's probe output (exposed for vold_tests). `blockSize`: the disk's logical block size, which
 * sets how much room a GPT takes at either end (34 and 33 sectors for 512-byte blocks, 48 and 40 for 4096).
 */
status_t ParseLayout(const std::string& text, uint64_t diskBytes, Layout* out, uint32_t blockSize = 512);

/* The logical block size of a block device (512 or 4096), or 0. */
uint32_t BlockSize(int fd);

/*
 * Whether a probe's answer would offer the user a format: a public partition the VM read and found no filesystem it
 * can read on (not a damaged FAT or exFAT), or nothing on the disk Android can use. Such an answer counts only when a
 * second probe, from a new VM, gives the same answer (SameLayout).
 */
bool OffersFormat(const Layout& l);
/* Whether two probes of a disk gave the same answer, every field of every partition. */
bool SameLayout(const Layout& a, const Layout& b);

/* One VM for all public partitions of an SD card or USB drive (FsvmDrive.h). */
class Drive;

struct Volume {
    android::base::unique_fd fd; /* the whole disk, or the OBB file */
    bool obb = false;
    bool readOnly = false;
    uint64_t diskBytes = 0;
    uint32_t blockSize = 512;
    Part part; /* unused for an OBB */
    gid_t gid = 0;
    /* The owner every file and folder shows: MediaProvider's uid, so that it may set their times (stock's vfat and
     * exFAT let group media_rw do that, allow_utime), or root when it is not known */
    uid_t ownerUid = 0;
    std::string volumeId;
    std::string target;
};

/* MediaProvider's uid in `user`, from the package list, or 0 when the list does not name it. */
uid_t MediaProviderUid(userid_t user);

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

/* Kills fsvmd at once, with no write back (its volume's medium is gone), and reaps it. */
void Kill(pid_t pid);

/* fsvmd's exit status when a repair did not leave the volume clean (fsvmd main.rs EXIT_NOT_REPAIRED) */
static constexpr int kExitNotRepaired = 4;
/* fsvmd's exit status when its VM was refused at its start for want of memory: no pool room and the pieces left of
 * the hypervisor's budget too few (fsvmd main.rs EXIT_NO_ROOM) */
static constexpr int kExitNoRoom = 5;
/*
 * Starts a repair of `v` (a card's volume: its range, or the disk for a whole-disk filesystem) in a filesystem VM with
 * no time limit, and returns fsvmd's pid without waiting. Kill stops it at any time.
 */
pid_t StartRepair(Volume&& v);
/* Waits for repair `pid` to end, without a time limit: never with vold's lock held. 0 when the volume is clean now. */
int WaitRepair(pid_t pid);

/* Asks fsvmd to write its volume back and end, with the full time for it, without waiting: several volumes of a disk
 * that goes write back together, and each Stop then finds its fsvmd done. */
void RequestWriteBack(pid_t pid);

/* Takes one of the probes all disks share (4 at once, then one per 5 s). When none is left, says in `waitMs` when the
 * next one comes. vold's lock must be held. */
bool TakeProbe(int64_t nowMs, int64_t* waitMs);
/* For tests: all probes free again. */
void ResetProbes();

/*
 * What a disk remembers of its last partition read (Disk::readPartitionsInVm): a change event without a change reads
 * nothing, and change events in a burst are folded into one read.
 */
class ProbeMemory {
  public:
    enum class Next {
        kRead,  /* read the table now */
        kSkip,  /* nothing changed since the last read: the volumes it made stay */
        kLater, /* a change too soon after the last read: one read, 3 s later */
    };
    Next next(const std::string& seq, uint64_t size, int64_t nowMs, bool forced, bool justPartitioned,
              bool failed) const;
    /* A read of medium `seq` of `size` bytes starts now. True when the last read saw another medium. */
    bool took(const std::string& seq, uint64_t size, int64_t nowMs);
    /* Whether the last read saw this medium, true before any read. */
    bool holds(const std::string& seq, uint64_t size) const;
    /* The disk's volumes went without a new medium (vold reset): the next read makes them again. */
    void forget() { mProbed = false; }

  private:
    bool mProbed = false;
    std::string mSeq;
    uint64_t mSize = 0;
    int64_t mAtMs = 0;
};

/*
 * Room for protected VMs: pool memory, or pieces of the hypervisor's budget for ordinary memory (Fsvm.cpp RoomFor).
 * stopObbForDrive stops one OBB image's VM for a drive and says whether it did. onPlaceFree runs, without vold's lock,
 * whenever one of vold's VMs ended. A start without room fails with EAGAIN, and so does a probe, format or mount whose
 * VM the kernel or the hypervisor refused at its start (kExitNoRoom).
 */
void SetPlaceHooks(std::function<bool()> stopObbForDrive, std::function<void()> onPlaceFree);
size_t PlacesInUse();
/* Whether a drive waits for room: an OBB image's VM does not start meanwhile. */
void SetDrivesWaiting(bool waiting);
/* Whether a VM of `mib` MiB would find room now, read from the pool and the piece count. */
bool RoomForVm(uint64_t mib);
/* The VM size vold admits a start by, the same fsvmd gives that VM: a probe's (64 MiB), an OBB image's from its size
 * (ObbVmMib), a repair's first VM (RepairVmMib), a volume's smallest serving VM (96 MiB), a drive's (DriveVmMib). */
static constexpr uint64_t kProbeVmMib = 64;
static constexpr uint64_t kServeVmMib = 96;
uint64_t ObbVmMib(uint64_t diskBytes);
uint64_t RepairVmMib(const std::string& fsType, uint64_t metaBytes);

/* Counts a serving fsvmd that ended (Mount counts the ones that start). */

void Served(bool obb, bool starting);

/*
 * Stops fsvmd `pid` (a stop, then a kill 2 s later) unless it is cancelled within `ms`: the steps of a mount after the
 * VM served the volume, and of an unmount before fsvmd stops, go through fsvmd, and vold holds its lock until they
 * end. A drive's partition is guarded with `stuck` instead (Drive::stuck): its FUSE connection is aborted, which
 * fails the steps at once and leaves the drive's other partitions served.
 */
class Guard {
  public:
    Guard(pid_t pid, int ms);
    Guard(std::function<void()> stuck, int ms);
    ~Guard();

  private:
    struct State;
    std::shared_ptr<State> mState;
};

/*
 * A device-mapper range of a disk whose table Android's kernel does not read: a dm-linear device over exactly sectors
 * `start` to `start + length` of `disk`, the range the VM read. A partition's VM, its format and adoptable storage get
 * such a range instead of the whole disk, so one partition's VM cannot reach another. The kernel reads no table on
 * it (device-mapper disks carry GENHD_FL_NO_PART). Returns vold's node for it (/dev/block/vold/<name>, labelled
 * vold_device like the disk's) in `node` and its device in `dev`. Refused unless `disk` holds the medium `seq` the
 * range was read from, before and after it is set up (SameMedium).
 */
std::string RangeName(dev_t disk, uint32_t index);
status_t CreateRange(const std::string& name, dev_t disk, uint64_t start, uint64_t length, bool readOnly,
                     std::string* node, dev_t* dev = nullptr, const std::string& seq = "");
/* Removes a range and its node. Never waits: a range a VM still holds is removed by the kernel once it is closed
 * (deferred removal). */
status_t DeleteRange(const std::string& name, const std::string& node);
/* Removes every range of `disk` left from before (vold restarted, a medium changed). */
void DeleteRangesOf(dev_t disk);
/* Turns every range of `disk` into one that fails all I/O: the medium under them changed, and nothing written for the
 * old one (a VM's write back, a private volume's unmount) may reach the new one at the old one's offsets. */
void FailRangesOf(dev_t disk);
/* Removes the ranges of disks that are gone (unplugged while a VM still held a range). */
void SweepRanges();

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
