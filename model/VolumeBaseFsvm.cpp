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

/*
 * DiamaneOS: VolumeBase's part for volumes the filesystem VM serves (model/VolumeBaseFsvm.inc). Kept apart from
 * VolumeBase.cpp, which calls these from short hooks, so that upstream changes there merge without touching this code.
 */

#include "VolumeBase.h"

namespace android {
namespace vold {

VolumeBase::State VolumeBase::reportedState() const {
    return (mState == State::kMounted && mReportReadOnly) ? State::kMountedReadOnly : mState;
}

bool VolumeBase::mayMount() const {
    return mState == State::kUnmounted || mState == State::kUnmountable || mState == State::kUnopenable ||
           mState == State::kNotWrittenBack || mState == State::kWaitingForPlace;
}

void VolumeBase::mountFailed() {
    // Waiting for a protected VM place: it says so, and its disk makes it again when a place is free.
    if (waitingForPlace()) {
        setState(State::kWaitingForPlace);
        return;
    }
    setState(mountFailedInVm() ? State::kUnopenable : State::kUnmountable);
    onMountFailed();
}

void VolumeBase::announceReadOnly(bool readOnly) {
    if (mState != State::kMounted || mReportReadOnly == readOnly) return;
    mReportReadOnly = readOnly;
    setState(State::kMounted);
}

void VolumeBase::announceCheckPending(bool pending) {
    const bool changed = mCheckPending != pending;
    mCheckPending = pending;
    auto listener = getListener();
    if (listener) listener->onVolumeCheckPending(getId(), pending);
    // A volume shown read-only already: its state again, so the framework shows the new text.
    if (changed && mState == State::kMounted && mReportReadOnly) setState(State::kMounted);
}

void VolumeBase::setUnopenable() {
    if (mState == State::kUnmounted) setState(State::kUnopenable);
}

}  // namespace vold
}  // namespace android
