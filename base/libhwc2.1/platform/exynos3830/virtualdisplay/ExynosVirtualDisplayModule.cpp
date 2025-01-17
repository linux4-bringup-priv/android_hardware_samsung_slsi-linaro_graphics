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
#include "ExynosGraphicBuffer.h"

using namespace vendor::graphics;

ExynosVirtualDisplayModule::ExynosVirtualDisplayModule(DisplayIdentifier node)
    :   ExynosVirtualDisplay(node)
{
    mGLESFormat = HAL_PIXEL_FORMAT_EXYNOS_YCbCr_420_SP_M;

    mBaseWindowIndex = 0;
    mMaxWindowNum = 0;
}

ExynosVirtualDisplayModule::~ExynosVirtualDisplayModule ()
{

}

bool ExynosVirtualDisplayModule::is2StepBlendingRequired(exynos_image &src, buffer_handle_t outbuf)
{
    ExynosGraphicBufferMeta gmeta(outbuf);
    if (!outbuf) {
    //    HWC_LOGE(NULL, "%s:: outbuf is null but return value is true", __func__);
        return true;
    }
    if (gmeta.width != src.w || gmeta.height != src.h)
        return true;

    return false;
}
