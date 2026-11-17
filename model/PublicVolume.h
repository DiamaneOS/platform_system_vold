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

#ifndef ANDROID_VOLD_PUBLIC_VOLUME_H
#define ANDROID_VOLD_PUBLIC_VOLUME_H

#include "VolumeBase.h"
#include "fs/Fsvm.h"

#include <cutils/multiuser.h>

namespace android {
namespace vold {

/*
 * Shared storage provided by public (vfat) partition.
 *
 * Knows how to mount itself and then spawn a sdcardfs daemon to synthesize
 * permissions.  AsecVolume and ObbVolume can be stacked above it.
 *
 * This volume is not inherently multi-user aware, so it has two possible
 * modes of operation:
 * 1. If primary storage for the device, it only binds itself to the
 * owner user.
 * 2. If secondary storage, it binds itself for all users, but masks
 * away the Android directory for secondary users.
 */
class PublicVolume : public VolumeBase {
  public:
    explicit PublicVolume(dev_t device);
    /* DiamaneOS: a partition the filesystem VM read from the whole disk `diskDevice` (phase 2) */
    PublicVolume(dev_t diskDevice, const std::string& diskDevPath, const std::string& diskSysPath,
                 uint64_t diskBytes, const fsvm::Part& part);
    virtual ~PublicVolume();

    status_t bindMountForUser(userid_t user_id);
    std::string getStableName();

  protected:
    status_t doCreate() override;
    status_t doDestroy() override;
    status_t doMount() override;
    status_t doUnmount() override;
    status_t doFormat(const std::string& fsType) override;

    status_t readMetadata();
    status_t initAsecStage();

    bool mountFailedInVm() const override { return mVmFailed; }
    status_t checkInVm();
    status_t mountInVm(gid_t gid);
    status_t formatInVm(const std::string& fsType);
    status_t addTempPartition(std::string* path);
    void removeTempPartition();

  private:
    /* Kernel device representing partition */
    dev_t mDevice;
    /* Block device path */
    std::string mDevPath;
    /* Mount point of raw partition */
    std::string mRawPath;

    std::string mSdcardFsDefault;
    std::string mSdcardFsRead;
    std::string mSdcardFsWrite;
    std::string mSdcardFsFull;

    /* Whether we mounted FUSE for this volume */
    bool mFuseMounted;

    /* Whether to use sdcardfs for this volume */
    bool mUseSdcardFs;

    /* Filesystem type */
    std::string mFsType;
    /* Filesystem UUID */
    std::string mFsUuid;
    /* User-visible filesystem label */
    std::string mFsLabel;

    /* fsvmd serving this volume from a protected VM, or -1 */
    pid_t mFsvmdPid = -1;

    /* DiamaneOS phase 2: the partition the VM read, on the whole disk at mDiskDevPath */
    bool mInVm = false;
    fsvm::Part mPart;
    std::string mDiskDevPath;
    std::string mDiskSysPath;
    uint64_t mDiskBytes = 0;
    /* Set when the last mount failed in the filesystem VM */
    bool mVmFailed = false;
    /* A partition device added for fsck or format, removed again afterwards */
    std::string mTempNode;
    bool mTempAdded = false;

    DISALLOW_COPY_AND_ASSIGN(PublicVolume);
};

}  // namespace vold
}  // namespace android

#endif
