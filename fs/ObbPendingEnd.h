/* Copyright (C) 2026 DiamaneOS
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <functional>
#include <set>
#include <string>
#include <utility>
namespace android::vold::fsvm {
// Manager-lock confined scheduling for spontaneous ends.
class ObbPendingEnd {
  public:
    using End = std::function<bool(const std::string&)>;
    using Delay = std::function<void(std::function<void()>)>;
    void end(const std::string& id, End destroy, Delay delay) {
        if (pending(id) || destroy(id)) return;
        mEntries.insert(id);
        retry(id, kRetries, std::move(destroy), std::move(delay));
    }
    bool pending(const std::string& id) const { return mEntries.count(id); }
  private:
    static constexpr int kRetries = 3;
    void retry(std::string id, int remaining, End destroy, Delay delay) {
        delay([this, id, remaining, destroy, delay] {
            if (destroy(id) || remaining == 1) mEntries.erase(id);
            else retry(id, remaining - 1, destroy, delay);
        });
    }
    std::set<std::string> mEntries;
};
}
