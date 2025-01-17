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
#undef LOG_TAG
#define LOG_TAG "virtualdisplaymodule"

#include "ExynosVirtualDisplayModule.h"
#include "ExynosDisplayFbInterfaceModule.h"
#include "ExynosVirtualDisplayFbInterfaceModule.h"

ExynosVirtualDisplayModule::ExynosVirtualDisplayModule(DisplayIdentifier node)
    :   ExynosVirtualDisplay(node)
{
    mGLESFormat = HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M;

    mBaseWindowIndex = 0;
    mMaxWindowNum = 2;
    mUseDpu = true;
}

ExynosVirtualDisplayModule::~ExynosVirtualDisplayModule ()
{

}

void ExynosVirtualDisplayModule::initDisplayInterface(
        uint32_t __unused interfaceType,
        void* deviceData, size_t& deviceDataSize)
{
    mDisplayInterface = std::make_unique<ExynosVirtualDisplayFbInterfaceModule>();
    mDisplayInterface->init(mDisplayInfo.displayIdentifier,
            deviceData, deviceDataSize);
}
