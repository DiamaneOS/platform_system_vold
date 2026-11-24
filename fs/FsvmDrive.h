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

#ifndef ANDROID_VOLD_FSVM_DRIVE_H
#define ANDROID_VOLD_FSVM_DRIVE_H

#include "Fsvm.h"

#include <android-base/unique_fd.h>

#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace android {
namespace vold {
namespace fsvm {

/*
 * vold's end of a drive's control socket (fsvmd drive_msg.rs): each request starts with an ID its answer echoes, so an
 * answer vold stopped waiting for never passes for a later request's. A request that changes a partition (ATTACH,
 * DETACH, FORMAT, REPAIR, CANCEL) and got no answer in time stays pending for that partition until its late answer
 * comes (answers that came meanwhile are read before every later request) or the drive's fsvmd ends: meanwhile the
 * partition takes no request that would change it.
 */
class DriveControl {
  public:
    explicit DriveControl(uint32_t firstId = 1) : mNextId(firstId) {}
    /* A new fsvmd's socket, or none: what was pending goes with the old one. */
    void reset(android::base::unique_fd fd = {});
    bool connected() const { return mFd >= 0; }
    /*
     * Sends `msg` with `fds`, and waits up to `timeoutMs` for its answer: `kind`, `disk`, its value and for a format
     * the text after it. -ETIMEDOUT without an answer (pending for `disk` when `mutating`), -EPIPE when fsvmd is gone,
     * -EIO for an answer that is not one.
     */
    status_t call(const std::string& msg, const std::vector<int>& fds, char kind, int disk, bool mutating,
                  int timeoutMs, int* value, std::string* rest = nullptr);
    /* Reads the answers that came meanwhile, without waiting: late ones settle their requests. */
    void settle();
    /* Whether a request that changes partition `disk` is still pending. */
    bool pending(int disk) const;
    bool hasPending() const { return !mPending.empty(); }

  private:
    /* One answer within `timeoutMs` (0: only one that is there now). */
    status_t next(int timeoutMs, uint32_t* id, char* kind, int* disk, int* value, std::string* rest);

    android::base::unique_fd mFd;
    uint32_t mNextId = 1;
    /* Requests that change a partition, sent and not answered in time: ID and partition */
    struct Pending { char kind; int disk; };
    std::map<uint32_t, Pending> mPending;
};

/* fsvmd's FORMAT answers besides 0 and an errno (drive_msg.rs FORMAT_UNVERIFIED, FORMAT_UNCERTAIN). */
static constexpr int kFormatUnverified = 4001;
static constexpr int kFormatUncertain = 4002;
static constexpr int kFormatUncertainStopped = 4003;

/*
 * What a format's answer means for partition `p` formatted as `fsType` (Drive::format): OK with `out` the new
 * partition, or a failure. `recheck` is set when the disk must be read again because what is on it now is not known
 * for sure: the format tool succeeded but its filesystem was not read back (OK, the volume keeps the type asked for
 * and no UUID until then), or the request broke off or got an answer that contradicts it (a failure, perhaps
 * formatted).
 */
status_t FormatOutcome(status_t call, int value, const std::string& text, const Part& p, const std::string& fsType,
                       Part* out, bool* recheck);

/*
 * The VM sizes fsvmd tries for a drive serving `parts`, in its order (fsvmd drive.rs Drive::session): its own size
 * (DriveMemoryMib), then the smallest serving VM when that is smaller. vold admits a drive's VM by the last one.
 */
std::vector<uint64_t> DriveVmMib(const std::vector<Part>& parts);

/*
 * One VM for all public partitions of one SD card or USB drive (fsvmd --drive, vendor/diamaneos fsvm/fsvmd/src/
 * drive.rs). Each partition keeps a device-mapper range of its own, bound to the medium the probe read, and is a disk
 * of the VM, all given at the VM's start. Each has its own FUSE mount, its own session in the VM (a server of its own
 * there) and its own check, repair and read-only state, so one partition's dirty mark, check or broken session never
 * changes another's. The drive takes one fsvmd slot and is placed once, by the memory of all its partitions together
 * (DriveMemoryMib). A probe of the whole disk runs before (Disk), in a VM of its own that serves nothing. A format or
 * repair of a partition runs in this drive's VM, never in one that serves another drive.
 *
 * The drive's fsvmd starts with the first partition served, formatted or repaired, and ends when none is any more,
 * and at an eject, a removal or a vold reset (end), or at once when the medium changed (kill). All of it with vold's
 * lock held, except WaitDriveRepair.
 */
class Drive {
  public:
    /* `diskBytes`: the disk's size when it was read, which an erase checks it still has */
    Drive(dev_t disk, const std::string& diskPath, const std::string& diskId, uint64_t diskBytes = 0);
    ~Drive();
    Drive(const Drive&) = delete;
    Drive& operator=(const Drive&) = delete;

    /* The partitions the VM serves (DriveParts of the probe's answer), set before any is served. */
    void setParts(std::vector<Part> parts);
    /* Whether `p` is one of them. */
    bool serves(const Part& p) const;

