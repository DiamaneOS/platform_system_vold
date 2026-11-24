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
 * DiamaneOS: the members of class ObbVolume (model/ObbVolume.h) for OBB images the filesystem VM serves. Included
 * inside the class at its end, which it leaves private. Defined in ObbVolumeFsvm.cpp.
 */
  public:
    status_t cleanupInVm();
    status_t confirmEndInVm();
    void onFsvmdExit(pid_t pid) override;
    /* The OBB file vold opens (below /data/media, or below a public volume's /mnt/media_rw/<uuid>) */
    const std::string& getSourcePath() const { return mSourcePath; }
    /* Asks its fsvmd to stop now, so that several OBB VMs end together (VolumeManager::unmountObbsBelow) */
    void requestStop();
    /* Stops the VM of the OBB image mounted last, for a drive that needs its place (vold's lock is held). Whether one
     * stopped. */
    static bool stopOneForDrive();

  private:
    /* The caller's user (the constructor's `user`) */
    userid_t mUser;
    /* fsvmd serving the image from the filesystem VM, or -1 */
    pid_t mFsvmdPid = -1;
    /* doMount and doUnmount with the filesystem VM */
    status_t mountInVm();
    void stopInVm();
