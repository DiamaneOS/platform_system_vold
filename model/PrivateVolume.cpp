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

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <libdm/dm.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
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

status_t PrivateVolume::doCreate() {
    if (CreateDeviceNode(mRawDevPath, mRawDevice)) {
        return -EIO;
    }

    // Recover from stale vold by tearing down any old mappings
    auto& dm = dm::DeviceMapper::Instance();
    // TODO(b/149396179) there appears to be a race somewhere in the system where trying
    // to delete the device fails with EBUSY; for now, work around this by retrying.
    bool ret;
    int tries = 10;
    while (tries-- > 0) {
        ret = dm.DeleteDeviceIfExists(getId());
        if (ret || errno != EBUSY) {
            break;
        }
        PLOG(ERROR) << "Cannot remove dm device " << getId();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ret) {
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
    auto& dm = dm::DeviceMapper::Instance();
    // TODO(b/149396179) there appears to be a race somewhere in the system where trying
    // to delete the device fails with EBUSY; for now, work around this by retrying.
    bool ret;
    int tries = 10;
    while (tries-- > 0) {
        ret = dm.DeleteDevice(getId());
        if (ret || errno != EBUSY) {
            break;
        }
        PLOG(ERROR) << "Cannot remove dm device " << getId();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!ret) {
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
