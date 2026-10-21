// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The DiamaneOS Project
#pragma once

#include <string>

namespace android {
namespace vold {

// Removes a dm device, retrying for up to 10 s while it is busy. Logs its holders if it stays busy.
bool DeleteDmDeviceWaiting(const std::string& name, bool ifExists);

}  // namespace vold
}  // namespace android