    /*
     * Serves partition `p` through FUSE at `target` from the drive's VM, as Mount does for a volume of its own.
     * Returns OK once it is served, with the ID its Watch callbacks name in `serving` and whether it is served
     * read-only, and in `checking` whether only while the VM checks it. `readOnly` (a repair of it failed), or a drive
     * opened read-only (its write-protect switch), is read-only for good: FUSE is mounted read-only, fsvmd is asked for
     * it, and it is reported read-only whatever fsvmd answers. -EAGAIN when there is no room or no slot for the drive's VM (the volume waits), -EIO otherwise,
     * and nothing stays mounted.
     */
    status_t attach(const Part& p, gid_t gid, uid_t ownerUid, const std::string& volumeId, const std::string& target,
                    bool readOnly, pid_t* serving, bool* servedReadOnly, bool* checking = nullptr);
    /* Whether the drive's disk opened read-only (its write-protect switch) when its VM started */
    bool readOnly() const { return mReadOnly; }
    bool attached(const Part& p) const;
    /* Decide before any FUSE disconnect or detach. */
    status_t prepareDetach(const Part& p);
    bool hasBackings() const { return !mRanges.empty(); }
    status_t fenceBackings(bool all);
    /*
     * The partition's FUSE mount went (or a mount failed): it is written back and unmounted in the VM (`orderly`), or
     * ended at once. 0, kExitNotWrittenBack, or -1 when the drive's fsvmd had ended. The VM ends with the last one.
     */
    int detach(const Part& p, bool orderly);
    /*
     * Formats partition `p` in the drive's VM and reads the new filesystem into `out` (p with its fields). Refused
     * (-ENODEV) before anything is started or written unless the disk still holds the medium `p` was read from (a
     * known sequence) and has its size, checked again once the VM runs. `recheck`: FormatOutcome.
     */
    status_t format(const Part& p, const std::string& fsType, Part* out, bool* recheck,
                    bool* writerStopped);
    bool hasUnfinishedMutation();

    /* A repair keeps the VM while its partition is unmounted for it: before the unmount, and repairEnded after. */
    void holdForRepair();
    void repairEnded();
    /* Starts the repair of `p` with no time limit and returns its token for WaitDriveRepair, or -1. */
    pid_t startRepair(const Part& p);
    /* Refuse an unsafe repair before its preliminary unmount can write back. */
    bool prepareRepair(const Part& p);
    bool prepareFormat(const Part& p) { return prepareRepair(p); }
    /* An eject or removal during the repair stops it (the partition keeps its dirty mark), and repairEnded. */
    void cancelRepair(const Part& p);

    /* The drive goes with all its partitions: each is written back now, at once (SIGUSR1, as RequestWriteBack). */
    void requestWriteBack();
    /* Ends the drive's fsvmd and its VM, whatever is served (each partition's detach then answers -1). */
    void end();
    /* The medium changed: the drive's fsvmd is killed at once, nothing is written back. */
    void kill();
    /* What a Guard does when the steps around partition `p` hang: its FUSE connection is aborted. */
    std::function<void()> stuck(const Part& p) const;

  private:
    friend struct StorageTestAccess;
    int indexOf(const Part& p) const;
    status_t start();
    status_t request(const std::string& msg, const std::vector<int>& fds, char kind, int disk, bool mutating,
                     int timeoutMs, int* value, std::string* rest = nullptr);
    /* Whether partition `p` may be erased now: its medium, known, and the disk's size as read */
    bool mayErase(const Part& p) const;
    void gone();
    void endIfIdle();
    void removeRanges();

    dev_t mDisk;
    std::string mDiskPath;
    /* The disk's ID (disk:179,0): fsvmd --drive names it, so that `ps` shows which drive a VM serves */
    std::string mDiskId;
    std::vector<Part> mParts;
    /* Actual block descriptors passed to fsvmd, in VM slot order. */
    std::vector<RangeBacking> mRanges;
    uint64_t mDiskBytes = 0;
    bool mReadOnly = false;
    android::base::unique_fd mMediumFd;
    pid_t mPid = -1;
    DriveControl mControl;
    std::set<int> mAttached;
    /* Partitions served when the drive's fsvmd ended: their detach says they were not written back */
    std::set<int> mLost;
    /* The FUSE connection of each partition served (/sys/fs/fuse/connections/N) */
    std::map<int, unsigned> mFuseConn;
    /* Formats and repairs running */
    int mBusy = 0;
};

/* Waits for the repair `token` (Drive::startRepair) to end, without a time limit: never with vold's lock held. 0 when
 * the partition is clean now, -1 otherwise. */
int WaitDriveRepair(pid_t token);

/*
 * VM memory for one drive's VM serving `parts`, as fsvmd sizes it (fsvm proto layout.rs drive_memory_mib): the
 * largest partition's own serving VM, plus for each other one 16 MiB and its metadata share, 96 to 256 MiB.
 */
uint64_t DriveMemoryMib(const std::vector<Part>& parts);

/*
 * The partitions of a probe's answer `l` a drive's VM serves: the public ones numbered up to `maxIndex`, at most
 * `max` of them counted as Disk counts its volumes, without the damaged ones (never mounted or formatted).
 */
std::vector<Part> DriveParts(const Layout& l, uint32_t maxIndex, size_t max);

/* Parses one answer of a drive's fsvmd: its request ID, then `K <value>`, or `<kind> <disk> <value>` and for a format
 * the probe text after a newline (in `rest`). False when it is neither. */
bool ParseDriveAnswer(const std::string& msg, uint32_t* id, char* kind, int* disk, int* value, std::string* rest);

/* Reads a format's probe text (the partition `p` as a disk of its own) into `out`: `p` with the new filesystem. */
status_t FormattedPart(const std::string& text, const Part& p, Part* out);

}  // namespace fsvm
}  // namespace vold
}  // namespace android

#endif
