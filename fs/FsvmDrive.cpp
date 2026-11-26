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

#include "FsvmDrive.h"
#include "FsvmChild.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>

#include "Utils.h"

using android::base::ParseInt;
using android::base::ParseUint;
using android::base::Split;
using android::base::StringPrintf;
using android::base::unique_fd;

namespace android {
namespace vold {
namespace fsvm {

/* The ID a served partition's Watch callbacks carry (PublicVolume mServing): never a pid. */
static std::atomic<pid_t> sNextServing{-2};
/* Repairs running in drives' VMs, by token (never a pid): the pipe their result comes on. */
static std::mutex sRepairsLock;
static std::map<pid_t, unique_fd> sRepairs;
static pid_t sNextRepair = -2;

/* fsvm proto layout.rs: MIN_SERVE_MIB, MAX_SERVE_MIB, DRIVE_PART_MIB and serve_memory_mib. */
static constexpr uint64_t kMinServeMiB = 96;
static constexpr uint64_t kMaxServeMiB = 256;
static constexpr uint64_t kDrivePartMiB = 16;

static uint64_t MetaMiB(const Part& p) {
    // Rounded up without an overflow, as Rust's div_ceil: a card may claim any size.
    const uint64_t mib = (p.metaBytes >> 20) + ((p.metaBytes & ((1 << 20) - 1)) != 0);
    return std::min<uint64_t>(mib, 1 << 20);
}

static uint64_t ServeMiB(const Part& p) {
    const uint64_t meta = MetaMiB(p);
    uint64_t want = p.fsType == "exfat" ? 80 + meta : 64 + std::min<uint64_t>(meta * 5 / 4, 160);
    return std::clamp(want, kMinServeMiB, kMaxServeMiB);
}

static uint64_t MetaShareMiB(const Part& p) {
    const uint64_t meta = MetaMiB(p);
    return p.fsType == "exfat" ? 16 + meta : std::min<uint64_t>(meta * 5 / 4, 160);
}

uint64_t DriveMemoryMib(const std::vector<Part>& parts) {
    if (parts.empty()) return kMinServeMiB;
    // The last of equal ones, as Rust's max_by_key takes it: the sum does not depend on which.
    size_t largest = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        if (ServeMiB(parts[i]) >= ServeMiB(parts[largest])) largest = i;
    }
    uint64_t want = ServeMiB(parts[largest]);
    for (size_t i = 0; i < parts.size(); i++) {
        if (i != largest) want += kDrivePartMiB + MetaShareMiB(parts[i]);
    }
    return std::clamp(want, kMinServeMiB, kMaxServeMiB);
}

std::vector<uint64_t> DriveVmMib(const std::vector<Part>& parts) {
    const uint64_t own = DriveMemoryMib(parts);
    if (own > kMinServeMiB) return {own, kMinServeMiB};
    return {own};
}

std::vector<Part> DriveParts(const Layout& l, uint32_t maxIndex, size_t max) {
    std::vector<Part> out;
    size_t counted = 0;
    for (const auto& p : l.parts) {
        if (p.cls != Part::Class::kPublic || p.unread || p.index > maxIndex) continue;
        if (++counted > max) break;
        if (!p.damaged) out.push_back(p);
    }
    return out;
}

bool ParseDriveAnswer(const std::string& msg, uint32_t* id, char* kind, int* disk, int* value, std::string* rest) {
    size_t nl = msg.find('\n');
    std::vector<std::string> f = Split(msg.substr(0, nl), " ");
    if (rest) *rest = nl == std::string::npos ? "" : msg.substr(nl + 1);
    // The request's ID first: plain digits, no leading zero, 1 to 4294967295.
    if (f.size() < 2 || f[0].empty() || f[0].size() > 10 || f[0][0] == '0' ||
        f[0].find_first_not_of("0123456789") != std::string::npos || !ParseUint(f[0], id, UINT32_MAX)) {
        return false;
    }
    f.erase(f.begin());
    if (f[0].size() != 1) return false;
    *kind = f[0][0];
    if (*kind == 'K') {
        *disk = 0;
        return f.size() == 2 && ParseInt(f[1], value, 0, 4095) && nl == std::string::npos;
    }
    if (std::string("ADFRC").find(*kind) == std::string::npos || f.size() != 3) return false;
    if (nl != std::string::npos && *kind != 'F') return false;
    return ParseInt(f[1], disk, 0, 7) && ParseInt(f[2], value, 0, 4095);
}

status_t FormattedPart(const std::string& text, const Part& p, Part* out) {
    Layout l;
    if (ParseLayout(text, p.length * 512, &l) != OK || l.table != "none" || l.parts.size() != 1) return -EIO;
    const Part& f = l.parts[0];
    if (f.index != 0 || f.start != 0 || f.length != p.length || f.fsType.empty() || f.damaged) return -EIO;
    *out = p;
    out->fsType = f.fsType;
    out->fsUuid = f.fsUuid;
    out->label = f.label;
    out->metaBytes = f.metaBytes;
    out->damaged = false;
    out->noFsConfirmed = false;
    return OK;
}

status_t FormatOutcome(status_t call, int value, const std::string& text, const Part& p, const std::string& fsType,
                       Part* out, bool* recheck) {
    *recheck = false;
    // The request went out and no answer came, or none that is one: the format may have run.
    if (call != OK) {
        LOG(ERROR) << "fsvm: no answer to the format of partition " << p.index << " (" << call
                   << "), whether it was formatted is not known: it is read again";
        *recheck = true;
        return -EIO;
    }
    switch (value) {
        case 0:
            if (FormattedPart(text, p, out) == OK && out->fsType == fsType) return OK;
            LOG(ERROR) << "fsvm: the drive's VM answered the format of partition " << p.index
                       << " with another filesystem than " << fsType << ": it is read again";
            *out = p;
            *recheck = true;
            return -EIO;
        case kFormatUnverified:
            // Formatted: the volume keeps the type just written (its mount checks it) and has no UUID until the disk
            // is read again.
            LOG(WARNING) << "fsvm: partition " << p.index << " formatted as " << fsType
                         << ", the new filesystem was not read back: it is read again";
            *out = p;
            out->fsType = fsType;
            out->fsUuid.clear();
            out->label.clear();
            out->metaBytes = 0;
            out->noFsConfirmed = false;
            *recheck = true;
            return OK;
        case kFormatUncertain:
            LOG(ERROR) << "fsvm: the format of partition " << p.index
                       << " broke off in the VM, whether it was formatted is not known: it is read again";
            *recheck = true;
            return -EIO;
        case EAGAIN:
            return -EAGAIN;
        default:
            LOG(ERROR) << "fsvm: partition " << p.index << " was not formatted (" << value << ")";
            return -EIO;
    }
}

void DriveControl::reset(unique_fd fd) {
    mFd = std::move(fd);
    mPending.clear();
    if (mNextId == 0) mNextId = 1;
}

bool DriveControl::pending(int disk) const {
    for (const auto& [id, d] : mPending) {
        if (d.disk == disk) return true;
    }
    return false;
}

status_t DriveControl::next(int timeoutMs, uint32_t* id, char* kind, int* disk, int* value, std::string* rest) {
    if (mFd < 0) return -EPIPE;
    struct pollfd pfd = {mFd.get(), POLLIN, 0};
    int r = TEMP_FAILURE_RETRY(poll(&pfd, 1, timeoutMs));
    if (r == 0) return -ETIMEDOUT;
    if (r < 0) return -EIO;
    char buf[4096];
    ssize_t n = TEMP_FAILURE_RETRY(recv(mFd.get(), buf, sizeof(buf), MSG_DONTWAIT | MSG_TRUNC));
    if (n < 0 && errno == EAGAIN) return -ETIMEDOUT;
    if (n <= 0) return -EPIPE;
    if (n > static_cast<ssize_t>(sizeof(buf)) || !ParseDriveAnswer(std::string(buf, n), id, kind, disk, value, rest)) {
        LOG(ERROR) << "fsvm: a malformed answer from the drive's fsvmd";
        return -EIO;
    }
    return OK;
}

void DriveControl::settle() {
    uint32_t id = 0;
    char k = 0;
    int d = -1, v = -1;
    std::string rest;
    // Bound unsolicited traffic, including the drain before a new request's deadline.
    for (int count = 0; count < 64; count++) {
        status_t res = next(0, &id, &k, &d, &v, &rest);
        if (res == -ETIMEDOUT) return;
        auto it = mPending.find(id);
        if (res != OK || (it != mPending.end() && (it->second.kind != k || it->second.disk != d))) {
            // A contradictory completion never settles a mutation. Restart the connection to recover.
            mFd.reset();
            return;
        }
        if (it != mPending.end()) mPending.erase(it);
    }
    mFd.reset();
}

status_t DriveControl::call(const std::string& msg, const std::vector<int>& fds, char kind, int disk, bool mutating,
                            int timeoutMs, int* value, std::string* rest) {
    if (mFd < 0) return -EPIPE;
    settle();
    if (mFd < 0 || mNextId == 0) {
        // IDs are never reused on a connection, including after u32 exhaustion.
        mFd.reset();
        return -EPIPE;
    }
    const uint32_t id = mNextId++;
    const int64_t deadline = NowMs() + timeoutMs;
    const std::string text = std::to_string(id) + " " + msg;
    struct iovec iov = {const_cast<char*>(text.data()), text.size()};
    union {
        char buf[CMSG_SPACE(sizeof(int) * 4)];
        struct cmsghdr align;
    } control = {};
    struct msghdr mh = {};
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;
    if (!fds.empty()) {
        if (fds.size() > 4) return -EINVAL;
        mh.msg_control = control.buf;
        mh.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        struct cmsghdr* c = CMSG_FIRSTHDR(&mh);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }
    if (TEMP_FAILURE_RETRY(sendmsg(mFd.get(), &mh, MSG_NOSIGNAL)) != static_cast<ssize_t>(text.size())) {
        PLOG(ERROR) << "fsvm: the drive's fsvmd did not take a request";
        return -EPIPE;
    }
    if (mutating) mPending[id] = {kind, disk};
    for (;;) {
        const int64_t left = deadline - NowMs();
        uint32_t got = 0;
        char k = 0;
        int d = -1, v = -1;
        status_t res = left > 0 ? next(static_cast<int>(left), &got, &k, &d, &v, rest) : -ETIMEDOUT;
        if (res == -ETIMEDOUT) {
            if (NowMs() < deadline) continue;
            LOG(ERROR) << "fsvm: the drive's fsvmd did not answer " << msg.substr(0, msg.find(' ')) << " within "
                       << timeoutMs << " ms" << (mutating ? ", it stays pending" : "");
            if (mutating) mPending[id] = {kind, disk};
            return -ETIMEDOUT;
        }
        if (res != OK) {
            // A malformed answer: whether this request ran is not known.
            if (mutating && res == -EIO) mPending[id] = {kind, disk};
            mFd.reset();
            return res;
        }
        if (got != id) {
            // An answer to an earlier request vold stopped waiting for: never this one's.
            auto it = mPending.find(got);
            if (it != mPending.end()) {
                if (it->second.kind != k || it->second.disk != d) {
                    mFd.reset();
                    return -EPIPE;
                }
                mPending.erase(it);
            }
            continue;
        }
        if (k != kind || d != disk) {
            LOG(ERROR) << "fsvm: the drive's fsvmd answered request " << id << " as another one";
            mFd.reset();
            return -EIO;
        }
        mPending.erase(id);
        *value = v;
        return OK;
    }
}

int WaitDriveRepair(pid_t token) {
    unique_fd rd;
    {
        std::lock_guard<std::mutex> lock(sRepairsLock);
        auto it = sRepairs.find(token);
        if (it == sRepairs.end()) return -1;
        rd = std::move(it->second);
        sRepairs.erase(it);
    }
    // No time limit: the drive's fsvmd writes OK when the partition is clean now, and the pipe closes at the
    // repair's end, at a cancel, and when the drive's fsvmd ends.
    std::string text;
    char buf[16];
    for (;;) {
        ssize_t n = TEMP_FAILURE_RETRY(read(rd.get(), buf, sizeof(buf)));
        if (n <= 0) break;
        text.append(buf, n);
        if (text.size() > 16) break;
    }
    return text == "OK\n" ? 0 : -1;
}

/* The FUSE connection of the mount at `target`, from the mount table (no request reaches the mount): the minor of
   its device. 0 when not found. */
static unsigned FuseConnection(const std::string& target) {
    std::string table;
    if (!android::base::ReadFileToString("/proc/self/mountinfo", &table)) return 0;
    unsigned conn = 0;
    for (const auto& line : Split(table, "\n")) {
        std::vector<std::string> f = Split(line, " ");
        auto dash = std::find(f.begin(), f.end(), "-");
        if (f.size() < 5 || f[4] != target || dash == f.end() || dash + 1 == f.end() || *(dash + 1) != "fuse") {
            continue;
        }
        std::vector<std::string> dev = Split(f[2], ":");
        if (dev.size() == 2 && dev[0] == "0") ParseUint(dev[1], &conn);
    }
    return conn;
}

Drive::Drive(dev_t disk, const std::string& diskPath, const std::string& diskId, uint64_t diskBytes)
    : mDisk(disk), mDiskPath(diskPath), mDiskId(diskId), mDiskBytes(diskBytes) {}

Drive::~Drive() {
    end();
}

void Drive::setParts(std::vector<Part> parts) {
    end();
    mLost.clear();
    mParts = std::move(parts);
}

int Drive::indexOf(const Part& p) const {
    for (size_t k = 0; k < mParts.size(); k++) {
        const Part& q = mParts[k];
        if (q.index == p.index && q.start == p.start && q.length == p.length) return static_cast<int>(k);
    }
    return -1;
}

bool Drive::serves(const Part& p) const {
    return indexOf(p) >= 0;
}

void Drive::removeRanges() {
    for (const auto& [name, node] : mRanges) DeleteRange(name, node);
    mRanges.clear();
    mNodes.clear();
}

status_t Drive::start() {
    if (mPid > 0) return OK;
    if (mParts.empty()) return -EIO;
    // A card with its write-protect switch on, as the kernel mount handled it: every range read-only.
    bool readOnly = false;
    {
        unique_fd probe(TEMP_FAILURE_RETRY(open(mDiskPath.c_str(), O_RDWR | O_CLOEXEC)));
        readOnly = probe < 0 && (errno == EROFS || errno == EACCES);
    }
    std::vector<unique_fd> fds;
    std::vector<std::string> disks;
    uint32_t blockSize = 0;
    for (const Part& p : mParts) {
        std::string node = mDiskPath;
        if (p.index == 0) {
            // A filesystem on the whole disk: the disk holds nothing else, and its VM gets the disk itself, only while
            // it holds the medium the probe read (a range checks that as it is set up).
            if (!SameMedium(mDisk, p.diskSeq)) {
                LOG(ERROR) << "fsvm: the medium changed since " << mDiskPath << " was read, not serving it";
                removeRanges();
                return -ENODEV;
            }
        } else {
            // Exactly this partition's sectors, as a device-mapper range of the disk: the VM's disk for it.
            std::string name = RangeName(mDisk, p.index);
            if (status_t res = CreateRange(name, mDisk, p.start, p.length, readOnly, &node, nullptr, p.diskSeq);
                res != OK) {
                removeRanges();
                return res;
            }
            mRanges.emplace_back(name, node);
        }
        unique_fd fd(TEMP_FAILURE_RETRY(open(node.c_str(), (readOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC)));
        uint32_t bs = fd >= 0 ? BlockSize(fd.get()) : 0;
        if (!bs || (blockSize && bs != blockSize)) {
            PLOG(ERROR) << "fsvm: no disk for partition " << p.index << " of " << mDiskPath;
            removeRanges();
            return -EIO;
        }
        blockSize = bs;
        disks.push_back(StringPrintf("%" PRIu64 ":%s:%" PRIu64, p.length * 512,
                                     p.fsType.empty() ? "-" : p.fsType.c_str(), p.metaBytes));
        mNodes.push_back(node);
        fds.push_back(std::move(fd));
    }
    mReadOnly = readOnly;
    std::vector<std::string> args = {"--drive", mDiskId, "--block-size", std::to_string(blockSize)};
    for (const auto& d : disks) {
        args.push_back("--disk");
        args.push_back(d);
    }
    if (readOnly) args.push_back("--read-only");
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        PLOG(ERROR) << "fsvm: socketpair for the drive " << mDiskPath;
        removeRanges();
        return -EIO;
    }
    unique_fd mine(sv[0]), theirs(sv[1]);
    // Admitted by the smallest VM fsvmd tries for the drive: a drive that fits only there does not wait.
    const std::vector<uint64_t> sizes = DriveVmMib(mParts);
    const uint64_t mib = sizes.front();
    pid_t pid = Spawn(args, theirs.get(), -1, -1, sizes.back());
    if (pid < 0) {
        int e = errno;
        // EAGAIN: no room for the VM. EBUSY: every fsvmd slot serves a drive already. Both wait for a place.
        LOG(ERROR) << "fsvm: fsvmd did not start for the drive " << mDiskPath
                   << (e == EBUSY ? ": all fsvmd slots are taken" : "");
        removeRanges();
        return e == EAGAIN || e == EBUSY ? -EAGAIN : -EIO;
    }
    theirs.reset();
    mControl.reset(std::move(mine));
    mPid = pid;
    std::vector<int> raw;
    for (const auto& fd : fds) raw.push_back(fd.get());
    int value = -1;
    if (request(StringPrintf("DISKS %zu", fds.size()), raw, 'K', 0, false, kStartMs, &value) != OK || value != 0) {
        LOG(ERROR) << "fsvm: the drive's fsvmd refused its disks (" << value << ")";
        kill();
        return -EIO;
    }
    LOG(INFO) << "fsvm: " << mDiskPath << " is served by one VM of " << mib << " MiB for its " << mParts.size()
              << " partitions, fsvmd pid " << pid << (readOnly ? ", read-only" : "");
    return OK;
}

status_t Drive::request(const std::string& msg, const std::vector<int>& fds, char kind, int disk, bool mutating,
                        int timeoutMs, int* value, std::string* rest) {
    status_t res = mControl.call(msg, fds, kind, disk, mutating, timeoutMs, value, rest);
    if (res == -EPIPE && mPid > 0) {
        LOG(ERROR) << "fsvm: the drive's fsvmd ended";
        gone();
    }
    return res;
}

bool Drive::mayErase(const Part& p) const {
    if (!IsMedium(mDisk, p.diskSeq) || !HasSize(mDisk, mDiskBytes)) {
        LOG(ERROR) << "fsvm: " << mDiskPath << " does not hold the medium it was read from now (or it is not known), "
                   << "not formatting partition " << p.index;
        return false;
    }
    return true;
}

void Drive::gone() {
    mControl.reset();
    if (mPid > 0) {
        Reap(mPid, kTermMs, kTermMs);
        LOG(ERROR) << "fsvm: the fsvmd of the drive " << mDiskPath << " ended, its partitions are not served";
    }
    mPid = -1;
    mLost.insert(mAttached.begin(), mAttached.end());
    mAttached.clear();
    mFuseConn.clear();
    removeRanges();
}

void Drive::endIfIdle() {
    if (mPid > 0 && mAttached.empty() && mBusy == 0) end();
}

void Drive::end() {
    if (mPid > 0) {
        // fsvmd sees its socket close, writes back what is still served, stops the VM and ends.
        mControl.reset();
        int rc = Reap(mPid, kVmEndMs, kTermMs);
        LOG(INFO) << "fsvm: the VM of the drive " << mDiskPath << " ended (" << rc << ")";
    }
    mControl.reset();
    mPid = -1;
    mLost.insert(mAttached.begin(), mAttached.end());
    mAttached.clear();
    mFuseConn.clear();
    removeRanges();
}

void Drive::kill() {
    if (mPid > 0) {
        LOG(WARNING) << "fsvm: the medium of " << mDiskPath << " changed, its VM stops without writing back";
        Reap(mPid, 0, 0);
    }
    mControl.reset();
    mPid = -1;
    mLost.insert(mAttached.begin(), mAttached.end());
    mAttached.clear();
    mFuseConn.clear();
    removeRanges();
}

void Drive::requestWriteBack() {
    if (mPid <= 0 || mAttached.empty()) return;
    RequestWriteBack(mPid);
}

status_t Drive::attach(const Part& p, gid_t gid, uid_t ownerUid, const std::string& volumeId,
                       const std::string& target, bool readOnly, pid_t* serving, bool* servedReadOnly,
                       bool* checking) {
    *serving = 0;
    if (servedReadOnly) *servedReadOnly = false;
    if (checking) *checking = false;
    const int k = indexOf(p);
    if (k < 0 || mAttached.count(k) || (p.fsType != "vfat" && p.fsType != "exfat")) {
        LOG(ERROR) << "fsvm: " << volumeId << " is not a partition its drive's VM can serve now";
        return -EIO;
    }
    if (status_t res = start(); res != OK) return res == -EAGAIN ? -EAGAIN : -EIO;
    mControl.settle();
    if (mControl.pending(k)) {
        // An earlier request for the partition got no answer yet (a format or detach still running in the VM).
        LOG(ERROR) << "fsvm: an earlier request for " << volumeId << " is still pending, not serving it now";
        endIfIdle();
        return -EIO;
    }
    // Read-only for good: the kernel's FUSE mount refuses writes itself, whatever the VM would do.
    const bool fixedReadOnly = readOnly || mReadOnly;
    unique_fd fuse;
    if (MountFuse(target, gid, fixedReadOnly, &fuse) != OK) {
        endIfIdle();
        return -EIO;
    }
    const unsigned conn = FuseConnection(target);
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) {
        umount2(target.c_str(), MNT_DETACH);
        endIfIdle();
        return -EIO;
    }
    unique_fd readyRead(pp[0]), readyWrite(pp[1]);
    int value = -1;
    status_t res = request(StringPrintf("ATTACH %d %u %u %d %s %s", k, gid, ownerUid, fixedReadOnly ? 1 : 0,
                                        p.fsType.c_str(), volumeId.c_str()),
                           {fuse.get(), readyWrite.get()}, 'A', k, true, kStartMs, &value);
    fuse.reset();
    readyWrite.reset();
    if (res != OK || value != 0) {
        LOG(ERROR) << "fsvm: the drive's fsvmd did not take " << volumeId << " (" << res << ", " << value << ")";
        umount2(target.c_str(), MNT_DETACH);
        endIfIdle();
        return -EIO;
    }
    mLost.erase(k);
    mAttached.insert(k);
    // The VM starts with the first partition, and each partition's session mounts it: as for a volume of its own.
    struct pollfd pfd = {readyRead.get(), POLLIN, 0};
    char c = 0;
    if (TEMP_FAILURE_RETRY(poll(&pfd, 1, kReadyTimeoutMs)) == 1 &&
        TEMP_FAILURE_RETRY(read(readyRead.get(), &c, 1)) == 1 && (c == 'R' || c == 'r' || c == 'c')) {
        // r and c: served read-only, by the VM or while it checks the partition, then W or F (Watch). R: as asked,
        // which is read-only when it was asked so.
        if (servedReadOnly) *servedReadOnly = fixedReadOnly || c == 'r' || c == 'c';
        if (checking) *checking = !fixedReadOnly && c == 'c';
        *serving = sNextServing--;
        if (conn) mFuseConn[k] = conn;
        LOG(INFO) << "fsvm: " << volumeId << " served by its drive's VM, disk " << k
                  << (c == 'c' ? ", read-only while the VM checks it" : "");
        unique_fd watch = Connection(mPid);
        if (watch >= 0) {
            Watch(std::move(watch), std::move(readyRead), volumeId, *serving, true);
        } else {
            PLOG(WARNING) << "fsvm: " << volumeId << " is not watched";
        }
        return OK;
    }
    // N: the drive's VM was refused at its start for want of memory.
    detach(p, false);
    umount2(target.c_str(), MNT_DETACH);
    if (c == 'N') {
        NoRoomForDrive();
        LOG(WARNING) << "fsvm: no protected VM place for " << volumeId << ": its drive's VM was refused at its start";
        return -EAGAIN;
    }
    LOG(ERROR) << "fsvm: the drive's VM did not serve " << volumeId << ", not mounting it";
    return -EIO;
}

int Drive::detach(const Part& p, bool orderly) {
    const int k = indexOf(p);
    // Served when the drive's fsvmd ended, was ended or killed: nothing says it was written back.
    if (k >= 0 && mLost.erase(k)) return -1;
    if (k < 0 || !mAttached.count(k) || mPid <= 0) return 0;
    mFuseConn.erase(k);
    int value = -1;
    status_t res = request(StringPrintf("DETACH %d %d", k, orderly ? 1 : 0), {}, 'D', k, true,
                           orderly ? kSyncMs : kTermMs + kKillMs, &value);
    mAttached.erase(k);
    endIfIdle();
    if (res != OK) return -1;
    // 1: never served, 5: its VM never started. Nothing was written to it.
    if (value == 0 || value == 1 || value == kExitNoRoom) return 0;
    return value == kExitNotWrittenBack ? kExitNotWrittenBack : -1;
}

status_t Drive::format(const Part& p, const std::string& fsType, Part* out, bool* recheck) {
    *recheck = false;
    const int k = indexOf(p);
    if (k < 0 || mAttached.count(k) || (fsType != "vfat" && fsType != "exfat")) return -EIO;
    // Before anything starts or is written: the medium the partition was read from, known, and the disk's size.
    if (!mayErase(p)) return -ENODEV;
    mBusy++;
    status_t res = start();
    if (res == OK) {
        mControl.settle();
        int value = -1;
        std::string text;
        if (mControl.pending(k)) {
            // An earlier request for the partition got no answer yet: what it did is not known.
            LOG(ERROR) << "fsvm: an earlier request for partition " << p.index
                       << " is still pending, not formatting it";
            res = -EBUSY;
        } else if (!mayErase(p)) {
            // Checked again: a VM's start can take seconds. The format runs through the VM's disk for the partition,
            // set up for that medium.
            res = -ENODEV;
        } else {
            // No discard first: the VM's format tool writes the new filesystem, nothing else touches the medium.
            status_t call = request(StringPrintf("FORMAT %d %s", k, fsType.c_str()), {}, 'F', k, true,
                                    kFormatTimeoutMs, &value, &text);
            res = FormatOutcome(call, value, text, p, fsType, out, recheck);
        }
        if (res == OK) {
            mParts[k] = *out;
            LOG(INFO) << "fsvm: partition " << p.index << " of " << mDiskPath << " formatted as " << fsType
                      << " in its drive's VM";
        }
    }
    mBusy--;
    endIfIdle();
    if (res == -EAGAIN) NoRoomForDrive();
    return res;
}

void Drive::holdForRepair() {
    mBusy++;
}

void Drive::repairEnded() {
    if (mBusy > 0) mBusy--;
    endIfIdle();
}

pid_t Drive::startRepair(const Part& p) {
    const int k = indexOf(p);
    if (k < 0 || mPid <= 0 || mAttached.count(k)) return -1;
    int pp[2];
    if (pipe2(pp, O_CLOEXEC)) return -1;
    unique_fd rd(pp[0]), wr(pp[1]);
    int value = -1;
    mControl.settle();
    if (mControl.pending(k) ||
        request(StringPrintf("REPAIR %d", k), {wr.get()}, 'R', k, true, kStartMs, &value) != OK || value != 0) {
        LOG(ERROR) << "fsvm: the drive's fsvmd did not start the repair of partition " << p.index;
        return -1;
    }
    std::lock_guard<std::mutex> lock(sRepairsLock);
    pid_t token = sNextRepair--;
    sRepairs[token] = std::move(rd);
    LOG(INFO) << "fsvm: repairing partition " << p.index << " of " << mDiskPath << " in its drive's VM";
    return token;
}

void Drive::cancelRepair(const Part& p) {
    const int k = indexOf(p);
    int value = -1;
    if (k >= 0 && mPid > 0) request(StringPrintf("CANCEL %d", k), {}, 'C', k, true, kStartMs, &value);
    repairEnded();
}

std::function<void()> Drive::stuck(const Part& p) const {
    auto it = mFuseConn.find(indexOf(p));
    if (it == mFuseConn.end()) return {};
    const std::string abort = StringPrintf("/sys/fs/fuse/connections/%u/abort", it->second);
    return [abort] {
        if (!android::base::WriteStringToFile("1", abort)) PLOG(ERROR) << "fsvm: " << abort;
    };
}

}  // namespace fsvm
}  // namespace vold
}  // namespace android
