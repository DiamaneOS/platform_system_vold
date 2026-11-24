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

#ifndef ANDROID_VOLD_FSVM_CHILD_H
#define ANDROID_VOLD_FSVM_CHILD_H

#include <android-base/unique_fd.h>
#include <stdint.h>
#include <sys/types.h>
#include <utils/Errors.h>

#include <string>
#include <vector>

/*
 * DiamaneOS: how vold starts, watches and stops fsvmd (fs/Fsvm.cpp), shared with a drive's fsvmd (fs/FsvmDrive.cpp).
 * Only for those two files.
 */
namespace android {
namespace vold {
namespace fsvm {

/* vold holds its lock while it waits on fsvmd. The bounds, explained in fs/Fsvm.cpp. */
static constexpr int kReadyTimeoutMs = 37000;
static constexpr int kSyncMs = 26000;
static constexpr int kTermMs = 2000;
static constexpr int kKillMs = 1000;
static constexpr int kVmEndMs = 10000;
static constexpr int kStartMs = 3000;
static constexpr int kFormatTimeoutMs = 45000;

int64_t NowMs();

/*
 * Has a free slot's fsvmd service run fsvmd with `args` and exactly the descriptors given as 3, 4 and 5 (-1: none),
 * once there is room for its VM of `mib` MiB (0: the size its kind takes). Returns its pid, or -1 with errno set
 * (EAGAIN: no room).
 */
pid_t Spawn(const std::vector<std::string>& args, int fd3, int fd4, int fd5, uint64_t mib = 0);

/* Waits for fsvmd `pid` to end within `waitMs`, then a stop and `termMs`, then a kill. Its exit status, or -1. */
int Reap(pid_t pid, int waitMs, int termMs);

/* A second descriptor of fsvmd `pid`'s connection to its service, and a stop (T), kill (K) or write back (W) on it. */
android::base::unique_fd Connection(pid_t pid);
void Tell(int conn, char c);

/*
 * Passes what fsvmd says of the VM's check from `ready` to the volume `volumeId`, as onCheckDone(serving, c), and
 * when fsvmd ended (its connection `conn` closed) onFsvmdExit(serving). With `pipeEnds`, the pipe closing ends the
 * serving as well: a drive's partition, whose fsvmd serves the drive's other partitions on.
 */
void Watch(android::base::unique_fd conn, android::base::unique_fd ready, std::string volumeId, pid_t serving,
           bool pipeEnds);

/* Reads everything written to `fd` until it closes, within `deadline` (NowMs), at most `max` bytes. */
status_t ReadAll(int fd, int64_t deadline, size_t max, std::string* out);

/* Mounts FUSE at `target` with vold's options for a card, on a new /dev/fuse descriptor in `fuse`. */
status_t MountFuse(const std::string& target, gid_t gid, bool readOnly, android::base::unique_fd* fuse);

/* A drive's VM was refused at its start for want of memory: one OBB image's VM stops, so that a retry finds room. */
void NoRoomForDrive();

}  // namespace fsvm
}  // namespace vold
}  // namespace android

#endif
