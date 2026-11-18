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

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <private/android_filesystem_config.h>

#include <dirent.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

#include "VolumeManager.h"
#include "model/VolumeBase.h"

using android::base::ParseUint;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;

namespace android {
namespace vold {
namespace fsvm {

static constexpr char kFsvmd[] = "/system_ext/bin/fsvmd";
/*
 * vold holds its lock while it waits, and system_server's watchdog asks for that lock (60 s). fsvmd keeps to 20 s
 * for a probe and 30 s for a mount (fsvmd PROBE_BUDGET and SERVE_BUDGET: VM starts, the check, the mount and the
 * first free-space count), vold waits a little longer, a stop takes at most 2 s, and the steps after the VM mount
 * get 15 s (Guard). So a mount holds the lock for at most 50 s, a probe for 25 s.
 */
static constexpr int kProbeTimeoutMs = 23000;
static constexpr int kReadyTimeoutMs = 33000;
static constexpr int kStopMs = 2000;
/* Serving VMs at a time, OBB images excluded (ObbVolume has its own caps). */
static constexpr int kMaxServing = 6;
static std::atomic<int> sServing{0};
static constexpr size_t kMaxProbeOutput = 4096;
static constexpr size_t kMaxParts = 16;
static constexpr uint32_t kMaxIndex = 128;
static constexpr size_t kMaxLabelBytes = 47;

/* Forks and runs fsvmd as the system user with exactly the descriptors given as 3, 4 and 5 (-1: none). */
static pid_t Spawn(const std::vector<std::string>& args, int fd3, int fd4, int fd5) {
    std::vector<const char*> argv;
    argv.push_back(kFsvmd);
    for (const auto& a : args) argv.push_back(a.c_str());
    argv.push_back(nullptr);

    pid_t child = fork();
    if (child != 0) return child;
    // Only async-signal-safe calls from here.
    int fds[3] = {fd3, fd4, fd5};
    int high[3];
    for (int i = 0; i < 3; i++) {
        high[i] = -1;
        if (fds[i] >= 0 && (high[i] = fcntl(fds[i], F_DUPFD, 10)) < 0) _exit(127);
    }
    for (int i = 0; i < 3; i++) {
        if (high[i] >= 0) {
            if (dup2(high[i], 3 + i) < 0) _exit(127);
        } else {
            close(3 + i);
        }
    }
    syscall(__NR_close_range, 6, ~0U, 0);
    if (setgroups(0, nullptr) || setresgid(AID_SYSTEM, AID_SYSTEM, AID_SYSTEM) ||
        setresuid(AID_SYSTEM, AID_SYSTEM, AID_SYSTEM)) {
        _exit(127);
    }
    execv(kFsvmd, const_cast<char**>(argv.data()));
    _exit(127);
}

static int64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* Waits for `pid` to exit within `ms`, else kills it. Returns its exit status, or -1. */
static int Reap(pid_t pid, int ms) {
    int status = 0;
    for (int waited = 0; waited <= ms; waited += 100) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        usleep(100 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

static bool IsHex(const std::string& s, size_t from, size_t n) {
    if (s.size() < from + n) return false;
    for (size_t i = from; i < from + n; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static bool IsUuid(const std::string& s) {
    return s.size() == 9 && IsHex(s, 0, 4) && s[4] == '-' && IsHex(s, 5, 4);
}

static bool IsGuid(const std::string& s) {
    return s.size() == 36 && IsHex(s, 0, 8) && s[8] == '-' && IsHex(s, 9, 4) && s[13] == '-' &&
           IsHex(s, 14, 4) && s[18] == '-' && IsHex(s, 19, 4) && s[23] == '-' && IsHex(s, 24, 12);
}

/* Only plain decimal digits: ParseUint alone would take spaces, a sign or hex. */
static bool Digits(const std::string& s) {
    return !s.empty() && s.size() <= 20 && s.find_first_not_of("0123456789") == std::string::npos;
}

static bool DecodeLabel(const std::string& hex, std::string* out) {
    out->clear();
    if (hex == "-") return true;
    if (hex.empty() || hex.size() % 2 || hex.size() > 2 * kMaxLabelBytes) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned v = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[i + k];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else return false;
        }
        // fsvmd checked the name rules. Here: no control characters, whatever reaches the framework.
        if (v < 0x20 || v == 0x7f) return false;
        out->push_back(static_cast<char>(v));
    }
    return true;
}

status_t ParseLayout(const std::string& text, uint64_t diskBytes, Layout* out) {
    *out = Layout();
    if (text.size() > kMaxProbeOutput || diskBytes % 512 || text.empty() || text.back() != '\n') return -EINVAL;
    std::vector<std::string> lines = Split(text.substr(0, text.size() - 1), "\n");
    if (lines.size() < 2) return -EINVAL;
    std::vector<std::string> head = Split(lines[0], " ");
    size_t count = 0;
    if (head.size() != 4 || head[0] != "LAYOUT" || head[1] != "1" || !Digits(head[3]) || !ParseUint(head[3], &count) ||
        count > kMaxParts || lines.size() != count + 2 || lines.back() != "END") {
        return -EINVAL;
    }
    if (head[2] != "none" && head[2] != "mbr" && head[2] != "gpt") return -EINVAL;
    out->table = head[2];
    const uint64_t sectors = diskBytes / 512;
    for (size_t i = 0; i < count; i++) {
        std::vector<std::string> f = Split(lines[i + 1], " ");
        Part p;
        if (f.size() != 10 || f[0] != "PART" || !Digits(f[1]) || !Digits(f[3]) || !Digits(f[4]) || !Digits(f[8]) ||
            !ParseUint(f[1], &p.index, kMaxIndex) ||
            !ParseUint(f[3], &p.start) || !ParseUint(f[4], &p.length) || !ParseUint(f[8], &p.metaBytes)) {
            return -EINVAL;
        }
        if (f[2] == "public") p.cls = Part::Class::kPublic;
        else if (f[2] == "meta") p.cls = Part::Class::kMeta;
        else if (f[2] == "expand") p.cls = Part::Class::kExpand;
        else return -EINVAL;
        if (p.length == 0 || p.start > sectors || p.length > sectors - p.start) return -EINVAL;
        if (f[5] == "vfat" || f[5] == "exfat") p.fsType = f[5];
        else if (f[5] != "-") return -EINVAL;
        if (IsUuid(f[6])) p.fsUuid = f[6];
        else if (f[6] != "-") return -EINVAL;
        if (IsGuid(f[7])) p.partGuid = f[7];
        else if (f[7] != "-") return -EINVAL;
        if (!DecodeLabel(f[9], &p.label)) return -EINVAL;
        if ((head[2] == "none") != (p.index == 0)) return -EINVAL;
        if (head[2] == "mbr" && p.index > 4) return -EINVAL;
        if (head[2] != "gpt" && (p.cls != Part::Class::kPublic || !p.partGuid.empty())) return -EINVAL;
        if (head[2] == "gpt" &&
            (p.partGuid.empty() || sectors < 67 || p.start < 34 || p.start + p.length > sectors - 33)) {
            return -EINVAL;
        }
        if (p.cls != Part::Class::kPublic && !p.fsType.empty()) return -EINVAL;
        for (const auto& q : out->parts) {
            if (q.index == p.index || (q.cls == p.cls && p.cls != Part::Class::kPublic)) return -EINVAL;
            if (p.start < q.start + q.length && q.start < p.start + p.length) return -EINVAL;
        }
        out->parts.push_back(p);
    }
    return OK;
}

/* Reads everything fsvmd writes to `fd` until it closes, within `deadline` (NowMs). */
static status_t ReadAll(int fd, int64_t deadline, size_t max, std::string* out) {
    char buf[512];
    for (;;) {
        int64_t left = deadline - NowMs();
        if (left <= 0) return -ETIMEDOUT;
        struct pollfd pfd = {fd, POLLIN, 0};
        int r = TEMP_FAILURE_RETRY(poll(&pfd, 1, static_cast<int>(left)));
        if (r <= 0) return -ETIMEDOUT;
        ssize_t n = TEMP_FAILURE_RETRY(read(fd, buf, sizeof(buf)));
        if (n < 0) return -errno;
        if (n == 0) return OK;
        if (out->size() + n > max) return -E2BIG;
        out->append(buf, n);
    }
}

status_t Probe(const std::string& diskPath, uint64_t diskBytes, Layout* out) {
    unique_fd dev(TEMP_FAILURE_RETRY(open(diskPath.c_str(), O_RDONLY | O_CLOEXEC)));
    if (dev < 0) {
        PLOG(ERROR) << "fsvm: opening " << diskPath;
        return -errno;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -errno;
    unique_fd rd(p[0]), wr(p[1]);
    pid_t child = Spawn({"--probe", "--disk-bytes", std::to_string(diskBytes)}, dev.get(), -1, wr.get());
    if (child < 0) {
        PLOG(ERROR) << "fsvm: fork";
        return -errno;
    }
    wr.reset();
    dev.reset();
    std::string text;
    status_t res = ReadAll(rd.get(), NowMs() + kProbeTimeoutMs, kMaxProbeOutput, &text);
    int rc = Reap(child, res == OK ? 5000 : 0);
    if (res != OK || rc != 0) {
        LOG(ERROR) << "fsvm: the filesystem VM did not read " << diskPath << " (" << res << ", exit " << rc << ")";
        return res != OK ? res : -EIO;
    }
    res = ParseLayout(text, diskBytes, out);
    if (res != OK) {
        LOG(ERROR) << "fsvm: refusing the layout of " << diskPath;
        return res;
    }
    LOG(INFO) << "fsvm: " << diskPath << " has a " << out->table << " table with " << out->parts.size()
              << " partitions Android uses";
    return OK;
}

/*
 * Waits for fsvmd to end and, when it ended on its own while its volume is still mounted (its VM broke and no new
 * one came up), has the volume unmounted and reported as not opened.
 */
static void Watch(unique_fd pidfd, std::string volumeId, pid_t pid) {
    std::thread([pidfd = std::move(pidfd), volumeId, pid]() {
        struct pollfd pfd = {pidfd.get(), POLLIN, 0};
        while (TEMP_FAILURE_RETRY(poll(&pfd, 1, -1)) != 1) {
        }
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        auto vol = VolumeManager::Instance()->findVolume(volumeId);
        if (vol) vol->onFsvmdExit(pid);
    }).detach();
}

status_t Mount(Volume&& v, pid_t* pid) {
    *pid = -1;
    if (!v.obb && sServing >= kMaxServing) {
        LOG(ERROR) << "fsvm: " << kMaxServing << " volumes are served already, not " << v.volumeId;
        return -EBUSY;
    }
    unique_fd fuse(TEMP_FAILURE_RETRY(open("/dev/fuse", O_RDWR | O_CLOEXEC)));
    if (fuse < 0) {
        PLOG(ERROR) << "fsvm: opening /dev/fuse";
        return -errno;
    }
    std::string opts = StringPrintf(
            "fd=%d,rootmode=40000,default_permissions,allow_other,user_id=0,group_id=%d,max_read=131072",
            fuse.get(), v.gid);
    unsigned long flags = MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | (v.obb ? MS_RDONLY : 0);
    if (mount("/dev/fuse", v.target.c_str(), "fuse", flags, opts.c_str())) {
        PLOG(ERROR) << "fsvm: mounting FUSE on " << v.target;
        return -errno;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC)) {
        int e = errno;
        umount2(v.target.c_str(), MNT_DETACH);
        return -e;
    }
    unique_fd readyRead(p[0]), readyWrite(p[1]);
    std::vector<std::string> args;
    if (v.obb) {
        args = {"--obb", "--volume", v.volumeId, "--gid", std::to_string(v.gid), "--disk-bytes",
                std::to_string(v.diskBytes)};
    } else {
        args = {"--volume", v.volumeId, "--gid", std::to_string(v.gid), "--disk-bytes", std::to_string(v.diskBytes),
                "--partition", std::to_string(v.part.index), "--start", std::to_string(v.part.start),
                "--length", std::to_string(v.part.length), "--fs", v.part.fsType.empty() ? "-" : v.part.fsType,
                "--meta-bytes", std::to_string(v.part.metaBytes)};
        if (v.readOnly) args.push_back("--read-only");
    }
    pid_t child = Spawn(args, v.fd.get(), fuse.get(), readyWrite.get());
    if (child < 0) {
        int e = errno;
        PLOG(ERROR) << "fsvm: fork";
        umount2(v.target.c_str(), MNT_DETACH);
        return -e;
    }
    readyWrite.reset();
    v.fd.reset();
    fuse.reset();
    // Opened before anyone can reap the child, so it always names this process.
    unique_fd pidfd(static_cast<int>(syscall(__NR_pidfd_open, child, 0)));

    struct pollfd pfd = {readyRead.get(), POLLIN, 0};
    char c = 0;
    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, kReadyTimeoutMs)) == 1 &&
        TEMP_FAILURE_RETRY(read(readyRead.get(), &c, 1)) == 1 && c == 'R') {
        LOG(INFO) << "fsvm: " << v.volumeId << " served by the filesystem VM, fsvmd pid " << child;
        *pid = child;
        if (!v.obb) sServing++;
        if (pidfd >= 0) {
            Watch(std::move(pidfd), v.volumeId, child);
        } else {
            PLOG(WARNING) << "fsvm: no pidfd for fsvmd " << child;
        }
        return OK;
    }
    LOG(ERROR) << "fsvm: the filesystem VM did not serve " << v.volumeId << ", not mounting it";
    Stop(child);
    umount2(v.target.c_str(), MNT_DETACH);
    return -EIO;
}

void Stop(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGTERM);
    if (Reap(pid, kStopMs) < 0) LOG(WARNING) << "fsvm: fsvmd " << pid << " killed";
}

void Served(bool obb, bool starting) {
    if (obb) return;
    if (starting) {
        sServing++;
    } else if (sServing > 0) {
        sServing--;
    }
}

struct Guard::State {
    std::mutex lock;
    std::condition_variable cv;
    bool done = false;
    unique_fd pidfd;
};

Guard::Guard(pid_t pid, int ms) : mState(std::make_shared<State>()) {
    // Through a pidfd, so that a process that ended and was reaped meanwhile can never be mistaken for another.
    mState->pidfd.reset(static_cast<int>(syscall(__NR_pidfd_open, pid, 0)));
    std::thread([state = mState, pid, ms]() {
        std::unique_lock<std::mutex> l(state->lock);
        if (!state->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return state->done; }) &&
            state->pidfd >= 0) {
            LOG(ERROR) << "fsvm: the mount steps after the VM took too long, stopping fsvmd " << pid;
            // Ends its FUSE connection, so the steps waiting on it fail at once.
            syscall(__NR_pidfd_send_signal, state->pidfd.get(), SIGKILL, nullptr, 0);
        }
    }).detach();
}

