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
 * DiamaneOS: public volumes the filesystem VM serves (fs/Fsvm.cpp). Kept apart from PublicVolume.cpp, which calls
 * these from short hooks, so that upstream changes there merge without touching this code.
 */

#include "PublicVolume.h"

#include "Disk.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "fs/Exfat.h"
#include "fs/Fsvm.h"
#include "fs/Vfat.h"

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <android-base/stringprintf.h>
#include <private/android_filesystem_config.h>
#include <utils/Timers.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <thread>

using android::base::StringPrintf;

namespace android {
namespace vold {

PublicVolume::PublicVolume(dev_t diskDevice, const std::string& diskDevPath, const std::string& diskSysPath,
                           uint64_t diskBytes, const fsvm::Part& part)
    : VolumeBase(Type::kPublic), mDevice(diskDevice) {
    // The ID the kernel's partition device had, as before phase 2.
    setId(StringPrintf("public:%u,%u", major(diskDevice), minor(diskDevice) + part.index));
    mDevPath = diskDevPath;
    mFuseMounted = false;
    mUseSdcardFs = IsSdcardfsUsed();
    mInVm = true;
    mPart = part;
    mDiskDevPath = diskDevPath;
    mDiskSysPath = diskSysPath;
    mDiskBytes = diskBytes;
}

status_t PublicVolume::readMetadataInVm() {
    // What the filesystem VM read, checked by fsvmd. Nothing on Android parses the volume.
    mFsType = mPart.fsType;
    mFsUuid = mPart.fsUuid;
    mFsLabel = mPart.label;
    auto listener = getListener();
    if (listener) listener->onVolumeMetadataChanged(getId(), mFsType, mFsUuid, mFsLabel);
    return OK;
}

status_t PublicVolume::destroyInVm() {
    cancelRepairInVm();
    removeRange();
    return OK;
}

pid_t PublicVolume::startRepairInVm(pid_t servingPid) {
    if (servingPid != mFsvmdPid || getState() != State::kMounted || mRepairPid > 0) return -1;
    // Nothing to write back: the volume was read-only since its mount.
    unmount();
    showChecking();
    fsvm::Volume v;
    std::string path;
    const bool range = mPart.index > 0;
    if (createRange(false, &path) != OK) {
        showUnmounted();
        return -1;
    }
    v.fd.reset(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_CLOEXEC)));
    v.blockSize = v.fd >= 0 ? fsvm::BlockSize(v.fd.get()) : 0;
    if (v.fd < 0 || !v.blockSize || (!range && !fsvm::SameMedium(mDevice, mPart.diskSeq))) {
        LOG(ERROR) << getId() << ": no disk for the repair";
        removeRange();
        showUnmounted();
        return -1;
    }
    v.part = mPart;
    if (range) {
        // The partition alone, as a disk of its own, as for the mount (serveInVm).
        v.diskBytes = mPart.length * 512;
        v.part.index = 0;
        v.part.start = 0;
    } else {
        v.diskBytes = mDiskBytes;
    }
    v.gid = AID_MEDIA_RW;
    v.volumeId = getId();
    mRepairPid = fsvm::StartRepair(std::move(v));
    if (mRepairPid <= 0) {
        mRepairPid = -1;
        removeRange();
        showUnmounted();
        return -1;
    }
    return mRepairPid;
}

void PublicVolume::endRepairInVm(pid_t repairPid, int rc) {
    // Stopped meanwhile (an eject or removal): nothing more.
    if (repairPid != mRepairPid) return;
    mRepairPid = -1;
    removeRange();
    if (rc != 0) {
        LOG(ERROR) << getId() << ": the repair in the filesystem VM failed (" << rc << "), it stays read-only";
        mRepairFailed = true;
    } else {
        LOG(INFO) << getId() << ": repaired in the filesystem VM, mounting it again";
    }
    showUnmounted();
    mount();
}

