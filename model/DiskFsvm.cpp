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
#include "ObbVolume.h"
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
#include <atomic>
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

status_t Disk::beforePartitionInVm() {
    if (!isVmDisk()) return OK;
    // A disk the filesystem VM could not read is never partitioned (formatted), whoever asks: nothing showed that it
    // holds no data.
    if (mProbeFailed) {
        LOG(WARNING) << mId << ": the filesystem VM could not read this disk, not partitioning it";
        return -EBUSY;
    }
    destroyAllVolumes();
    mJustPartitioned = true;
    // sgdisk then writes the new table. Zeroing the old one first means it never reads a table from the card.
    if (fsvm::ZeroTables(mDevPath, mSize) != OK) {
        LOG(ERROR) << "Failed to clear the old partition table";
        mJustPartitioned = false;
        return -EIO;
    }
    return OK;
}

void Disk::beforeDestroyInVm() {
    if (!isVmDisk()) return;
    // A card swapped in a USB reader without a change event yet: the old one's volumes must not write back to it.
    // A disk that is gone has no sequence to read, and its volumes stop as before.
    std::string seq;
    if (ReadFileToString(mSysPath + "/diskseq", &seq) && !mProbe.holds(seq, mSize)) dropOldMedium();
    // The volumes go now without a new medium (vold reset): create() must read the table again and make them, not
    // keep the "nothing changed" answer of the last read. A format offer waits for two new answers again.
    mProbe.forget();
    mVerdictPending = false;
}

void Disk::dropOldMedium() {
    LOG(WARNING) << mId << ": the medium changed, the old one's volumes are cut off before they stop";
    fsvm::FailRangesOf(mDevice);
    for (const auto& vol : mVolumes) {
        // A VM disk's public volumes are all PublicVolume (createPublicVolumeInVm).
        if (vol->getType() == VolumeBase::Type::kPublic) static_cast<PublicVolume*>(vol.get())->dropMedium();
    }
}

void Disk::removeRanges() {
    for (const auto& name : mRanges) {
        fsvm::DeleteRange(name, "/dev/block/vold/" + name);
    }
    mRanges.clear();
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
    // Adoptable storage: only a card this phone adopted has a key for its partition GUID. Without one, nothing of the
    // card is set up.
    std::string normalizedGuid;
    std::string keyRaw;
    if (NormalizeHex(part.partGuid, normalizedGuid) ||
        !ReadFileToString(BuildKeyPath(normalizedGuid), &keyRaw)) {
        LOG(WARNING) << mId << ": no key for the adoptable storage partition " << part.partGuid;
        return true;
    }
    // dm-crypt sits on a device-mapper range of exactly the partition the VM read, like each public partition's VM.
    // Android's kernel gets no partition of the card, so any number of public partitions can sit next to it (all
    // ranges hold the card as device-mapper, which may share it).
    std::string name = fsvm::RangeName(mDevice, part.index), node;
    dev_t rangeDevice;
    if (fsvm::CreateRange(name, mDevice, part.start, part.length, false, &node, &rangeDevice, part.diskSeq) != OK) {
        LOG(ERROR) << mId << ": could not set up the adoptable storage range";
        return false;
    }
    mRanges.push_back(name);
    // The volume IDs stay what they were when Android's kernel had the partition (private:179,2).
    auto keyBuffer = KeyBuffer(keyRaw.begin(), keyRaw.end());
    auto vol = std::shared_ptr<VolumeBase>(
            new PrivateVolume(rangeDevice, makedev(major(mDevice), minor(mDevice) + part.index), keyBuffer));
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
    vol->setPartGuid(part.partGuid);
    vol->create();
    return true;
}

