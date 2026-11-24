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
#include "DmWait.h"
#include "KeyBuffer.h"
#include "ObbVolume.h"
#include "PrivateVolume.h"
#include "PublicVolume.h"
#include "RandUtils.h"
#include "Utils.h"
#include "VolumeBase.h"
#include "VolumeEncryption.h"
#include "VolumeManager.h"
#include "fs/Fsvm.h"
#include "fs/FsvmDrive.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/stringprintf.h>
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
using android::base::StringPrintf;
using android::base::WriteStringToFile;

int VolumeManager::replaceRetainedDisk(dev_t device) {
    for (auto it = mDisks.begin(); it != mDisks.end(); ++it) {
        if ((*it)->getDevice() != device) continue;
        if (!(*it)->removalPending()) return -EEXIST;
        if ((*it)->destroy() != android::OK) return -EAGAIN;
        mDisks.erase(it);
        break;
    }
    return android::OK;
}

int VolumeManager::destroyShutdownDisks() {
    int refused = android::OK;
    mDisks.remove_if([&](const auto& disk) {
        if (disk->isStub()) return true;
        const auto res = disk->destroy();
        if (res != android::OK && refused == android::OK) refused = res;
        return res == android::OK;
    });
    return refused;
}

int VolumeManager::resetRemovableDisks(bool inVm) {
    for (auto it = mDisks.begin(); it != mDisks.end();) {
        const auto& disk = *it;
        bool vmDisk = false;
        DIAMANEOS_FS_IN_VM(vmDisk = disk->isVmDisk();)
        if (disk->isStub() || vmDisk != inVm) {
            ++it;
            continue;
        }
        const bool removed = disk->removalPending();
        if (disk->destroy() != android::OK) return -EAGAIN;
        if (removed) {
            it = mDisks.erase(it);  // A queued replacement has its own object.
        } else if (disk->create() != android::OK) {
            mPendingDisks.push_back(disk);
            it = mDisks.erase(it);
        } else {
            ++it;
        }
    }
    return android::OK;
}

