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

#include "PrivateVolume.h"
#include "EmulatedVolume.h"
#include "Utils.h"
#include "VolumeEncryption.h"
#include "VolumeManager.h"
#include "fs/Ext4.h"
#include "fs/F2fs.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <cutils/fs.h>
#include <libdm/dm.h>
#include <private/android_filesystem_config.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <set>
#include <thread>

using android::base::StringPrintf;

namespace android {
namespace vold {

PrivateVolume::PrivateVolume(dev_t device, const KeyBuffer& keyRaw)
    : VolumeBase(Type::kPrivate), mRawDevice(device), mKeyRaw(keyRaw) {
    setId(StringPrintf("private:%u,%u", major(device), minor(device)));
    mRawDevPath = StringPrintf("/dev/block/vold/%s", getId().c_str());
}

PrivateVolume::~PrivateVolume() {}

status_t PrivateVolume::readMetadata() {
    status_t res = ReadMetadata(mDmDevPath, &mFsType, &mFsUuid, &mFsLabel);

    auto listener = getListener();
    if (listener) listener->onVolumeMetadataChanged(getId(), mFsType, mFsUuid, mFsLabel);

    return res;
}

// DiamaneOS: after an unmount, other mount namespaces can keep the card's dm device busy for a
// while (b/149396179); with AOSP's one second of retries a storage reset left the card unmountable.
// Wait up to ten seconds, and name the processes that still have it mounted if it stays busy.
static void LogDmDeviceHolders(const std::string& name) {
    std::string path;
    struct stat st;
    if (!dm::DeviceMapper::Instance().GetDmDevicePathByName(name, &path) ||
        stat(path.c_str(), &st) != 0) {
        return;
    }
    const std::string dev = StringPrintf("%u:%u", major(st.st_rdev), minor(st.st_rdev));
    std::set<std::string> holders;
    auto proc = std::unique_ptr<DIR, int (*)(DIR*)>(opendir("/proc"), closedir);
    if (!proc) return;
    struct dirent* de;
    while ((de = readdir(proc.get())) != nullptr && holders.size() < 10) {
        pid_t pid;
        std::string mountinfo;
        if (!android::base::ParseInt(de->d_name, &pid) ||
            !android::base::ReadFileToString(StringPrintf("/proc/%d/mountinfo", pid), &mountinfo)) {
            continue;
        }
        for (const auto& line : android::base::Split(mountinfo, "\n")) {
            auto fields = android::base::Split(line, " ");
            if (fields.size() > 2 && fields[2] == dev) {
                std::string comm;
                android::base::ReadFileToString(StringPrintf("/proc/%d/comm", pid), &comm);
                holders.insert(android::base::Trim(comm));
                break;
            }
        }
    }
    LOG(ERROR) << name << " is still mounted by: " << android::base::Join(holders, ", ");
}

static bool DeleteDmDeviceWaiting(const std::string& name, bool ifExists) {
    auto& dm = dm::DeviceMapper::Instance();
    for (int i = 0; i < 50; i++) {
        bool ret = ifExists ? dm.DeleteDeviceIfExists(name) : dm.DeleteDevice(name);
        if (ret || errno != EBUSY) return ret;
        if (i % 10 == 0) PLOG(ERROR) << "Cannot remove dm device " << name;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LogDmDeviceHolders(name);
    errno = EBUSY;
    return false;
}

status_t PrivateVolume::doCreate() {
    if (CreateDeviceNode(mRawDevPath, mRawDevice)) {
        return -EIO;
    }

    // Recover from stale vold by tearing down any old mappings
    if (!DeleteDmDeviceWaiting(getId(), true)) {
        return -EIO;
    }

    // TODO: figure out better SELinux labels for private volumes

    if (!setup_ext_volume(getId(), mRawDevPath, mKeyRaw, &mDmDevPath)) {
        LOG(ERROR) << getId() << " failed to setup metadata encryption";
        return -EIO;
    }

    return OK;
}

status_t PrivateVolume::doDestroy() {
    if (!DeleteDmDeviceWaiting(getId(), false)) {
        return -EIO;
    }
    return DestroyDeviceNode(mRawDevPath);
}

status_t PrivateVolume::doMount() {
    if (readMetadata()) {
        LOG(ERROR) << getId() << " failed to read metadata";
        return -EIO;
    }

    mPath = StringPrintf("/mnt/expand/%s", mFsUuid.c_str());
    setPath(mPath);

    if (PrepareDir(mPath, 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount point " << mPath;
        return -EIO;
    }

    if (mFsType == "ext4") {
        // DiamaneOS: the card is removable and its contents are only confidential, not
        // authenticated, so check it fully before the kernel parses it, in the untrusted fsck
        // domain (which can reach only vold's own device nodes), with safe repairs only.
        struct stat st;
        if (stat(mDmDevPath.c_str(), &st) != 0 || !S_ISBLK(st.st_mode)) {
            PLOG(ERROR) << getId() << " cannot stat " << mDmDevPath;
            return -EIO;
        }
        std::string checkNode = StringPrintf("/dev/block/vold/private_fsck:%u,%u",
                                             major(st.st_rdev), minor(st.st_rdev));
        if (CreateDeviceNode(checkNode, st.st_rdev)) {
            PLOG(ERROR) << getId() << " cannot create " << checkNode;
            return -EIO;
        }
        int res = ext4::CheckUntrusted(checkNode);
        DestroyDeviceNode(checkNode);
        if (res == 0 || res == 1) {
            LOG(DEBUG) << getId() << " passed filesystem check";
        } else {
            LOG(ERROR) << getId() << " failed filesystem check: " << res;
            return -EIO;
        }

        // No executables on the card (apps stay internal); a later error makes it read-only.
        if (ext4::Mount(mDmDevPath, mPath, false, false, false, "errors=remount-ro")) {
            PLOG(ERROR) << getId() << " failed to mount";
            return -EIO;
        }

    } else if (mFsType == "f2fs") {
        int res = f2fs::Check(mDmDevPath);
        if (res == 0) {
            LOG(DEBUG) << getId() << " passed filesystem check";
        } else {
            PLOG(ERROR) << getId() << " failed filesystem check";
            return -EIO;
        }

        if (f2fs::Mount(mDmDevPath, mPath)) {
            PLOG(ERROR) << getId() << " failed to mount";
            return -EIO;
        }

    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        return -EIO;
    }

    RestoreconRecursive(mPath);

    int attrs = 0;
    if (!IsSdcardfsUsed()) attrs = FS_CASEFOLD_FL;

    // Verify that common directories are ready to roll
    if (PrepareDir(mPath + "/app", 0771, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/user", 0511, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/user_de", 0511, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/misc_ce", 0511, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/misc_de", 0511, AID_SYSTEM, AID_SYSTEM) ||
        PrepareDir(mPath + "/media", 0550, AID_MEDIA_RW, AID_MEDIA_RW, attrs) ||
        PrepareDir(mPath + "/media/0", 0770, AID_MEDIA_RW, AID_MEDIA_RW) ||
        PrepareDir(mPath + "/local", 0751, AID_ROOT, AID_ROOT) ||
        PrepareDir(mPath + "/local/tmp", 0771, AID_SHELL, AID_SHELL)) {
        PLOG(ERROR) << getId() << " failed to prepare";
        return -EIO;
    }

    return OK;
}

void PrivateVolume::doPostMount() {
    auto vol_manager = VolumeManager::Instance();
    std::string mediaPath(mPath + "/media");

    // Create a new emulated volume stacked above us for all added users, they will automatically
    // be destroyed during unmount
    for (userid_t user : vol_manager->getStartedUsers()) {
        auto vol = std::shared_ptr<VolumeBase>(
                new EmulatedVolume(mediaPath, mRawDevice, mFsUuid, user));
        vol->setMountUserId(user);
        addVolume(vol);
        vol->create();
    }
}

status_t PrivateVolume::doUnmount() {
    ForceUnmount(mPath);

    if (TEMP_FAILURE_RETRY(rmdir(mPath.c_str()))) {
        PLOG(ERROR) << getId() << " failed to rmdir mount point " << mPath;
    }

    return OK;
}

status_t PrivateVolume::doFormat(const std::string& fsType) {
    std::string resolvedFsType = fsType;
    if (fsType == "auto") {
        // DiamaneOS: always ext4 with metadata checksums. f2fs is not safe on removable media
        // (no inode checksums, its checker only scans images marked dirty).
        resolvedFsType = "ext4";
        LOG(DEBUG) << "Resolved auto to " << resolvedFsType;
    }

    if (resolvedFsType == "ext4") {
        // TODO: change reported mountpoint once we have better selinux support
        if (ext4::Format(mDmDevPath, 0, "/data", true)) {
            PLOG(ERROR) << getId() << " failed to format";
            return -EIO;
        }
    } else if (resolvedFsType == "f2fs") {
        if (f2fs::Format(mDmDevPath, false, {}, {})) {
            PLOG(ERROR) << getId() << " failed to format";
            return -EIO;
        }
    } else {
        LOG(ERROR) << getId() << " unsupported filesystem " << fsType;
        return -EINVAL;
    }

    return OK;
}

}  // namespace vold
}  // namespace android
