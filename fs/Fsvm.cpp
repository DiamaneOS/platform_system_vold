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

#include "Fsvm.h"

#include <android-base/logging.h>
#include <android-base/stringprintf.h>
#include <android-base/unique_fd.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

using android::base::StringPrintf;
using android::base::unique_fd;

namespace android {
namespace vold {
namespace fsvm {

static constexpr char kFsvmd[] = "/system_ext/bin/fsvmd";
/* A small protected VM answers in about 2 s. This leaves room for a refused first start and a busy phone. */
static constexpr int kReadyTimeoutMs = 20000;

status_t Mount(const std::string& source, const std::string& target, gid_t gid, const std::string& volumeId,
               pid_t* pid) {
    *pid = -1;
    bool readOnly = false;
    unique_fd dev(TEMP_FAILURE_RETRY(open(source.c_str(), O_RDWR | O_CLOEXEC)));
    if (dev < 0 && (errno == EROFS || errno == EACCES)) {
        // A card with its write-protect switch on, as the kernel mount handled it: served read-only.
        readOnly = true;
        dev.reset(TEMP_FAILURE_RETRY(open(source.c_str(), O_RDONLY | O_CLOEXEC)));
    }
    if (dev < 0) {
        PLOG(ERROR) << "fsvm: opening " << source;
        return -errno;
    }
    unique_fd fuse(TEMP_FAILURE_RETRY(open("/dev/fuse", O_RDWR | O_CLOEXEC)));
    if (fuse < 0) {
        PLOG(ERROR) << "fsvm: opening /dev/fuse";
        return -errno;
    }
    std::string opts = StringPrintf(
            "fd=%d,rootmode=40000,default_permissions,allow_other,user_id=0,group_id=%d,max_read=131072",
            fuse.get(), gid);
    if (mount("/dev/fuse", target.c_str(), "fuse", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME,
              opts.c_str())) {
        PLOG(ERROR) << "fsvm: mounting FUSE on " << target;
        return -errno;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC)) {
        int e = errno;
        umount2(target.c_str(), MNT_DETACH);
        return -e;
    }
    unique_fd readyRead(p[0]);
    unique_fd readyWrite(p[1]);
    std::string gidArg = std::to_string(gid);
    const char* argv[] = {kFsvmd, "--volume", volumeId.c_str(), "--gid", gidArg.c_str(),
                          readOnly ? "--read-only" : nullptr, nullptr};

    pid_t child = fork();
    if (child == 0) {
        // Only async-signal-safe calls from here. fsvmd gets the block device as 3, the FUSE descriptor as 4 and
        // the ready pipe as 5, nothing else, and runs as the system user without capabilities.
        int fds[3] = {dev.get(), fuse.get(), readyWrite.get()};
        int high[3];
        for (int i = 0; i < 3; i++) {
            high[i] = fcntl(fds[i], F_DUPFD, 10);
            if (high[i] < 0) _exit(127);
        }
        for (int i = 0; i < 3; i++) {
            if (dup2(high[i], 3 + i) < 0) _exit(127);
        }
        syscall(__NR_close_range, 6, ~0U, 0);
        if (setgroups(0, nullptr) || setresgid(AID_SYSTEM, AID_SYSTEM, AID_SYSTEM) ||
            setresuid(AID_SYSTEM, AID_SYSTEM, AID_SYSTEM)) {
            _exit(127);
        }
        execv(kFsvmd, const_cast<char**>(argv));
        _exit(127);
    }
    if (child < 0) {
        int e = errno;
        PLOG(ERROR) << "fsvm: fork";
        umount2(target.c_str(), MNT_DETACH);
        return -e;
    }
    readyWrite.reset();
    dev.reset();
    fuse.reset();

    struct pollfd pfd = {readyRead.get(), POLLIN, 0};
    char c = 0;
    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, kReadyTimeoutMs)) == 1 &&
        TEMP_FAILURE_RETRY(read(readyRead.get(), &c, 1)) == 1 && c == 'R') {
        LOG(INFO) << "fsvm: " << volumeId << " served by the filesystem VM, fsvmd pid " << child;
        *pid = child;
        return OK;
    }
    LOG(ERROR) << "fsvm: the filesystem VM did not serve " << volumeId << ", not mounting it";
    Stop(child);
    umount2(target.c_str(), MNT_DETACH);
    return -EIO;
}

void Stop(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    for (int i = 0; i < 50; i++) {
        if (waitpid(pid, nullptr, WNOHANG) != 0) return;
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