bool PublicVolume::cancelRepairInVm() {
    if (mRepairPid <= 0) return false;
    LOG(WARNING) << getId() << ": the repair stops, the volume keeps its dirty mark";
    pid_t pid = mRepairPid;
    mRepairPid = -1;
    fsvm::Kill(pid);
    removeRange();
    showUnmounted();
    return true;
}

PublicVolume::UnmountInVm::UnmountInVm(PublicVolume* vol) : mVol(vol) {
    if (mVol->mFsvmdPid > 0) mStuck = std::make_unique<fsvm::Guard>(mVol->mFsvmdPid, 15000);
    VolumeManager::Instance()->unmountObbsBelow(mVol->getInternalPath());
}

PublicVolume::UnmountInVm::~UnmountInVm() {
    mStuck.reset();
    mVol->stopInVm();
}

void PublicVolume::UnmountInVm::fuseGone() {
    mStuck.reset();
    mVol->stopInVm();
}

status_t PublicVolume::createRange(bool readOnly, std::string* path) {
    if (mPart.index == 0) {
        // A filesystem on the whole disk: the disk holds nothing else.
        *path = mDiskDevPath;
        return OK;
    }
    // A range left from before (its VM still held it when it was to go) is removed first.
    if (!mRangeName.empty()) removeRange();
    // The VM, fsck and the format tools get exactly this partition's sectors, as a device-mapper range of the disk.
    // Android's kernel reads nothing from it and adds no partition to the disk.
    std::string name = fsvm::RangeName(mDevice, mPart.index);
    status_t res = fsvm::CreateRange(name, mDevice, mPart.start, mPart.length, readOnly, &mRangeNode, nullptr,
                                     mPart.diskSeq);
    if (res != OK) return res;
    mRangeName = name;
    *path = mRangeNode;
    return OK;
}

void PublicVolume::removeRange() {
    if (mRangeName.empty()) return;
    if (fsvm::DeleteRange(mRangeName, mRangeNode) != OK) {
        LOG(WARNING) << getId() << ": range " << mRangeName << " still in use, left in place";
    }
    mRangeName.clear();
    mRangeNode.clear();
}

