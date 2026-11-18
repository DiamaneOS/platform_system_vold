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
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
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

static constexpr char kService[] = "/dev/socket/fsvmd";
/*
 * vold holds its lock while it waits, and system_server's watchdog asks for that lock (60 s). Every wait is bounded:
 *
 * - probe: fsvmd keeps to 20 s (PROBE_BUDGET), vold reads for at most 23 s, then Reap takes at most 4 s in all.
 *   At most 27 s.
 * - mount: fsvmd keeps to 30 s (SERVE_BUDGET: VM starts, the check, the mount, the first free-space count), vold
 *   waits 33 s. The steps after it (MediaProvider's FUSE, bind mounts) end at the deadline PublicVolume's Guard
 *   sets: 48 s after the mount began, at least 8 s. A failed mount adds a stop of at most 3 s. At most 51 s.
 * - unmount: the steps before the stop get 15 s (a Guard ends fsvmd when FUSE requests hang), the orderly stop at
 *   most kSyncMs plus 3 s. At most 30 s.
 */
static constexpr int kProbeTimeoutMs = 23000;
static constexpr int kReadyTimeoutMs = 33000;
/* After vold unmounted FUSE, fsvmd writes back and unmounts in the VM (fsvmd CLOSE_LOCK_WAIT plus CLOSE_BUDGET). */
static constexpr int kSyncMs = 12000;
/* After a stop request, then after a kill. */
static constexpr int kTermMs = 2000;
static constexpr int kKillMs = 1000;
/* The service answers a request at once: it only forks. */
static constexpr int kStartMs = 3000;
/* Serving VMs at a time, OBB images excluded (ObbVolume has its own caps). */
static constexpr int kMaxServing = 6;
static std::atomic<int> sServing{0};
static constexpr size_t kMaxProbeOutput = 4096;
static constexpr size_t kMaxParts = 16;
static constexpr uint32_t kMaxIndex = 128;
static constexpr size_t kMaxLabelBytes = 47;

static int64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/*
 * fsvmd runs through init's fsvmd service (vendor/diamaneos fsvm/fsvmd/fsvmd.rc), as the system user without
 * capabilities: vold holds neither setuid nor setgid, and changes no ids. Each fsvmd has one connection to the
 * service, kept here by its pid. The service passes on a stop (T) or kill (K), kills fsvmd when vold closes the
 * connection, and closes it after it reported how fsvmd ended (X <status>). See fsvmd/src/launcher.rs.
 */
static std::mutex sChildrenLock;
static std::map<pid_t, unique_fd> sChildren;

/* Reads one answer of the service within `ms`: P <pid>, X <status> or E <errno>. */
static bool ReadAnswer(int fd, int ms, char* kind, uint64_t* value) {
    struct pollfd pfd = {fd, POLLIN, 0};
    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, ms)) != 1 || !(pfd.revents & POLLIN)) return false;
    char buf[32];
    ssize_t n = TEMP_FAILURE_RETRY(recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT));
    if (n < 3 || buf[1] != ' ') return false;
    buf[n] = '\0';
    *kind = buf[0];
    return ParseUint(buf + 2, value, static_cast<uint64_t>(INT32_MAX));
}

/* Has the service run fsvmd with exactly the descriptors given as 3, 4 and 5 (-1: none). Returns its pid, or -1. */
static pid_t Spawn(const std::vector<std::string>& args, int fd3, int fd4, int fd5) {
    // Not blocking: a service that is not accepting (stopped, restarting) refuses at once instead of holding vold.
    unique_fd s(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (s < 0) return -1;
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strlcpy(addr.sun_path, kService, sizeof(addr.sun_path));
    if (TEMP_FAILURE_RETRY(connect(s.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)))) {
        PLOG(ERROR) << "fsvm: the fsvmd service is not there";
        return -1;
    }
    std::string msg("FSVMD1", 6);
    msg.push_back('\0');
    int fds[3] = {fd3, fd4, fd5};
    int pass[3];
    int n = 0, mask = 0;
    for (int i = 0; i < 3; i++) {
        if (fds[i] >= 0) {
            mask |= 1 << i;
            pass[n++] = fds[i];
        }
    }
    msg.push_back(static_cast<char>('0' + mask));
    msg.push_back('\0');
    for (const auto& a : args) {
        msg += a;
        msg.push_back('\0');
    }
    struct iovec iov = {msg.data(), msg.size()};
    union {
        char buf[CMSG_SPACE(sizeof(int) * 3)];
        struct cmsghdr align;
    } control = {};
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (n > 0) {
        mh.msg_control = control.buf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * n);
        struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * n);
        memcpy(CMSG_DATA(c), pass, sizeof(int) * n);
    }
    if (TEMP_FAILURE_RETRY(sendmsg(s.get(), &mh, MSG_NOSIGNAL)) != static_cast<ssize_t>(msg.size())) {
        PLOG(ERROR) << "fsvm: sending the request to the fsvmd service";
        return -1;
    }
    char kind = 0;
    uint64_t value = 0;
    if (!ReadAnswer(s.get(), kStartMs, &kind, &value) || kind != 'P' || value == 0) {
        LOG(ERROR) << "fsvm: the fsvmd service did not start fsvmd ("
                   << (kind == 'E' ? strerror(static_cast<int>(value)) : "no answer") << ")";
        errno = kind == 'E' ? static_cast<int>(value) : EIO;
        return -1;
    }
    pid_t pid = static_cast<pid_t>(value);
    std::lock_guard<std::mutex> lock(sChildrenLock);
    sChildren[pid] = std::move(s);
    return pid;
}