Guard::~Guard() {
    {
        std::lock_guard<std::mutex> l(mState->lock);
        mState->done = true;
    }
    mState->cv.notify_all();
}

static bool ReadNumber(const std::string& path, uint64_t* out) {
    std::string s;
    if (!android::base::ReadFileToString(path, &s)) return false;
    return ParseUint(android::base::Trim(s), out);
}

/* Finds partition `index` of the disk at `diskSysPath` with exactly `start` and `length`. */
static bool FindPartition(const std::string& diskSysPath, const Part& p, dev_t* dev) {
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(diskSysPath.c_str()), closedir);
    if (!dir) return false;
    while (struct dirent* e = readdir(dir.get())) {
        if (e->d_name[0] == '.') continue;
        std::string base = diskSysPath + "/" + e->d_name;
        uint64_t index, start, size;
        if (!ReadNumber(base + "/partition", &index) || index != p.index) continue;
        if (!ReadNumber(base + "/start", &start) || !ReadNumber(base + "/size", &size) || start != p.start ||
            size != p.length) {
            return false;
        }
        std::string d;
        unsigned maj, min;
        if (!android::base::ReadFileToString(base + "/dev", &d) ||
            sscanf(android::base::Trim(d).c_str(), "%u:%u", &maj, &min) != 2) {
            return false;
        }
        *dev = makedev(maj, min);
        return true;
    }
    return false;
}