status_t PublicVolume::checkInVm() {
    mVmFailed = false;
    if (mInVm) {
        // The filesystem check runs in the VM, after the mount and outside vold's lock (fsvmd fs.rs Checker), with the
        // fsck_msdos and fsck.exfat vold ran here before. Not with the same arguments: the VM checks a volume only
        // when it is marked dirty, and serves it read-only until the check passed.
        if (mPart.unread) {
            LOG(ERROR) << getId() << ": the filesystem VM could not read this disk";
            mVmFailed = true;
            return -EIO;
        }
        if (mPart.damaged) {
            // A FAT or exFAT the VM found damaged: could not be opened, never offered for formatting.
            LOG(ERROR) << getId() << ": the filesystem VM found a damaged FAT or exFAT boot sector";
            mVmFailed = true;
            return -EIO;
        }
        if (mFsType != "vfat" && mFsType != "exfat") {
            if (mPart.noFsConfirmed) {
                // Two VMs in a row read the volume and found no filesystem they can read: unmountable, with
                // Android's own format offer, as stock.
                LOG(ERROR) << getId() << ": the filesystem VM found no FAT or exFAT filesystem, twice";
                return -EIO;
            }
            LOG(ERROR) << getId() << ": no filesystem, not confirmed by a second probe";
            mVmFailed = true;
            return -EIO;
        }
    } else if (mFsType == "vfat") {
        // A volume on a partition device of its own (a virtual disk): fsck runs here, in its own sandbox
        // (fsck_untrusted), as stock.
        if (vfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else if (mFsType == "exfat") {
        if (exfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        mVmFailed = true;
        return -EIO;
    }
    // From here every failure of the mount is the VM path's (a mount point left by an ended fsvmd, the VM start,
    // FUSE): the volume could not be opened, and nothing offers to format it.
    mVmFailed = true;
    return OK;
}

void PublicVolume::onFsvmdExit(pid_t pid) {
    if (pid != mFsvmdPid || getState() != State::kMounted) return;
    LOG(ERROR) << getId() << ": fsvmd ended on its own, the volume could not be served any more";
    unmount();
    setUnopenable();
}

void PublicVolume::onCheckDone(pid_t pid, char state) {
    if (pid != mFsvmdPid || getState() != State::kMounted) return;
    // The volume was reported read-only while the VM checked it (Mount, c): writable once the check passed.
    switch (state) {
        case 'W':
            LOG(INFO) << getId() << ": checked in the filesystem VM, writable now";
            announceReadOnly(false);
            break;
        case 'c':
            LOG(WARNING) << getId() << ": a new filesystem VM checks the volume again, read-only meanwhile";
            announceReadOnly(true);
            break;
        case 'E': {
            if (mRepairFailed) {
                // A repair failed for this volume before: read-only, with SystemUI's read-only notice.
                LOG(WARNING) << getId() << ": the errors need a repair, which failed before: it stays read-only";
                announceReadOnly(true);
                break;
            }
            LOG(WARNING) << getId() << ": the errors need a longer repair, the volume is taken from apps for it";
            // Outside the Watch thread and vold's lock: the repair has no time limit.
            std::thread([id = getId(), pid] {
                std::unique_lock<std::mutex> lock(VolumeManager::Instance()->getLock());
                auto vol = VolumeManager::Instance()->findVolume(id);
                if (!vol || vol->getType() != Type::kPublic) return;
                pid_t repair = static_cast<PublicVolume*>(vol.get())->startRepairInVm(pid);
                if (repair <= 0) return;
                lock.unlock();
                int rc = fsvm::WaitRepair(repair);
                lock.lock();
                vol = VolumeManager::Instance()->findVolume(id);
                if (vol && vol->getType() == Type::kPublic) {
                    static_cast<PublicVolume*>(vol.get())->endRepairInVm(repair, rc);
                }
            }).detach();
            break;
        }
        default:
            // A check that failed keeps the volume read-only, with SystemUI's read-only notice.
            LOG(WARNING) << getId() << ": the check in the filesystem VM did not pass, it stays read-only";
            announceReadOnly(true);
            break;
    }
}

void PublicVolume::dropMedium() {
    // A card swapped in a USB reader: a write back would reach the new card at the old one's offsets. The disk has
    // cut the ranges off already (fsvm::FailRangesOf), a whole-disk volume's VM holds the disk itself.
    if (mFsvmdPid <= 0) return;
    LOG(WARNING) << getId() << ": the medium changed, its VM stops without writing back";
    fsvm::Kill(mFsvmdPid);
    fsvm::Served(false, false);
    mFsvmdPid = -1;
    mWriteBackFailed = true;
}

status_t PublicVolume::serveInVm(gid_t gid, bool* servedReadOnly) {
    fsvm::Volume v;
    std::string path = mInVm ? mDiskDevPath : mDevPath;
    // A card with its write-protect switch on, as the kernel mount handled it: served read-only, through a
    // read-only range.
    bool readOnly = false;
    if (mInVm) {
        android::base::unique_fd probe(TEMP_FAILURE_RETRY(open(mDiskDevPath.c_str(), O_RDWR | O_CLOEXEC)));
        readOnly = probe < 0 && (errno == EROFS || errno == EACCES);
    }
    // A partition: its VM gets exactly its range, never the whole disk, so it cannot reach another partition or the
    // adoptable storage next to it.
    const bool range = mInVm && mPart.index > 0;
    if (range) {
        status_t res = createRange(readOnly, &path);
        if (res != OK) {
            LOG(ERROR) << getId() << ": no range of its own (" << res << ")";
            return res;
        }
    }
    v.fd.reset(TEMP_FAILURE_RETRY(open(path.c_str(), (readOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC)));
    if (v.fd < 0 && !readOnly && (errno == EROFS || errno == EACCES)) {
        readOnly = true;
        v.fd.reset(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    }
    v.readOnly = readOnly;
    if (v.fd < 0) {
        int e = errno;
        PLOG(ERROR) << getId() << ": opening " << path;
        if (range) removeRange();
        return -e;
    }
    v.blockSize = fsvm::BlockSize(v.fd.get());
    if (!v.blockSize) {
        if (range) removeRange();
        return -EINVAL;
    }
    // A whole-disk volume's VM gets the disk itself: only while it holds the medium the probe read (a range checks
    // that as it is set up).
    if (mInVm && !range && !fsvm::SameMedium(mDevice, mPart.diskSeq)) {
        LOG(ERROR) << getId() << ": the medium changed since it was read, not serving it";
        return -ENODEV;
    }
    if (range) {
        // The partition alone, as a disk of its own: the filesystem starts at its first sector.
        v.diskBytes = mPart.length * 512;
        v.part = mPart;
        v.part.index = 0;
        v.part.start = 0;
    } else if (mInVm) {
        v.part = mPart;
        v.diskBytes = mDiskBytes;
    } else {
        // A volume on a partition device of its own (a virtual disk): the VM sees it as a whole disk.
        if (GetBlockDevSize(v.fd.get(), &v.diskBytes) != OK || v.diskBytes < 512) return -EIO;
        v.diskBytes &= ~511ULL;
        v.part.index = 0;
        v.part.start = 0;
        v.part.length = v.diskBytes / 512;
        v.part.fsType = mFsType;
    }
    v.gid = gid;
    // MediaProvider of the user the volume is mounted for owns its files: it sets their times when an app's write
    // through /storage closes, which stock's FAT and exFAT drivers let group media_rw do (allow_utime).
    v.ownerUid = fsvm::MediaProviderUid(getMountUserId());
    v.volumeId = getId();
    v.target = mRawPath;
    status_t res = fsvm::Mount(std::move(v), &mFsvmdPid, servedReadOnly);
    if (res != OK && range) removeRange();
    return res;
}

status_t PublicVolume::mountInVm(gid_t gid) {
    mMountStartMs = systemTime(SYSTEM_TIME_BOOTTIME) / 1000000;
    mWriteBackFailed = false;
    bool servedReadOnly = false;
    if (serveInVm(gid, &servedReadOnly) != OK) {
        LOG(ERROR) << getId() << " was not served by the filesystem VM";
        rmdir(mRawPath.c_str());
        return -EIO;
    }
    // Served read-only by the VM (a write-protected card, or its driver found an error): the user gets a notice. A
    // volume marked dirty is served read-only while the VM checks it after the mount, and reported so until the
    // check passed (onCheckDone).
    if (servedReadOnly) LOG(WARNING) << getId() << ": served read-only by the filesystem VM";
    setReportReadOnly(servedReadOnly);
    return OK;
}

int PublicVolume::guardMs() const {
    // The steps after the VM served the volume (MediaProvider's FUSE, which may have to start first after boot) get
    // whatever the VM left of 52 s, at least 15 s: a quick VM mount leaves them more. vold holds its lock throughout.
    int64_t spent = systemTime(SYSTEM_TIME_BOOTTIME) / 1000000 - mMountStartMs;
    return static_cast<int>(std::max<int64_t>(15000, 52000 - spent));
}

void PublicVolume::stopInVm() {
    if (mFsvmdPid > 0) {
        // Its FUSE mount is gone: fsvmd writes the volume back and unmounts it in the VM before it ends. When that
        // failed, or fsvmd had to be killed, the volume is not reported as safely unmounted.
        int rc = fsvm::Stop(mFsvmdPid, true);
        if (rc != 0) {
            LOG(ERROR) << getId() << ": not everything may have been written back (" << rc << ")";
            mWriteBackFailed = true;
        }
        fsvm::Served(false, false);
    }
    mFsvmdPid = -1;
    removeRange();
}

status_t PublicVolume::formatInVm(const std::string& fsType) {
    if (mPart.unread || mPart.damaged) {
        LOG(ERROR) << getId() << ": the filesystem VM could not read this volume, not formatting it";
        return -EIO;
    }
    // Both are formatted in the VM, whatever mkfs tools Android has.
    bool isVfatSup = true;
    bool isExfatSup = true;
    std::string pick;
    if (fsType == "auto") {
        // As for kernel mounted cards: exFAT for SDXC (>~32GiB) cards when both are supported.
        if (isVfatSup && isExfatSup) {
            pick = mPart.length * 512 > 32896ULL * 1024 * 1024 ? "exfat" : "vfat";
        } else if (isExfatSup) {
            pick = "exfat";
        } else if (isVfatSup) {
            pick = "vfat";
        }
    } else if ((fsType == "vfat" && isVfatSup) || (fsType == "exfat" && isExfatSup)) {
        pick = fsType;
    }
    if (pick.empty()) {
        LOG(ERROR) << "Unsupported filesystem " << fsType;
        return -EINVAL;
    }
    std::string dev;
    status_t res = createRange(false, &dev);
    if (res != OK) {
        LOG(ERROR) << getId() << " has no range to format";
        return res;
    }
    if (WipeBlockDevice(dev) != OK) {
        LOG(WARNING) << getId() << " failed to wipe";
    }
    // In the VM, as everything else that touches the volume's filesystem: no mkfs runs on Android.
    res = fsvm::Format(dev, pick, mDevice, mPart.diskSeq);
    removeRange();
    if (res != OK) {
        LOG(ERROR) << getId() << " failed to format";
        return -EIO;
    }
    // The new filesystem has a new serial, so the UUID changes. The VM reads it again, as blkid did at mount. If it
    // cannot say, the volume keeps the type just written (fsvmd checks it at mount) and has no UUID until then.
    mPart.fsType = pick;
    mPart.fsUuid.clear();
    mPart.label.clear();
    mPart.metaBytes = 0;
    mPart.noFsConfirmed = false;
    // One probe only, of at most 10 s: vold holds its lock for the wipe and the format already, and after a
    // partitioning for the probe of the new table too.
    fsvm::Layout layout;
    if (fsvm::Probe(mDiskDevPath, mDiskBytes, &layout, 10000) == OK) {
        for (const auto& p : layout.parts) {
            if (p.index == mPart.index && p.start == mPart.start && p.length == mPart.length && p.fsType == pick) {
                std::string seq = mPart.diskSeq;
                mPart = p;
                mPart.diskSeq = seq;
                return OK;
            }
        }
    }
    LOG(WARNING) << getId() << ": the filesystem VM did not confirm the new " << pick << " filesystem";
    return OK;
}

void PublicVolume::onMountFailed() {
    // A refused VM start (memory, right after boot) is the usual cause. Nothing was written to the volume, so its
    // disk tries again later, as for a failed probe. Not for a disk the VM could not read: the disk retries that.
    if (!mInVm || !mVmFailed || mPart.unread) return;
    auto disk = VolumeManager::Instance()->findDisk(getDiskId());
    if (disk) disk->retryMountLater();
}

void PublicVolume::doPostMount() {
    if (!mInVm) return;
    auto disk = VolumeManager::Instance()->findDisk(getDiskId());
    if (disk) disk->noteMounted();
}

void PublicVolume::prepareStop() {
    // The disk goes with all its volumes: each fsvmd starts writing back now, so that they do it at the same time and
    // each later stop finds its fsvmd done (vold's lock is held for all of them).
    if (getState() == State::kMounted && mFsvmdPid > 0) fsvm::RequestWriteBack(mFsvmdPid);
}

}  // namespace vold
}  // namespace android
