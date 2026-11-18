/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "PublicVolume.h"

#include "AppFuseUtil.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "fs/Exfat.h"
#include "fs/Fsvm.h"
#include "fs/Vfat.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/scopeguard.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>
#include <utils/Timers.h>

#include <fcntl.h>
#include <stdlib.h>

#include <algorithm>
#include <optional>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <android_vold_flags.h>

using android::base::GetBoolProperty;
using android::base::StringPrintf;
namespace flags = android::vold::flags;

namespace android {
namespace vold {

static const char* kSdcardFsPath = "/system/bin/sdcard";

static const char* kAsecPath = "/mnt/secure/asec";

PublicVolume::PublicVolume(dev_t device) : VolumeBase(Type::kPublic), mDevice(device) {
    setId(StringPrintf("public:%u,%u", major(device), minor(device)));
    mDevPath = StringPrintf("/dev/block/vold/%s", getId().c_str());
    mFuseMounted = false;
    mUseSdcardFs = IsSdcardfsUsed();
}

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

PublicVolume::~PublicVolume() {}

status_t PublicVolume::readMetadata() {
    status_t res;
    if (mInVm) {
        // What the filesystem VM read, checked by fsvmd. Nothing on Android parses the volume.
        mFsType = mPart.fsType;
        mFsUuid = mPart.fsUuid;
        mFsLabel = mPart.label;
        res = OK;
    } else {
        res = ReadMetadataUntrusted(mDevPath, &mFsType, &mFsUuid, &mFsLabel);
    }

    auto listener = getListener();
    if (listener) listener->onVolumeMetadataChanged(getId(), mFsType, mFsUuid, mFsLabel);

    return res;
}

status_t PublicVolume::initAsecStage() {
    std::string legacyPath(mRawPath + "/android_secure");
    std::string securePath(mRawPath + "/.android_secure");

    // Recover legacy secure path
    if (!access(legacyPath.c_str(), R_OK | X_OK) && access(securePath.c_str(), R_OK | X_OK)) {
        if (rename(legacyPath.c_str(), securePath.c_str())) {
            PLOG(WARNING) << getId() << " failed to rename legacy ASEC dir";
        }
    }

    if (TEMP_FAILURE_RETRY(mkdir(securePath.c_str(), 0700))) {
        if (errno != EEXIST) {
            PLOG(WARNING) << getId() << " creating ASEC stage failed";
            return -errno;
        }
    }

    BindMount(securePath, kAsecPath);

    return OK;
}

std::string PublicVolume::getStableName() {
    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }
    return stableName;
}

status_t PublicVolume::doCreate() {
    // A volume the VM serves uses the whole disk's node.
    if (mInVm) return OK;
    return CreateDeviceNode(mDevPath, mDevice);
}

status_t PublicVolume::doDestroy() {
    if (mInVm) {
        removeTempPartition();
        return OK;
    }
    return DestroyDeviceNode(mDevPath);
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
    return OK;
}

void PublicVolume::onFsvmdExit(pid_t pid) {
    if (pid != mFsvmdPid || getState() != State::kMounted) return;
    LOG(ERROR) << getId() << ": fsvmd ended on its own, the volume could not be served any more";
    unmount();
    setUnopenable();
}

status_t PublicVolume::mountInVm(gid_t gid) {
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
    status_t res = fsvm::Mount(std::move(v), &mFsvmdPid);
    if (res != OK && ownRange) removeTempPartition();
    return res;
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
    for (int attempt = 0; attempt < 2; attempt++) {
        fsvm::Layout layout;
        if (fsvm::Probe(mDiskDevPath, mDiskBytes, &layout) != OK) continue;
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

status_t PublicVolume::doMount() {
    bool isVisible = isVisibleForWrite();
    readMetadata();

#ifdef VOLD_PUBLIC_FS_IN_VM
    // DiamaneOS: only the filesystem VM's kernel parses the volume, so Android's kernel need not support it. fsck
    // still runs first in its own sandbox (fsck_untrusted).
    mVmFailed = false;
    if (mInVm) {
        status_t res = checkInVm();
        if (res != OK) return res;
    } else if (mFsType == "vfat") {
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
#else
    if (mFsType == "vfat" && vfat::IsSupported()) {
        if (vfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else if (mFsType == "exfat" && exfat::IsSupported()) {
        if (exfat::Check(mDevPath)) {
            LOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }
    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }
#endif

    std::string stableName = getStableName();

    mRawPath = StringPrintf("/mnt/media_rw/%s", stableName.c_str());

    mSdcardFsDefault = StringPrintf("/mnt/runtime/default/%s", stableName.c_str());
    mSdcardFsRead = StringPrintf("/mnt/runtime/read/%s", stableName.c_str());
    mSdcardFsWrite = StringPrintf("/mnt/runtime/write/%s", stableName.c_str());
    mSdcardFsFull = StringPrintf("/mnt/runtime/full/%s", stableName.c_str());

    setInternalPath(mRawPath);
    if (isVisible) {
        setPath(StringPrintf("/storage/%s", stableName.c_str()));
    } else {
        setPath(mRawPath);
    }

    if (fs_prepare_dir(mRawPath.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

#ifdef VOLD_PUBLIC_FS_IN_VM
    // DiamaneOS: Android's kernel never mounts a FAT or exFAT volume. fsvmd serves it from a protected VM. If the VM
    // does not serve it, the volume is not mounted: there is no fallback to the kernel mount.
    mMountStartMs = systemTime(SYSTEM_TIME_BOOTTIME) / 1000000;
    mWriteBackFailed = false;
    if (mountInVm(isVisible ? AID_MEDIA_RW : AID_EXTERNAL_STORAGE) != OK) {
        LOG(ERROR) << getId() << " was not served by the filesystem VM";
        mVmFailed = true;
        rmdir(mRawPath.c_str());
        return -EIO;
    }
    // Every later step reads through fsvmd. Any failure from here on is the VM path's, and must not offer a format.
    mVmFailed = true;
    // The steps after the VM served the volume (MediaProvider's FUSE, which may have to start first after boot) get
    // whatever the VM left of 48 s, at least 15 s: a quick VM mount leaves them more. vold holds its lock throughout.
    int64_t spent = systemTime(SYSTEM_TIME_BOOTTIME) / 1000000 - mMountStartMs;
    fsvm::Guard guard(mFsvmdPid, static_cast<int>(std::max<int64_t>(15000, 48000 - spent)));
#else
    if (mFsType == "vfat") {
        if (vfat::Mount(mDevPath, mRawPath, false, false, false, AID_ROOT,
                        (isVisible ? AID_MEDIA_RW : AID_EXTERNAL_STORAGE), 0007, true)) {
            PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
            return -EIO;
        }
    } else if (mFsType == "exfat") {
        if (exfat::Mount(mDevPath, mRawPath, AID_ROOT,
                         (isVisible ? AID_MEDIA_RW : AID_EXTERNAL_STORAGE), 0007)) {
            PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
            return -EIO;
        }
    }
#endif

    if (getMountFlags() & MountFlags::kPrimary) {
        initAsecStage();
    }

    if (!isVisible) {
        // Not visible to apps, so no need to spin up sdcardfs or FUSE
        return OK;
    }

    if (mUseSdcardFs) {
        if (fs_prepare_dir(mSdcardFsDefault.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsRead.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsWrite.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mSdcardFsFull.c_str(), 0700, AID_ROOT, AID_ROOT)) {
            PLOG(ERROR) << getId() << " failed to create sdcardfs mount points";
            return -errno;
        }

        dev_t before = GetDevice(mSdcardFsFull);

        int sdcardFsPid;
        if (!(sdcardFsPid = fork())) {
            if (getMountFlags() & MountFlags::kPrimary) {
                // clang-format off
                if (execl(kSdcardFsPath, kSdcardFsPath,
                        "-u", "1023", // AID_MEDIA_RW
                        "-g", "1023", // AID_MEDIA_RW
                        "-U", std::to_string(getMountUserId()).c_str(),
                        "-w",
                        mRawPath.c_str(),
                        stableName.c_str(),
                        NULL)) {
                    // clang-format on
                    PLOG(ERROR) << "Failed to exec";
                }
            } else {
                // clang-format off
                if (execl(kSdcardFsPath, kSdcardFsPath,
                        "-u", "1023", // AID_MEDIA_RW
                        "-g", "1023", // AID_MEDIA_RW
                        "-U", std::to_string(getMountUserId()).c_str(),
                        mRawPath.c_str(),
                        stableName.c_str(),
                        NULL)) {
                    // clang-format on
                    PLOG(ERROR) << "Failed to exec";
                }
            }

            LOG(ERROR) << "sdcardfs exiting";
            _exit(1);
        }

        if (sdcardFsPid == -1) {
            PLOG(ERROR) << getId() << " failed to fork";
            return -errno;
        }

        nsecs_t start = systemTime(SYSTEM_TIME_BOOTTIME);
        while (before == GetDevice(mSdcardFsFull)) {
            LOG(DEBUG) << "Waiting for sdcardfs to spin up...";
            usleep(50000);  // 50ms

            nsecs_t now = systemTime(SYSTEM_TIME_BOOTTIME);
            if (nanoseconds_to_milliseconds(now - start) > 5000) {
                LOG(WARNING) << "Timed out while waiting for sdcardfs to spin up";
                return -ETIMEDOUT;
            }
        }
        /* sdcardfs will have exited already. The filesystem will still be running */
        TEMP_FAILURE_RETRY(waitpid(sdcardFsPid, nullptr, 0));
    }

    // We need to mount FUSE *after* sdcardfs, since the FUSE daemon may depend
    // on sdcardfs being up.
    LOG(INFO) << "Mounting public fuse volume";
    android::base::unique_fd fd;
    int user_id = getMountUserId();
    int result = MountUserFuse(user_id, getInternalPath(), stableName, &fd);

    if (result != 0) {
        LOG(ERROR) << "Failed to mount public fuse volume";
        doUnmount();
        return -result;
    }

    mFuseMounted = true;
    auto callback = getMountCallback();
    if (callback) {
        bool is_ready = false;
        callback->onVolumeChecking(std::move(fd), getPath(), getInternalPath(), &is_ready);
        if (!is_ready) {
            LOG(ERROR) << "Failed to complete public volume mount";
            doUnmount();
            return -EIO;
        }
    }

    ConfigureReadAheadForFuse(GetFuseMountPathForUser(user_id, stableName), 256u);

    // See comment in model/EmulatedVolume.cpp
    ConfigureMaxDirtyRatioForFuse(GetFuseMountPathForUser(user_id, stableName), 40u);

    auto vol_manager = VolumeManager::Instance();
    // Create bind mounts for all running users
    for (userid_t started_user : vol_manager->getStartedUsers()) {
        userid_t mountUserId = getMountUserId();
        if (started_user == mountUserId) {
            // No need to bind mount for the user that owns the mount
            continue;
        }
        if (mountUserId != VolumeManager::Instance()->getSharedStorageUser(started_user)) {
            // No need to bind if the user does not share storage with the mount owner
            continue;
        }
        // Create mount directory for the user as there is a chance that no other Volume is mounted
        // for the user (ex: if the user is just started), so /mnt/user/user_id  does not exist yet.
        auto mountDirStatus = PrepareMountDirForUser(started_user);
        if (mountDirStatus != OK) {
            LOG(ERROR) << "Failed to create Mount Directory for user " << started_user;
        }
        auto bindMountStatus = bindMountForUser(started_user);
        if (bindMountStatus != OK) {
            LOG(ERROR) << "Bind Mounting Public Volume: " << stableName
                       << " for user: " << started_user << "Failed. Error: " << bindMountStatus;
        }
    }
    return OK;
}

status_t PublicVolume::bindMountForUser(userid_t user_id) {
    userid_t mountUserId = getMountUserId();
    std::string stableName = getStableName();

    LOG(INFO) << "Bind Mounting Public Volume for user: " << user_id
              << ".Mount owner: " << mountUserId;
    auto sourcePath = GetFuseMountPathForUser(mountUserId, stableName);
    auto destPath = GetFuseMountPathForUser(user_id, stableName);
    PrepareDir(destPath, 0770, AID_ROOT, AID_MEDIA_RW);
    auto mountRes = BindMount(sourcePath, destPath);
    LOG(INFO) << "Mount status: " << mountRes;

    return mountRes;
}

status_t PublicVolume::doUnmount() {
#ifdef VOLD_PUBLIC_FS_IN_VM
    // DiamaneOS: fsvmd stops on every way out, also when unmounting FUSE fails below. A stuck fsvmd (its VM hangs,
    // FUSE requests wait) is ended after 15 s, so the steps waiting on it fail at once.
    auto stop = android::base::make_scope_guard([this] { stopInVm(); });
    std::optional<fsvm::Guard> stuck;
    if (mFsvmdPid > 0) stuck.emplace(mFsvmdPid, 15000);
    // OBB images served from this card go first: their VMs read through this volume.
    VolumeManager::Instance()->unmountObbsBelow(getInternalPath());
#endif
    // Unmount the storage before we kill the FUSE process. If we kill
    // the FUSE process first, most file system operations will return
    // ENOTCONN until the unmount completes. This is an exotic and unusual
    // error code and might cause broken behaviour in applications.

    // We don't kill processes here if enhance_fuse_unmount as we make sure that we kill processes
    // only if unmounting failed
    if (!mFuseMounted || !flags::enhance_fuse_unmount()) {
        KillProcessesUsingPath(getPath());
    }

    if (mFuseMounted) {
        std::string stableName = getStableName();

        // Unmount bind mounts for running users
        auto vol_manager = VolumeManager::Instance();
        userid_t user_id = getMountUserId();
        std::vector<std::string> bind_mount_paths;
        for (userid_t started_user : vol_manager->getStartedUsers()) {
            if (started_user == user_id) {
                // No need to remove bind mount for the user that owns the mount
                continue;
            }
            if (user_id != VolumeManager::Instance()->getSharedStorageUser(started_user)) {
                // No need to remove bind mount
                // if the user does not share storage with the mount owner
                continue;
            }

            auto mountPath = GetFuseMountPathForUser(started_user, stableName);
            if (flags::enhance_fuse_unmount()) {
                // Add it to list so that we unmount it as part of UnmountUserFuseEnhanced
                bind_mount_paths.push_back(mountPath);
            } else {
                LOG(INFO) << "Removing Public Volume Bind Mount for: " << started_user;
                ForceUnmount(mountPath);
                rmdir(mountPath.c_str());
            }
        }

        if (flags::enhance_fuse_unmount()) {
            status_t result = UnmountUserFuseEnhanced(getMountUserId(), getInternalPath(),
                                                      stableName, getPath(), bind_mount_paths);
            if (result != OK) {
                PLOG(INFO) << "UnmountUserFuseEnhanced failed on public fuse volume";
                return result;
            }
        } else {
            if (UnmountUserFuse(getMountUserId(), getInternalPath(), stableName) != OK) {
                PLOG(INFO) << "UnmountUserFuse failed on public fuse volume";
                return -errno;
            }
        }


        mFuseMounted = false;
    }

    ForceUnmount(kAsecPath);

    if (mUseSdcardFs) {
        ForceUnmount(mSdcardFsDefault);
        ForceUnmount(mSdcardFsRead);
        ForceUnmount(mSdcardFsWrite);
        ForceUnmount(mSdcardFsFull);

        rmdir(mSdcardFsDefault.c_str());
        rmdir(mSdcardFsRead.c_str());
        rmdir(mSdcardFsWrite.c_str());
        rmdir(mSdcardFsFull.c_str());

        mSdcardFsDefault.clear();
        mSdcardFsRead.clear();
        mSdcardFsWrite.clear();
        mSdcardFsFull.clear();
    }

    if (ForceUnmount(mRawPath) != 0){
        umount2(mRawPath.c_str(),MNT_DETACH);
        PLOG(INFO) << "use umount lazy if force unmount fail";
    }
#ifdef VOLD_PUBLIC_FS_IN_VM
    // The FUSE mount is gone, so fsvmd and its VM can stop, after writing the volume back. The guard for the steps
    // before must not cut that short.
    stuck.reset();
    stopInVm();
#endif
    if(rmdir(mRawPath.c_str()) != 0) {
        PLOG(INFO) << "rmdir mRawPath=" << mRawPath << " fail";
        KillProcessesUsingPath(getPath());
    }
    mRawPath.clear();

    return OK;
}

status_t PublicVolume::doFormat(const std::string& fsType) {
#ifdef VOLD_PUBLIC_FS_IN_VM
    if (mInVm) return formatInVm(fsType);
#endif
    bool isVfatSup = vfat::IsSupported();
    bool isExfatSup = exfat::IsSupported();
    status_t res = OK;

    enum { NONE, VFAT, EXFAT } fsPick = NONE;

    // Resolve auto requests
    if (fsType == "auto" && isVfatSup && isExfatSup) {
        uint64_t size = 0;

        res = GetBlockDevSize(mDevPath, &size);
        if (res != OK) {
            LOG(ERROR) << "Couldn't get device size " << mDevPath;
            return res;
        }

        // If both vfat & exfat are supported use exfat for SDXC (>~32GiB) cards
        if (size > 32896LL * 1024 * 1024) {
            fsPick = EXFAT;
        } else {
            fsPick = VFAT;
        }
    } else if (fsType == "auto" && isExfatSup) {
        fsPick = EXFAT;
    } else if (fsType == "auto" && isVfatSup) {
        fsPick = VFAT;
    }

    // Resolve explicit requests
    if (fsType == "vfat" && isVfatSup) {
        fsPick = VFAT;
    } else if (fsType == "exfat" && isExfatSup) {
        fsPick = EXFAT;
    }

    if (WipeBlockDevice(mDevPath) != OK) {
        LOG(WARNING) << getId() << " failed to wipe";
    }

    if (fsPick == VFAT) {
        res = vfat::Format(mDevPath, 0);
    } else if (fsPick == EXFAT) {
        res = exfat::Format(mDevPath);
    } else {
        LOG(ERROR) << "Unsupported filesystem " << fsType;
        return -EINVAL;
    }

    if (res != OK) {
        LOG(ERROR) << getId() << " failed to format";
        res = -errno;
    }

    return res;
}

}  // namespace vold
}  // namespace android
