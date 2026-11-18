/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include "ObbVolume.h"
#include "Loop.h"
#include "Utils.h"
#include "VoldUtil.h"
#include "VolumeManager.h"
#include "fs/Fsvm.h"
#include "fs/Vfat.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <cutils/multiuser.h>
#include <private/android_filesystem_config.h>
#include <utils/Timers.h>

#include <android-base/strings.h>
#include <map>
#include <android-base/unique_fd.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>

using android::base::StringPrintf;

namespace android {
namespace vold {

ObbVolume::ObbVolume(int id, const std::string& sourcePath, gid_t ownerGid)
    : VolumeBase(Type::kObb) {
    setId(StringPrintf("obb:%d", id));
    mSourcePath = sourcePath;
    mOwnerGid = ownerGid;
}

ObbVolume::~ObbVolume() {}

#ifdef VOLD_PUBLIC_FS_IN_VM
// DiamaneOS: an OBB is a FAT image an app wrote. Android's kernel never reads it: no loop device (a loop device alone
// gets its partition table read when loop.max_part is set), no FAT mount. fsvmd serves it from the filesystem VM,
// read-only. Each mounted OBB holds a VM, so at most a few at a time: in all, per user and per app, and an app
// that just unmounted one waits a moment before the next (a mount is a VM start under vold's lock).
static int sObbVms = 0;
static constexpr int kMaxObbVms = 4;
static std::map<gid_t, int> sObbVmsByOwner;
static constexpr int kMaxObbVmsPerOwner = 2;
static std::map<userid_t, int> sObbVmsByUser;
static constexpr int kMaxObbVmsPerUser = 3;
static std::map<gid_t, int64_t> sObbLastUnmountMs;
static constexpr int64_t kObbCooldownMs = 1000;
static constexpr uint64_t kMaxObbBytes = 8ULL << 30;

static int64_t BootMs() {
    return systemTime(SYSTEM_TIME_BOOTTIME) / 1000000;
}

/* "Android" and "obb" as the server's check takes them: in any case. */
static bool IsObbDir(const std::vector<std::string>& p, size_t at) {
    return strcasecmp(p[at].c_str(), "Android") == 0 && strcasecmp(p[at + 1].c_str(), "obb") == 0;
}

/*
 * Opens an OBB read-only, refusing symlinks, magic links, other filesystems and anything outside its root, and not
 * blocking (a FIFO in its place must not hold vold, which holds its lock). Two places, each exactly
 * <root>/<...>/Android/obb/<package>/<file>, no "." or ".." component, nothing empty:
 *
 * - /data/media/<user>/Android/obb/..., internal shared storage, for the owner's user
 * - /mnt/media_rw/<uuid>/Android/obb/..., an SD card or USB drive vold serves through the filesystem VM, mounted for
 *   the owner's user. The file is read through that volume's FUSE mount, so through its own VM.
 */
static int OpenObb(const std::string& path, userid_t ownerUser) {
    static constexpr char kData[] = "/data/media/";
    static constexpr char kCard[] = "/mnt/media_rw/";
    std::string root;
    std::vector<std::string> parts;
    if (android::base::StartsWith(path, kData)) {
        parts = android::base::Split(path.substr(sizeof(kData) - 1), "/");
        if (parts.size() != 5 || parts[0] != std::to_string(ownerUser) || !IsObbDir(parts, 1)) return -EINVAL;
        root = "/data/media";
    } else if (android::base::StartsWith(path, kCard)) {
        parts = android::base::Split(path.substr(sizeof(kCard) - 1), "/");
        if (parts.size() != 5 || parts[0].empty() || parts[0].size() > 64 || !IsObbDir(parts, 1)) return -EINVAL;
        root = kCard + parts[0];
        auto vol = VolumeManager::Instance()->findVolumeWithFilter([&](const VolumeBase& v) {
            return v.getType() == VolumeBase::Type::kPublic && v.getState() == VolumeBase::State::kMounted &&
                   v.getInternalPath() == root && v.getMountUserId() == ownerUser;
        });
        if (!vol) return -EINVAL;
        // Below the volume's root: the component before Android/obb is the root itself.
        parts[0] = ".";
    } else {
        return -EINVAL;
    }
    for (size_t i = 0; i < parts.size(); i++) {
        if (parts[i].empty() || parts[i] == ".." || parts[i].size() > 255) return -EINVAL;
        if (parts[i] == "." && (i != 0 || root == "/data/media")) return -EINVAL;
    }
    std::string rel = android::base::Join(
            std::vector<std::string>(parts.begin() + (parts[0] == "." ? 1 : 0), parts.end()), "/");
    android::base::unique_fd dir(TEMP_FAILURE_RETRY(open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)));
    if (dir < 0) return -errno;
    struct open_how how = {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    int fd = TEMP_FAILURE_RETRY(
            static_cast<int>(syscall(__NR_openat2, dir.get(), rel.c_str(), &how, sizeof(how))));
    return fd < 0 ? -errno : fd;
}
#endif

void ObbVolume::onFsvmdExit(pid_t pid) {
#ifdef VOLD_PUBLIC_FS_IN_VM
    if (pid == mFsvmdPid && getState() == State::kMounted) {
        LOG(ERROR) << getId() << ": fsvmd ended on its own";
        unmount();
    }
#endif
}

status_t ObbVolume::doCreate() {
#ifdef VOLD_PUBLIC_FS_IN_VM
    return OK;
#endif
    if (Loop::create(mSourcePath, mLoopPath)) {
        PLOG(ERROR) << getId() << " failed to create loop";
        return -1;
    }
    return OK;
}

status_t ObbVolume::doDestroy() {
#ifdef VOLD_PUBLIC_FS_IN_VM
    return OK;
#endif
    if (!mLoopPath.empty() && Loop::destroyByDevice(mLoopPath.c_str())) {
        PLOG(WARNING) << getId() << " failed to destroy loop";
    }
    mLoopPath.clear();
    return OK;
}

status_t ObbVolume::doMount() {
#ifdef VOLD_PUBLIC_FS_IN_VM
    if (mFsvmdPid > 0) return -EBUSY;
    const userid_t user = multiuser_get_user_id(mOwnerGid);
    if (sObbVms >= kMaxObbVms || sObbVmsByOwner[mOwnerGid] >= kMaxObbVmsPerOwner ||
        sObbVmsByUser[user] >= kMaxObbVmsPerUser) {
        LOG(ERROR) << getId() << " not mounted: too many OBB images are mounted already";
        return -EBUSY;
    }
    auto last = sObbLastUnmountMs.find(mOwnerGid);
    if (last != sObbLastUnmountMs.end() && BootMs() - last->second < kObbCooldownMs) {
        LOG(ERROR) << getId() << " not mounted: the same app unmounted one a moment ago";
        return -EBUSY;
    }
    android::base::unique_fd fd(OpenObb(mSourcePath, user));
    struct stat st;
    if (fd < 0 || fstat(fd.get(), &st) || !S_ISREG(st.st_mode) || st.st_size < 512 ||
        static_cast<uint64_t>(st.st_size) > kMaxObbBytes) {
        LOG(ERROR) << getId() << " not mounted: not an OBB file of this user's shared storage or card";
        return -EINVAL;
    }
    auto obbPath = StringPrintf("/mnt/obb/%s", getId().c_str());
    setPath(obbPath);
    if (fs_prepare_dir(obbPath.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount point";
        return -1;
    }
    fsvm::Volume v;
    v.fd = std::move(fd);
    v.obb = true;
    v.readOnly = true;
    // The VM sees whole sectors. The signature footer after the image is not part of the filesystem.
    v.diskBytes = static_cast<uint64_t>(st.st_size) & ~511ULL;
    v.gid = mOwnerGid;
    v.volumeId = getId();
    v.target = obbPath;
    if (fsvm::Mount(std::move(v), &mFsvmdPid) != OK) {
        rmdir(obbPath.c_str());
        return -EIO;
    }
    sObbVms++;
    sObbVmsByOwner[mOwnerGid]++;
    sObbVmsByUser[user]++;
    return OK;
#endif
    auto path = StringPrintf("/mnt/obb/%s", getId().c_str());
    setPath(path);

    if (fs_prepare_dir(path.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount point";
        return -1;
    }
    // clang-format off
    if (android::vold::vfat::Mount(mLoopPath, path, true, false, true,
                                   0, mOwnerGid, 0227, false)) {
        // clang-format on
        PLOG(ERROR) << getId() << " failed to mount";
        return -1;
    }
    return OK;
}

status_t ObbVolume::doUnmount() {
    auto path = getPath();

    KillProcessesUsingPath(path);
    ForceUnmount(path);
#ifdef VOLD_PUBLIC_FS_IN_VM
    if (mFsvmdPid > 0) {
        fsvm::Stop(mFsvmdPid, true);
        mFsvmdPid = -1;
        sObbVms--;
        if (--sObbVmsByOwner[mOwnerGid] <= 0) sObbVmsByOwner.erase(mOwnerGid);
        const userid_t user = multiuser_get_user_id(mOwnerGid);
        if (--sObbVmsByUser[user] <= 0) sObbVmsByUser.erase(user);
        sObbLastUnmountMs[mOwnerGid] = BootMs();
    }
#endif
    rmdir(path.c_str());

    return OK;
}

}  // namespace vold
}  // namespace android
