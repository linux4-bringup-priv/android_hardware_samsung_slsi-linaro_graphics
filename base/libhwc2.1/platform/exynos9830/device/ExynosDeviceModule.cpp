/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "ExynosDeviceModule.h"
#include "ExynosDisplay.h"
#include <cutils/properties.h>

ExynosDeviceModule::ExynosDeviceModule() {
}

ExynosDeviceModule::~ExynosDeviceModule() {
}

void ExynosDeviceModule::setCPUClocksPerCluster(uint32_t fps) {

    for (uint32_t i = 0; i < cpuPropTable.size(); i++) {
        long cl_min_handle = mEPICRequestFcnPtr(cpuPropTable[i].minLockId);
        mEPICAcquireOptionFcnPtr(cl_min_handle, perfTable[fps].minClock[i], 0);
        ALOGI("CPU set : Cluster(%d), min_clock(%d)", i, perfTable[fps].minClock[i]);
    }

    return;
}
