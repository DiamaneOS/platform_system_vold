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
 * DiamaneOS: OBB images served from the filesystem VM (fs/Fsvm.cpp). Kept apart from ObbVolume.cpp, which calls
 * these from short hooks, so that upstream changes there merge without touching this code.
 */

#include "ObbVolume.h"
#include "VolumeManager.h"
#include "fs/Fsvm.h"
#include "fs/ObbPendingEnd.h"
#include <thread>
#include <chrono>
#include "model/PublicVolume.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <linux/openat2.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <optional>
#include <vector>

using android::base::StringPrintf;

namespace android {
namespace vold {

// DiamaneOS: an OBB is a FAT image an app wrote. Android's kernel never reads it: no loop device (a loop device alone
// gets its partition table read when loop.max_part is set), no FAT mount. fsvmd serves it from the filesystem VM,
// read-only. Each mounted OBB holds a VM, so at most a few at a time: in all, per user and per app. A mount is a VM
// start under vold's lock, so one app may start at most 4 within 10 s (mounting the next OBB right after an unmount
// is fine).
static int sObbVms = 0;
static constexpr int kMaxObbVms = 4;
static std::map<gid_t, int> sObbVmsByOwner;
static constexpr int kMaxObbVmsPerOwner = 2;
static std::map<userid_t, int> sObbVmsByUser;
static constexpr int kMaxObbVmsPerUser = 3;
static std::map<gid_t, std::vector<int64_t>> sObbStartsMs;
static constexpr size_t kMaxObbStarts = 4;
static constexpr int64_t kObbStartWindowMs = 10000;
/* How long opening an OBB on a card may take: the card's own fsvmd answers it, and vold holds its lock. */
static constexpr int kCardOpenMs = 8000;
static constexpr uint64_t kMaxObbBytes = 8ULL << 30;

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
static int OpenObb(const std::string& path, userid_t ownerUser, struct stat* st) {
    static constexpr char kData[] = "/data/media/";
    static constexpr char kCard[] = "/mnt/media_rw/";
    std::string root;
    std::vector<std::string> parts;
    std::shared_ptr<PublicVolume> card;
    if (android::base::StartsWith(path, kData)) {
        parts = android::base::Split(path.substr(sizeof(kData) - 1), "/");
        // The owner's storage, or the storage of the user it shares storage with (a clone profile).
        userid_t shared = VolumeManager::Instance()->getSharedStorageUser(ownerUser);
        if (parts.size() != 5 || !IsObbDir(parts, 1) ||
            (parts[0] != std::to_string(ownerUser) &&
             (shared == USER_UNKNOWN || parts[0] != std::to_string(shared)))) {
            return -EINVAL;
        }
        root = "/data/media";
    } else if (android::base::StartsWith(path, kCard)) {
        parts = android::base::Split(path.substr(sizeof(kCard) - 1), "/");
        if (parts.size() != 5 || parts[0].empty() || parts[0].size() > 64 || !IsObbDir(parts, 1)) return -EINVAL;
        root = kCard + parts[0];
        userid_t shared = VolumeManager::Instance()->getSharedStorageUser(ownerUser);
        auto vol = VolumeManager::Instance()->findVolumeWithFilter([&](const VolumeBase& v) {
            return v.getType() == VolumeBase::Type::kPublic && v.getState() == VolumeBase::State::kMounted &&
                   v.getInternalPath() == root &&
                   (v.getMountUserId() == ownerUser || (shared != USER_UNKNOWN && v.getMountUserId() == shared));
        });
        if (!vol) return -EINVAL;
        card = std::static_pointer_cast<PublicVolume>(vol);
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
    // On a card every step below is a request to the card's fsvmd, under vold's lock: a card that does not answer
    // within kCardOpenMs has its FUSE connection ended (the card's volume ends unopenable), which fails the steps at
    // once.
    std::unique_ptr<fsvm::Guard> guard;
    if (card) guard = card->guardInVm(kCardOpenMs);
    android::base::unique_fd dir(TEMP_FAILURE_RETRY(open(root.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC)));
    if (dir < 0) return -errno;
    struct open_how how = {};
    how.flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(
            static_cast<int>(syscall(__NR_openat2, dir.get(), rel.c_str(), &how, sizeof(how)))));
    if (fd < 0) return -errno;
    if (fstat(fd.get(), st)) return -errno;
    return fd.release();
}

void ObbVolume::requestStop() {
    if (mFsvmdPid > 0) fsvm::RequestStop(mFsvmdPid);
}

/* The OBB images an fsvmd serves now, in the order they were mounted. With vold's lock held. */
static std::vector<ObbVolume*> sObbInVm;

/*
 * An OBB whose VM ended without the framework asking for it (a drive took its place, its fsvmd ended, its card goes)
 * is destroyed right after its unmount: the framework gets onVolumeDestroyed, the one terminal event of the volume,
 * drops its record and tells the app. A later destroyObb of the same ID finds nothing and does nothing. The ID is
 * taken by value: destroyObb frees the volume.
 */
static fsvm::ObbPendingEnd& sPendingEnds = *new fsvm::ObbPendingEnd;
static void EndObb(std::string id) {
    sPendingEnds.end(id, [](const std::string& target) {
        return VolumeManager::Instance()->destroyObb(target) == OK;
    }, [](std::function<void()> work) {
        std::thread([work = std::move(work)] {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
            work();
        }).detach();
    });
}

bool ObbVolume::stopOneForDrive() {
    for (auto it = sObbInVm.rbegin(); it != sObbInVm.rend(); ++it) {
        ObbVolume* vol = *it;
        if (vol->getState() != State::kMounted) continue;
        LOG(WARNING) << vol->getId() << ": its VM stops, a drive needs the protected VM place (OBB images come "
                     << "last), the app's OBB mount ends";
        vol->unmount();
        EndObb(vol->getId());
        return true;
    }
    return false;
}

void ObbVolume::onFsvmdExit(pid_t pid) {
    if (pid == mFsvmdPid && getState() == State::kMounted) {
        LOG(ERROR) << getId() << ": fsvmd ended on its own";
        unmount();
        EndObb(getId());  // the Watch thread holds a reference until this returns
    }
}

status_t ObbVolume::mountInVm() {
    if (mFsvmdPid > 0) return -EBUSY;
    const userid_t user = mUser;
    // An app's second VM only while two slots stay free for others: two apps cannot take all of them.
    const int own = sObbVmsByOwner.count(mOwnerGid) ? sObbVmsByOwner[mOwnerGid] : 0;
    if (sObbVms >= kMaxObbVms || own >= kMaxObbVmsPerOwner || (own > 0 && sObbVms >= kMaxObbVms - 2) ||
        (sObbVmsByUser.count(user) && sObbVmsByUser[user] >= kMaxObbVmsPerUser)) {
        LOG(ERROR) << getId() << " not mounted: too many OBB images are mounted already";
        return -EBUSY;
    }
    auto& starts = sObbStartsMs[mOwnerGid];
    const int64_t now = fsvm::NowMs();
    starts.erase(std::remove_if(starts.begin(), starts.end(),
                                [&](int64_t t) { return now - t >= kObbStartWindowMs; }),
                 starts.end());
    if (starts.size() >= kMaxObbStarts) {
        LOG(ERROR) << getId() << " not mounted: the same app started " << kMaxObbStarts << " within 10 s";
        return -EBUSY;
    }
    starts.push_back(now);
    struct stat st;
    android::base::unique_fd fd(OpenObb(mSourcePath, user, &st));
    if (fd < 0 || !S_ISREG(st.st_mode) || st.st_size < 512 ||
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
    if (status_t res = fsvm::Mount(std::move(v), &mFsvmdPid); res != OK) {
        if (res == -EAGAIN) {
            LOG(ERROR) << getId() << " not mounted: no protected VM place is free, and OBB images come after drives";
        }
        rmdir(obbPath.c_str());
        return -EIO;
    }
    sObbInVm.push_back(this);
    sObbVms++;
    sObbVmsByOwner[mOwnerGid]++;
    sObbVmsByUser[user]++;
    return OK;
}

status_t ObbVolume::confirmEndInVm() {
    if (getState() == State::kMounted) {
        status_t res = unmount();
        if (res != OK) return res;
    }
    return cleanupInVm();
}

status_t ObbVolume::cleanupInVm() {
    const std::string path = getPath();
    status_t res = path.empty() ? OK : ForceUnmount(path);
    stopInVm();
    if (res == OK && !path.empty()) rmdir(path.c_str());
    return res;
}

void ObbVolume::stopInVm() {
    if (mFsvmdPid > 0) {
        // Read-only: nothing to write back, so no orderly wait.
        sObbInVm.erase(std::remove(sObbInVm.begin(), sObbInVm.end(), this), sObbInVm.end());
        fsvm::Stop(mFsvmdPid, false);
        mFsvmdPid = -1;
        sObbVms--;
        if (--sObbVmsByOwner[mOwnerGid] <= 0) sObbVmsByOwner.erase(mOwnerGid);
        if (--sObbVmsByUser[mUser] <= 0) sObbVmsByUser.erase(mUser);
    }
}

}  // namespace vold
}  // namespace android

/* VolumeManager.h unmountObbsBelow, here with the rest of the OBB code. VolumeManager is outside vold's namespace. */
void VolumeManager::unmountObbsBelow(const std::string& path) {
    if (path.empty()) return;
    std::vector<std::shared_ptr<android::vold::VolumeBase>> below;
    for (const auto& vol : mObbVolumes) {
        auto obb = std::static_pointer_cast<android::vold::ObbVolume>(vol);
        if (android::base::StartsWith(obb->getSourcePath(), path + "/") &&
            vol->getState() == android::vold::VolumeBase::State::kMounted) {
            below.push_back(vol);
            // All of them are asked to stop first, so their VMs end together, not one after the other.
            obb->requestStop();
        }
    }
    for (const auto& vol : below) {
        LOG(INFO) << vol->getId() << ": unmounting, its volume " << path << " goes";
        vol->unmount();
        android::vold::EndObb(vol->getId());
    }
}
