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
    /* The fsvmd serving this volume, or -1 (an OBB on the card guards its own steps with it) */
    pid_t getFsvmdPid() const { return mFsvmdPid; }
    /* A partition the filesystem VM read from the whole disk `diskDevice` (phase 2) */
    PublicVolume(dev_t diskDevice, const std::string& diskDevPath, const std::string& diskSysPath,
                 uint64_t diskBytes, const fsvm::Part& part);
    void prepareStop() override;
    /* The disk's medium changed under the volume: its fsvmd goes now, without writing back */
    void dropMedium();

  protected:
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
    status_t serveInVm(gid_t gid, bool* servedReadOnly);
    int guardMs() const;
    status_t formatInVm(const std::string& fsType);
    /* A device-mapper range of exactly this partition (fs/Fsvm.h CreateRange), or the disk for index 0 */
    status_t createRange(bool readOnly, std::string* path);
    void removeRange();
    /*
     * The repair of errors the check after the mount found, when it would take longer than apps may wait: the volume
     * is unmounted and shows as checking (as stock's check before the mount), the VM repairs it with no time limit
     * (fsvm::StartRepair) while vold waits without its lock, then it is mounted again. An eject or removal meanwhile
     * stops the repair (the volume keeps its dirty mark). A repair that failed (a check that failed, too little memory
     * for the card's table) leaves the volume read-only, with its notice, from the next check on.
     */
    pid_t startRepairInVm(pid_t servingPid);
    void endRepairInVm(pid_t repairPid, int rc);
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
    /* fsvmd serving this volume from a protected VM, or -1 */
    pid_t mFsvmdPid = -1;
    /* The last stop of fsvmd could not write the volume back (state kNotWrittenBack) */
    bool mWriteBackFailed = false;
    /* When the current mount began (fsvm, NowMs), for the deadline of the steps after the VM served it */
    int64_t mMountStartMs = 0;
    /* Stops fsvmd, frees its serving slot and removes the partition's range, on every way out of an unmount */
    void stopInVm();

    /* Phase 2: the partition the VM read, on the whole disk at mDiskDevPath */
    bool mInVm = false;
    fsvm::Part mPart;
    std::string mDiskDevPath;
    std::string mDiskSysPath;
    uint64_t mDiskBytes = 0;
    /* Set when the last mount failed in the filesystem VM */
    bool mVmFailed = false;
    /* The partition's device-mapper range while its VM or a format uses it (dm name and vold's node) */
    std::string mRangeName;
    std::string mRangeNode;
    /* The fsvmd repairing the volume, or -1, and whether a repair failed for it */
    pid_t mRepairPid = -1;
    bool mRepairFailed = false;
