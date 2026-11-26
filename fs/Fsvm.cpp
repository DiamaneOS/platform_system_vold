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
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <libdm/dm.h>
#include <packagelistparser/packagelistparser.h>
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <iterator>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#include "Utils.h"
#include "VolumeManager.h"
#include "model/VolumeBase.h"

using android::base::ParseUint;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;

namespace android {
namespace vold {
namespace fsvm {

/*
 * One fsvmd service per slot, each with a user of its own (vendor/diamaneos fsvm/fsvmd.rc and config.fs): one probe,
 * kMaxServing served volumes and four OBB images (ObbVolumeFsvm.cpp kMaxObbVms) at a time. No service runs from boot:
 * vold starts a slot it does not use now for each fsvmd (StartOn), and the service ends with that fsvmd.
 * scripts/p3-slots-check.py checks that these lists, fsvmd.rc and config.fs agree.
 */
static constexpr char kServicePrefix[] = "/dev/socket/fsvmd_";
static const char* const kProbeSlots[] = {"probe"};
static const char* const kVolumeSlots[] = {"0", "1", "2", "3", "4", "5"};
static const char* const kObbSlots[] = {"obb_0", "obb_1", "obb_2", "obb_3"};
/*
 * vold holds its lock while it waits, and system_server's watchdog asks for that lock (60 s). Every wait is bounded:
 *
 * - probe: fsvmd keeps to 20 s (PROBE_BUDGET), vold reads for at most 23 s, then Reap takes at most 4 s in all.
 *   At most 27 s. Probes of all disks share a limit (TakeProbe), so a device that comes and goes in a loop cannot
 *   keep vold starting VMs.
 * - format: the wipe and the format tool, then one probe of at most 10 s (plus 1 s for the kill), never the 27 s.
 * - OBB on a card: the open and its attributes go through the card's fsvmd, guarded for 8 s.
 * - mount: fsvmd keeps to 34 s (SERVE_BUDGET: VM starts, the mount, the first free-space count), vold waits 37 s.
 *   A volume marked dirty is mounted read-only and checked in the VM afterwards, outside the lock (fsvmd fs.rs
 *   Checker): the check is not part of the mount any more. The steps after it (MediaProvider's FUSE, bind mounts) end at the deadline
 *   PublicVolume's Guard sets: 52 s after the mount began, at least 15 s. A failure before the VM served adds a stop
 *   of at most 3 s (55 s). A failure after it runs doUnmount, whose steps fail at once on the ended FUSE connection and whose stop
 *   finds fsvmd ended: a few seconds more.
 * - unmount: OBB images served from the card are stopped together first (3 s), the steps before the stop get 15 s
 *   (a Guard ends fsvmd when FUSE requests hang), the orderly stop at most kSyncMs plus 3 s. At most 47 s. When a
 *   disk goes with several volumes, all of them are asked to write back first (RequestWriteBack) and do so at the
 *   same time, so each later stop finds its fsvmd done.
 * - ranges: a removal is one try, never a wait (a range still held goes when it is closed, DeleteRange).
 */
static constexpr int kProbeTimeoutMs = 23000;
static constexpr int kReadyTimeoutMs = 37000;
/*
 * After vold unmounted FUSE, fsvmd writes back and unmounts in the VM: 2 s for a call in progress, then as long as the
 * VM's dirty data keeps going down, at most 22 s in all (fsvmd CLOSE_LOCK_WAIT and CLOSE_MAX).
 */
static constexpr int kSyncMs = 26000;
/* After a stop request, then after a kill. */
static constexpr int kTermMs = 2000;
static constexpr int kKillMs = 1000;
/*
 * After a probe's or a format's answer fsvmd still ends its VM, and the hypervisor gives the VM's memory back first,
 * which took more than 3 s after a format. An answer that came whole counts however fsvmd ends.
 */
static constexpr int kVmEndMs = 10000;
/* init starts the service, which then answers a request at once: it only forks. */
static constexpr int kStartMs = 3000;
/* Serving VMs at a time, OBB images excluded (ObbVolume has its own caps). */
static constexpr int kMaxServing = 6;
static_assert(std::size(kVolumeSlots) == kMaxServing, "one fsvmd slot per served volume");
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
/* The slots vold uses now, and each fsvmd's slot: a slot is vold's from its service's start until its fsvmd was reaped
   (the service ends with it). Under sChildrenLock. */
static std::set<std::string> sBusySlots;
static std::map<pid_t, std::string> sSlotOf;
/* A repair's pipe, from StartRepair to WaitRepair. Under sChildrenLock. */
static std::map<pid_t, unique_fd> sRepairPipes;

/* Drops fsvmd `pid` from the tables after its end was read elsewhere (WaitRepair): its connection, and its slot. */
static void Forget(pid_t pid) {
    std::lock_guard<std::mutex> lock(sChildrenLock);
    sChildren.erase(pid);
    auto slot = sSlotOf.find(pid);
    if (slot != sSlotOf.end()) {
        sBusySlots.erase(slot->second);
        sSlotOf.erase(slot);
    }
}

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

/* Has slot `slot`'s service run fsvmd with exactly the descriptors given as 3, 4 and 5 (-1: none). Returns its pid,
 * or -1 with errno set (EBUSY: the slot runs a fsvmd already). */
static pid_t SpawnOn(const char* slot, const std::vector<std::string>& args, int fd3, int fd4, int fd5) {
    // Not blocking: a service that is not accepting (stopped, restarting) refuses at once instead of holding vold.
    unique_fd s(socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (s < 0) return -1;
    struct sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s%s", kServicePrefix, slot);
    if (TEMP_FAILURE_RETRY(connect(s.get(), reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)))) {
        return -1;  // not started yet, or already serving (StartOn tries again)
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
        errno = kind == 'E' ? static_cast<int>(value) : EIO;
        if (errno != EBUSY) {
            LOG(ERROR) << "fsvm: the fsvmd_" << slot << " service did not start fsvmd ("
                       << (kind == 'E' ? strerror(static_cast<int>(value)) : "no answer") << ")";
        }
        return -1;
    }
    pid_t pid = static_cast<pid_t>(value);
    std::lock_guard<std::mutex> lock(sChildrenLock);
    sChildren[pid] = std::move(s);
    return pid;
}

/*
 * Starts the slot's service (none runs from boot: init's ctl.start, which fsvmd.te allows vold for these services
 * only) and hands it the request, within kStartMs. A service that is still ending after its last fsvmd ignores the
 * start: it is asked again until its socket answers.
 */
static pid_t StartOn(const char* slot, const std::vector<std::string>& args, int fd3, int fd4, int fd5) {
    const std::string service = std::string("fsvmd_") + slot;
    const int64_t deadline = NowMs() + kStartMs;
    int64_t askAt = 0;
    for (;;) {
        if (NowMs() >= askAt) {
            android::base::SetProperty("ctl.start", service);
            askAt = NowMs() + 500;
        }
        pid_t pid = SpawnOn(slot, args, fd3, fd4, fd5);
        if (pid > 0) return pid;
        int e = errno;
        if ((e != ECONNREFUSED && e != ENOENT) || NowMs() >= deadline) {
            PLOG(ERROR) << "fsvm: the " << service << " service did not take the request";
            errno = e;
            return -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

/* The first slot of the request's kind (a probe, an OBB image or a volume) vold does not use now runs fsvmd. */
static pid_t Spawn(const std::vector<std::string>& args, int fd3, int fd4, int fd5) {
    // A format runs where probes run (the probe slot), one at a time under vold's lock.
    const bool probe = !args.empty() && (args[0] == "--probe" || args[0] == "--format");
    const bool obb = !args.empty() && args[0] == "--obb";
    const char* const* slots = probe ? kProbeSlots : obb ? kObbSlots : kVolumeSlots;
    size_t n = probe ? std::size(kProbeSlots) : obb ? std::size(kObbSlots) : std::size(kVolumeSlots);
    for (size_t i = 0; i < n; i++) {
        {
            std::lock_guard<std::mutex> lock(sChildrenLock);
            if (!sBusySlots.insert(slots[i]).second) continue;
        }
        pid_t pid = StartOn(slots[i], args, fd3, fd4, fd5);
        std::lock_guard<std::mutex> lock(sChildrenLock);
        if (pid > 0) {
            sSlotOf[pid] = slots[i];
            return pid;
        }
        int e = errno;
        sBusySlots.erase(slots[i]);
        errno = e;
        if (errno != EBUSY) return -1;
    }
    LOG(ERROR) << "fsvm: no free fsvmd slot for " << (probe ? "a probe or format" : obb ? "an OBB image" : "a volume");
    errno = EBUSY;
    return -1;
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
        // The service ends with its fsvmd: the slot is free for the next start.
        auto slot = sSlotOf.find(pid);
        if (slot != sSlotOf.end()) {
            sBusySlots.erase(slot->second);
            sSlotOf.erase(slot);
        }
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
        // "bad": a FAT or exFAT boot sector that fails the VM's checks.
        if (f[5] == "vfat" || f[5] == "exfat") p.fsType = f[5];
        else if (f[5] == "bad") p.damaged = true;
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
        if (p.cls != Part::Class::kPublic && (!p.fsType.empty() || p.damaged)) return -EINVAL;
        // A whole-disk entry names a filesystem, sound or damaged (a blank disk has no entry).
        if (p.index == 0 && p.fsType.empty() && !p.damaged) return -EINVAL;
        for (const auto& q : out->parts) {
            if (q.index == p.index || (q.cls == p.cls && p.cls != Part::Class::kPublic)) return -EINVAL;
            if (p.start < q.start + q.length && q.start < p.start + p.length) return -EINVAL;
        }
        out->parts.push_back(p);
    }
    return OK;
}

bool OffersFormat(const Layout& l) {
    bool usable = false;
    for (const auto& p : l.parts) {
        if (p.cls == Part::Class::kExpand || p.damaged) usable = true;
        if (p.cls != Part::Class::kPublic) continue;
        if (!p.fsType.empty()) usable = true;
        else if (!p.damaged) return true;
    }
    return !usable;
}

bool SameLayout(const Layout& a, const Layout& b) {
    if (a.diskSeq != b.diskSeq || a.diskBytes != b.diskBytes) return false;
    if (a.table != b.table || a.parts.size() != b.parts.size()) return false;
    for (size_t i = 0; i < a.parts.size(); i++) {
        const Part &p = a.parts[i], &q = b.parts[i];
        if (p.unread || q.unread || p.index != q.index || p.cls != q.cls || p.start != q.start ||
            p.length != q.length || p.fsType != q.fsType || p.fsUuid != q.fsUuid || p.partGuid != q.partGuid ||
            p.label != q.label || p.metaBytes != q.metaBytes || p.damaged != q.damaged) {
            return false;
        }
    }
    return true;
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

status_t Probe(const std::string& diskPath, uint64_t diskBytes, Layout* out, int timeoutMs) {
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
    // Writes through a range (a format, a VM with O_DIRECT) bypass the disk's own page cache: the VM must read the
    // disk as it is now, not as an earlier read left it in that cache.
    posix_fadvise(dev.get(), 0, 0, POSIX_FADV_DONTNEED);
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
    int readMs = timeoutMs > 0 ? std::min(timeoutMs, kProbeTimeoutMs) : kProbeTimeoutMs;
    status_t res = ReadAll(rd.get(), NowMs() + readMs, kMaxProbeOutput, &text);
    int rc = Reap(child, res == OK ? kVmEndMs : 0, 0);
    // -1: answered, then stopped while its VM was still ending. Any other exit status is a failure.
    if (res != OK || (rc != 0 && rc != -1)) {
        LOG(ERROR) << "fsvm: the filesystem VM did not read " << diskPath << " (" << res << ", exit " << rc << ")";
        return res != OK ? res : -EIO;
    }
    if (rc == -1) LOG(WARNING) << "fsvm: fsvmd did not end within " << kVmEndMs << " ms after the probe of " << diskPath;
    res = ParseLayout(text, diskBytes, out, blockSize);
    if (res != OK) {
        LOG(ERROR) << "fsvm: refusing the layout of " << diskPath;
        return res;
    }
    LOG(INFO) << "fsvm: " << diskPath << " has a " << out->table << " table with " << out->parts.size()
              << " partitions Android uses";
    return OK;
}

/* fsvmd keeps to 40 s for a format (FORMAT_BUDGET), vold reads for 45 s: under vold's lock, as mkfs ran before. */
static constexpr int kFormatTimeoutMs = 45000;

status_t Format(const std::string& path, const std::string& fsType, dev_t disk, const std::string& seq) {
    if (fsType != "vfat" && fsType != "exfat") return -EINVAL;
    unique_fd dev(TEMP_FAILURE_RETRY(open(path.c_str(), O_RDWR | O_CLOEXEC)));
    if (dev < 0) {
        PLOG(ERROR) << "fsvm: opening " << path << " to format it";
        return -errno;
    }
    // A format chosen for the medium the probe read, never for one put in since.
    if (disk && !SameMedium(disk, seq)) {
        LOG(ERROR) << "fsvm: the medium changed since " << path << " was read, not formatting it";
        return -ENODEV;
    }
    uint64_t bytes = 0;
    uint32_t blockSize = BlockSize(dev.get());
    if (GetBlockDevSize(dev.get(), &bytes) != OK || bytes < 512 || !blockSize) return -EINVAL;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -errno;
    unique_fd rd(p[0]), wr(p[1]);
    pid_t child = Spawn({"--format", fsType, "--disk-bytes", std::to_string(bytes & ~511ULL), "--block-size",
                         std::to_string(blockSize)},
                        dev.get(), -1, wr.get());
    if (child < 0) {
        LOG(ERROR) << "fsvm: fsvmd did not start to format " << path;
        return -EIO;
    }
    wr.reset();
    dev.reset();
    std::string text;
    status_t res = ReadAll(rd.get(), NowMs() + kFormatTimeoutMs, 16, &text);
    // The VM answers OK only after the format and a sync of the disk (fsvm_guest.c do_format).
    const bool done = res == OK && text == "OK\n";
    int rc = Reap(child, done ? kVmEndMs : 0, 0);
    if (!done || (rc != 0 && rc != -1)) {
        LOG(ERROR) << "fsvm: the filesystem VM did not format " << path << " (" << res << ", exit " << rc << ")";
        return -EIO;
    }
    if (rc == -1) LOG(WARNING) << "fsvm: fsvmd did not end within " << kVmEndMs << " ms after formatting " << path;
    LOG(INFO) << "fsvm: " << path << " formatted as " << fsType << " in the filesystem VM";
    return OK;
}

/*
 * Waits for fsvmd to end and, when it ended on its own while its volume is still mounted (its VM broke and no new
 * one came up), has the volume unmounted and reported as not opened. Meanwhile passes on what fsvmd says of the VM's
 * check after the mount, from its pipe: W (writable now), F (it stays read-only), c (a new VM checks it again,
 * read-only meanwhile), E (the errors need a repair longer than apps may wait, PublicVolume::startRepairInVm).
 */
static void Watch(unique_fd conn, unique_fd ready, std::string volumeId, pid_t pid) {
    std::thread([conn = std::move(conn), ready = std::move(ready), volumeId, pid]() {
        // The service closes the connection once fsvmd ended. Answers are left for Reap to read.
        struct pollfd pfd[2] = {{conn.get(), 0, 0}, {ready.get(), POLLIN, 0}};
        for (;;) {
            int n = TEMP_FAILURE_RETRY(poll(pfd, ready >= 0 ? 2 : 1, -1));
            if (n <= 0) continue;
            if (pfd[0].revents) break;
            if (pfd[1].revents & POLLIN) {
                char c = 0;
                if (TEMP_FAILURE_RETRY(read(ready.get(), &c, 1)) == 1 && (c == 'W' || c == 'F' || c == 'c' || c == 'E')) {
                    std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
                    auto vol = VolumeManager::Instance()->findVolume(volumeId);
                    if (vol) vol->onCheckDone(pid, c);
                    continue;
                }
            }
            if (pfd[1].revents) pfd[1].fd = -1;  // fsvmd closed it or wrote something else: only the end counts
        }
        std::lock_guard<std::mutex> lock(VolumeManager::Instance()->getLock());
        auto vol = VolumeManager::Instance()->findVolume(volumeId);
        if (vol) vol->onFsvmdExit(pid);
    }).detach();
}

status_t Mount(Volume&& v, pid_t* pid, bool* servedReadOnly) {
    if (servedReadOnly) *servedReadOnly = false;
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
                "--meta-bytes", std::to_string(v.part.metaBytes), "--owner-uid", std::to_string(v.ownerUid)};
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
        TEMP_FAILURE_RETRY(read(readyRead.get(), &c, 1)) == 1 && (c == 'R' || c == 'r' || c == 'c')) {
        // r: asked for writing, served read-only by the VM. c: marked dirty, read-only while the VM checks it, then
        // W or F on the same pipe (fsvmd main.rs). Both are reported read-only: apps would get EROFS from a volume
        // reported writable.
        if (servedReadOnly) *servedReadOnly = c == 'r' || c == 'c';
        LOG(INFO) << "fsvm: " << v.volumeId << " served by the filesystem VM, fsvmd pid " << child
                  << (c == 'c' ? ", read-only while the VM checks it" : "");
        *pid = child;
        if (!v.obb) sServing++;
        if (conn >= 0) {
            // A card's pipe stays open for the check's end, and for a check a new VM starts after a broken session.
            if (v.obb) readyRead.reset();
            Watch(std::move(conn), std::move(readyRead), v.volumeId, child);
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

int Stop(pid_t pid, bool orderly) {
    if (pid <= 0) return 0;
    int64_t t0 = NowMs();
    int rc = Reap(pid, orderly ? kSyncMs : 0, kTermMs);
    if (rc < 0) {
        LOG(WARNING) << "fsvm: fsvmd " << pid << " killed after " << NowMs() - t0 << " ms";
    } else if (rc == kExitNotWrittenBack) {
        LOG(ERROR) << "fsvm: fsvmd " << pid << " could not write its volume back (" << NowMs() - t0 << " ms)";
    } else {
        LOG(INFO) << "fsvm: fsvmd " << pid << " ended (" << rc << ") after " << NowMs() - t0 << " ms";
    }
    return rc;
}

void RequestStop(pid_t pid) {
    unique_fd conn = Connection(pid);
    if (conn >= 0) Tell(conn.get(), 'T');
}

void Kill(pid_t pid) {
    if (pid <= 0) return;
    Reap(pid, 0, 0);
    LOG(WARNING) << "fsvm: fsvmd " << pid << " killed without writing back";
}

pid_t StartRepair(Volume&& v) {
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    unique_fd rd(p[0]), wr(p[1]);
    // On a volume slot: a repair may take long, and the probe slot stays free for other disks.
    std::vector<std::string> args = {"--repair", "--volume", v.volumeId, "--gid", std::to_string(v.gid),
                                     "--disk-bytes", std::to_string(v.diskBytes), "--block-size",
                                     std::to_string(v.blockSize), "--partition", std::to_string(v.part.index),
                                     "--start", std::to_string(v.part.start), "--length", std::to_string(v.part.length),
                                     "--fs", v.part.fsType.empty() ? "-" : v.part.fsType,
                                     // The repair VM is sized like the volume's serving VM.
                                     "--meta-bytes", std::to_string(v.part.metaBytes)};
    pid_t pid = Spawn(args, v.fd.get(), -1, wr.get());
    if (pid <= 0) {
        LOG(ERROR) << "fsvm: no fsvmd for the repair of " << v.volumeId;
        return -1;
    }
    Served(false, true);
    std::lock_guard<std::mutex> lock(sChildrenLock);
    sRepairPipes[pid] = std::move(rd);
    LOG(INFO) << "fsvm: repairing " << v.volumeId << " in the filesystem VM, fsvmd pid " << pid;
    return pid;
}

int WaitRepair(pid_t pid) {
    unique_fd conn = Connection(pid);
    unique_fd rd;
    {
        std::lock_guard<std::mutex> lock(sChildrenLock);
        auto it = sRepairPipes.find(pid);
        if (it != sRepairPipes.end()) {
            rd = std::move(it->second);
            sRepairPipes.erase(it);
        }
    }
    char kind = 0;
    uint64_t value = 0;
    // No time limit: the end, or the connection closing (Kill reaped it first, an eject).
    bool ended = conn >= 0 && ReadAnswer(conn.get(), -1, &kind, &value) && kind == 'X';
    std::string text;
    if (rd >= 0) ReadAll(rd.get(), NowMs() + 1000, 16, &text);
    Forget(pid);
    Served(false, false);
    if (!ended) return -1;
    if (value == 0 && text == "OK\n") return 0;
    return value == 0 ? -1 : static_cast<int>(value);
}

void RequestWriteBack(pid_t pid) {
    unique_fd conn = Connection(pid);
    if (conn >= 0) Tell(conn.get(), 'W');
}

/*
 * Probes for all disks together: 4 at once, then one per 5 s. A probe is a VM start under vold's lock, and a device
 * that detaches and attaches in a loop is a new disk each time. Only called with vold's lock held.
 */
static constexpr int kProbeBurst = 4;
static constexpr int64_t kProbeEveryMs = 5000;
static int sProbes = kProbeBurst;
static int64_t sProbeRefillMs = 0;

bool TakeProbe(int64_t nowMs, int64_t* waitMs) {
    if (sProbes >= kProbeBurst) {
        sProbeRefillMs = nowMs;
    } else if (nowMs - sProbeRefillMs >= kProbeEveryMs) {
        int64_t n = (nowMs - sProbeRefillMs) / kProbeEveryMs;
        sProbes = static_cast<int>(std::min<int64_t>(kProbeBurst, sProbes + n));
        sProbeRefillMs = sProbes >= kProbeBurst ? nowMs : sProbeRefillMs + n * kProbeEveryMs;
    }
    if (sProbes == 0) {
        *waitMs = std::max<int64_t>(1, sProbeRefillMs + kProbeEveryMs - nowMs);
        return false;
    }
    sProbes--;
    return true;
}

void ResetProbes() {
    sProbes = kProbeBurst;
    sProbeRefillMs = 0;
}

static constexpr int64_t kRereadGapMs = 3000;

ProbeMemory::Next ProbeMemory::next(const std::string& seq, uint64_t size, int64_t nowMs, bool forced,
                                    bool justPartitioned, bool failed) const {
    if (forced || !mProbed || justPartitioned) return Next::kRead;
    if (!failed && holds(seq, size)) return Next::kSkip;
    if (nowMs - mAtMs < kRereadGapMs) return Next::kLater;
    return Next::kRead;
}

bool ProbeMemory::took(const std::string& seq, uint64_t size, int64_t nowMs) {
    const bool other = !holds(seq, size);
    mProbed = true;
    mSeq = seq;
    mSize = size;
    mAtMs = nowMs;
    return other;
}

bool ProbeMemory::holds(const std::string& seq, uint64_t size) const {
    return !mProbed || (seq == mSeq && size == mSize);
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

static std::string RangePrefix(dev_t disk) {
    return StringPrintf("fsvm-%u-%u-", major(disk), minor(disk));
}

std::string RangeName(dev_t disk, uint32_t index) {
    return RangePrefix(disk) + std::to_string(index);
}

uid_t MediaProviderUid(userid_t user) {
    // The MediaProvider module's package, AOSP's name and Google's. Read as the old sdcard daemon read the list.
    struct Found {
        appid_t appId = 0;
    } found;
    packagelist_parse(
            [](pkg_info* info, void* data) {
                auto* f = static_cast<Found*>(data);
                std::string name = info->name ? info->name : "";
                if (name == "com.android.providers.media.module" ||
                    name == "com.google.android.providers.media.module") {
                    f->appId = multiuser_get_app_id(info->uid);
                }
                packagelist_free(info);
                return f->appId == 0;
            },
            &found);
    if (found.appId < AID_APP_START) {
        LOG(ERROR) << "fsvm: MediaProvider is not in the package list, a card's files show root as their owner";
        return 0;
    }
    return multiuser_get_uid(user, found.appId);
}

bool SameMedium(dev_t disk, const std::string& seq) {
    if (seq.empty()) return true;
    std::string now;
    android::base::ReadFileToString(StringPrintf("/sys/dev/block/%u:%u/diskseq", major(disk), minor(disk)), &now);
    return android::base::Trim(now) == android::base::Trim(seq);
}

status_t CreateRange(const std::string& name, dev_t disk, uint64_t start, uint64_t length, bool readOnly,
                     std::string* node, dev_t* dev, const std::string& seq) {
    node->clear();
    if (length == 0 || start > UINT64_MAX / 512 - length) return -EINVAL;
    // Only over the medium the range was read from (a card swapped in a USB reader keeps the disk).
    if (!SameMedium(disk, seq)) {
        LOG(ERROR) << "fsvm: the medium changed since it was read, no range " << name;
        return -ENODEV;
    }
    auto& dm = android::dm::DeviceMapper::Instance();
    // A range left from before (vold restarted while it existed) goes first.
    DeleteRange(name, "/dev/block/vold/" + name);
    android::dm::DmTable table;
    // By device number: the kernel opens the disk itself, no path is looked up.
    if (!table.Emplace<android::dm::DmTargetLinear>(0, length, StringPrintf("%u:%u", major(disk), minor(disk)),
                                                     start)) {
        return -EINVAL;
    }
    table.set_readonly(readOnly);
    std::string path;
    if (!dm.CreateDevice(name, table, &path, std::chrono::seconds(5))) {
        // Also a range of the same name a VM still holds, waiting for its deferred removal: the mount is tried again.
        PLOG(ERROR) << "fsvm: creating the range " << name;
        return -EIO;
    }
    struct stat st;
    if (stat(path.c_str(), &st) || !S_ISBLK(st.st_mode)) {
        PLOG(ERROR) << "fsvm: the range " << name << " has no device";
        DeleteRange(name, "");
        return -EIO;
    }
    // The medium may have changed while the range was set up: then it goes again, before anything used it.
    if (!SameMedium(disk, seq)) {
        LOG(ERROR) << "fsvm: the medium changed while the range " << name << " was set up";
        DeleteRange(name, "");
        return -ENODEV;
    }
    // vold's own node for it, labelled vold_device like the disk's: only fsvmd's VM manager and crosvm may use it,
    // through the descriptor vold opens.
    std::string n = "/dev/block/vold/" + name;
    if (CreateDeviceNode(n, st.st_rdev) != OK) {
        DeleteRange(name, "");
        return -EIO;
    }
    *node = n;
    if (dev) *dev = st.st_rdev;
    LOG(INFO) << "fsvm: range " << name << " is sectors " << start << " to " << start + length << " of "
              << major(disk) << ":" << minor(disk) << (readOnly ? ", read-only" : "");
    return OK;
}

status_t DeleteRange(const std::string& name, const std::string& node) {
    if (!node.empty()) DestroyDeviceNode(node);
    auto& dm = android::dm::DeviceMapper::Instance();
    // One try. A VM that is slow to end, or stuck on a stalled USB disk, still holds it: the kernel then removes it
    // once it is closed (deferred removal), and vold, which holds its lock here, never waits for that.
    if (dm.DeleteDeviceIfExists(name)) return OK;
    if (errno == EBUSY && dm.DeleteDeviceIfExistsDeferred(name)) {
        LOG(WARNING) << "fsvm: range " << name << " still in use, removed as soon as it is closed";
        return OK;
    }
    PLOG(ERROR) << "fsvm: removing the range " << name;
    return -EBUSY;
}

static void ForEachRange(const std::string& prefix, const std::function<void(const std::string&)>& fn) {
    std::vector<android::dm::DeviceMapper::DmBlockDevice> devices;
    if (!android::dm::DeviceMapper::Instance().GetAvailableDevices(&devices)) return;
    for (const auto& d : devices) {
        if (android::base::StartsWith(d.name(), prefix)) fn(d.name());
    }
}

void DeleteRangesOf(dev_t disk) {
    ForEachRange(RangePrefix(disk), [](const std::string& name) {
        LOG(WARNING) << "fsvm: removing the range " << name << " left from before";
        DeleteRange(name, "/dev/block/vold/" + name);
    });
}

void FailRangesOf(dev_t disk) {
    auto& dm = android::dm::DeviceMapper::Instance();
    ForEachRange(RangePrefix(disk), [&dm](const std::string& name) {
        std::vector<android::dm::DeviceMapper::TargetInfo> targets;
        uint64_t sectors = 0;
        if (dm.GetTableInfo(name, &targets)) {
            for (const auto& t : targets) sectors = std::max<uint64_t>(sectors, t.spec.sector_start + t.spec.length);
        }
        android::dm::DmTable table;
        if (!sectors || !table.Emplace<android::dm::DmTargetError>(0, sectors) ||
            !dm.LoadTableAndActivate(name, table)) {
            PLOG(ERROR) << "fsvm: the range " << name << " could not be cut off from the changed medium";
            return;
        }
        LOG(WARNING) << "fsvm: the medium changed, the range " << name << " fails all I/O now";
    });
}

void SweepRanges() {
    ForEachRange("fsvm-", [](const std::string& name) {
        unsigned maj = 0, min = 0, index = 0;
        char end = 0;
        if (sscanf(name.c_str(), "fsvm-%u-%u-%u%c", &maj, &min, &index, &end) != 3) return;
        // The disk is still there: its own probe removes what it left (DeleteRangesOf).
        if (access(StringPrintf("/sys/dev/block/%u:%u", maj, min).c_str(), F_OK) == 0) return;
        LOG(WARNING) << "fsvm: removing the range " << name << " of a disk that is gone";
        DeleteRange(name, "/dev/block/vold/" + name);
    });
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
