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
#include "fs/Fsvm.h"
#include "fs/Vfat.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <android-base/strings.h>
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
// read-only. Each mounted OBB holds a VM, so at most a few at a time.
static int sObbVms = 0;
static constexpr int kMaxObbVms = 4;
static constexpr uint64_t kMaxObbBytes = 8ULL << 30;

/* Opens an OBB in /data/media/<user>/Android/obb/, read-only, refusing symlinks and anything outside. */
static int OpenObb(const std::string& path) {
    static constexpr char kRoot[] = "/data/media/";
    if (!android::base::StartsWith(path, kRoot)) return -EINVAL;
    auto rest = path.substr(sizeof(kRoot) - 1);
    auto slash = rest.find('/');
    if (slash == 0 || slash == std::string::npos ||
        rest.find_first_not_of("0123456789") != slash ||
        !android::base::StartsWith(rest.substr(slash), "/Android/obb/")) {
        return -EINVAL;
    }
    android::base::unique_fd root(TEMP_FAILURE_RETRY(open("/data/media", O_PATH | O_DIRECTORY | O_CLOEXEC)));
    if (root < 0) return -errno;
    struct open_how how = {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    int fd = TEMP_FAILURE_RETRY(static_cast<int>(
            syscall(__NR_openat2, root.get(), path.c_str() + sizeof(kRoot) - 1, &how, sizeof(how))));
    return fd < 0 ? -errno : fd;
}
#endif

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
    if (sObbVms >= kMaxObbVms) {
        LOG(ERROR) << getId() << " not mounted: " << kMaxObbVms << " OBB images are mounted already";
        return -EBUSY;
    }
    android::base::unique_fd fd(OpenObb(mSourcePath));
    struct stat st;
    if (fd < 0 || fstat(fd.get(), &st) || !S_ISREG(st.st_mode) || st.st_size < 512 ||
        static_cast<uint64_t>(st.st_size) > kMaxObbBytes) {
        LOG(ERROR) << getId() << " not mounted: not an OBB file below /data/media";
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
        fsvm::Stop(mFsvmdPid);
        mFsvmdPid = -1;
        sObbVms--;
    }
#endif
    rmdir(path.c_str());

    return OK;
}

}  // namespace vold
}  // namespace android
