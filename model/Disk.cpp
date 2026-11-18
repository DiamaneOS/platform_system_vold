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

#include "Disk.h"
#include "FsCrypt.h"
#include "PrivateVolume.h"
#include "PublicVolume.h"
#include "RandUtils.h"
#include "Utils.h"
#include "VolumeBase.h"
#include "VolumeEncryption.h"
#include "VolumeManager.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <fscrypt/fscrypt.h>

#include <fcntl.h>
#include <inttypes.h>
#include <chrono>
#include <mutex>
#include <thread>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <vector>

using android::base::ReadFileToString;
using android::base::StringPrintf;
using android::base::WriteStringToFile;

namespace android {
namespace vold {

static const char* kSgdiskPath = "/system/bin/sgdisk";
static const char* kSgdiskToken = " \t\n";

static const char* kSysfsLoopMaxMinors = "/sys/module/loop/parameters/max_part";
static const char* kSysfsMmcMaxMinorsDeprecated = "/sys/module/mmcblk/parameters/perdev_minors";
static const char* kSysfsMmcMaxMinors = "/sys/module/mmc_block/parameters/perdev_minors";

static const unsigned int kMajorBlockLoop = 7;
static const unsigned int kMajorBlockScsiA = 8;
static const unsigned int kMajorBlockScsiB = 65;
static const unsigned int kMajorBlockScsiC = 66;
static const unsigned int kMajorBlockScsiD = 67;
static const unsigned int kMajorBlockScsiE = 68;
static const unsigned int kMajorBlockScsiF = 69;
static const unsigned int kMajorBlockScsiG = 70;
static const unsigned int kMajorBlockScsiH = 71;
static const unsigned int kMajorBlockScsiI = 128;
static const unsigned int kMajorBlockScsiJ = 129;
static const unsigned int kMajorBlockScsiK = 130;
static const unsigned int kMajorBlockScsiL = 131;
static const unsigned int kMajorBlockScsiM = 132;
static const unsigned int kMajorBlockScsiN = 133;
static const unsigned int kMajorBlockScsiO = 134;
static const unsigned int kMajorBlockScsiP = 135;
static const unsigned int kMajorBlockMmc = 179;
static const unsigned int kMajorBlockDynamicMin = 234;
static const unsigned int kMajorBlockDynamicMax = 512;

static const char* kGptBasicData = "EBD0A0A2-B9E5-4433-87C0-68B6B72699C7";
static const char* kGptAndroidMeta = "19A710A2-B3CA-11E4-B026-10604B889DCF";
static const char* kGptAndroidExpand = "193D1EA4-B3CA-11E4-B075-10604B889DCF";

enum class Table {
    kUnknown,
    kMbr,
    kGpt,
};

static bool isNvmeBlkDevice(unsigned int major, const std::string& sysPath) {
    return sysPath.find("nvme") != std::string::npos && major >= kMajorBlockDynamicMin &&
           major <= kMajorBlockDynamicMax;
}

Disk::Disk(const std::string& eventPath, dev_t device, const std::string& nickname, int flags)
    : mDevice(device),
      mSize(-1),
      mNickname(nickname),
      mFlags(flags),
      mCreated(false),
      mJustPartitioned(false) {
    mId = StringPrintf("disk:%u,%u", major(device), minor(device));
    mEventPath = eventPath;
    mSysPath = StringPrintf("/sys/%s", eventPath.c_str());
    mDevPath = StringPrintf("/dev/block/vold/%s", mId.c_str());
    CreateDeviceNode(mDevPath, mDevice);
}

Disk::~Disk() {
    CHECK(!mCreated);
    DestroyDeviceNode(mDevPath);
}

std::shared_ptr<VolumeBase> Disk::findVolume(const std::string& id) {
    for (auto vol : mVolumes) {
        if (vol->getId() == id) {
            return vol;
        }
        auto stackedVol = vol->findVolume(id);
        if (stackedVol != nullptr) {
            return stackedVol;
        }
    }
    return nullptr;
}

void Disk::listVolumes(VolumeBase::Type type, std::list<std::string>& list) const {
    for (const auto& vol : mVolumes) {
        if (vol->getType() == type) {
            list.push_back(vol->getId());
        }
        // TODO: consider looking at stacked volumes
    }
}

std::vector<std::shared_ptr<VolumeBase>> Disk::getVolumes() const {
    std::vector<std::shared_ptr<VolumeBase>> vols;
    for (const auto& vol : mVolumes) {
        vols.push_back(vol);
        auto stackedVolumes = vol->getVolumes();
        vols.insert(vols.end(), stackedVolumes.begin(), stackedVolumes.end());
    }

    return vols;
}

status_t Disk::create() {
    CHECK(!mCreated);
    mCreated = true;

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskCreated(getId(), mFlags);

    if (isStub()) {
        createStubVolume();
        return OK;
    }
    readMetadata();
    readPartitions();
    return OK;
}

status_t Disk::destroy() {
    CHECK(mCreated);
    destroyAllVolumes();
    mCreated = false;

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskDestroyed(getId());

    return OK;
}

void Disk::createPublicVolume(dev_t device) {
    auto vol = std::shared_ptr<VolumeBase>(new PublicVolume(device));
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
}

void Disk::createPrivateVolume(dev_t device, const std::string& partGuid) {
    std::string normalizedGuid;
    if (NormalizeHex(partGuid, normalizedGuid)) {
        LOG(WARNING) << "Invalid GUID " << partGuid;
        return;
    }

    std::string keyRaw;
    if (!ReadFileToString(BuildKeyPath(normalizedGuid), &keyRaw)) {
        PLOG(ERROR) << "Failed to load key for GUID " << normalizedGuid;
        return;
    }

    LOG(DEBUG) << "Found key for GUID " << normalizedGuid;

    auto keyBuffer = KeyBuffer(keyRaw.begin(), keyRaw.end());
    auto vol = std::shared_ptr<VolumeBase>(new PrivateVolume(device, keyBuffer));
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
    vol->setPartGuid(partGuid);
    vol->create();
}

void Disk::createStubVolume() {
    CHECK(mVolumes.size() == 1);
    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskMetadataChanged(getId(), mSize, mLabel, mSysPath);
    if (listener) listener->onDiskScanned(getId());
    mVolumes[0]->setDiskId(getId());
    mVolumes[0]->create();
}

void Disk::destroyAllVolumes() {
    for (const auto& vol : mVolumes) {
        vol->destroy();
    }
    mVolumes.clear();
#ifdef VOLD_PUBLIC_FS_IN_VM
    removeAddedPartitions();
#endif
}

bool Disk::isVmDisk() const {
    unsigned int m = major(mDevice);
    // clang-format off
    switch (m) {
        case kMajorBlockMmc:
        case kMajorBlockScsiA: case kMajorBlockScsiB: case kMajorBlockScsiC:
        case kMajorBlockScsiD: case kMajorBlockScsiE: case kMajorBlockScsiF:
        case kMajorBlockScsiG: case kMajorBlockScsiH: case kMajorBlockScsiI:
        case kMajorBlockScsiJ: case kMajorBlockScsiK: case kMajorBlockScsiL:
        case kMajorBlockScsiM: case kMajorBlockScsiN: case kMajorBlockScsiO:
        case kMajorBlockScsiP:
            return true;
        default:
            return false;
    }
    // clang-format on
}

void Disk::removeAddedPartitions() {
    for (uint32_t index : mAddedPartitions) {
        fsvm::DeletePartition(mDevPath, index);
    }
    mAddedPartitions.clear();
}

void Disk::createPublicVolumeInVm(const fsvm::Part& part) {
    // A crafted table can list many FAT partitions. Each served one holds a VM.
    static constexpr int kMaxPublicPerDisk = 4;
    if (mPublicVolumes >= kMaxPublicPerDisk) {
        LOG(WARNING) << mId << ": not serving partition " << part.index << ", " << kMaxPublicPerDisk
                     << " public volumes on this disk already";
        return;
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

void Disk::reprobeInVm(bool changed) {
    if (changed) {
        mReprobePending = false;
        readPartitions();
        return;
    }
    // Only while the disk still shows the result of a failed probe, and nothing is mounted from it.
    if (!mProbeFailed) return;
    for (const auto& vol : mVolumes) {
        if (vol->getState() == VolumeBase::State::kMounted) return;
    }
    LOG(INFO) << mId << ": reading the partition table again";
    readPartitions();
}

static void ScheduleReprobe(const std::string& diskId, int delaySeconds, bool changed) {
    std::thread([diskId, delaySeconds, changed] {
        std::this_thread::sleep_for(std::chrono::seconds(delaySeconds));
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        auto disk = VolumeManager::Instance()->findDisk(diskId);
        if (disk) disk->reprobeInVm(changed);
    }).detach();
}

static int64_t BootMs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

status_t Disk::readPartitionsInVm(int maxMinors) {
    // A change event without a change (same medium sequence, same size) probes nothing: a device that sends them in
    // a loop must not keep vold starting VMs. A real change comes at most once in 3 s, later ones are folded into one.
    std::string seq;
    ReadFileToString(mSysPath + "/diskseq", &seq);
    if (mProbed && !mJustPartitioned && !mProbeFailed && seq == mProbedSeq && mSize == mProbedSize) {
        LOG(VERBOSE) << mId << ": nothing changed, the partition table is not read again";
        return OK;
    }
    if (mProbed && !mJustPartitioned && BootMs() - mProbedAtMs < 3000) {
        if (!mReprobePending) {
            mReprobePending = true;
            ScheduleReprobe(getId(), 3, true);
        }
        return OK;
    }
    mProbed = true;
    mProbedSeq = seq;
    mProbedSize = mSize;
    mProbedAtMs = BootMs();

    std::string partscan;
    if (ReadFileToString(mSysPath + "/partscan", &partscan) && android::base::Trim(partscan) != "0") {
        LOG(ERROR) << mId << ": Android's kernel reads this disk's partition table "
                   << "(CONFIG_BLK_NO_EXTERNAL_PART_SCAN is missing)";
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

    // Android's kernel did not read the table and sgdisk does not either: the filesystem VM does. Partitions vold
    // added before, or a kernel without the option made, go first.
    fsvm::Layout layout;
    mProbeFailed = fsvm::DeleteAllPartitions(mDevPath, mSysPath) != OK ||
                   fsvm::Probe(mDevPath, mSize, &layout) != OK;
    if (mProbeFailed) {
        LOG(ERROR) << mId << ": the filesystem VM could not read the disk";
        createUnopenableVolume();
        // A protected VM start can be refused for a while after boot: try twice more, later.
        if (++mProbeFailures <= 2) ScheduleReprobe(getId(), mProbeFailures == 1 ? 15 : 60, false);
    } else {
        mProbeFailures = 0;
        for (const auto& part : layout.parts) {
            switch (part.cls) {
                case fsvm::Part::Class::kPublic:
                    if (part.index > static_cast<uint32_t>(maxMinors)) {
                        LOG(WARNING) << mId << ": partition " << part.index << " is beyond " << maxMinors;
                        break;
                    }
                    createPublicVolumeInVm(part);
                    break;
                case fsvm::Part::Class::kExpand:
                    // This phone's adoptable storage that could not be added must not look unsupported (and
                    // offered for formatting): it shows as a card that could not be opened.
                    if (!createPrivateVolumeInVm(part)) createUnopenableVolume();
                    break;
                case fsvm::Part::Class::kMeta:
                    break;
            }
        }
    }

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskScanned(getId());

    mJustPartitioned = false;
    return OK;
}

status_t Disk::readMetadata() {
    mSize = -1;
    mLabel.clear();

    if (GetBlockDevSize(mDevPath, &mSize) != OK) {
        mSize = -1;
    }

    unsigned int majorId = major(mDevice);
    switch (majorId) {
        case kMajorBlockLoop: {
            mLabel = "Virtual";
            break;
        }
        // clang-format off
        case kMajorBlockScsiA: case kMajorBlockScsiB: case kMajorBlockScsiC:
        case kMajorBlockScsiD: case kMajorBlockScsiE: case kMajorBlockScsiF:
        case kMajorBlockScsiG: case kMajorBlockScsiH: case kMajorBlockScsiI:
        case kMajorBlockScsiJ: case kMajorBlockScsiK: case kMajorBlockScsiL:
        case kMajorBlockScsiM: case kMajorBlockScsiN: case kMajorBlockScsiO:
        case kMajorBlockScsiP: {
            // clang-format on
            std::string path(mSysPath + "/device/vendor");
            std::string tmp;
            if (!ReadFileToString(path, &tmp)) {
                PLOG(WARNING) << "Failed to read vendor from " << path;
                return -errno;
            }
            tmp = android::base::Trim(tmp);
            mLabel = tmp;
            break;
        }
        case kMajorBlockMmc: {
            std::string path(mSysPath + "/device/manfid");
            std::string tmp;
            if (!ReadFileToString(path, &tmp)) {
                PLOG(WARNING) << "Failed to read manufacturer from " << path;
                return -errno;
            }
            tmp = android::base::Trim(tmp);
            int64_t manfid;
            if (!android::base::ParseInt(tmp, &manfid)) {
                PLOG(WARNING) << "Failed to parse manufacturer " << tmp;
                return -EINVAL;
            }
            // Our goal here is to give the user a meaningful label, ideally
            // matching whatever is silk-screened on the card.  To reduce
            // user confusion, this list doesn't contain white-label manfid.
            switch (manfid) {
                // clang-format off
                case 0x000003: mLabel = "SanDisk"; break;
                case 0x00001b: mLabel = "Samsung"; break;
                case 0x000028: mLabel = "Lexar"; break;
                case 0x000074: mLabel = "Transcend"; break;
                    // clang-format on
            }
            break;
        }
        default: {
            if (IsVirtioBlkDevice(majorId)) {
                LOG(DEBUG) << "Recognized experimental block major ID " << majorId
                           << " as virtio-blk (emulator's virtual SD card device)";
                mLabel = "Virtual";
                break;
            }
            if (isNvmeBlkDevice(majorId, mSysPath)) {
                std::string path(mSysPath + "/device/model");
                std::string tmp;
                if (!ReadFileToString(path, &tmp)) {
                    PLOG(WARNING) << "Failed to read vendor from " << path;
                    return -errno;
                }
                mLabel = tmp;
                break;
            }
            LOG(WARNING) << "Unsupported block major type " << majorId;
            return -ENOTSUP;
        }
    }

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskMetadataChanged(getId(), mSize, mLabel, mSysPath);

    return OK;
}

status_t Disk::readPartitions() {
    int maxMinors = getMaxMinors();
    if (maxMinors < 0) {
        return -ENOTSUP;
    }

#ifdef VOLD_PUBLIC_FS_IN_VM
    if (isVmDisk()) {
        return readPartitionsInVm(maxMinors);
    }
#endif

    destroyAllVolumes();

    // Parse partition table

    std::vector<std::string> cmd;
    cmd.push_back(kSgdiskPath);
    cmd.push_back("--android-dump");
    cmd.push_back(mDevPath);

    std::vector<std::string> output;
    status_t res = ForkExecvp(cmd, &output);
    if (res != OK) {
        LOG(WARNING) << "sgdisk failed to scan " << mDevPath;

        auto listener = VolumeManager::Instance()->getListener();
        if (listener) listener->onDiskScanned(getId());

        mJustPartitioned = false;
        return res;
    }

    Table table = Table::kUnknown;
    bool foundParts = false;
    for (const auto& line : output) {
        auto split = android::base::Split(line, kSgdiskToken);
        auto it = split.begin();
        if (it == split.end()) continue;

        if (*it == "DISK") {
            if (++it == split.end()) continue;
            if (*it == "mbr") {
                table = Table::kMbr;
            } else if (*it == "gpt") {
                table = Table::kGpt;
            } else {
                LOG(WARNING) << "Invalid partition table " << *it;
                continue;
            }
        } else if (*it == "PART") {

            if (++it == split.end()) continue;
            int i = 0;
            if (!android::base::ParseInt(*it, &i, 1, maxMinors)) {
                LOG(WARNING) << "Invalid partition number " << *it;
                continue;
            }
            dev_t partDevice = makedev(major(mDevice), minor(mDevice) + i);

            if (table == Table::kMbr) {
                if (++it == split.end()) continue;
                int type = 0;
                if (!android::base::ParseInt("0x" + *it, &type)) {
                    LOG(WARNING) << "Invalid partition type " << *it;
                    continue;
                }

                switch (type) {
                    case 0x06:  // FAT16
                    case 0x07:  // HPFS/NTFS/exFAT
                    case 0x0b:  // W95 FAT32 (LBA)
                    case 0x0c:  // W95 FAT32 (LBA)
                    case 0x0e:  // W95 FAT16 (LBA)
                        createPublicVolume(partDevice);
                        foundParts = true;
                        break;
                }
            } else if (table == Table::kGpt) {
                if (++it == split.end()) continue;
                auto typeGuid = *it;
                if (++it == split.end()) continue;
                auto partGuid = *it;

                if (android::base::EqualsIgnoreCase(typeGuid, kGptBasicData)) {
                    createPublicVolume(partDevice);
                    foundParts = true;
                } else if (android::base::EqualsIgnoreCase(typeGuid, kGptAndroidExpand)) {
                    createPrivateVolume(partDevice, partGuid);
                    foundParts = true;
                }
            }
        }
    }

    // Ugly last ditch effort, treat entire disk as partition
    if (table == Table::kUnknown || !foundParts) {
        LOG(WARNING) << mId << " has unknown partition table; trying entire device";

        std::string fsType;
        std::string unused;
        if (ReadMetadataUntrusted(mDevPath, &fsType, &unused, &unused) == OK) {
            createPublicVolume(mDevice);
        } else {
            LOG(WARNING) << mId << " failed to identify, giving up";
        }
    }

    auto listener = VolumeManager::Instance()->getListener();
    if (listener) listener->onDiskScanned(getId());

    mJustPartitioned = false;
    return OK;
}

void Disk::initializePartition(std::shared_ptr<StubVolume> vol) {
    CHECK(isStub());
    CHECK(mVolumes.empty());
    mVolumes.push_back(vol);
}

status_t Disk::unmountAll() {
    for (const auto& vol : mVolumes) {
        vol->unmount();
    }
    return OK;
}

status_t Disk::partitionPublic() {
    int res;

    destroyAllVolumes();
    mJustPartitioned = true;

#ifdef VOLD_PUBLIC_FS_IN_VM
    // sgdisk below writes the new table. Zeroing the old one first means it never reads a table from the card.
    if (isVmDisk() && fsvm::ZeroTables(mDevPath, mSize) != OK) {
        LOG(ERROR) << "Failed to clear the old partition table";
        mJustPartitioned = false;
        return -EIO;
    }
#endif

    // First nuke any existing partition table
    std::vector<std::string> cmd;
    cmd.push_back(kSgdiskPath);
    cmd.push_back("--zap-all");
    cmd.push_back(mDevPath);

    // Zap sometimes returns an error when it actually succeeded, so
    // just log as warning and keep rolling forward.
    if ((res = ForkExecvp(cmd)) != 0) {
        LOG(WARNING) << "Failed to zap; status " << res;
    }

    // Now let's build the new MBR table. We heavily rely on sgdisk to
    // force optimal alignment on the created partitions.
    cmd.clear();
    cmd.push_back(kSgdiskPath);
    cmd.push_back("--new=0:0:-0");
    cmd.push_back("--typecode=0:0c00");
    cmd.push_back("--gpttombr=1");
    cmd.push_back(mDevPath);

    if ((res = ForkExecvp(cmd)) != 0) {
        LOG(ERROR) << "Failed to partition; status " << res;
        return res;
    }

#ifdef VOLD_PUBLIC_FS_IN_VM
    // Android's kernel does not reread a table it never read. The VM reads the new one, and the volumes come from it.
    if (isVmDisk()) return readPartitions();
#endif
    return OK;
}

status_t Disk::partitionPrivate() {
    return partitionMixed(0);
}

status_t Disk::partitionMixed(int8_t ratio) {
    int res;

    destroyAllVolumes();
    mJustPartitioned = true;

#ifdef VOLD_PUBLIC_FS_IN_VM
    // sgdisk below writes the new table. Zeroing the old one first means it never reads a table from the card.
    if (isVmDisk() && fsvm::ZeroTables(mDevPath, mSize) != OK) {
        LOG(ERROR) << "Failed to clear the old partition table";
        mJustPartitioned = false;
        return -EIO;
    }
#endif

    // First nuke any existing partition table
    std::vector<std::string> cmd;
    cmd.push_back(kSgdiskPath);
    cmd.push_back("--zap-all");
    cmd.push_back(mDevPath);

    // Zap sometimes returns an error when it actually succeeded, so
    // just log as warning and keep rolling forward.
    if ((res = ForkExecvp(cmd)) != 0) {
        LOG(WARNING) << "Failed to zap; status " << res;
    }

    // We've had some success above, so generate both the private partition
    // GUID and encryption key and persist them.
    std::string partGuidRaw;
    if (GenerateRandomUuid(partGuidRaw) != OK) {
        LOG(ERROR) << "Failed to generate GUID";
        return -EIO;
    }

    KeyBuffer key;
    if (!generate_volume_key(&key)) {
        LOG(ERROR) << "Failed to generate key";
        return -EIO;
    }
    std::string keyRaw(key.begin(), key.end());

    std::string partGuid;
    StrToHex(partGuidRaw, partGuid);

    if (!WriteStringToFile(keyRaw, BuildKeyPath(partGuid))) {
        LOG(ERROR) << "Failed to persist key";
        return -EIO;
    } else {
        LOG(DEBUG) << "Persisted key for GUID " << partGuid;
    }

    // Now let's build the new GPT table. We heavily rely on sgdisk to
    // force optimal alignment on the created partitions.
    cmd.clear();
    cmd.push_back(kSgdiskPath);

    // If requested, create a public partition first. Mixed-mode partitioning
    // like this is an experimental feature.
    if (ratio > 0) {
        if (ratio < 10 || ratio > 90) {
            LOG(ERROR) << "Mixed partition ratio must be between 10-90%";
            return -EINVAL;
        }

        uint64_t splitMb = ((mSize / 100) * ratio) / 1024 / 1024;
        cmd.push_back(StringPrintf("--new=0:0:+%" PRId64 "M", splitMb));
        cmd.push_back(StringPrintf("--typecode=0:%s", kGptBasicData));
        cmd.push_back("--change-name=0:shared");
    }

    // Define a metadata partition which is designed for future use; there
    // should only be one of these per physical device, even if there are
    // multiple private volumes.
    cmd.push_back("--new=0:0:+16M");
    cmd.push_back(StringPrintf("--typecode=0:%s", kGptAndroidMeta));
    cmd.push_back("--change-name=0:android_meta");

    // Define a single private partition filling the rest of disk.
    cmd.push_back("--new=0:0:-0");
    cmd.push_back(StringPrintf("--typecode=0:%s", kGptAndroidExpand));
    cmd.push_back(StringPrintf("--partition-guid=0:%s", partGuid.c_str()));
    cmd.push_back("--change-name=0:android_expand");

    cmd.push_back(mDevPath);

    if ((res = ForkExecvp(cmd)) != 0) {
        LOG(ERROR) << "Failed to partition; status " << res;
        return res;
    }

#ifdef VOLD_PUBLIC_FS_IN_VM
    if (isVmDisk()) return readPartitions();
#endif
    return OK;
}

int Disk::getMaxMinors() {
    // Figure out maximum partition devices supported
    unsigned int majorId = major(mDevice);
    switch (majorId) {
        case kMajorBlockLoop: {
            std::string tmp;
            if (!ReadFileToString(kSysfsLoopMaxMinors, &tmp)) {
                LOG(ERROR) << "Failed to read max minors";
                return -errno;
            }
            return std::stoi(tmp);
        }
        // clang-format off
        case kMajorBlockScsiA: case kMajorBlockScsiB: case kMajorBlockScsiC:
        case kMajorBlockScsiD: case kMajorBlockScsiE: case kMajorBlockScsiF:
        case kMajorBlockScsiG: case kMajorBlockScsiH: case kMajorBlockScsiI:
        case kMajorBlockScsiJ: case kMajorBlockScsiK: case kMajorBlockScsiL:
        case kMajorBlockScsiM: case kMajorBlockScsiN: case kMajorBlockScsiO:
        case kMajorBlockScsiP: {
            // clang-format on
            // Per Documentation/devices.txt this is static
            return 15;
        }
        case kMajorBlockMmc: {
            // Per Documentation/devices.txt this is dynamic
            std::string tmp;
            if (!ReadFileToString(kSysfsMmcMaxMinors, &tmp) &&
                !ReadFileToString(kSysfsMmcMaxMinorsDeprecated, &tmp)) {
                LOG(ERROR) << "Failed to read max minors";
                return -errno;
            }
            return std::stoi(tmp);
        }
        default: {
            if (IsVirtioBlkDevice(majorId)) {
                // drivers/block/virtio_blk.c has "#define PART_BITS 4", so max is
                // 2^4 - 1 = 15
                return 15;
            }
            if (isNvmeBlkDevice(majorId, mSysPath)) {
                // despite kernel nvme driver supports up to 1M minors,
                //     #define NVME_MINORS (1U << MINORBITS)
                // sgdisk can not support more than 127 partitions, due to
                //     #define MAX_MBR_PARTS 128
                return 127;
            }
        }
    }

    LOG(ERROR) << "Unsupported block major type " << majorId;
    return -ENOTSUP;
}

}  // namespace vold
}  // namespace android
