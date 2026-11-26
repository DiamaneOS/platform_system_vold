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
 * DiamaneOS: the members of class VolumeBase (model/VolumeBase.h) for volumes the filesystem VM serves. Included
 * inside the class at its end, which it leaves private. Defined in VolumeBaseFsvm.cpp.
 */
  public:
    /* The fsvmd serving this volume ended on its own (vold's lock is held) */
    virtual void onFsvmdExit(pid_t pid) {}
    /* What fsvmd says of the VM's check after the mount (vold's lock is held): 'W' writable now, 'F' it stays
     * read-only, 'c' a new VM checks it again, read-only meanwhile, 'E' its errors need a repair without apps */
    virtual void onCheckDone(pid_t pid, char state) {}
    /* The volume is about to go with others of its disk: start writing it back now (PublicVolume) */
    virtual void prepareStop() {}
    /* The last mount found no protected VM place free: the volume shows as checking, and its disk makes it again for
     * the framework to mount as soon as a place is free (PublicVolume, Disk::remakeWaitingVolumes) */
    virtual bool waitingForPlace() const { return false; }

  protected:
    /* Whether the last failed mount failed in the filesystem VM (state kUnopenable) */
    virtual bool mountFailedInVm() const { return false; }
    /* Whether the last unmount could not write the volume back (state kNotWrittenBack) */
    virtual bool writeBackFailed() const { return false; }
    /* A failed mount (state set already), for a retry later (PublicVolume) */
    virtual void onMountFailed() {}

    /* Report a mounted volume as mounted read-only (the filesystem VM served it read-only) */
    void setReportReadOnly(bool readOnly) { mReportReadOnly = readOnly; }
    /* The same for a volume mounted already: the framework hears of it at once */
    void announceReadOnly(bool readOnly);
    /* Whether the VM checks the read-only volume now, so a check may still make it writable: the framework hears of it
     * at every mount and when it changes, before the state it goes with */
    void announceCheckPending(bool pending);
    /* After an unmount, reports the volume as not opened */
    void setUnopenable();
    /* A repair without apps (PublicVolume): the volume shows as checking, as stock's check before the mount, then
     * as unmounted again */
    void showChecking() { setState(State::kChecking); }
    void showUnmounted() { setState(State::kUnmounted); }
    /* An unmount (eject) or removal during that repair stops it: true when one ran */
    virtual bool cancelRepairInVm() { return false; }

  private:
    /* See setReportReadOnly */
    bool mReportReadOnly = false;
    /* See announceCheckPending */
    bool mCheckPending = false;
    /* The state the framework hears: a volume the filesystem VM served read-only stays kMounted inside vold, and the
     * framework sees it as mounted read-only */
    State reportedState() const;
    /* Whether the volume may be mounted or get new flags or a new user: stock's unmounted and unmountable, and the
     * VM's unopenable and not written back */
    bool mayMount() const;
    /* A failed mount: unopenable when it failed in the VM, else unmountable, then onMountFailed */
    void mountFailed();
