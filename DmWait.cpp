// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The DiamaneOS Project

#include "DmWait.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <libdm/dm.h>

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <memory>
#include <set>
#include <thread>

using android::base::StringPrintf;

namespace android {
namespace vold {

// DiamaneOS: after an unmount, other mount namespaces can keep the card's dm device busy for a
// while (b/149396179). With AOSP's one second of retries a storage reset left the card unmountable.
// Wait up to ten seconds, and name the processes that still have it mounted if it stays busy.
static void LogDmDeviceHolders(const std::string& name) {
    std::string path;
    struct stat st;
    if (!dm::DeviceMapper::Instance().GetDmDevicePathByName(name, &path) ||
        stat(path.c_str(), &st) != 0) {
        return;
    }
    const std::string dev = StringPrintf("%u:%u", major(st.st_rdev), minor(st.st_rdev));
    std::set<std::string> holders;
    auto proc = std::unique_ptr<DIR, int (*)(DIR*)>(opendir("/proc"), closedir);
    if (!proc) return;
    struct dirent* de;
    while ((de = readdir(proc.get())) != nullptr && holders.size() < 10) {
        pid_t pid;
        std::string mountinfo;
        if (!android::base::ParseInt(de->d_name, &pid) ||
            !android::base::ReadFileToString(StringPrintf("/proc/%d/mountinfo", pid), &mountinfo)) {
            continue;
        }
        for (const auto& line : android::base::Split(mountinfo, "\n")) {
            auto fields = android::base::Split(line, " ");
            if (fields.size() > 2 && fields[2] == dev) {
                std::string comm;
                android::base::ReadFileToString(StringPrintf("/proc/%d/comm", pid), &comm);
                holders.insert(android::base::Trim(comm));
                break;
            }
        }
    }
    LOG(ERROR) << name << " is still mounted by: " << android::base::Join(holders, ", ");
}

bool DeleteDmDeviceWaiting(const std::string& name, bool ifExists) {
    auto& dm = dm::DeviceMapper::Instance();
    for (int i = 0; i < 50; i++) {
        bool ret = ifExists ? dm.DeleteDeviceIfExists(name) : dm.DeleteDevice(name);
        if (ret || errno != EBUSY) return ret;
        if (i % 10 == 0) PLOG(ERROR) << "Cannot remove dm device " << name;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    LogDmDeviceHolders(name);
    errno = EBUSY;
    return false;
}

}  // namespace vold
}  // namespace android
