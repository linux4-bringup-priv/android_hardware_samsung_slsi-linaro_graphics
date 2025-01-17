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
#include <cutils/properties.h>

#include "ExynosResourceManagerModule.h"
#include "ExynosPrimaryDisplayModule.h"
#include "ExynosMPPModule.h"

ExynosResourceManagerModule::ExynosResourceManagerModule()
        : ExynosResourceManager()
{
    static_cast<ExynosMPPModule*>(getExynosMPP(MPP_DPP_GF, 0))->setSharedMPP(
            getExynosMPP(MPP_DPP_VGF, 0));
    static_cast<ExynosMPPModule*>(getExynosMPP(MPP_DPP_GF, 1))->setSharedMPP(
            getExynosMPP(MPP_DPP_VGRFS, 0));
    static_cast<ExynosMPPModule*>(getExynosMPP(MPP_DPP_VGF, 0))->setSharedMPP(
            getExynosMPP(MPP_DPP_GF, 0));
    static_cast<ExynosMPPModule*>(getExynosMPP(MPP_DPP_VGRFS, 0))->setSharedMPP(
        getExynosMPP(MPP_DPP_GF, 1));
}

ExynosResourceManagerModule::~ExynosResourceManagerModule()
{
}

uint32_t ExynosResourceManagerModule::getExceptionScenarioFlag(ExynosMPP *mpp) {
    uint32_t ret = ExynosResourceManager::getExceptionScenarioFlag(mpp);

    if (mpp->mPhysicalType != MPP_G2D)
        return ret;

    /* Check whether camera preview is running */
    /* when camera is operating, HWC can't use G2D */
    char value[PROPERTY_VALUE_MAX];
    bool preview;
    property_get("persist.vendor.sys.camera.preview", value, "0");
    preview = !!atoi(value);

    if (preview)
        ret |= static_cast<uint32_t>(DisableType::DISABLE_SCENARIO);

    return ret;
}

void ExynosResourceManagerModule::preAssignWindows()
{
    ExynosPrimaryDisplayModule *primaryDisplay =
        (ExynosPrimaryDisplayModule *)getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    primaryDisplay->mBaseWindowIndex = 0;
    primaryDisplay->mMaxWindowNum =
        primaryDisplay->mDisplayInterface->getMaxWindowNum();

    bool extraDisplayConnected = false;
    for (auto display: mDisplays) {
        if ((display == nullptr) ||
            (display->mType == HWC_DISPLAY_PRIMARY) ||
            (display->mUseDpu == false))
            continue;
        if (display->mPlugState == true) {
            extraDisplayConnected = true;
        }
    }
    if (extraDisplayConnected) {
        primaryDisplay->mBaseWindowIndex =
            PRIMARY_DISP_BASE_WIN[mDeviceInfo.displayMode];
        primaryDisplay->mMaxWindowNum -= primaryDisplay->mBaseWindowIndex;
    }
}
