/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include "ExynosDisplayFbInterface.h"
#include "ExynosVirtualDisplayFbInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"

using namespace android;

ExynosVirtualDisplayFbInterface::ExynosVirtualDisplayFbInterface()
    : ExynosDisplayFbInterface() {
}

void ExynosVirtualDisplayFbInterface::init(const DisplayIdentifier &display,
                                           void *__unused deviceData, const size_t __unused deviceDataSize) {
    mDisplayIdentifier = display;

    mDisplayFd = open(display.deconNodeName.c_str(), O_RDWR);
    if (mDisplayFd < 0)
        ALOGE("%s:: %s failed to open framebuffer for WB", __func__,
              mDisplayIdentifier.name.c_str());
}

int32_t ExynosVirtualDisplayFbInterface::getColorModes(uint32_t *outNumModes, int32_t *outModes) {
    *outNumModes = 1;

    if (outModes != NULL) {
        outModes[0] = HAL_COLOR_MODE_NATIVE;
    }
    return HWC2_ERROR_NONE;
}
