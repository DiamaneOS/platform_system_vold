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

#include <android-base/file.h>
#include <android-base/unique_fd.h>
#include <gtest/gtest.h>
#include <selinux/android.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <fcntl.h>
#include <poll.h>
#include <cstdlib>
#include <thread>
#include <deque>
#include "../fs/Fsvm.h"
#include "../fs/FsvmDrive.h"
#include "../fs/ObbPendingEnd.h"
#include "../model/Disk.h"
#include "../model/PublicVolume.h"

extern struct selabel_handle* sehandle;
namespace android::vold::fsvm {

TEST(FsvmStorage, BusyObbExhaustsSpontaneousRetriesAndCanBeUnmountedExplicitly) {
    ObbPendingEnd pending;
    std::deque<std::function<void()>> callbacks;
    bool busy = true;
    bool present = true;
    int attempts = 0;
    int destroyed = 0;
    auto destroy = [&](const std::string&) {
        ++attempts;
        if (busy) return false;
        if (present) ++destroyed;
        present = false;
        return true;
    };
    auto delay = [&](std::function<void()> work) { callbacks.push_back(std::move(work)); };

    pending.end("obb", destroy, delay);
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(1u, callbacks.size());
        auto work = std::move(callbacks.front());
        callbacks.pop_front();
        work();
    }
    EXPECT_TRUE(callbacks.empty());
    EXPECT_FALSE(pending.pending("obb"));
    EXPECT_EQ(4, attempts);
    EXPECT_EQ(0, destroyed);
    EXPECT_TRUE(present);

    busy = false;
    EXPECT_TRUE(destroy("obb"));
    EXPECT_FALSE(present);
    EXPECT_EQ(1, destroyed);
}

struct StorageTestAccess {
    static status_t prepareAdoptedFixture(Disk& d, const std::string& seq, uint64_t bytes) {
        d.mSize = bytes;
        d.mProbe.took(seq, bytes, NowMs());
        d.mEraseGeneration = seq;
        d.mEraseBytes = bytes;
        return d.partitionInVm(TableKind::kMixed, 10);
    }
    static void arm(Drive& d, android::base::unique_fd fd) { d.mControl.reset(std::move(fd)); d.mPid = INT32_MAX; }
    static void disarm(Drive& d) { d.mPid = -1; d.mControl.reset(); }
    static void add(Disk& d, std::shared_ptr<PublicVolume> p) { d.mVolumes.push_back(p); d.mPublicVolumes++; }
    static std::shared_ptr<PublicVolume> first(Disk& d) {
        return std::static_pointer_cast<PublicVolume>(d.mVolumes.at(0));
    }
    static void waiting(PublicVolume& p) { p.mWaitingForPlace = true; }
    static bool readOnly(PublicVolume& p) { return p.readOnlyRequestedInVm(); }
    static void clear(Disk& d) { d.destroyAllVolumes(); }
    static void resetVolumes(Disk& d) { d.beforeDestroyInVm(); d.destroyAllVolumes(); d.mPublicVolumes = 0; }
    static bool recreate(Disk& d, const Part& part) { return d.createPublicVolumeInVm(part); }
    static void recordMedium(Disk& d) { d.mSize = 64ULL << 30; d.mProbe.took("41", d.mSize, NowMs()); }
    static bool resetKeepsIdentity(const Disk& d) { return d.mProbe.mediumRead() == "41" && d.mForceProbe; }

