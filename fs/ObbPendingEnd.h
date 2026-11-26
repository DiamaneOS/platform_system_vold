/* Copyright (C) 2026 DiamaneOS
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <functional>
#include <set>
#include <string>
#include <utility>
namespace android::vold::fsvm {
// Manager-lock confined ownership of spontaneous ends until lower cleanup succeeds.
class ObbPendingEnd {
  public:
    using End = std::function<bool(const std::string&)>;
    using Delay = std::function<void(std::function<void()>)>;
    void end(const std::string& id, End destroy, Delay delay) {
        if (destroy(id)) { mEntries.erase(id); return; }
        if (!mEntries.insert(id).second) return;
        retry(id, std::move(destroy), std::move(delay));
    }
    bool pending(const std::string& id) const { return mEntries.count(id); }
  private:
    void retry(std::string id, End destroy, Delay delay) {
        delay([this, id, destroy, delay] {
            if (!mEntries.count(id)) return;
            if (destroy(id)) mEntries.erase(id);
            else retry(id, destroy, delay);
        });
    }
    std::set<std::string> mEntries;
};
}