void Disk::createWaitingVolume() {
    mJustPartitioned = false;
    fsvm::Part whole;
    whole.length = mSize / 512;
    whole.unread = true;
    whole.waitingForPlace = true;
    createPublicVolumeInVm(whole);
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

/*
 * Waiting for a protected VM place (fsvm::Spawn found none): volumes by ID, disks whose probe waits by ID. With vold's
 * lock held. Whenever fsvm gives a place back, the waiting volumes are mounted again and the waiting disks read again,
 * in a thread of their own that takes vold's lock. A volume or disk that is gone meanwhile is dropped from the list.
 */
static std::set<std::string> sWaitingVolumes;
static std::set<std::string> sWaitingDisks;
static std::atomic<bool> sPlaceRetryScheduled{false};

static void RetryWaitingForPlace() {
    std::thread([] {
        // The place given back is the hypervisor's once the old VM is gone: a moment for its crosvm to end.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        sPlaceRetryScheduled = false;
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        auto disks = sWaitingDisks;
        sWaitingDisks.clear();
        for (const auto& id : disks) {
            auto disk = VolumeManager::Instance()->findDisk(id);
            if (disk) disk->reprobeInVm(Disk::Reprobe::kPlace);
        }
        auto volumes = sWaitingVolumes;
        sWaitingVolumes.clear();
        std::set<std::string> remade;
        for (const auto& id : volumes) {
            auto vol = VolumeManager::Instance()->findVolume(id);
            if (!vol || !vol->waitingForPlace() || !remade.insert(vol->getDiskId()).second) continue;
            auto disk = VolumeManager::Instance()->findDisk(vol->getDiskId());
            if (disk) disk->remakeWaitingVolumes();
        }
        fsvm::SetDrivesWaiting(!sWaitingVolumes.empty() || !sWaitingDisks.empty());
    }).detach();
}

/* Drops waiting volumes and disks that are gone (ejected while they waited). With vold's lock held. */
static void PruneWaiting() {
    for (auto it = sWaitingVolumes.begin(); it != sWaitingVolumes.end();) {
        auto vol = VolumeManager::Instance()->findVolume(*it);
        it = vol && vol->waitingForPlace() ? std::next(it) : sWaitingVolumes.erase(it);
    }
    for (auto it = sWaitingDisks.begin(); it != sWaitingDisks.end();) {
        it = VolumeManager::Instance()->findDisk(*it) ? std::next(it) : sWaitingDisks.erase(it);
    }
    fsvm::SetDrivesWaiting(!sWaitingVolumes.empty() || !sWaitingDisks.empty());
}

static void EnsurePlaceHooks() {
    static bool set = false;
    if (set) return;
    set = true;
    // Room can also come free outside vold: a VM it did not start ends (a self-test's, a test VM). While anything
    // waits, the pool and the piece count are read every 10 s, and the waiting drives try again once there is room.
    std::thread([] {
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            bool waiting;
            {
                std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
                PruneWaiting();
                waiting = !sWaitingVolumes.empty() || !sWaitingDisks.empty();
            }
            if (waiting && fsvm::RoomForDrive() && !sPlaceRetryScheduled.exchange(true)) RetryWaitingForPlace();
        }
    }).detach();
    fsvm::SetPlaceHooks([] { return ObbVolume::stopOneForDrive(); },
                        [] {
                            // Called with or without vold's lock: only a flag here, the work in a thread.
                            if (sPlaceRetryScheduled.exchange(true)) return;
                            RetryWaitingForPlace();
                        });
}

void Disk::waitForPlace(const std::string& volumeId) {
    EnsurePlaceHooks();
    sWaitingVolumes.insert(volumeId);
    fsvm::SetDrivesWaiting(true);
}

void Disk::waitForPlaceDisk(const std::string& diskId) {
    EnsurePlaceHooks();
    sWaitingDisks.insert(diskId);
    fsvm::SetDrivesWaiting(true);
}