    static pid_t repair(PublicVolume& p) { p.mServing = 41; return p.startRepairInVm(41); }
    static bool connected(const Drive& d) { return d.mControl.connected(); }
    static status_t retry(Disk& d, const std::string& id) {
#ifdef DIAMANEOS_STORAGE_BEFORE
        (void)id;
        return d.retryPlaceholder();
#else
        return d.retryPlaceholder(id);
#endif
    }
    static status_t noSlot(Disk& d, const std::string& sys) {
        d.mSysPath = sys;
        d.mSize = 64ULL << 30;
        d.mProbe.took("41", d.mSize, NowMs());
        d.mJustPartitioned = true;
        d.mPartitionResult = OK;
        d.mReadLayout = [](const std::string&, uint64_t, Layout*) { return -EAGAIN; };
        d.readPartitionsInVm(16);
        return d.mPartitionResult;
    }
    static status_t convertWithNoSlot(Disk& d, const std::string& seq, uint64_t bytes) {
        d.mSize = bytes;
        d.mProbe.took(seq, bytes, NowMs());
        d.mEraseGeneration = seq;
        d.mEraseBytes = bytes;
        d.mReadLayout = [](const std::string&, uint64_t, Layout*) { return -EAGAIN; };
        return d.partitionInVm(TableKind::kPublic, 0);
    }
    static void markPending(Disk& d) { d.mRecheckPending = true; }
    static bool isPending(const Disk& d) { return d.mRecheckPending; }
    static status_t verifyUncertainFormat(Disk& d, const std::string& sys, const Part& before,
                                         const Part& after, bool writerStopped) {
        d.mSysPath = sys;
        recordMedium(d);
        d.mFailedRepairParts.insert(before);
        d.mPendingFormats.clear();
        d.mPendingFormats.push_back({before, before.fsType, false, writerStopped});
        d.mRecheckPending = true;
        d.mForceProbe = true;
        // The probe invokes the function pointer synchronously on this test thread.
        static thread_local const Part* readback = nullptr;
        const auto previousProbe = d.mReadLayout;
        readback = &after;
        d.mReadLayout = [](const std::string&, uint64_t, Layout* layout) -> status_t {
            layout->parts = {*readback};
            return OK;
        };
        const status_t result = d.readPartitionsInVm(16);
        d.mReadLayout = previousProbe;
        readback = nullptr;
        return result;
    }
};

class StateVolume : public PublicVolume {
  public:
    using PublicVolume::PublicVolume;
    int unmounts = 0;
    int formats = 0;
  protected:
    status_t doMount() override { return OK; }
    status_t doUnmount() override { unmounts++; return OK; }
    status_t doFormat(const std::string&) override { formats++; return OK; }
    void doPostMount() override {}
};

class RetryVolume : public StateVolume {
  public:
    using StateVolume::StateVolume;
    status_t setupResult = -EIO;

  protected:
    status_t doCreate() override { return setupResult; }
};

TEST(FsvmStorageTest, FailedTargetedCreationCanBeRetriedWithoutPublishing) {
    Part part;
    RetryVolume volume(makedev(179, 1048574), "/nonexistent", "/nonexistent",
                       8 << 20, part, nullptr);
    volume.setSilent(true);
    EXPECT_EQ(-EIO, volume.create(false));
    EXPECT_EQ(VolumeBase::State::kUnmounted, volume.getState());
    volume.setupResult = OK;
    ASSERT_EQ(OK, volume.create(false));
    EXPECT_EQ(OK, volume.destroy());
}

TEST(FsvmStorageTest, AutomaticUnreadMountSettlesAfterOneExplicitRetry) {
    auto probes = [](bool explicitRetry) {
        std::deque<bool> callbacks{explicitRetry};
        int probes = 0;
        int turns = 0;
        while (!callbacks.empty() && ++turns < 5) {
            const bool manual = callbacks.front();
            callbacks.pop_front();
            if (ReprobeOnMount(true, manual)) {
                probes++;
                callbacks.push_back(false);
            }
        }
        EXPECT_TRUE(callbacks.empty());
        return probes;
    };
    EXPECT_EQ(0, probes(false));
    EXPECT_EQ(1, probes(true));
    EXPECT_FALSE(ReprobeOnMount(false, true));
}

