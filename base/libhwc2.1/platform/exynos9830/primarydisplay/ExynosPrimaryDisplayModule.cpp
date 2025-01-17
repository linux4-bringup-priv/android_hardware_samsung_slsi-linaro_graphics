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

#include <hardware/hardware.h>
#include <hidl/HidlSupport.h>
#include <android/hardware/power/1.0/IPower.h>
#include "ExynosPrimaryDisplayModule.h"
#include "ExynosHWCDebug.h"
#include "ExynosLayer.h"
#include "ExynosGraphicBuffer.h"

using android::hardware::power::V1_0::IPower;
using namespace vendor::graphics;

ExynosPrimaryDisplayModule::ExynosPrimaryDisplayModule(DisplayIdentifier node)
    :    ExynosPrimaryDisplay(node)
{
}

ExynosPrimaryDisplayModule::~ExynosPrimaryDisplayModule () {
}

int32_t ExynosPrimaryDisplayModule::validateWinConfigData()
{
    if (mDisplayConfigPending)
        setActiveConfigInternal(mActiveConfig);

    return ExynosDisplay::validateWinConfigData();
}

void ExynosPrimaryDisplayModule::doPreProcessing(DeviceValidateInfo &validateInfo,
        uint64_t &geometryChanged)
{

    bool prevAdjustDisplayFrame = mDisplayControl.adjustDisplayFrame;
    if (validateInfo.nonPrimaryDisplays.size() > 0) {
        mDisplayControl.adjustDisplayFrame = true;
    } else {
        mDisplayControl.adjustDisplayFrame = false;
    }
    if (prevAdjustDisplayFrame != mDisplayControl.adjustDisplayFrame)
        setGeometryChanged(GEOMETRY_DISPLAY_ADJUST_SIZE_CHANGED, geometryChanged);

    ExynosDisplay::doPreProcessing(validateInfo, geometryChanged);
}
