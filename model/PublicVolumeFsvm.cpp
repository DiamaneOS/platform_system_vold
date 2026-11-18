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
#include <android-base/stringprintf.h>
#include <utils/Timers.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>

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

status_t PublicVolume::addTempPartition(std::string* path) {
    if (mPart.index == 0) {
        *path = mDiskDevPath;
        return OK;
    }
    // A partition left from before (its VM still held it when it was to go) is removed first.
    if (mTempAdded || !mTempNode.empty()) removeTempPartition();
    // fsck and the format tools need the partition as a device of its own. Android's kernel gets it for their run
    // only, at the range the VM read, and reads nothing from it itself.
    dev_t dev;
    status_t res = fsvm::AddPartition(mDiskDevPath, mDiskSysPath, mPart, &dev, &mTempAdded);
    if (res != OK) return res;
    mTempNode = StringPrintf("/dev/block/vold/%s", getId().c_str());
    res = CreateDeviceNode(mTempNode, dev);
    if (res != OK) {
        removeTempPartition();
        return res;
    }
    *path = mTempNode;
    return OK;
}

void PublicVolume::removeTempPartition() {
    if (!mTempNode.empty()) {
        DestroyDeviceNode(mTempNode);
        mTempNode.clear();
    }
    // crosvm of a VM that just stopped may still hold the partition for a moment: try for 2 s.
    for (int i = 0; mTempAdded && i < 20; i++) {
        status_t res = fsvm::DeletePartition(mDiskDevPath, mPart.index);
        if (res == OK || res == -ENXIO) {
            mTempAdded = false;
        } else if (res == -EBUSY) {
            usleep(100 * 1000);
        } else {
            break;
        }
    }
    if (mTempAdded) LOG(WARNING) << getId() << ": partition " << mPart.index << " still in use, left in place";
}

status_t PublicVolume::checkInVm() {
    mVmFailed = false;
    if (mInVm) {
        // The filesystem check runs in the VM, as part of fsvmd's mount, with the fsck_msdos and fsck.exfat vold ran
        // here before. Not with the same arguments: the VM checks a volume only when it is marked dirty.
        if (mPart.unread) {
            LOG(ERROR) << getId() << ": the filesystem VM could not read this disk";
            mVmFailed = true;
            return -EIO;
        }
        if (mFsType != "vfat" && mFsType != "exfat") {
            // The VM read the volume and found nothing Android mounts: unsupported, with the format offer, as stock.
            LOG(ERROR) << getId() << ": the filesystem VM found no FAT or exFAT filesystem";
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

status_t PublicVolume::serveInVm(gid_t gid, bool* servedReadOnly) {
    fsvm::Volume v;
    std::string path = mInVm ? mDiskDevPath : mDevPath;
    // A mixed card: the VM gets this partition only, never the adoptable storage next to it.
    bool ownRange = mInVm && mPart.ownRange && mPart.index > 0;
    if (ownRange) {
        status_t res = addTempPartition(&path);
        if (res != OK) {
            LOG(ERROR) << getId() << ": no partition device of its own on this mixed card (" << res << ")";
            return res;
        }
    }
    v.fd.reset(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_CLOEXEC)));
    if (v.fd < 0 && (errno == EROFS || errno == EACCES)) {
        // A card with its write-protect switch on, as the kernel mount handled it: served read-only.
        v.readOnly = true;
        v.fd.reset(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDONLY | O_CLOEXEC)));
    }
    if (v.fd < 0) {
        int e = errno;
        PLOG(ERROR) << getId() << ": opening " << path;
        if (ownRange) removeTempPartition();
        return -e;
    }
    v.blockSize = fsvm::BlockSize(v.fd.get());
    if (!v.blockSize) {
        if (ownRange) removeTempPartition();
        return -EINVAL;
    }
    if (ownRange) {
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
    v.volumeId = getId();
    v.target = mRawPath;
    status_t res = fsvm::Mount(std::move(v), &mFsvmdPid, servedReadOnly);
    if (res != OK && ownRange) removeTempPartition();
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
    // A volume marked dirty whose check did not finish in time is served read-only. The user gets a notice.
    if (servedReadOnly) LOG(WARNING) << getId() << ": served read-only, its check did not finish";
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
    if (mPart.ownRange) removeTempPartition();
}

status_t PublicVolume::formatInVm(const std::string& fsType) {
    if (mPart.unread) {
        LOG(ERROR) << getId() << ": the filesystem VM could not read this disk, not formatting it";
        return -EIO;
    }
    bool isVfatSup = vfat::IsSupported();
    bool isExfatSup = exfat::IsSupported();
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
    status_t res = addTempPartition(&dev);
    if (res != OK) {
        LOG(ERROR) << getId() << " has no partition device to format";
        return res;
    }
    if (WipeBlockDevice(dev) != OK) {
        LOG(WARNING) << getId() << " failed to wipe";
    }
    res = pick == "vfat" ? vfat::Format(dev, 0) : exfat::Format(dev);
    removeTempPartition();
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
    // One probe only, of at most 10 s: vold holds its lock for the wipe and the format already, and after a
    // partitioning for the probe of the new table too.
    fsvm::Layout layout;
    if (fsvm::Probe(mDiskDevPath, mDiskBytes, &layout, 10000) == OK) {
        for (const auto& p : layout.parts) {
            if (p.index == mPart.index && p.start == mPart.start && p.length == mPart.length && p.fsType == pick) {
                mPart = p;
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