TEST(FsvmStorageTest, ConfirmationMatchesTheTrimmedKernelGeneration) {
    ProbeMemory probe;
    probe.took("41\n", 4096, NowMs());
    EXPECT_TRUE(probe.holds("41", 4096));
    EXPECT_FALSE(probe.holds("42", 4096));
    EXPECT_FALSE(probe.holds("41", 8192));
}

TEST(FsvmStorageTest, FailedRepairSurvivesRepeatedPlacementRetry) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    ASSERT_NE(nullptr, sehandle);
    Disk disk("/devices/virtual/block/fsvm-retry-test", makedev(179, 1048574), "test", Disk::kSd);
    Part part;
    part.index = 1;
    part.length = 4096;
    auto pub = std::make_shared<PublicVolume>(disk.getDevice(), disk.getDevPath(), disk.getSysPath(),
                                               64ULL << 30, part, nullptr);
    pub->setSilent(true);
    ASSERT_EQ(OK, pub->create());
    pub->markRepairFailed();
    StorageTestAccess::add(disk, pub);
    for (int attempt = 0; attempt < 3; attempt++) {
        pub = StorageTestAccess::first(disk);
        StorageTestAccess::waiting(*pub);
        disk.remakeWaitingVolumes();
        EXPECT_TRUE(StorageTestAccess::readOnly(*StorageTestAccess::first(disk))) << attempt;
    }
    StorageTestAccess::clear(disk);
    auto requested = std::make_shared<PublicVolume>(disk.getDevice(), disk.getDevPath(), disk.getSysPath(),
                                                  64ULL << 30, part, nullptr);
    requested->setSilent(true);
    ASSERT_EQ(OK, requested->create());
    ASSERT_EQ(OK, requested->setMountFlags(VolumeBase::MountFlags::kReadOnly));
    StorageTestAccess::add(disk, requested);
    StorageTestAccess::waiting(*requested);
    disk.remakeWaitingVolumes();
    ASSERT_EQ(OK, StorageTestAccess::first(disk)->setMountFlags(0));
    EXPECT_TRUE(StorageTestAccess::readOnly(*StorageTestAccess::first(disk)));
    StorageTestAccess::clear(disk);
    for (bool explicitRequest : {false, true}) {
        Disk reset("/devices/virtual/block/fsvm-reset-test", makedev(179, 1048573), "test", Disk::kSd);
        StorageTestAccess::recordMedium(reset);
        auto held = std::make_shared<PublicVolume>(reset.getDevice(), reset.getDevPath(), reset.getSysPath(),
                                                   64ULL << 30, part, nullptr);
        held->setSilent(true);
        ASSERT_EQ(OK, held->create());
        if (explicitRequest) {
            ASSERT_EQ(OK, held->setMountFlags(VolumeBase::MountFlags::kReadOnly));
        } else {
            held->markRepairFailed();
        }
        StorageTestAccess::add(reset, held);
        StorageTestAccess::resetVolumes(reset);
        EXPECT_TRUE(StorageTestAccess::resetKeepsIdentity(reset));
        ASSERT_TRUE(StorageTestAccess::recreate(reset, part));
        EXPECT_TRUE(StorageTestAccess::readOnly(*StorageTestAccess::first(reset)));
        StorageTestAccess::clear(reset);
    }
}

TEST(FsvmStorageTest, MountedSiblingCannotCancelVerification) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    Disk disk("/devices/virtual/block/fsvm-recheck-test", makedev(179, 1048574), "test", Disk::kSd);
    Part part;
    part.index = 1;
    auto pub = std::make_shared<StateVolume>(disk.getDevice(), disk.getDevPath(), disk.getSysPath(),
                                             64ULL << 30, part, nullptr);
    pub->setSilent(true);
    ASSERT_EQ(OK, pub->create());
    StorageTestAccess::add(disk, pub);
    ASSERT_EQ(OK, pub->mount());
    StorageTestAccess::markPending(disk);
    disk.reprobeInVm(Disk::Reprobe::kAfterFormat);
    EXPECT_TRUE(StorageTestAccess::isPending(disk));
    ASSERT_EQ(OK, pub->unmount());
    StorageTestAccess::clear(disk);
}

