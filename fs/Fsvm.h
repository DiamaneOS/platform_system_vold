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

#ifndef ANDROID_VOLD_FSVM_H
#define ANDROID_VOLD_FSVM_H

#include <sys/types.h>
#include <utils/Errors.h>

#include <string>

namespace android {
namespace vold {
namespace fsvm {

/*
 * Serves the FAT or exFAT volume on `source` at `target` through FUSE, with the filesystem parsed only inside a
 * protected VM (fsvmd). Returns OK once the VM serves the volume, with fsvmd's pid in `pid`. On any failure nothing
 * stays mounted: there is no fallback to Android's kernel.
 */
status_t Mount(const std::string& source, const std::string& target, gid_t gid, const std::string& volumeId,
               pid_t* pid);

/* Stops fsvmd after its FUSE mount is gone. */
void Stop(pid_t pid);

}  // namespace fsvm
}  // namespace vold
}  // namespace android

#endif
