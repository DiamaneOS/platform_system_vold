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
 * DiamaneOS phase 2: SD cards and USB disks, whose partition tables only the filesystem VM reads (fs/Fsvm.cpp).
 * Kept apart from Disk.cpp, which calls these from short hooks, so that upstream changes there merge without
 * touching this code.
 */

#include "Disk.h"
#include "PrivateVolume.h"
#include "PublicVolume.h"
#include "Utils.h"
#include "VolumeBase.h"
#include "VolumeManager.h"
#include "fs/Fsvm.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>

#include <sys/sysmacros.h>
#include <time.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <mutex>
#include <set>
#include <thread>

using android::base::ReadFileToString;

namespace android {
namespace vold {

bool Disk::isVmDisk() const {
    // SD cards (mmcblk) and disks behind USB (sd): the majors the kernel option CONFIG_BLK_NO_EXTERNAL_PART_SCAN
    // covers, as in Disk.cpp.
    unsigned int m = major(mDevice);
    return m == 179 || m == 8 || (m >= 65 && m <= 71) || (m >= 128 && m <= 135);
}

void Disk::prepareStopAll() {
    // Every volume of the disk starts writing back at once, before they are stopped one after the other under vold's
    // lock: each later stop then finds its fsvmd done, and the whole disk keeps to one volume's time.
    for (const auto& vol : mVolumes) vol->prepareStop();
}

void Disk::removeAddedPartitions() {
    for (uint32_t index : mAddedPartitions) {
        fsvm::DeletePartition(mDevPath, index);
    }
    mAddedPartitions.clear();
}

bool Disk::createPublicVolumeInVm(const fsvm::Part& part) {
    // A crafted table can list many FAT partitions. Each served one holds a VM.
    static constexpr int kMaxPublicPerDisk = 4;
    if (mPublicVolumes >= kMaxPublicPerDisk) {
        LOG(WARNING) << mId << ": not serving partition " << part.index << ", " << kMaxPublicPerDisk
                     << " public volumes on this disk already";
        return false;
    }
    mPublicVolumes++;
    auto vol = std::shared_ptr<VolumeBase>(new PublicVolume(mDevice, mDevPath, mSysPath, mSize, part));
    if (mJustPartitioned) {
        LOG(DEBUG) << "Device just partitioned; silently formatting";
        vol->setSilent(true);
        vol->create();
        vol->format("auto");
        vol->destroy();
        vol->setSilent(false);
    }

    mVolumes.push_back(vol);
    vol->setDiskId(getId());
    vol->create();
    return true;
}

bool Disk::createPrivateVolumeInVm(const fsvm::Part& part) {
    // Adoptable storage: only a card this phone adopted has a key for its partition GUID. Without one, nothing is
    // added to Android's kernel.
    std::string normalizedGuid;
    std::string keyRaw;
    if (NormalizeHex(part.partGuid, normalizedGuid) ||
        !ReadFileToString(BuildKeyPath(normalizedGuid), &keyRaw)) {
        LOG(WARNING) << mId << ": no key for the adoptable storage partition " << part.partGuid;
        return true;
    }
    dev_t partDevice;
    bool added = false;
    if (fsvm::AddPartition(mDevPath, mSysPath, part, &partDevice, &added) != OK) {
        LOG(ERROR) << mId << ": could not add the adoptable storage partition";
        return false;
    }
    if (added) mAddedPartitions.push_back(part.index);
    createPrivateVolume(partDevice, part.partGuid);
    return true;
}

void Disk::createUnopenableVolume() {
    // Not "unsupported, format it?": one volume that cannot be opened, and says so.
    mJustPartitioned = false;
    fsvm::Part whole;
    whole.length = mSize / 512;
    whole.unread = true;
    createPublicVolumeInVm(whole);
}

static int64_t BootMs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void ScheduleReprobe(const std::string& diskId, int delaySeconds, Disk::Reprobe why) {
    std::thread([diskId, delaySeconds, why] {
        std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        auto disk = VolumeManager::Instance()->findDisk(diskId);
        if (disk) disk->reprobeInVm(why);
    }).detach();
}

/* Disks whose probe was put off, by ID: one pending retry each, however often the device comes back. Only touched
   with vold's lock held. */
static std::set<std::string> sDeferred;

static void ScheduleDeferred(const std::string& diskId, int delaySeconds) {
    if (!sDeferred.insert(diskId).second) return;
    std::thread([diskId, delaySeconds] {
        std::this_thread::sleep_for(std::chrono::seconds(std::max(1, delaySeconds)));
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        sDeferred.erase(diskId);
        auto disk = VolumeManager::Instance()->findDisk(diskId);
        if (disk) disk->reprobeInVm(Disk::Reprobe::kDeferred);
    }).detach();
}

void Disk::reprobeInVm(Reprobe why) {
    switch (why) {
        case Reprobe::kChanged:
            mReprobePending = false;
            readPartitions();
            return;
        case Reprobe::kDeferred:
            // The disk still shows the placeholder of the probe that was put off.
            if (!mProbeFailed) return;
            mForceProbe = true;
            readPartitions();
            return;
        case Reprobe::kFailedProbe:
        case Reprobe::kMountRetry:
            break;
    }
    if (why == Reprobe::kMountRetry) mMountRetryPending = false;
    // Only while nothing of the disk is mounted, and (a failed probe) it still shows the result of that probe or
    // (a failed mount) a volume still could not be opened. The new volumes are mounted by the framework, as at
    // insertion.
    bool unopenable = false;
    for (const auto& vol : mVolumes) {
        if (vol->getState() == VolumeBase::State::kMounted) return;
        if (vol->getState() == VolumeBase::State::kUnopenable) unopenable = true;
    }
    if (why == Reprobe::kFailedProbe && !mProbeFailed) return;
    if (why == Reprobe::kMountRetry && !unopenable) return;
    LOG(INFO) << mId << ": reading the partition table again"
              << (why == Reprobe::kMountRetry ? " to mount it again" : "");
    mForceProbe = why == Reprobe::kMountRetry;
    readPartitions();
}

void Disk::retryMountLater() {
    // A refused VM start (right after boot, memory in too many pieces) passes: the volumes are made again and
    // mounted, at most three times, 15 s, 60 s and 180 s later. A volume that mounts resets the count.
    static constexpr int kDelays[] = {15, 60, 180};
    if (mMountRetryPending || mMountRetries >= static_cast<int>(std::size(kDelays))) return;
    mMountRetryPending = true;
    int delay = kDelays[mMountRetries++];
    LOG(INFO) << mId << ": mounting again in " << delay << " s";
    ScheduleReprobe(getId(), delay, Reprobe::kMountRetry);
}

void Disk::noteMounted() {
    mMountRetries = 0;
}

status_t Disk::readPartitionsInVm(int maxMinors) {
    // A change event without a change (same medium sequence, same size) probes nothing: a device that sends them in
    // a loop must not keep vold starting VMs. A real change comes at most once in 3 s, later ones are folded into one.
    // A retry vold scheduled itself (a refused VM start, a probe put off) skips both rules.
    std::string seq;
    ReadFileToString(mSysPath + "/diskseq", &seq);
    const bool forced = mForceProbe;
    mForceProbe = false;
    if (!forced && mProbed && !mJustPartitioned && !mProbeFailed && seq == mProbedSeq && mSize == mProbedSize) {
        LOG(VERBOSE) << mId << ": nothing changed, the partition table is not read again";
        return OK;
    }
    if (!forced && mProbed && !mJustPartitioned && BootMs() - mProbedAtMs < 3000) {
        if (seq != mProbedSeq || mSize != mProbedSize) {
            // A new medium (a card changed in a USB reader): the volumes and partitions of the old one go now, not
            // at the probe that is put off. Android's kernel keeps its partitions across a media change.
            LOG(INFO) << mId << ": the medium changed, dropping the old one's volumes until it is read";
            destroyAllVolumes();
            mPublicVolumes = 0;
            fsvm::DeleteAllPartitions(mDevPath, mSysPath);
        }
        if (!mReprobePending) {
            mReprobePending = true;
            ScheduleReprobe(getId(), 3, Reprobe::kChanged);
        }
        return OK;
    }
    mProbed = true;
    mProbedSeq = seq;
    mProbedSize = mSize;
    mProbedAtMs = BootMs();

    // Fail closed: a kernel that reads this disk's table itself (CONFIG_BLK_NO_EXTERNAL_PART_SCAN missing, dropped
    // because the SD or MMC block driver is a module, or external_part_scan=1) gets no volume served from it.
    std::string partscan;
    const bool kernelScans =
            !ReadFileToString(mSysPath + "/partscan", &partscan) || android::base::Trim(partscan) != "0";
    if (kernelScans) {
        LOG(ERROR) << mId << ": Android's kernel reads this disk's partition table "
                   << "(CONFIG_BLK_NO_EXTERNAL_PART_SCAN is missing), not serving it";
    }

    destroyAllVolumes();
    mPublicVolumes = 0;

    // No medium (an empty slot in a USB card reader): nothing to read, as when sgdisk failed before.
    if (mSize == static_cast<uint64_t>(-1) || mSize < 512) {
        LOG(WARNING) << mId << " has no medium";
        auto listener = VolumeManager::Instance()->getListener();
        if (listener) listener->onDiskScanned(getId());
        mJustPartitioned = false;
        return OK;
    }

    // Probes are limited for all disks together: a device that detaches and attaches in a loop is a new disk each
    // time. A disk over the limit shows as one that could not be opened, and is read when a probe is free again.
    // A format the user asked for is not put off.
    int64_t waitMs = 0;
    if (!kernelScans && !mJustPartitioned && !fsvm::TakeProbe(BootMs(), &waitMs)) {
        LOG(WARNING) << mId << ": too many disks were read lately, reading this one in " << waitMs << " ms";
        fsvm::DeleteAllPartitions(mDevPath, mSysPath);
        createUnopenableVolume();
        mProbeFailed = true;
        ScheduleDeferred(getId(), static_cast<int>((waitMs + 999) / 1000));
        auto listener = VolumeManager::Instance()->getListener();
        if (listener) listener->onDiskScanned(getId());
        return OK;
    }

    // Android's kernel did not read the table and sgdisk does not either: the filesystem VM does. Partitions vold
    // added before, or a kernel without the option made, go first.
    fsvm::Layout layout;
    mProbeFailed = kernelScans || fsvm::DeleteAllPartitions(mDevPath, mSysPath) != OK ||
                   fsvm::Probe(mDevPath, mSize, &layout) != OK;
    if (mProbeFailed) {
        LOG(ERROR) << mId << ": the filesystem VM could not read the disk";
        createUnopenableVolume();
        // A protected VM start can be refused for a while after boot: try twice more, later.
        if (!kernelScans && ++mProbeFailures <= 2) {
            ScheduleReprobe(getId(), mProbeFailures == 1 ? 15 : 60, Reprobe::kFailedProbe);
        }
    } else {
        mProbeFailures = 0;
        // A mixed card (adoptable storage next to a public partition): each public partition is served through a
        // partition device of its own, so its VM never gets the adoptable storage.
        bool mixed = std::any_of(layout.parts.begin(), layout.parts.end(),
                                 [](const fsvm::Part& p) { return p.cls != fsvm::Part::Class::kPublic; });
        bool skipped = false;
        for (const auto& part : layout.parts) {
            switch (part.cls) {
                case fsvm::Part::Class::kPublic: {
                    if (part.index > static_cast<uint32_t>(maxMinors)) {
                        LOG(WARNING) << mId << ": partition " << part.index << " is beyond " << maxMinors;
                        skipped = true;
                        break;
                    }
                    fsvm::Part pub = part;
                    pub.ownRange = mixed;
                    if (!createPublicVolumeInVm(pub)) skipped = true;
                    break;
                }
                case fsvm::Part::Class::kExpand:
                    // This phone's adoptable storage that could not be added must not look unsupported (and
                    // offered for formatting): it shows as a card that could not be opened.
                    if (!createPrivateVolumeInVm(part)) createUnopenableVolume();
                    break;
                case fsvm::Part::Class::kMeta:
                    break;
            }
        }
        // Partitions that hold data but could not be served must not leave a disk without volumes: that would look
        // blank and get the set-up (format) offer.
        if (skipped && mVolumes.empty()) {
            LOG(WARNING) << mId << ": no partition of this disk could be served";
            createUnopenableVolume();
            mProbeFailed = true;  // and the disk is not partitioned (formatted) either
        }
    }

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskScanned(getId());

    mJustPartitioned = false;
    return OK;
}

}  // namespace vold
}  // namespace android
