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

/* vold_fsvm_layout_fuzzer's corpus as a regression test: every kept input runs through the target again. */

#include <dirent.h>

#include <string>
#include <vector>

#include <android-base/file.h>
#include <gtest/gtest.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size);
extern void (*gFsvmLayoutBroken)(const char* what);

static std::vector<std::string> sBroken;

TEST(FsvmLayoutCorpus, EveryInputKeepsTheRules) {
    gFsvmLayoutBroken = [](const char* what) { sBroken.push_back(what); };
    std::string dir = android::base::GetExecutableDirectory() + "/fsvm_layout_corpus";
    DIR* d = opendir(dir.c_str());
    ASSERT_NE(nullptr, d) << "no corpus at " << dir;
    std::vector<std::string> names;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] != '.') names.push_back(e->d_name);
    }
    closedir(d);
    ASSERT_GE(names.size(), 10u) << "the corpus holds only " << names.size() << " inputs";
    size_t fromFsvmd = 0;
    for (const auto& n : names) {
        std::string data;
        ASSERT_TRUE(android::base::ReadFileToString(dir + "/" + n, &data)) << n;
        fromFsvmd += data.rfind("FSVMD-OUTPUT ", 0) == 0;  // the hash is checked by the target
        sBroken.clear();
        LLVMFuzzerTestOneInput(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        EXPECT_TRUE(sBroken.empty()) << n << ": " << (sBroken.empty() ? "" : sBroken[0]);
    }
    EXPECT_GE(fromFsvmd, 3u) << "the corpus holds too few reports written by fsvmd";
}