TEST(FsvmStorageTest, RetiredReplyMustMatchTheWholeTuple) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    android::base::unique_fd peer(sv[1]);
    DriveControl c;
    c.reset(android::base::unique_fd(sv[0]));
    int value = -1;
    ASSERT_EQ(-ETIMEDOUT, c.call("FORMAT 0 vfat", {}, 'F', 0, true, 1, &value));
    const char bad[] = "1 D 0 0";
    ASSERT_EQ(static_cast<ssize_t>(sizeof(bad)-1), send(peer.get(), bad, sizeof(bad)-1, 0));
    c.settle();
    EXPECT_TRUE(c.pending(0));
    EXPECT_FALSE(c.connected());
}

TEST(FsvmStorageTest, ContradictoryCurrentReplyStaysUncertain) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    android::base::unique_fd peer(sv[1]);
    DriveControl c;
    c.reset(android::base::unique_fd(sv[0]));
    std::thread server([&] {
        char buf[128];
        recv(peer.get(), buf, sizeof(buf), 0);
        const char bad[] = "1 F 1 0";
        send(peer.get(), bad, sizeof(bad)-1, 0);
    });
    int value = -1;
    EXPECT_EQ(-EIO, c.call("FORMAT 0 vfat", {}, 'F', 0, true, 5000, &value));
    server.join();
    EXPECT_TRUE(c.pending(0));
    EXPECT_FALSE(c.connected());
}

TEST(FsvmStorageTest, IdExhaustionRequiresANewConnection) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    android::base::unique_fd peer(sv[1]);
    DriveControl c(UINT32_MAX);
    c.reset(android::base::unique_fd(sv[0]));
    int value = -1;
    ASSERT_EQ(-ETIMEDOUT, c.call("FORMAT 0 vfat", {}, 'F', 0, true, 1, &value));
    EXPECT_EQ(-EPIPE, c.call("FORMAT 1 vfat", {}, 'F', 1, true, 1, &value));
    EXPECT_TRUE(c.pending(0));
}

TEST(FsvmStorageTest, LiveRepairChecksItsMediumBeforeDispatch) {
    int sv[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv));
    android::base::unique_fd peer(sv[1]);
    Part p;
    p.index = 1;
    p.start = 2048;
    p.length = 4096;
    p.fsType = "vfat";
    p.diskSeq = "71";
    Drive drive(makedev(4095, 1048575), "/nonexistent", "disk:4095,1048575", 8 << 20);
    drive.setParts({p});
    StorageTestAccess::arm(drive, android::base::unique_fd(sv[0]));
    std::thread server([&] {
        struct pollfd wait = {peer.get(), POLLIN, 0};
        if (poll(&wait, 1, 100) <= 0) return;
        char buf[256];
        ssize_t n = recv(peer.get(), buf, sizeof(buf), 0);
        if (n <= 0) return;
        std::string request(buf, n);
        std::string answer = request.substr(0, request.find(' ')) + " R 0 0";
        send(peer.get(), answer.data(), answer.size(), 0);
    });
    pid_t token = drive.startRepair(p);
    EXPECT_EQ(-1, token);
    server.join();
    if (token < -1) WaitDriveRepair(token);
    StorageTestAccess::disarm(drive);
}

