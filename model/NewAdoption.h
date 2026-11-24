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

#pragma once

#include <android-base/logging.h>

#include <errno.h>

#include <string>

namespace android {
namespace vold {

/*
 * DiamaneOS: no new adoptable storage. A private or mixed partition table (Disk::partitionMixed, which
 * partitionPrivate calls) is refused before anything is written: an adopted card's filesystem reaches Android's kernel
 * decrypted but not authenticated. Cards adopted before keep working: they mount, can be formatted as portable
 * storage, migrated back and forgotten.
 *
 * The switch for its return (authenticated adoption, for example dm-crypt with dm-integrity): this constant, together
 * with config_newAdoptableStorage in frameworks/base core/res/res/values/config_diamaneos.xml, which the system
 * server, SystemUI and Settings read.
 */
constexpr bool kNewAdoptableStorage = false;

inline int RefuseNewAdoption(const std::string& diskId) {
    LOG(WARNING) << diskId << ": not partitioned as adoptable storage, new adoptable storage is off";
    return -EPERM;
}

}  // namespace vold
}  // namespace android