static status_t Blkpg(const std::string& diskPath, int op, const Part& p) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(diskPath.c_str(), O_RDONLY | O_CLOEXEC)));
    if (fd < 0) return -errno;
    struct blkpg_partition bp = {};
    bp.pno = static_cast<int>(p.index);
    bp.start = static_cast<long long>(p.start * 512);
    bp.length = static_cast<long long>(p.length * 512);
    struct blkpg_ioctl_arg a = {};
    a.op = op;
    a.datalen = sizeof(bp);
    a.data = &bp;
    return ioctl(fd.get(), BLKPG, &a) ? -errno : OK;
}

status_t AddPartition(const std::string& diskPath, const std::string& diskSysPath, const Part& p, dev_t* dev,
                      bool* added) {
    *added = false;
    if (p.index == 0 || p.index > kMaxIndex || p.length == 0 ||
        p.start > UINT64_MAX / 512 - p.length) {
        return -EINVAL;
    }
    status_t res = Blkpg(diskPath, BLKPG_ADD_PARTITION, p);
    if (res != OK) {
        LOG(ERROR) << "fsvm: adding partition " << p.index << " of " << diskPath << ": " << strerror(-res);
        return res;
    }
    *added = true;
    if (!FindPartition(diskSysPath, p, dev)) {
        LOG(ERROR) << "fsvm: partition " << p.index << " of " << diskPath << " not found after adding it";
        DeletePartition(diskPath, p.index);
        *added = false;
        return -ENOENT;
    }
    return OK;
}