TEST(FsvmStorageTest, ConfirmedTableRejectsDifferentRangesClassesAndGuids) {
    TableRequest request;
    request.diskBytes = 64ULL << 30;
    request.kind = TableKind::kPrivate;
    request.partGuids = {RandomGuid(), RandomGuid()};
    Table table;
    ASSERT_EQ(OK, BuildTable(request, &table));
    Layout layout;
    layout.table = "gpt";
    layout.diskBytes = request.diskBytes;
    for (size_t i = 0; i < table.parts.size(); i++) {
        Part p;
        p.index = i + 1;
        p.cls = i == 0 ? Part::Class::kMeta : Part::Class::kExpand;
        p.start = table.parts[i].first;
        p.length = table.parts[i].last - table.parts[i].first + 1;
        // GPT stores the first three GUID fields little endian.
        auto g = request.partGuids[i];
        char text[33];
        const int order[] = {3,2,1,0,5,4,7,6,8,9,10,11,12,13,14,15};
        for (int k = 0; k < 16; k++) snprintf(text + 2*k, 3, "%02x", g[order[k]]);
        p.partGuid = text;
        layout.parts.push_back(p);
    }
    ASSERT_TRUE(MatchesTable(layout, request, table));
    auto changed = layout;
    changed.parts[1].start++;
    EXPECT_FALSE(MatchesTable(changed, request, table));
    changed = layout;
    changed.parts[1].cls = Part::Class::kPublic;
    EXPECT_FALSE(MatchesTable(changed, request, table));
    changed = layout;
    changed.parts[1].partGuid = "00000000000000000000000000000000";
    EXPECT_FALSE(MatchesTable(changed, request, table));
}

TEST(FsvmStorageTest, TableWriterRechecksBeforeEveryWriteAndFlush) {
    TemporaryFile file;
    ASSERT_EQ(0, ftruncate(file.fd, 8 << 20));
    TableRequest request;
    request.diskBytes = 8 << 20;
    Table table;
    ASSERT_EQ(OK, BuildTable(request, &table));
    int checks = 0;
    EXPECT_EQ(-ENODEV, WriteTable(file.fd, table, [&] { return ++checks < 2; }));
    EXPECT_EQ(2, checks);
    char head[512];
    ASSERT_EQ(static_cast<ssize_t>(sizeof(head)), pread(file.fd, head, sizeof(head), 0));
    for (char byte : head) EXPECT_EQ(0, byte);
    checks = 0;
    EXPECT_EQ(-ENODEV, WriteTable(file.fd, table, [&] { return ++checks < 3; }));
    EXPECT_EQ(3, checks);
    EXPECT_TRUE(FencesMedium(makedev(179, 0)));
    EXPECT_FALSE(FencesMedium(makedev(8, 0)));
}

// An explicitly selected destructive fixture, never part of a normal test run.
TEST(FsvmStorageTest, DISABLED_PrepareExistingAdoptedCardFixture) {
    ASSERT_STREQ("erase-test-card", getenv("DIAMANEOS_PREPARE_ADOPTED"));
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    std::string sys, seq, size;
    ASSERT_TRUE(android::base::Realpath("/sys/dev/block/179:0", &sys));
    ASSERT_TRUE(android::base::ReadFileToString(sys + "/diskseq", &seq));
    ASSERT_TRUE(android::base::ReadFileToString(sys + "/size", &size));
    ASSERT_EQ(0u, sys.find("/sys/"));
    Disk disk(sys.substr(4), makedev(179, 0), "test card", Disk::kSd | Disk::kAdoptable);
    status_t res = StorageTestAccess::prepareAdoptedFixture(disk, seq, std::stoull(size) * 512);
    StorageTestAccess::clear(disk);
    EXPECT_EQ(OK, res);
}


TEST(FsvmStorageTest, UnsupportedFormatPreservesTheMountedVolume) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    Part p;
    p.index = 1;
    p.start = 2048;
    p.length = 4096;
    p.diskSeq = "41";
    auto drive = std::make_shared<Drive>(makedev(8, 1048574), "/nonexistent", "disk:8,1048574", 8 << 20);
    auto volume = std::make_shared<StateVolume>(makedev(8, 1048574), "/nonexistent", "/nonexistent",
                                                8 << 20, p, drive);
    volume->setSilent(true);
    ASSERT_EQ(OK, volume->create());
    ASSERT_EQ(OK, volume->mount());
    EXPECT_NE(OK, volume->format("auto"));
    EXPECT_EQ(0, volume->unmounts);
    EXPECT_EQ(0, volume->formats);
    EXPECT_EQ(VolumeBase::State::kMounted, volume->getState());
    volume->destroy();
}