void Disk::remakeWaitingVolumes() {
    std::vector<fsvm::Part> parts;
    for (auto it = mVolumes.begin(); it != mVolumes.end();) {
        // No RTTI in vold: the type says which volumes are public ones.
        if ((*it)->getType() != VolumeBase::Type::kPublic || !(*it)->waitingForPlace()) {
            ++it;
            continue;
        }
        auto pub = std::static_pointer_cast<PublicVolume>(*it);
        LOG(INFO) << pub->getId() << ": a protected VM place is free, made again for the framework to mount";
        parts.push_back(pub->partInVm());
        pub->destroy();
        it = mVolumes.erase(it);
        if (mPublicVolumes > 0) mPublicVolumes--;
    }
    for (const auto& part : parts) createPublicVolumeInVm(part);
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
            mForceProbe = false;
            return;
        case Reprobe::kPlace:
            // The disk still shows the placeholder of the probe that found no place.
            if (!mWaitingForPlace) return;
            mWaitingForPlace = false;
            mForceProbe = true;
            readPartitions();
            mForceProbe = false;
            return;
        case Reprobe::kConfirm:
            // The second probe of an answer that would offer a format, unless the disk was read again since. Both
            // flags go after it, also when readPartitions returned before reading them (getMaxMinors).
            mConfirmScheduled = false;
            if (!mVerdictPending) return;
            mForceProbe = true;
            mConfirmProbe = true;
            readPartitions();
            mForceProbe = false;
            mConfirmProbe = false;
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
    mForceProbe = false;
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
    // A retry vold scheduled itself (a refused VM start, a probe put off) skips both rules, and so does a disk made
    // again after a vold reset (beforeDestroyInVm).
    std::string seq;
    ReadFileToString(mSysPath + "/diskseq", &seq);
    const bool forced = mForceProbe;
    mForceProbe = false;
    const bool confirming = mConfirmProbe;
    mConfirmProbe = false;
    switch (mProbe.next(seq, mSize, BootMs(), forced, mJustPartitioned, mProbeFailed)) {
        case fsvm::ProbeMemory::Next::kRead:
            break;
        case fsvm::ProbeMemory::Next::kSkip:
            LOG(VERBOSE) << mId << ": nothing changed, the partition table is not read again";
            return OK;
        case fsvm::ProbeMemory::Next::kLater:
            if (!mProbe.holds(seq, mSize)) {
                // A new medium (a card changed in a USB reader): the volumes and partitions of the old one go now,
                // not at the probe that is put off. Android's kernel keeps its partitions across a media change.
                LOG(INFO) << mId << ": the medium changed, dropping the old one's volumes until it is read";
                dropOldMedium();
                destroyAllVolumes();
                mPublicVolumes = 0;
                mVerdictPending = false;
                fsvm::DeleteRangesOf(mDevice);
                fsvm::DeleteAllPartitions(mDevPath, mSysPath);
            }
            if (!mReprobePending) {
                mReprobePending = true;
                ScheduleReprobe(getId(), 3, Reprobe::kChanged);
            }
            return OK;
    }
    // Another medium than the one the volumes were made for: nothing of theirs may reach it (dropOldMedium).
    const bool newMedium = mProbe.took(seq, mSize, BootMs());

    // Fail closed: a kernel that reads this disk's table itself gets no volume served from it.
    // The compile-time CONFIG_BLK_NO_EXTERNAL_PART_SCAN option requires built-in SD and MMC block drivers.
    std::string partscan;
    const bool kernelScans =
            !ReadFileToString(mSysPath + "/partscan", &partscan) || android::base::Trim(partscan) != "0";
    if (kernelScans) {
        LOG(ERROR) << mId << ": Android's kernel reads this disk's partition table "
                   << "(CONFIG_BLK_NO_EXTERNAL_PART_SCAN is missing), not serving it";
    }

    if (newMedium) dropOldMedium();
    destroyAllVolumes();
    mPublicVolumes = 0;
    // Ranges left from before (vold restarted, the medium changed) go before the disk is read again, and those of
    // disks that are gone.
    fsvm::DeleteRangesOf(mDevice);
    fsvm::SweepRanges();

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
    if (!kernelScans && !mJustPartitioned && !confirming && !fsvm::TakeProbe(BootMs(), &waitMs)) {
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
    EnsurePlaceHooks();
    status_t probed = kernelScans ? -EIO : fsvm::DeleteAllPartitions(mDevPath, mSysPath);
    if (probed == OK) probed = fsvm::Probe(mDevPath, mSize, &layout);
    if (probed == -EAGAIN) {
        // No protected VM place: the disk's placeholder says it waits for one (VOLUME_STATE_WAITING_FOR_PLACE) and the
        // disk is read as soon as a place is free, never shared with another drive's VM and never given up.
        LOG(WARNING) << mId << ": waiting for a protected VM place to read the disk";
        mProbeFailed = true;
        mWaitingForPlace = true;
        createWaitingVolume();
        waitForPlaceDisk(getId());
        auto listener = VolumeManager::Instance()->getListener();
        if (listener) listener->onDiskScanned(getId());
        mJustPartitioned = false;
        return OK;
    }
    mProbeFailed = probed != OK;
    // Everything made from this answer is bound to the medium it was read from (sequence read before the probe: a
    // medium changed during the probe fails the checks, never the other way round).
    layout.diskSeq = android::base::Trim(seq);
    layout.diskBytes = mSize;
    for (auto& p : layout.parts) p.diskSeq = layout.diskSeq;
    if (mProbeFailed) {
        LOG(ERROR) << mId << ": the filesystem VM could not read the disk";
        mVerdictPending = false;
        createUnopenableVolume();
        // A protected VM start can be refused for a while after boot: try twice more, later.
        if (!kernelScans && ++mProbeFailures <= 2) {
            ScheduleReprobe(getId(), mProbeFailures == 1 ? 15 : 60, Reprobe::kFailedProbe);
        }
    } else if (!mJustPartitioned && !confirming && fsvm::OffersFormat(layout)) {
        // The VM found no filesystem it can read (a new card, ext4, NTFS). That would offer the user a format, so a
        // second VM must give the same answer first. Until then the disk has no volume and is not scanned (no notice,
        // no format), and is not partitioned either. A table vold just wrote is formatted by vold itself.
        LOG(INFO) << mId << ": the filesystem VM found nothing it can read, asking a second VM";
        mVerdict = layout;
        mVerdictPending = true;
        mProbeFailed = true;
        if (!mConfirmScheduled) {
            mConfirmScheduled = true;
            ScheduleReprobe(getId(), 1, Reprobe::kConfirm);
        }
        mJustPartitioned = false;
        return OK;
    } else if (confirming && (!mVerdictPending || !fsvm::SameLayout(layout, mVerdict))) {
        // The second VM answered otherwise: no format offer, the disk cannot be opened for now and is read again
        // later, as after a failed probe.
        LOG(ERROR) << mId << ": a second filesystem VM answered otherwise, the disk could not be opened";
        mVerdictPending = false;
        mProbeFailed = true;
        createUnopenableVolume();
        if (++mProbeFailures <= 2) {
            ScheduleReprobe(getId(), mProbeFailures == 1 ? 15 : 60, Reprobe::kFailedProbe);
        }
    } else {
        mProbeFailures = 0;
        // Two VMs gave the same answer: a public partition without a filesystem they can read is offered for
        // formatting (unmountable, as stock), and a disk with nothing usable gets Android's "set up" notice.
        if (confirming) {
            LOG(INFO) << mId << ": a second filesystem VM gave the same answer";
            for (auto& p : layout.parts) {
                p.noFsConfirmed = p.cls == fsvm::Part::Class::kPublic && p.fsType.empty() && !p.damaged;
            }
        }
        mVerdictPending = false;
        bool skipped = false;
        for (const auto& part : layout.parts) {
            switch (part.cls) {
                case fsvm::Part::Class::kPublic: {
                    if (part.index > static_cast<uint32_t>(maxMinors)) {
                        LOG(WARNING) << mId << ": partition " << part.index << " is beyond " << maxMinors;
                        skipped = true;
                        break;
                    }
                    if (!createPublicVolumeInVm(part)) skipped = true;
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
