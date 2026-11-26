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
 * DiamaneOS: the members of class Disk (model/Disk.h) for SD cards and USB disks, whose tables only the filesystem
 * VM reads. Included inside the class, in its private part, which it leaves private. Defined in DiskFsvm.cpp.
 */
    bool isVmDisk() const;
    status_t readPartitionsInVm(int maxMinors);
    bool createPublicVolumeInVm(const fsvm::Part& part);
    /* A crafted table can list many FAT partitions: at most this many public volumes per disk, all served by the
     * drive's one VM, each a disk of it */
    static constexpr size_t kMaxPublicPerDisk = 4;
    bool createPrivateVolumeInVm(const fsvm::Part& part);
    void createUnopenableVolume();
    /* The placeholder of a disk whose probe waits for a protected VM place */
    void createWaitingVolume();
    /* The ranges the disk set up itself (adoptable storage) go, and the drive's VM ends */
    void removeRanges();
    /* Asks every volume of the disk to start writing back, before they are stopped one by one */
    void prepareStopAll();
    /* The medium changed: the old one's ranges fail all I/O and its VMs end without writing back */
    void dropOldMedium();
    /* Before sgdisk writes a new table (partitionPublic, partitionMixed): refused on a disk the VM could not read,
     * the volumes go and the old table is zeroed, so that sgdisk never reads one from the card */
    status_t beforePartitionInVm();
    /* Disk::destroy (vold reset, the disk removed): a medium swapped since the last read is cut off before the volumes
     * stop, and the next read makes them again */
    void beforeDestroyInVm();
  public:
    enum class Reprobe {
        /* the last probe failed (a protected VM start can be refused for a while after boot) */
        kFailedProbe,
        /* a change event came too soon after the last probe */
        kChanged,
        /* the probe was put off: too many disks were read lately */
        kDeferred,
        /* a volume could not be opened: its VM did not start */
        kMountRetry,
        /* the VM found nothing it can read: a second VM must answer the same before a format is offered */
        kConfirm,
        /* the probe found no protected VM place free, and one is free now */
        kPlace,
        /* a format's outcome was not read back: what is on the medium is read again */
        kAfterFormat,
    };
    /* A volume (by ID) or a disk's probe waits for a protected VM place: tried again whenever one is given back.
     * With vold's lock held. */
    static void waitForPlace(const std::string& volumeId);
    static void waitForPlaceDisk(const std::string& diskId);
    /* Makes the volumes that wait for a place again, so that the framework mounts them, as at insertion (a mount
     * vold started itself would have no callback to hand MediaProvider the FUSE descriptor) */
    void remakeWaitingVolumes();
    /* Reads the table again, later, when it is still worth it (DiskFsvm.cpp) */
    void reprobeInVm(Reprobe why);
    /* A volume of the disk failed to mount in its VM: the volumes are made again later, and the framework mounts them */
    void retryMountLater();
    /* A volume of the disk mounted: later failures get their retries again */
    void noteMounted();
    /* A format of one of the disk's volumes may or may not have written what it reported: the disk is read again */
    void recheckAfterFormat();
  private:
    /* The probe found no protected VM place free: the placeholder shows, and the disk is read when one is free */
    bool mWaitingForPlace = false;
    bool mForceProbe = false;
    int mMountRetries = 0;
    bool mMountRetryPending = false;
    bool mRecheckPending = false;
    int mProbeFailures = 0;
    bool mProbeFailed = false;
    /* The medium and size last probed, and when, so that change events without a change probe nothing */
    fsvm::ProbeMemory mProbe;
    bool mReprobePending = false;
    int mPublicVolumes = 0;
    /* Device-mapper ranges of the card the disk set up itself (adoptable storage), by name */
    std::vector<std::string> mRanges;
    /* The one VM that serves all public partitions of the medium last read (fs/FsvmDrive.h), or null */
    std::shared_ptr<fsvm::Drive> mDrive;
    /* An answer that would offer a format, waiting for a second VM's (Reprobe::kConfirm) */
    bool mVerdictPending = false;
    bool mConfirmScheduled = false;
    bool mConfirmProbe = false;
    fsvm::Layout mVerdict;
