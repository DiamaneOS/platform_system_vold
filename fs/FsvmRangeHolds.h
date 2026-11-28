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

#pragma once
#include "Fsvm.h"
#include <algorithm>
#include <vector>
namespace android::vold::fsvm {
class RangeHolds {
  public:
    void insert(const Part& p) {
        if (std::none_of(mParts.begin(), mParts.end(), [&](const Part& held) {
            return held.start == p.start && held.length == p.length && held.diskSeq == p.diskSeq;
        })) mParts.push_back(p);
    }
    bool count(const Part& p) const {
        return std::any_of(mParts.begin(), mParts.end(), [&](const Part& held) {
            bool same = held.diskSeq.empty() || p.diskSeq.empty() || held.diskSeq == p.diskSeq;
            return same && held.length && p.length &&
                   (held.start <= p.start ? p.start - held.start < held.length : held.start - p.start < p.length);
        });
    }
    // The hold of the same range goes: one of the same generation, and one recorded without a generation, which
    // count() takes for every generation. An unknown generation never releases a known one.
    void erase(const Part& p) {
        mParts.erase(std::remove_if(mParts.begin(), mParts.end(), [&](const Part& held) {
            return (held.diskSeq.empty() || held.diskSeq == p.diskSeq) && held.start == p.start &&
                   held.length == p.length;
        }), mParts.end());
    }
    void clear() { mParts.clear(); }
  private:
    std::vector<Part> mParts;
};
}
