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
#include "ExynosPrimaryDisplayFbInterfaceModule.h"

ExynosPrimaryDisplayFbInterfaceModule::ExynosPrimaryDisplayFbInterfaceModule()
: ExynosPrimaryDisplayFbInterface()
{
}

ExynosPrimaryDisplayFbInterfaceModule::~ExynosPrimaryDisplayFbInterfaceModule()
{
}

decon_idma_type ExynosPrimaryDisplayFbInterfaceModule::getDeconDMAType(
        uint32_t type, uint32_t index)
{
    return getDPPChannel(type, index);
}

int32_t ExynosPrimaryDisplayFbInterfaceModule::configFromDisplayConfig(decon_win_config &config,
        const exynos_win_config_data &display_config) {
    int32_t ret = ExynosPrimaryDisplayFbInterface::configFromDisplayConfig(config, display_config);

    if (display_config.hdrLayerDataspace != HAL_DATASPACE_UNKNOWN) {
        uint32_t transfer = (display_config.hdrLayerDataspace & HAL_DATASPACE_TRANSFER_MASK);
        if (dataspace_transfer_map.find(transfer) != dataspace_transfer_map.end())
            config.dpp_parm.hdr_std = dataspace_transfer_map.at(transfer).hdr_std;
    }
    return ret;
}