/* A second descriptor of fsvmd `pid`'s connection, for a thread that outlives vold's own (Watch, Guard). */
static unique_fd Connection(pid_t pid) {
    std::lock_guard<std::mutex> lock(sChildrenLock);
    auto it = sChildren.find(pid);
    if (it == sChildren.end()) return {};
    return unique_fd(fcntl(it->second.get(), F_DUPFD_CLOEXEC, 0));
}

static void Tell(int conn, char c) {
    TEMP_FAILURE_RETRY(send(conn, &c, 1, MSG_NOSIGNAL));
}

/*
 * Waits for fsvmd `pid` to exit within `waitMs`. Then, when `termMs` is not 0, asks it to stop and waits that long,
 * and last kills it and waits kKillMs. Returns its exit status, or -1 when it had to be killed. At most waitMs plus
 * termMs plus kKillMs in all.
 */
static int Reap(pid_t pid, int waitMs, int termMs) {
    unique_fd conn;
    {
        std::lock_guard<std::mutex> lock(sChildrenLock);
        auto it = sChildren.find(pid);
        if (it == sChildren.end()) return -1;
        conn = std::move(it->second);
        sChildren.erase(it);
    }
    char kind = 0;
    uint64_t value = 0;
    if (ReadAnswer(conn.get(), waitMs, &kind, &value) && kind == 'X') return static_cast<int>(value);
    if (termMs > 0) {
        Tell(conn.get(), 'T');
        if (ReadAnswer(conn.get(), termMs, &kind, &value) && kind == 'X') return static_cast<int>(value);
    }
    Tell(conn.get(), 'K');
    // The service reaps it at once, and closing the connection kills it in any case.
    ReadAnswer(conn.get(), kKillMs, &kind, &value);
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

uint32_t BlockSize(int fd) {
    int ss = 0;
    if (ioctl(fd, BLKSSZGET, &ss) || (ss != 512 && ss != 4096)) return 0;
    return static_cast<uint32_t>(ss);
}

status_t ParseLayout(const std::string& text, uint64_t diskBytes, Layout* out, uint32_t blockSize) {
    *out = Layout();
    if (text.size() > kMaxProbeOutput || diskBytes % 512 || text.empty() || text.back() != '\n') return -EINVAL;
    if (blockSize != 512 && blockSize != 4096) return -EINVAL;
    // The sectors a GPT with 128 entries takes at the start (MBR, header, entries) and at the end (entries, header).
    const uint64_t entryBlocks = (128 * 128 + blockSize - 1) / blockSize;
    const uint64_t gptHead = (2 + entryBlocks) * (blockSize / 512), gptTail = (1 + entryBlocks) * (blockSize / 512);
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
        // 4 primary partitions and up to 32 logical ones, numbered from 5 as Linux numbers them.
        if (head[2] == "mbr" && p.index > 36) return -EINVAL;
        if (head[2] != "gpt" && (p.cls != Part::Class::kPublic || !p.partGuid.empty())) return -EINVAL;
        if (head[2] == "gpt" &&
            (p.partGuid.empty() || sectors < gptHead + gptTail || p.start < gptHead ||
             p.start + p.length > sectors - gptTail)) {
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
    uint32_t blockSize = BlockSize(dev.get());
    if (!blockSize) {
        LOG(ERROR) << "fsvm: " << diskPath << " has a logical block size the VM does not take";
        return -EINVAL;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -errno;
    unique_fd rd(p[0]), wr(p[1]);
    pid_t child = Spawn({"--probe", "--disk-bytes", std::to_string(diskBytes), "--block-size",
                         std::to_string(blockSize)},
                        dev.get(), -1, wr.get());
    if (child < 0) {
        LOG(ERROR) << "fsvm: fsvmd did not start for " << diskPath;
        return -EIO;
    }
    wr.reset();
    dev.reset();
    std::string text;
    status_t res = ReadAll(rd.get(), NowMs() + kProbeTimeoutMs, kMaxProbeOutput, &text);
    int rc = Reap(child, res == OK ? 3000 : 0, 0);
    if (res != OK || rc != 0) {
        LOG(ERROR) << "fsvm: the filesystem VM did not read " << diskPath << " (" << res << ", exit " << rc << ")";
        return res != OK ? res : -EIO;
    }
    res = ParseLayout(text, diskBytes, out, blockSize);
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
static void Watch(unique_fd conn, std::string volumeId, pid_t pid) {
    std::thread([conn = std::move(conn), volumeId, pid]() {
        // The service closes the connection once fsvmd ended. Answers are left for Reap to read.
        struct pollfd pfd = {conn.get(), 0, 0};
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
                std::to_string(v.diskBytes), "--block-size", "512"};
    } else {
        args = {"--volume", v.volumeId, "--gid", std::to_string(v.gid), "--disk-bytes", std::to_string(v.diskBytes),
                "--block-size", std::to_string(v.blockSize),
                "--partition", std::to_string(v.part.index), "--start", std::to_string(v.part.start),
                "--length", std::to_string(v.part.length), "--fs", v.part.fsType.empty() ? "-" : v.part.fsType,
                "--meta-bytes", std::to_string(v.part.metaBytes)};
        if (v.readOnly) args.push_back("--read-only");
    }
    pid_t child = Spawn(args, v.fd.get(), fuse.get(), readyWrite.get());
    if (child < 0) {
        int e = errno;
        LOG(ERROR) << "fsvm: fsvmd did not start for " << v.volumeId;
        umount2(v.target.c_str(), MNT_DETACH);
        return -e;
    }
    readyWrite.reset();
    v.fd.reset();
    fuse.reset();
    unique_fd conn = Connection(child);

    struct pollfd pfd = {readyRead.get(), POLLIN, 0};
    char c = 0;
    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, kReadyTimeoutMs)) == 1 &&
        TEMP_FAILURE_RETRY(read(readyRead.get(), &c, 1)) == 1 && c == 'R') {
        LOG(INFO) << "fsvm: " << v.volumeId << " served by the filesystem VM, fsvmd pid " << child;
        *pid = child;
        if (!v.obb) sServing++;
        if (conn >= 0) {
            Watch(std::move(conn), v.volumeId, child);
        } else {
            PLOG(WARNING) << "fsvm: fsvmd " << child << " is not watched";
        }
        return OK;
    }
    LOG(ERROR) << "fsvm: the filesystem VM did not serve " << v.volumeId << ", not mounting it";
    Stop(child, false);
    umount2(v.target.c_str(), MNT_DETACH);
    return -EIO;
}

void Stop(pid_t pid, bool orderly) {
    if (pid <= 0) return;
    int64_t t0 = NowMs();
    int rc = Reap(pid, orderly ? kSyncMs : 0, kTermMs);
    if (rc < 0) {
        LOG(WARNING) << "fsvm: fsvmd " << pid << " killed after " << NowMs() - t0 << " ms";
    } else {
        LOG(INFO) << "fsvm: fsvmd " << pid << " ended (" << rc << ") after " << NowMs() - t0 << " ms";
    }
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
    unique_fd conn;
};

Guard::Guard(pid_t pid, int ms) : mState(std::make_shared<State>()) {
    // Through fsvmd's connection to the service, which signals only its own child, never a reused pid.
    mState->conn = Connection(pid);
    std::thread([state = mState, pid, ms]() {
        std::unique_lock<std::mutex> l(state->lock);
        if (!state->cv.wait_for(l, std::chrono::milliseconds(ms), [&] { return state->done; }) &&
            state->conn >= 0) {
            LOG(ERROR) << "fsvm: the steps around the VM took too long, stopping fsvmd " << pid;
            // Ends its FUSE connection, so the steps waiting on it fail at once. A stop first, so that fsvmd writes
            // the volume back if it is not stuck itself, then a kill.
            Tell(state->conn.get(), 'T');
            if (!state->cv.wait_for(l, std::chrono::milliseconds(kTermMs), [&] { return state->done; })) {
                Tell(state->conn.get(), 'K');
            }
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
