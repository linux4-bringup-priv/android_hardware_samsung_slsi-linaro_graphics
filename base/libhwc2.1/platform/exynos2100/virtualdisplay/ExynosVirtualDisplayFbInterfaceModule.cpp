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

#include "ExynosDisplayFbInterfaceModule.h"
#include "ExynosVirtualDisplayFbInterfaceModule.h"

ExynosVirtualDisplayFbInterfaceModule::ExynosVirtualDisplayFbInterfaceModule()
: ExynosVirtualDisplayFbInterface()
{
}

ExynosVirtualDisplayFbInterfaceModule::~ExynosVirtualDisplayFbInterfaceModule()
{
}

decon_idma_type ExynosVirtualDisplayFbInterfaceModule::getDeconDMAType(
        uint32_t type, uint32_t index)
{
    return getDPPChannel(type, index);
}

decon_idma_type ExynosVirtualDisplayFbInterfaceModule::getSubDeconDMAType(decon_idma_type channel)
{
    return getSubDeconChannel(channel);
}

int32_t ExynosVirtualDisplayFbInterfaceModule::preProcessForVirtual8K(struct decon_win_config* savedVirtualWinConfig)
{
    return remakeConfigForVirtual8K(savedVirtualWinConfig, &mFbConfigData);
}

int32_t ExynosVirtualDisplayFbInterfaceModule::postProcessForVirtual8K(struct decon_win_config savedVirtualWinConfig)
{
    return restoreConfigForVirtual8K(savedVirtualWinConfig, &mFbConfigData, mVirtual8KDPPIndex, mDisplayIdentifier);
}