namespace android {
namespace vold {

static bool KernelScansPartitions(const std::string& sysPath) {
    std::string partscan;
    return !ReadFileToString(sysPath + "/partscan", &partscan) || android::base::Trim(partscan) != "0";
}

bool Disk::isVmDisk() const {
    // SD cards (mmcblk) and disks behind USB (sd): the majors the kernel option CONFIG_BLK_NO_EXTERNAL_PART_SCAN
    // covers, as in Disk.cpp.
    unsigned int m = major(mDevice);
    return m == 179 || m == 8 || (m >= 65 && m <= 71) || (m >= 128 && m <= 135);
}

bool Disk::hasVmBackings() const {
    return (mDrive && mDrive->hasBackings()) || !mRanges.empty();
}

status_t Disk::preflightStop(bool* isolated) {
    if (isolated) *isolated = false;
    if (!isVmDisk() || !hasVmBackings()) return OK;
    const auto generation = mProbe.mediumRead();
    if (mRemovalPending || fsvm::MediumChanged(mDevice, generation)) {
        if (auto res = fenceOldMedium(); res != OK) return res;
        if (isolated) *isolated = true;
        return OK;
    }
    if (!fsvm::IsMedium(mDevice, generation) || !fsvm::HasSize(mDevice, mProbe.bytesRead())) {
        LOG(WARNING) << mId << ": could not verify the original medium, retaining its mounts";
        return -EAGAIN;
    }
    if (mDrive) {
        if (auto res = mDrive->fenceBackings(false); res != OK) return res;
    }
    for (auto& entry : mRanges) {
        auto& range = entry.second;
        if (range.fenceStarted) {
            if (auto res = fsvm::FenceRange(&range); res != OK) return res;
        }
    }
    return OK;
}

status_t Disk::prepareStopAll() {
    bool isolated = false;
    if (auto res = preflightStop(&isolated); res != OK) return res;
    if (isolated) {
        dropOldMedium();
        return OK;
    }
    // Every volume of the disk starts writing back at once, before they are stopped one after the other under vold's
    // lock: each later stop then finds its fsvmd done, and the whole disk keeps to one volume's time.
    if (mDrive) mDrive->requestWriteBack();
    for (const auto& vol : mVolumes) vol->prepareStop();
    return OK;
}

void Disk::publishDebugMedium() {
    if (major(mDevice) != 7) return;
    std::string seq;
    if (!ReadFileToString(StringPrintf("/sys/dev/block/%u:%u/diskseq", major(mDevice), minor(mDevice)), &seq) ||
        android::base::Trim(seq).empty()) return;
    mProbe.took(seq, mSize, fsvm::NowMs());
    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskMedium(getId(), android::base::Trim(seq), mSize);
}

status_t Disk::partitionConfirmed(int type, int ratio, const std::string& generation, uint64_t bytes,
                                  int64_t resetDeadline, bool freshConfirmation) {
    if (resetDeadline && (type != 0 || !(mFlags & kAdoptable) || !isVmDisk())) return -EINVAL;
    if (freshConfirmation && (type != 0 || !isVmDisk() || !mRecheckPending)) return -EINVAL;
    if (generation.empty() || mProbe.mediumRead().empty() || bytes != mSize || !mProbe.holds(generation, bytes) ||
        !fsvm::IsMedium(mDevice, generation) || !fsvm::HasSize(mDevice, bytes)) return -ENODEV;
    mEraseGeneration = generation;
    mEraseBytes = bytes;
    status_t res = -EINVAL;
    switch (type) {
        case 0:
            res = resetDeadline || freshConfirmation
                          ? partitionInVm(fsvm::TableKind::kPublic, 0, resetDeadline, freshConfirmation)
                          : partitionPublic();
            break;
        case 1: res = partitionPrivate(); break;
        case 2: res = partitionMixed(ratio); break;
    }
    // Debug loop disks use stock formatting and finish it before releasing the confirmed operation.
    if (res == OK && type == 0 && major(mDevice) == 7) res = readPartitions();
    mEraseGeneration.clear();
    mEraseBytes = 0;
    mJustPartitioned = false;
    return resetDeadline && res == OK && mRecheckPending ? -EAGAIN : res;
}

status_t Disk::retryPlaceholder(const std::string& volumeId) {
    auto missing = mMissingParts.find(volumeId);
    if (missing != mMissingParts.end()) {
        const auto part = missing->second;
        if (!fsvm::IsMedium(mDevice, part.diskSeq) || !fsvm::HasSize(mDevice, mSize)) return -ENODEV;
        if (!createPrivateVolumeInVm(part, getMaxMinors(), true)) return -EIO;
        auto old = std::find_if(mVolumes.begin(), mVolumes.end(), [&](const auto& vol) {
            return vol->getId() == volumeId;
        });
        if (old != mVolumes.end()) {
            (*old)->destroy();
            mVolumes.erase(old);
            if (mPublicVolumes > 0) mPublicVolumes--;
        }
        mMissingParts.erase(volumeId);
        return OK;
    }
    for (const auto& vol : mVolumes) {
        if (vol->getState() == VolumeBase::State::kMounted) return -EBUSY;
    }
    mForceProbe = true;
    return readPartitions();
}

status_t Disk::partitionInVm(fsvm::TableKind kind, int ratio, int64_t resetDeadline,
                             bool freshConfirmation) {
    // A disk the filesystem VM could not read is never partitioned (formatted), whoever asks: nothing showed that it
    // holds no data. An authorized reset expressly discards data on the selected medium.
    if (resetDeadline && fsvm::NowMs() >= resetDeadline) return -ETIMEDOUT;
    const bool mayReplaceUnverified = resetDeadline || freshConfirmation;
    if (mayReplaceUnverified && KernelScansPartitions(mSysPath)) return -ENOTSUP;
    if (!mayReplaceUnverified && mRecheckPending) return -EBUSY;
    if (!resetDeadline && mProbeFailed) {
        LOG(WARNING) << mId << ": the filesystem VM could not read this disk, not partitioning it";
        return -EBUSY;
    }
    if (kind == fsvm::TableKind::kMixed && (ratio < 10 || ratio > 90)) {
        LOG(ERROR) << "Mixed partition ratio must be between 10-90%";
        return -EINVAL;
    }
    // The confirmed medium is checked before requesting any old filesystem writeback.
    if (mEraseGeneration.empty() || mEraseBytes != mSize || !fsvm::FencesMedium(mDevice) ||
        !fsvm::IsMedium(mDevice, mEraseGeneration) || !fsvm::HasSize(mDevice, mEraseBytes)) {
        return -ENODEV;
    }
    if (mayReplaceUnverified) {
        const bool uncertain = std::any_of(mPendingFormats.begin(), mPendingFormats.end(),
                                          [](const auto& pending) { return !pending.writerStopped; });
        if (uncertain || (mDrive && mDrive->hasUnfinishedMutation())) return -EBUSY;
    }
    // Its volumes are unmounted first, so that a write back that fails shows: a volume whose last writes did not
    // reach the medium is never erased here. It stays, for a retry or to copy what can be read.
    for (const auto& vol : mVolumes) {
        if (vol->getState() == VolumeBase::State::kMounted) {
            if (vol->unmount() != OK &&
                (mayReplaceUnverified || vol->getState() == VolumeBase::State::kMounted)) return -EBUSY;
        }
    }
    if (mayReplaceUnverified && mDrive && mDrive->hasUnfinishedMutation()) return -EBUSY;
    for (const auto& vol : mVolumes) {
        if (!resetDeadline &&
            (vol->getState() == VolumeBase::State::kNotWrittenBack || notWrittenBack(vol->getId()))) {
            LOG(ERROR) << mId << ": " << vol->getId() << " was not written back, not erasing the disk";
            return -EBUSY;
        }
    }
    if (!resetDeadline && !mNotWrittenBack.empty()) {
        LOG(ERROR) << mId << ": " << *mNotWrittenBack.begin() << " was not written back, not erasing the disk";
        return -EBUSY;
    }
    // Only the medium the user chose (the one the last read saw), of its size then: checked before the disk is opened
    // and after, and everything goes through that descriptor. No partition tool runs here and nothing is read from
    // the medium: vold builds the table itself (fs/FsvmTable.cpp), as sgdisk wrote it.
    android::base::unique_fd fd;
    uint32_t blockSize = 0;
    if (!mProbe.holds(mEraseGeneration, mEraseBytes) ||
        fsvm::OpenToErase(mDevPath, mDevice, mEraseBytes, mEraseGeneration, &fd, &blockSize) != OK) {
        LOG(ERROR) << mId << ": not the medium that was read, not partitioning it";
        return -ENODEV;
    }
    fsvm::TableRequest r;
    r.kind = kind;
    r.diskBytes = mSize;
    r.blockSize = blockSize;
    r.ratio = ratio;
    r.diskGuid = fsvm::RandomGuid();
    // GUIDs for the partitions before android_expand (a public disk's one, kept only when it needs a GPT).
    if (kind != fsvm::TableKind::kPrivate) r.partGuids.push_back(fsvm::RandomGuid());
    // The private partition's GUID and its key, saved before the table is written (Disk::partitionMixed).
    std::string partGuid;
    KeyBuffer key;
    if (kind != fsvm::TableKind::kPublic) {
        r.partGuids.push_back(fsvm::RandomGuid());
        std::string partGuidRaw;
        fsvm::Guid expand;
        if (GenerateRandomUuid(partGuidRaw) != OK || !generate_volume_key(&key) ||
            StrToHex(partGuidRaw, partGuid) != OK || !fsvm::ParseGuid(partGuid, &expand)) {
            LOG(ERROR) << "Failed to generate GUID or key";
            return -EIO;
        }
        r.partGuids.push_back(expand);
    }
    fsvm::Table table;
    if (fsvm::BuildTable(r, &table) != OK) {
        LOG(ERROR) << mId << ": no table of this layout fits the disk";
        return -EINVAL;
    }
    if (kind != fsvm::TableKind::kPublic) {
        if (!WriteStringToFile(std::string(key.begin(), key.end()), BuildKeyPath(partGuid))) {
            LOG(ERROR) << "Failed to persist key";
            return -EIO;
        }
        LOG(DEBUG) << "Persisted key for GUID " << partGuid;
    }
    mEraseRequest = r;
    mEraseTable = table;
    return writeTableInVm(fd.get(), table, resetDeadline, freshConfirmation);
}

status_t Disk::writeTableInVm(int fd, const fsvm::Table& table, int64_t resetDeadline,
                             bool freshConfirmation) {
    if (auto res = destroyAllVolumes(); res != OK) return res;
    if (resetDeadline && fsvm::NowMs() >= resetDeadline) return -ETIMEDOUT;
    mJustPartitioned = true;
    mPartitionResult = -EINPROGRESS;
    if (status_t res = fsvm::WriteTable(fd, table, [this, fd] {
            return fsvm::EraseDescriptor(fd, mDevice, mEraseBytes, mEraseGeneration);
        }); res != OK) {
        LOG(ERROR) << mId << ": the new partition table was not written (" << res << ")";
        mJustPartitioned = false;
        // A partial table write forces a fresh probe even when the generation did not change.
        mForceProbe = true;
        readPartitions();
        return res;
    }
    if (resetDeadline || freshConfirmation) {
        mPendingFormats.clear();
        mRecheckPending = false;
    }
    if (resetDeadline) mNotWrittenBack.clear();
    // The filesystem VM reads the new table, and its new volumes are formatted (mJustPartitioned).
    mForceProbe = true;
    status_t res = readPartitions();
    return res == OK ? mPartitionResult : res;
}

void Disk::rememberReadOnlyVolumes() {
    for (const auto& vol : mVolumes) {
        if (vol->getType() != VolumeBase::Type::kPublic) continue;
        auto pub = std::static_pointer_cast<PublicVolume>(vol);
        if (pub->explicitReadOnlyInVm()) mReadOnlyParts.insert(pub->partInVm());
        if (pub->repairFailedInVm()) mFailedRepairParts.insert(pub->partInVm());
    }
}

void Disk::beforeDestroyInVm() {
    if (!isVmDisk()) return;
    // Keep the medium identity and read-only latches while a reset recreates its volumes.
    rememberReadOnlyVolumes();
    // Force a new table read without forgetting which medium the latches belong to.
    mForceProbe = true;
    mVerdictPending = false;
}

status_t Disk::fenceOldMedium() {
    if (mDrive) {
        if (auto res = mDrive->fenceBackings(true); res != OK) return res;
    }
    for (auto& entry : mRanges) {
        if (auto res = fsvm::FenceRange(&entry.second); res != OK) return res;
    }
    return OK;
}

void Disk::dropOldMedium() {
    LOG(WARNING) << mId << ": isolated the removed medium's backings before stopping its volumes";
    if (mDrive) mDrive->kill();
    for (const auto& vol : mVolumes) {
        // A VM disk's public volumes are all PublicVolume (createPublicVolumeInVm).
        if (vol->getType() == VolumeBase::Type::kPublic) static_cast<PublicVolume*>(vol.get())->dropMedium();
    }
}

void Disk::removeRanges() {
    for (const auto& [name, range] : mRanges) {
        // The held descriptor is released when the entry is removed.
        fsvm::DeleteRange(name, "/dev/block/vold/" + name);
    }
    mRanges.clear();
    // Its volumes are gone: an eject, a removal, a new read of the table. Its VM goes with them.
    if (mDrive) mDrive->end();
    mDrive.reset();
}

bool Disk::createPublicVolumeInVm(const fsvm::Part& part, bool repairFailed) {
    if (mPublicVolumes >= static_cast<int>(kMaxPublicPerDisk)) {
        LOG(WARNING) << mId << ": not serving partition " << part.index << ", " << kMaxPublicPerDisk
                     << " public volumes on this disk already";
        return false;
    }
    // Its ID (public:179,1) is the disk's minor plus the partition number: never one another disk's volume holds.
    const std::string volumeId = StringPrintf("public:%u,%u", major(mDevice), minor(mDevice) + part.index);
    if (VolumeManager::Instance()->findVolume(volumeId)) {
        LOG(ERROR) << mId << ": " << volumeId << " is another disk's volume, not serving partition " << part.index;
        return false;
    }
    mPublicVolumes++;
    // A placeholder (unread, waiting) is never served: no drive.
    auto drive = part.unread ? nullptr : mDrive;
    auto pub = std::make_shared<PublicVolume>(mDevice, mDevPath, mSysPath, mSize, part, drive);
    if (repairFailed) mFailedRepairParts.insert(part);
    if (mFailedRepairParts.count(part)) pub->markRepairFailed();
    if (mReadOnlyParts.count(part)) pub->markReadOnlyRequestedInVm();
    std::shared_ptr<VolumeBase> vol = pub;
    // Its disk is known to its format too (a format whose outcome is not read back has the disk read again).
    vol->setDiskId(getId());
    if (mJustPartitioned && mPartitionResult == OK) {
        LOG(DEBUG) << "Device just partitioned, silently formatting";
        vol->setSilent(true);
        vol->create();
        status_t formatted = vol->format("auto");
        if (formatted != OK) mPartitionResult = formatted;
        vol->destroy();
        vol->setSilent(false);
    }

    mVolumes.push_back(vol);
    vol->create();
    if (formatPending(part)) pub->markFormatPendingInVm(formatCompleted(part), formatWriterStopped(part));
    return true;
}

bool Disk::createPrivateVolumeInVm(const fsvm::Part& part, int maxMinors, bool retry) {
    // The key is selected by the partition GUID on this known disk generation, never a filesystem UUID.
    if (!fsvm::IsMedium(mDevice, part.diskSeq) || !mProbe.holds(part.diskSeq, mSize)) return false;
    // The volume's ID and its node's name come from the disk's own minors, as when Android's kernel numbered the
    // partition (private:179,2): only a partition number the disk has minors for (as for its public partitions), and
    // never an ID another volume holds, whatever disk it is on.
    dev_t idDevice;
    if (!fsvm::PartitionDevice(mDevice, part.index, maxMinors, &idDevice)) {
        LOG(ERROR) << mId << ": adoptable storage partition " << part.index << " is beyond " << maxMinors;
        return false;
    }
    const std::string volumeId = StringPrintf("private:%u,%u", major(idDevice), minor(idDevice));
    if (VolumeManager::Instance()->findVolume(volumeId)) {
        LOG(ERROR) << mId << ": " << volumeId << " is another disk's volume, not setting up its adoptable storage";
        return false;
    }
    // Adoptable storage: only a card this phone adopted has a key for its partition GUID. Without one (another phone's
    // card, or this phone's key missing or unreadable) nothing of the partition is set up, and the caller makes a
    // volume that could not be opened in its place: the disk never looks blank, and no format is offered for it.
    std::string normalizedGuid;
    std::string keyRaw;
    if (NormalizeHex(part.partGuid, normalizedGuid) ||
        !ReadFileToString(BuildKeyPath(normalizedGuid), &keyRaw)) {
        LOG(WARNING) << mId << ": no key for the adoptable storage partition " << part.partGuid;
        return false;
    }
    if (!fsvm::IsMedium(mDevice, part.diskSeq)) return false;
    // Remove any mapper left by a failed attempt before it can hold the lower range busy.
    if (retry && !DeleteDmDeviceWaiting(volumeId, true)) return false;
    // dm-crypt sits on a device-mapper range of exactly the partition the VM read, like each public partition's VM.
    // Android's kernel gets no partition of the card, so any number of public partitions can sit next to it (all
    // ranges hold the card as device-mapper, which may share it).
    std::string name = fsvm::RangeName(mDevice, part.index), node;
    if (auto old = mRanges.find(name); old != mRanges.end()) {
        if (fsvm::DeleteRange(name, "/dev/block/vold/" + name) != OK) return false;
        mRanges.erase(old);
    }
    dev_t rangeDevice;
    if (fsvm::CreateRange(name, mDevice, part.start, part.length, false, &node, &rangeDevice, part.diskSeq) != OK) {
        LOG(ERROR) << mId << ": could not set up the adoptable storage range";
        return false;
    }
    auto backing = fsvm::OpenRange(node, rangeDevice, true);
    if (backing < 0) {
        fsvm::DeleteRange(name, node);
        return false;
    }
    mRanges.emplace(name, fsvm::RangeBacking{name, std::move(backing), part.length, false});
    auto releaseRange = [&] {
        if (fsvm::DeleteRange(name, node) == OK) {
            mRanges.erase(name);
        }
    };
    // Its node (/dev/block/vold/private:179,2) is this range's: one left with another device goes first.
    const std::string volumeNode = "/dev/block/vold/" + volumeId;
    if (fsvm::ClaimNode(volumeNode, rangeDevice) != OK) {
        LOG(ERROR) << mId << ": the node of " << volumeId << " cannot be made for its range";
        if (retry) releaseRange();
        return false;
    }
    // The volume IDs stay what they were when Android's kernel had the partition (private:179,2).
    auto keyBuffer = KeyBuffer(keyRaw.begin(), keyRaw.end());
    auto vol = std::shared_ptr<VolumeBase>(new PrivateVolume(rangeDevice, idDevice, keyBuffer));
    if (mJustPartitioned && mPartitionResult == OK) {
        LOG(DEBUG) << "Device just partitioned, silently formatting";
        vol->setSilent(true);
        vol->create();
        status_t formatted = vol->format("auto");
        if (formatted != OK) mPartitionResult = formatted;
        vol->destroy();
        vol->setSilent(false);
    }
    mVolumes.push_back(vol);
    vol->setDiskId(getId());
    vol->setPartGuid(part.partGuid);
    if (vol->create(!retry) != OK && retry) {
        mVolumes.pop_back();
        DestroyDeviceNode(volumeNode);
        releaseRange();
        return false;
    }
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

void Disk::createUnopenableVolume(const fsvm::Part* missing) {
    // Not "unsupported, format it?": one volume that cannot be opened, and says so.
    mJustPartitioned = false;
    fsvm::Part whole;
    whole.length = mSize / 512;
    if (missing) {
        whole = *missing;
        mMissingParts[StringPrintf("public:%u,%u", major(mDevice), minor(mDevice) + whole.index)] = whole;
    }
    whole.unread = true;
    createPublicVolumeInVm(whole);
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
            bool volumes, disks;
            {
                std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
                PruneWaiting();
                volumes = !sWaitingVolumes.empty();
                disks = !sWaitingDisks.empty();
            }
            // Each by its own VM's size: a disk's probe takes less than a drive's smallest serving VM.
            const bool room = (disks && fsvm::RoomForVm(fsvm::kProbeVmMib)) ||
                              (volumes && fsvm::RoomForVm(fsvm::kServeVmMib));
            if (room && !sPlaceRetryScheduled.exchange(true)) RetryWaitingForPlace();
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
    std::vector<std::string> ids;
    for (const auto& vol : mVolumes) {
        // No RTTI in vold: the type says which volumes are public ones.
        if (vol->getType() == VolumeBase::Type::kPublic && vol->waitingForPlace()) ids.push_back(vol->getId());
    }
    for (const auto& id : ids) {
        LOG(INFO) << id << ": a protected VM place is free, made again for the framework to mount";
        remakePublicVolume(id, false);
    }
}

void Disk::remakePublicVolume(const std::string& id, bool repairFailed) {
    for (auto it = mVolumes.begin(); it != mVolumes.end(); ++it) {
        if ((*it)->getId() != id || (*it)->getType() != VolumeBase::Type::kPublic) continue;
        auto pub = std::static_pointer_cast<PublicVolume>(*it);
        const fsvm::Part part = pub->partInVm();
        if (pub->destroy() != OK) return;
        mVolumes.erase(it);
        if (mPublicVolumes > 0) mPublicVolumes--;
        if (pub->explicitReadOnlyInVm()) mReadOnlyParts.insert(part);
        if (pub->repairFailedInVm()) mFailedRepairParts.insert(part);
        createPublicVolumeInVm(part, repairFailed);
        return;
    }
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
        case Reprobe::kAfterFormat:
            break;
    }
    if (why == Reprobe::kMountRetry) mMountRetryPending = false;
    if (why == Reprobe::kAfterFormat && !mRecheckPending) return;
    // Only while nothing of the disk is mounted, and (a failed probe) it still shows the result of that probe or
    // (a failed mount) a volume still could not be opened. The new volumes are mounted by the framework, as at
    // insertion.
    bool unopenable = false;
    for (const auto& vol : mVolumes) {
        if (vol->getState() == VolumeBase::State::kMounted) {
            return;
        }
        if (vol->getState() == VolumeBase::State::kUnopenable) unopenable = true;
    }
    if (why == Reprobe::kFailedProbe && !mProbeFailed) return;
    if (why == Reprobe::kMountRetry && !unopenable) return;
    LOG(INFO) << mId << ": reading the partition table again"
              << (why == Reprobe::kMountRetry    ? " to mount it again"
                  : why == Reprobe::kAfterFormat ? " after a format whose outcome was not read back"
                                                 : "");
    mForceProbe = why == Reprobe::kMountRetry || why == Reprobe::kAfterFormat;
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

void Disk::recheckAfterFormat() {
    // The disk is read again (a new probe) once the format's volume is no longer in use: what is on the medium now
    // decides what the volume shows, and nothing offers to format it again on a guess.
    if (mRecheckPending) return;
    mRecheckPending = true;
    ScheduleReprobe(getId(), 3, Reprobe::kAfterFormat);
}

void Disk::recheckAfterFormat(const fsvm::Part& part, const std::string& fsType, bool completed,
                              bool writerStopped) {
    mPendingFormats.push_back({part, fsType, completed, writerStopped});
    recheckAfterFormat();
}

bool Disk::formatPending(const fsvm::Part& part) const {
    return std::any_of(mPendingFormats.begin(), mPendingFormats.end(), [&](const auto& expected) {
        return (part.diskSeq.empty() || expected.part.diskSeq.empty() || part.diskSeq == expected.part.diskSeq) &&
               part.start < expected.part.start + expected.part.length && expected.part.start < part.start + part.length;
    });
}

bool Disk::formatCompleted(const fsvm::Part& part) const {
    return std::any_of(mPendingFormats.begin(), mPendingFormats.end(), [&](const auto& expected) {
        return expected.completed && part.start == expected.part.start && part.length == expected.part.length &&
               !part.diskSeq.empty() && part.diskSeq == expected.part.diskSeq;
    });
}

bool Disk::formatWriterStopped(const fsvm::Part& part) const {
    return std::any_of(mPendingFormats.begin(), mPendingFormats.end(), [&](const auto& expected) {
        return expected.writerStopped && part.start == expected.part.start && part.length == expected.part.length &&
               !part.diskSeq.empty() && part.diskSeq == expected.part.diskSeq;
    });
}

void Disk::resumeFormatVerification() {
    if (mRecheckPending) ScheduleReprobe(getId(), 1, Reprobe::kAfterFormat);
}

void Disk::noteMounted(const std::string& volumeId) {
    mMountRetries = 0;
    // Mounted again: what it holds can be seen, and it may be formatted as any other volume.
    mNotWrittenBack.erase(volumeId);
}

void Disk::noteNotWrittenBack(const std::string& volumeId) {
    // Kept for this medium whatever the volume's state becomes (a retry that fails, a vold reset, a new read of the
    // table): its last writes did not reach the medium, and nothing erases it until it mounted again.
    mNotWrittenBack.insert(volumeId);
}

bool Disk::notWrittenBack(const std::string& volumeId) const {
    return mNotWrittenBack.count(volumeId) > 0;
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
    switch (mProbe.next(seq, mSize, fsvm::NowMs(), forced, mJustPartitioned, mProbeFailed)) {
        case fsvm::ProbeMemory::Next::kRead:
            break;
        case fsvm::ProbeMemory::Next::kSkip:
            LOG(VERBOSE) << mId << ": nothing changed, the partition table is not read again";
            return OK;
        case fsvm::ProbeMemory::Next::kLater:
            if (fsvm::MediumChanged(mDevice, mProbe.mediumRead())) {
                // A new medium (a card changed in a USB reader): the volumes and partitions of the old one go now,
                // not at the probe that is put off. Android's kernel keeps its partitions across a media change.
                LOG(INFO) << mId << ": the medium changed, dropping the old one's volumes until it is read";
                if (auto res = destroyAllVolumes(); res != OK) return res;
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
    // Keep the original identity and attachments until teardown has passed preflight.
    if (hasVmBackings() && android::base::Trim(seq).empty()) {
        mForceProbe = true;
        return -EAGAIN;
    }
    rememberReadOnlyVolumes();
    if (auto res = destroyAllVolumes(); res != OK) {
        mForceProbe = true;
        return res;
    }
    const bool newMedium = mProbe.took(seq, mSize, fsvm::NowMs());

    // Fail closed: a kernel that reads this disk's table itself gets no volume served from it.
    // The compile-time CONFIG_BLK_NO_EXTERNAL_PART_SCAN option requires built-in SD and MMC block drivers.
    const bool kernelScans = KernelScansPartitions(mSysPath);
    if (kernelScans) {
        LOG(ERROR) << mId << ": Android's kernel reads this disk's partition table "
                   << "(CONFIG_BLK_NO_EXTERNAL_PART_SCAN is missing), not serving it";
    }

    if (newMedium) {
        mReadOnlyParts.clear();
        mFailedRepairParts.clear();
        mPendingFormats.clear();
        mRecheckPending = false;
        mMissingParts.clear();
        // Another medium: what the old one's volumes did not write back concerns that one only.
        mNotWrittenBack.clear();
    }
    mMissingParts.clear();
    mPublicVolumes = 0;
    // Ranges left from before (vold restarted, the medium changed) go before the disk is read again, and those of
    // disks that are gone.
    fsvm::DeleteRangesOf(mDevice);
    fsvm::SweepRanges();

    // No medium (an empty slot in a USB card reader): nothing to read, as when sgdisk failed before.
    if (mSize == static_cast<uint64_t>(-1) || mSize < 512) {
        LOG(WARNING) << mId << " has no medium";
        sendScanned(false);
        if (mJustPartitioned) mPartitionResult = -EIO;
        mJustPartitioned = false;
        return OK;
    }

    // Probes are limited for all disks together: a device that detaches and attaches in a loop is a new disk each
    // time. A disk over the limit shows as one that could not be opened, and is read when a probe is free again.
    // A format the user asked for is not put off.
    int64_t waitMs = 0;
    if (!kernelScans && !mJustPartitioned && !confirming && !fsvm::TakeProbe(fsvm::NowMs(), &waitMs)) {
        LOG(WARNING) << mId << ": too many disks were read lately, reading this one in " << waitMs << " ms";
        fsvm::DeleteAllPartitions(mDevPath, mSysPath);
        createUnopenableVolume();
        mProbeFailed = true;
        ScheduleDeferred(getId(), static_cast<int>((waitMs + 999) / 1000));
        sendScanned(false);
        return OK;
    }

    // Android's kernel did not read the table and sgdisk does not either: the filesystem VM does. Partitions vold
    // added before, or a kernel without the option made, go first.
    fsvm::Layout layout;
    EnsurePlaceHooks();
    status_t probed = kernelScans ? -EIO :
            (mReadLayout ? mReadLayout(mDevPath, mSize, &layout) : fsvm::DeleteAllPartitions(mDevPath, mSysPath));
    if (probed == OK && !mReadLayout) probed = fsvm::Probe(mDevPath, mSize, &layout);
    if (probed == -EAGAIN) {
        // No protected VM place: the disk's placeholder says it waits for one (VOLUME_STATE_WAITING_FOR_PLACE) and the
        // disk is read as soon as a place is free, never shared with another drive's VM and never given up.
        LOG(WARNING) << mId << ": waiting for a protected VM place to read the disk";
        mProbeFailed = true;
        mWaitingForPlace = true;
        if (mJustPartitioned) mPartitionResult = -EAGAIN;
        createWaitingVolume();
        waitForPlaceDisk(getId());
        sendScanned(false);
        if (mJustPartitioned) mPartitionResult = -EIO;
        mJustPartitioned = false;
        return OK;
    }
    if (mJustPartitioned && (probed != OK || seq.empty() ||
        android::base::Trim(seq) != android::base::Trim(mEraseGeneration) || mSize != mEraseBytes ||
        !fsvm::IsMedium(mDevice, mEraseGeneration) || !fsvm::HasSize(mDevice, mEraseBytes))) {
        mJustPartitioned = false;
        mPartitionResult = -ENODEV;
        probed = -ENODEV;
    }
    mProbeFailed = probed != OK;
    // Everything made from this answer is bound to the medium it was read from (sequence read before the probe: a
    // medium changed during the probe fails the checks, never the other way round).
    layout.diskSeq = android::base::Trim(seq);
    layout.diskBytes = mSize;
    for (auto& p : layout.parts) p.diskSeq = layout.diskSeq;
    if (probed == OK) {
        mPendingFormats.erase(std::remove_if(mPendingFormats.begin(), mPendingFormats.end(), [&](const auto& expected) {
            // A probe describes the filesystem. It cannot certify that its previous writer exited.
            if (!expected.writerStopped || expected.part.diskSeq.empty() ||
                expected.part.diskSeq != layout.diskSeq) return false;
            const bool verified = std::any_of(layout.parts.begin(), layout.parts.end(), [&](const auto& part) {
                return part.cls == fsvm::Part::Class::kPublic && part.start == expected.part.start &&
                       part.length == expected.part.length && part.fsType == expected.fsType &&
                       !part.fsUuid.empty() && !part.damaged && !part.unread &&
                       (expected.part.fsUuid.empty() ||
                        !android::base::EqualsIgnoreCase(part.fsUuid, expected.part.fsUuid));
            });
            // Verification retires the pending format. The host's failed-repair hold stays.
            return verified;
        }), mPendingFormats.end());
        mRecheckPending = !mPendingFormats.empty();
    }
    if (mJustPartitioned && probed == OK && fsvm::MatchesTable(layout, mEraseRequest, mEraseTable)) {
        mPartitionResult = OK;
    }
    if (mJustPartitioned && !fsvm::MatchesTable(layout, mEraseRequest, mEraseTable)) {
        mJustPartitioned = false;
        mPartitionResult = -EIO;
        mProbeFailed = true;
    }
    if (mProbeFailed) {
        if (!mEraseGeneration.empty()) mPartitionResult = -EIO;
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
        if (mJustPartitioned) mPartitionResult = -EIO;
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
        // One VM for all public partitions of the drive, each a disk of it: all of them given at its start.
        mDrive = std::make_shared<fsvm::Drive>(mDevice, mDevPath, getId(), mSize);
        mDrive->setParts(fsvm::DriveParts(layout, static_cast<uint32_t>(maxMinors), kMaxPublicPerDisk));
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
                    if (!createPrivateVolumeInVm(part, maxMinors)) {
                        if (mJustPartitioned) mPartitionResult = -EIO;
                        createUnopenableVolume(&part);
                    }
                    break;
                case fsvm::Part::Class::kMeta:
                    break;
            }
        }
        if (mJustPartitioned && skipped) mPartitionResult = -EIO;
        // Partitions that hold data but could not be served must not leave a disk without volumes: that would look
        // blank and get the set-up (format) offer.
        if (skipped && mVolumes.empty()) {
            LOG(WARNING) << mId << ": no partition of this disk could be served";
            createUnopenableVolume();
            mProbeFailed = true;  // and the disk is not partitioned (formatted) either
        }
    }

    // A format is offered only for a disk two VMs found blank that has no volume: never for one that could not be
    // read or whose adoptable storage has no key.
    if (mRecheckPending && mVolumes.empty()) createUnopenableVolume();
    sendScanned(!mRecheckPending && fsvm::OffersFormatOnScan(mProbeFailed, layout, !mVolumes.empty()));

    mJustPartitioned = false;
    return OK;
}

void Disk::sendScanned(bool offerFormat) {
    auto listener = VolumeManager::Instance()->getListener();
    if (!listener) return;
    listener->onDiskMedium(getId(), android::base::Trim(mProbe.mediumRead()), mSize);
    listener->onDiskFormatOffer(getId(), offerFormat);
    listener->onDiskScanned(getId());
}

}  // namespace vold
}  // namespace android