TEST(FsvmStorageTest, UnsupportedRepairKeepsTheLiveControlAndDirtySibling) {
    int sockets[2];
    ASSERT_EQ(0, socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets));
    android::base::unique_fd peer(sockets[1]);
    Drive drive(makedev(8, 1048574), "/nonexistent", "disk:8,1048574", 8 << 20);
    Part p;
    p.index = 1;
    p.diskSeq = "41";
    StorageTestAccess::arm(drive, android::base::unique_fd(sockets[0]));
    EXPECT_FALSE(drive.prepareRepair(p));
    EXPECT_TRUE(StorageTestAccess::connected(drive));
    StorageTestAccess::disarm(drive);
}

TEST(FsvmStorageTest, ReadOnlyHoldFollowsTheRangeAfterAnIndexChange) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    Disk disk("/devices/virtual/block/fsvm-range-test", makedev(179, 1048574), "test", Disk::kSd);
    Part part;
    part.index = 1;
    part.start = 2048;
    part.length = 4096;
    part.diskSeq = "41";
    auto volume = std::make_shared<PublicVolume>(disk.getDevice(), disk.getDevPath(), disk.getSysPath(),
                                                64ULL << 30, part, nullptr);
    volume->setSilent(true);
    ASSERT_EQ(OK, volume->create());
    volume->markRepairFailed();
    StorageTestAccess::add(disk, volume);
    StorageTestAccess::resetVolumes(disk);
    part.index = 3;
    ASSERT_TRUE(StorageTestAccess::recreate(disk, part));
    EXPECT_TRUE(StorageTestAccess::readOnly(*StorageTestAccess::first(disk)));
    StorageTestAccess::clear(disk);
}

TEST(FsvmStorageTest, UnknownGenerationDoesNotReleaseReadOnlyHolds) {
    ProbeMemory probe;
    probe.took("41", 4096, NowMs());
    EXPECT_FALSE(probe.took("", 4096, NowMs()));
    EXPECT_EQ("41", probe.mediumRead());
}

TEST(FsvmStorageTest, MatchingProbeAfterUncertainFormatKeepsFailedRepairHold) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    TemporaryDir sys;
    ASSERT_TRUE(android::base::WriteStringToFile("0\n", std::string(sys.path) + "/partscan"));
    ASSERT_TRUE(android::base::WriteStringToFile("41\n", std::string(sys.path) + "/diskseq"));
    Disk disk("/devices/virtual/block/fsvm-format-hold", makedev(179, 1048573), "test", Disk::kSd);
    Part before;
    before.index = 1;
    before.start = 2048;
    before.length = 4096;
    before.diskSeq = "41";
    before.fsType = "vfat";
    before.fsUuid = "OLD";
    Part after = before;
    after.fsUuid = "NEW";

    ASSERT_EQ(OK, StorageTestAccess::verifyUncertainFormat(disk, sys.path, before, after, true));
    EXPECT_FALSE(StorageTestAccess::isPending(disk));
    EXPECT_TRUE(StorageTestAccess::readOnly(*StorageTestAccess::first(disk)));
    StorageTestAccess::clear(disk);

    ASSERT_EQ(OK, StorageTestAccess::verifyUncertainFormat(disk, sys.path, before, after, false));
    EXPECT_TRUE(StorageTestAccess::isPending(disk));
    EXPECT_EQ(VolumeBase::State::kUnopenable, StorageTestAccess::first(disk)->getState());
    StorageTestAccess::clear(disk);

    ASSERT_EQ(OK, StorageTestAccess::verifyUncertainFormat(disk, sys.path, before, before, true));
    EXPECT_TRUE(StorageTestAccess::isPending(disk));
    EXPECT_EQ(VolumeBase::State::kFormatResultUnverified, StorageTestAccess::first(disk)->getState());
    StorageTestAccess::clear(disk);
}

