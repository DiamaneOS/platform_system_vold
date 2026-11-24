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
 * DiamaneOS: the members of class PublicVolume (model/PublicVolume.h) for volumes the filesystem VM serves. Included
 * inside the class at its end, which it leaves private. Defined in PublicVolumeFsvm.cpp.
 */
  public:
    /* A guard for steps that go through this volume's FUSE mount within `ms` (fsvm::Guard), or null when the volume
     * is not served: a volume of its own stops its fsvmd, a drive's partition has its FUSE connection aborted. An OBB
     * on the card guards its own steps with it. */
    std::unique_ptr<fsvm::Guard> guardInVm(int ms) const;
    /* A partition the filesystem VM read from the whole disk `diskDevice` (phase 2), served by its drive's VM
     * `drive` (null for a placeholder, which is never served) */
    PublicVolume(dev_t diskDevice, const std::string& diskDevPath, const std::string& diskSysPath,
                 uint64_t diskBytes, const fsvm::Part& part, std::shared_ptr<fsvm::Drive> drive);
    void prepareStop() override;
    bool waitingForPlace() const override { return mWaitingForPlace; }
    /* What the filesystem VM read of the volume's partition (Disk::remakeWaitingVolumes) */
    const fsvm::Part& partInVm() const { return mPart; }
    /* The disk's medium changed under the volume: its fsvmd goes now, without writing back */
    void dropMedium();
    /* A repair of this volume failed or could not start (Disk::remakePublicVolume): read-only for good */
    void markRepairFailed() { mRepairFailed = true; }
    bool repairFailedInVm() const { return mRepairFailed; }
    void markReadOnlyRequestedInVm() { mRequestedReadOnly = true; }
    bool explicitReadOnlyInVm() const { return mRequestedReadOnly || (getMountFlags() & MountFlags::kReadOnly); }
    bool readOnlyRequestedInVm() const { return mRepairFailed || explicitReadOnlyInVm(); }
    bool unreadInVm() const { return mInVm && (mPart.unread || mFormatPending); }
    void markFormatPendingInVm(bool complete, bool writerStopped) {
        mFormatPending = true;
        mFormatCompleted = complete;
        mFormatWriterStopped = writerStopped;
        showFormatOutcome();
    }

  protected:
    status_t prepareUnmount() override;
    bool hasUnmountWork() const override { return mInVm && mAttached; }
    status_t prepareFormat() override;
    State formatOutcomeState() const override {
        if (!mFormatPending) return State::kUnmounted;
        if (mFormatCompleted) return State::kFormattedUnverified;
        return mFormatWriterStopped ? State::kFormatResultUnverified : State::kUnopenable;
    }
    bool mountFailedInVm() const override { return mVmFailed; }
    bool writeBackFailed() const override { return mWriteBackFailed; }
    void onFsvmdExit(pid_t pid) override;
    void onCheckDone(pid_t pid, char state) override;
    void onMountFailed() override;
    void doPostMount() override;
    /* What the filesystem VM read of the volume, as readMetadata reports it */
    status_t readMetadataInVm();
    /* doDestroy of a volume the VM serves: its range goes */
    status_t destroyInVm();
    /* The steps of doMount around the filesystem VM */
    status_t checkInVm();
    status_t mountInVm(gid_t gid);
    status_t serveInVm(gid_t gid, bool* servedReadOnly, bool* checking);
    int guardMs() const;
    status_t formatInVm(const std::string& fsType);
    /*
     * The repair of errors the check after the mount found, when it would take longer than apps may wait: the volume
     * is unmounted and shows as checking (as stock's check before the mount), the VM repairs it with no time limit
     * (its drive's VM, fsvm::Drive::startRepair, or a VM of its own, fsvm::StartRepair) while vold waits without its
     * lock, then it is mounted again. An eject or removal meanwhile stops the repair (the volume keeps its dirty
     * mark). A repair that failed (a check that failed, too little memory for the card's table) leaves the volume
     * read-only, with its notice, from the next check on.
     */
    pid_t startRepairInVm(pid_t servingPid);
    void endRepairInVm(pid_t repairPid, int rc);
    /* After a repair, or one that could not start: the volume is made again for the framework to mount */
    void remakeAfterRepair(bool failed);
    /* Mounted read-only, and never writable until it is made again: a failed repair, a write-protected drive */
    bool readOnlyForGood() const;
    /* Its last writes did not reach the medium: its disk keeps that for the medium (Disk::noteNotWrittenBack) */
    void noteNotWrittenBack();
    bool cancelRepairInVm() override;
    /*
     * doUnmount's part: fsvmd stops on every way out, also when unmounting FUSE fails. A stuck fsvmd (its VM hangs,
     * FUSE requests wait) is ended after 15 s, so the steps waiting on it fail at once. OBB images served from the
     * card go first, their VMs read through this volume. Once the FUSE mount is gone (fuseGone), fsvmd and its VM
     * stop after writing the volume back, and the guard for the steps before must not cut that short.
     */
    class UnmountInVm {
      public:
        explicit UnmountInVm(PublicVolume* vol);
        ~UnmountInVm();
        void fuseGone();

      private:
        PublicVolume* mVol;
        std::unique_ptr<fsvm::Guard> mStuck;
    };

  private:
    /* fsvmd serving this volume from a protected VM of its own (a virtual disk's), or -1 */
    pid_t mFsvmdPid = -1;
    /* The drive's VM that serves this partition, with the drive's other partitions (fs/FsvmDrive.h) */
    std::shared_ptr<fsvm::Drive> mDrive;
    /* Served by the drive's VM now */
    bool mAttached = false;
    /* What fsvmd's Watch callbacks name while the volume is served (fsvmd's pid, or the drive's ID for it), or 0 */
    pid_t mServing = 0;
    /* Serves the partition from its drive's VM */
    status_t serveOnDrive(gid_t gid, bool* servedReadOnly, bool* checking);
    /* The last stop of fsvmd could not write the volume back (state kNotWrittenBack) */
    bool mWriteBackFailed = false;
    /* When the current mount began (fsvm, NowMs), for the deadline of the steps after the VM served it */
    int64_t mMountStartMs = 0;
    /* Stops fsvmd and frees its serving slot, or detaches the partition from its drive's VM, on every way out of an
     * unmount */
    void stopInVm();

    /* Phase 2: the partition the VM read, on the whole disk at mDiskDevPath */
    bool mFormatPending = false;
    bool mFormatCompleted = false;
    bool mFormatWriterStopped = false;
    bool mInVm = false;
    fsvm::Part mPart;
    std::string mDiskDevPath;
    std::string mDiskSysPath;
    uint64_t mDiskBytes = 0;
    /* Set when the last mount failed in the filesystem VM */
    bool mVmFailed = false;
    /* The last mount found no protected VM place free (fsvm::Spawn): mounted again when one is free */
    bool mWaitingForPlace = false;
    /* The repair running for the volume (an fsvmd's pid, or a drive's token), or -1, and whether a repair failed */
    pid_t mRepairPid = -1;
    bool mRepairFailed = false;
    bool mRequestedReadOnly = false;

    friend struct fsvm::StorageTestAccess;