status_t DeleteAllPartitions(const std::string& diskPath, const std::string& diskSysPath) {
    std::unique_ptr<DIR, int (*)(DIR*)> dir(opendir(diskSysPath.c_str()), closedir);
    if (!dir) return -errno;
    status_t res = OK;
    while (struct dirent* e = readdir(dir.get())) {
        uint64_t index;
        if (e->d_name[0] == '.' || !ReadNumber(diskSysPath + "/" + e->d_name + "/partition", &index) ||
            index == 0 || index > kMaxIndex) {
            continue;
        }
        LOG(WARNING) << "fsvm: removing partition " << index << " of " << diskPath << " before reading its table";
        if (DeletePartition(diskPath, static_cast<uint32_t>(index)) != OK) res = -EBUSY;
    }
    return res;
}

status_t DeletePartition(const std::string& diskPath, uint32_t index) {
    Part p;
    p.index = index;
    status_t res = Blkpg(diskPath, BLKPG_DEL_PARTITION, p);
    if (res != OK && res != -ENXIO) {
        LOG(WARNING) << "fsvm: removing partition " << index << " of " << diskPath << ": " << strerror(-res);
    }
    return res;
}

status_t ZeroTables(const std::string& diskPath, uint64_t diskBytes) {
    static constexpr size_t kMiB = 1 << 20;
    unique_fd fd(TEMP_FAILURE_RETRY(open(diskPath.c_str(), O_WRONLY | O_CLOEXEC)));
    if (fd < 0) return -errno;
    std::vector<char> zero(kMiB, 0);
    uint64_t ends[2] = {0, diskBytes >= 2 * kMiB ? diskBytes - kMiB : 0};
    for (uint64_t at : ends) {
        size_t done = 0;
        while (done < kMiB) {
            ssize_t n = TEMP_FAILURE_RETRY(pwrite(fd.get(), zero.data() + done, kMiB - done, at + done));
            if (n <= 0) return n < 0 ? -errno : -EIO;
            done += n;
        }
    }
    return fsync(fd.get()) ? -errno : OK;
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
