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

ExynosDeviceModule::ExynosDeviceModule()
{
}

ExynosDeviceModule::~ExynosDeviceModule() {
    if (mIsEPICHandleInit == true) {
        for (uint32_t i = 0; i < cpuPropTable.size(); i++)
            mEPICFreeFcnPtr(cl_min_handle[i]);
    }
}

void ExynosDeviceModule::setCPUClocksPerCluster(uint32_t fps) {
    if (!mEPICHandle)
        return;

    if (mIsEPICHandleInit == false) {
        for (uint32_t i = 0; i < cpuPropTable.size(); i++)
            cl_min_handle[i] = mEPICRequestFcnPtr(cpuPropTable[i].minLockId);
        mIsEPICHandleInit = true;
    } else {
        if (mEPICOneShotTimer != NULL)
            mEPICOneShotTimer->stop();
        for (uint32_t i = 0; i < cpuPropTable.size(); i++)
            mEPICReleaseFcnPtr(cl_min_handle[i]);
    }

    if (mEPICOneShotTimer) {
        mEPICOneShotTimer->setInterval(std::chrono::milliseconds(100));
    } else {
        const auto callback = &ExynosDevice::releaseCPUPerfPerCluster;
        mEPICOneShotTimer = new OneShotTimer(std::chrono::milliseconds(100), NULL,
                                             [this, callback] { std::invoke(callback, this); });
    }

    return;
}

void ExynosDeviceModule::acquireCPUPerfPerCluster(uint32_t fps) {
    if (mIsEPICHandleInit == false || mEPICHandle == NULL) {
        HDEBUGLOGD(eDebugDefault, "EPIC handle didn't be initialized");
        return;
    }

    if (mEPICOneShotTimer == NULL) {
        HDEBUGLOGD(eDebugDefault, "OneShotTimer is not initialized");
        return;
    }

    const auto its = perfTable.find(fps);
    if (its == perfTable.end()) {
        ALOGI("%s, %d fps not found in perfTable", __func__, fps);
        return;
    }

    if (mEPICOneShotTimer->isTimerRunning() == false) {
        for (uint32_t i = 0; i < cpuPropTable.size(); i++) {
            mEPICAcquireOptionFcnPtr(cl_min_handle[i], perfTable[fps].minClock[i], 0);
            HDEBUGLOGD(eDebugDefault, "CPU set : Cluster(%d), min_clock(%d)", i, perfTable[fps].minClock[i]);
        }
    }
    mEPICOneShotTimer->reset();
}

void ExynosDeviceModule::releaseCPUPerfPerCluster() {
    if (mIsEPICHandleInit == false || mEPICHandle == NULL) {
        HDEBUGLOGD(eDebugDefault, "EPIC handle didn't be initialized");
        return;
    }
    for (uint32_t i = 0; i < cpuPropTable.size(); i++)
        mEPICReleaseFcnPtr(cl_min_handle[i]);
}