TEST(FsvmStorageTest, MissingPartitionRetryPreservesTheMountedSiblingObject) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    Disk disk("/devices/virtual/block/fsvm-sibling-test", makedev(179, 1048574), "test", Disk::kSd);
    Part part;
    part.index = 1;
    part.length = 4096;
    auto sibling = std::make_shared<StateVolume>(disk.getDevice(), disk.getDevPath(), disk.getSysPath(),
                                                64ULL << 30, part, nullptr);
    sibling->setSilent(true);
    ASSERT_EQ(OK, sibling->create());
    ASSERT_EQ(OK, sibling->mount());
    StorageTestAccess::add(disk, sibling);
    EXPECT_NE(OK, StorageTestAccess::retry(disk, "public:179,1048574"));
    EXPECT_EQ(0, sibling->unmounts);
    EXPECT_EQ(VolumeBase::State::kMounted, sibling->getState());
    EXPECT_EQ(1u, disk.getVolumes().size());
    if (!disk.getVolumes().empty()) EXPECT_EQ(sibling.get(), StorageTestAccess::first(disk).get());
    StorageTestAccess::clear(disk);
}

TEST(FsvmStorageTest, PostWritePlacementRefusalCannotReportSuccessfulConversion) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    TemporaryDir sys;
    ASSERT_TRUE(android::base::WriteStringToFile("0", std::string(sys.path) + "/partscan"));
    ASSERT_TRUE(android::base::WriteStringToFile("41", std::string(sys.path) + "/diskseq"));
    Disk disk("/devices/virtual/block/fsvm-no-slot-test", makedev(179, 1048574), "test", Disk::kSd);
    EXPECT_NE(OK, StorageTestAccess::noSlot(disk, sys.path));
    StorageTestAccess::clear(disk);
}

TEST(FsvmStorageTest, DISABLED_ConvertAdoptedCardWithReadbackRefused) {
    ASSERT_STREQ("erase-test-card", getenv("DIAMANEOS_CONVERT_ADOPTED"));
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    std::string sys, seq, size;
    ASSERT_TRUE(android::base::Realpath("/sys/dev/block/179:0", &sys));
    ASSERT_TRUE(android::base::ReadFileToString(sys + "/diskseq", &seq));
    ASSERT_TRUE(android::base::ReadFileToString(sys + "/size", &size));
    Disk disk(sys.substr(4), makedev(179, 0), "test card", Disk::kSd | Disk::kAdoptable);
    EXPECT_NE(OK, StorageTestAccess::convertWithNoSlot(disk, seq, std::stoull(size) * 512));
    StorageTestAccess::clear(disk);
}


TEST(FsvmStorageTest, UnsupportedRepairLeavesTheTargetMountedReadOnly) {
    if (!sehandle) sehandle = selinux_android_file_context_handle();
    Part p;
    p.index = 1;
    p.start = 2048;
    p.length = 4096;
    p.diskSeq = "41";
    auto drive = std::make_shared<Drive>(makedev(8, 1048574), "/nonexistent", "disk:8,1048574", 8 << 20);
    auto volume = std::make_shared<StateVolume>(makedev(8, 1048574), "/nonexistent", "/nonexistent",
                                                8 << 20, p, drive);
    volume->setSilent(true);
    ASSERT_EQ(OK, volume->create());
    ASSERT_EQ(OK, volume->mount());
    EXPECT_EQ(-1, StorageTestAccess::repair(*volume));
    EXPECT_EQ(0, volume->unmounts);
    EXPECT_EQ(VolumeBase::State::kMounted, volume->getState());
    EXPECT_TRUE(StorageTestAccess::readOnly(*volume));
    volume->destroy();
}

}  // namespace android::vold::fsvm
