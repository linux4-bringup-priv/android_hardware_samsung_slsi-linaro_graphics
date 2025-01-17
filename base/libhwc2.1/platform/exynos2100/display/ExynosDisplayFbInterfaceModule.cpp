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

#include "ExynosDisplayFbInterfaceModule.h"

decon_idma_type getDPPChannel(uint32_t type, uint32_t index) {
    for (auto &chMap : IDMA_CHANNEL_MAP) {
        if((chMap.type == type) && (chMap.index == index))
            return chMap.channel;
    }
    return MAX_DECON_DMA_TYPE;
}

decon_idma_type getSubDeconChannel(decon_idma_type channel)
{
    for(uint32_t i = 0; i < (sizeof(VIRTUAL_CHANNEL_PAIR_MAP) / sizeof(virtual_dpp_map)) ; i++) {
        if (channel == VIRTUAL_CHANNEL_PAIR_MAP[i].channel1)
            return VIRTUAL_CHANNEL_PAIR_MAP[i].channel2;
    }
    return IDMA(WB+1);
}

int32_t remakeConfigForVirtual8K(struct decon_win_config* savedVirtualWinConfig,
        decon_win_config_data *fbConfigData)
{
    struct decon_win_config *config = fbConfigData->config;
    int32_t padding = 4;

    /* TODO to be defined more clearly */
    /* Check virtual8K assigned */
    int32_t virtual8KDPPIndex = -1;
    for (uint32_t i = 0; i < MAX_DECON_WIN; i++) {
        if (config[i].src.w > MIN_WIDTH_USING_VIRTUAL_8K) {
            virtual8KDPPIndex = i;
            break;
        }
    }

    if (virtual8KDPPIndex < 0) return -1;

    /* Check last IDMA DPP */
    if (config[MAX_DECON_WIN-1].state != config[MAX_DECON_WIN-1].DECON_WIN_STATE_DISABLED) {
        ALOGE("Virtual 8K DPP is assigned but no room for DPP split!!");
        return -1;
    }

    /* Make a room for sub-dpp */
    for (uint32_t i = (MAX_DECON_WIN - 2); i > virtual8KDPPIndex; i--) {
        config[i+1] = config[i];
    }

    /* Copy virtualDPP */
    *savedVirtualWinConfig = config[virtual8KDPPIndex];
    config[virtual8KDPPIndex + 1] = config[virtual8KDPPIndex];

    /* With Rotation */
    if (config[virtual8KDPPIndex].dpp_parm.rot == DPP_ROT_90) {
        config[virtual8KDPPIndex].src.y =
            config[virtual8KDPPIndex].src.y + config[virtual8KDPPIndex].src.h/2 - padding;
        config[virtual8KDPPIndex].src.h = config[virtual8KDPPIndex].src.h/2 + padding;
        config[virtual8KDPPIndex].dst.w = config[virtual8KDPPIndex].dst.w/2;

        config[virtual8KDPPIndex].aux_src.spl_en = DPP_SPLIT_LEFT;
        config[virtual8KDPPIndex].aux_src.padd_w = 0;
        config[virtual8KDPPIndex].aux_src.padd_h = padding;
        config[virtual8KDPPIndex].aux_src.spl_drtn = DPP_SPLIT_HORIZONTAL;

        config[virtual8KDPPIndex+1].idma_type = getSubDeconChannel(config[virtual8KDPPIndex].idma_type);
        config[virtual8KDPPIndex+1].src.h = config[virtual8KDPPIndex+1].src.h/2 + padding;
        config[virtual8KDPPIndex+1].dst.x =
            config[virtual8KDPPIndex+1].dst.x + config[virtual8KDPPIndex+1].dst.w/2;
        config[virtual8KDPPIndex+1].dst.w = config[virtual8KDPPIndex+1].dst.w/2;

        config[virtual8KDPPIndex+1].aux_src.spl_en = DPP_SPLIT_RIGHT;
        config[virtual8KDPPIndex+1].aux_src.padd_w = 0;
        config[virtual8KDPPIndex+1].aux_src.padd_h = padding;
        config[virtual8KDPPIndex+1].aux_src.spl_drtn = DPP_SPLIT_HORIZONTAL;
        config[virtual8KDPPIndex+1].acq_fence = -1;
    } else {
        config[virtual8KDPPIndex].src.w = config[virtual8KDPPIndex].src.w/2 + padding;
        config[virtual8KDPPIndex].dst.w = config[virtual8KDPPIndex].dst.w/2;

        config[virtual8KDPPIndex].aux_src.spl_en = DPP_SPLIT_LEFT;
        config[virtual8KDPPIndex].aux_src.padd_w = padding;
        config[virtual8KDPPIndex].aux_src.padd_h = 0;
        config[virtual8KDPPIndex].aux_src.spl_drtn = DPP_SPLIT_VERTICAL;

        config[virtual8KDPPIndex+1].idma_type = getSubDeconChannel(config[virtual8KDPPIndex].idma_type);
        config[virtual8KDPPIndex+1].src.x =
            config[virtual8KDPPIndex+1].src.x + config[virtual8KDPPIndex+1].src.w/2 - padding;
        config[virtual8KDPPIndex+1].src.w = config[virtual8KDPPIndex+1].src.w/2 + padding;
        config[virtual8KDPPIndex+1].dst.x =
            config[virtual8KDPPIndex+1].dst.x + config[virtual8KDPPIndex+1].dst.w/2;
        config[virtual8KDPPIndex+1].dst.w = config[virtual8KDPPIndex+1].dst.w/2;

        config[virtual8KDPPIndex+1].aux_src.spl_en = DPP_SPLIT_RIGHT;
        config[virtual8KDPPIndex+1].aux_src.padd_w = padding;
        config[virtual8KDPPIndex+1].aux_src.padd_h = 0;
        config[virtual8KDPPIndex+1].aux_src.spl_drtn = DPP_SPLIT_VERTICAL;
        config[virtual8KDPPIndex+1].acq_fence = -1;
    }

    return virtual8KDPPIndex;
}

int32_t restoreConfigForVirtual8K(struct decon_win_config savedVirtualWinConfig,
        decon_win_config_data *fbConfigData, int32_t virtualDPPindex, DisplayIdentifier &display)
{

    struct decon_win_config *config = fbConfigData->config;

    /* Release fence close for sub-dpp */
    ExynosFenceTracer::getInstance().fence_close(config[virtualDPPindex+1].rel_fence, display,
            FENCE_TYPE_READBACK_RELEASE, FENCE_IP_FB, "FbInterface::postProcessForVirtual8K: subDPP's rel_fence");

    /* Remove sub-dpp */
    for (uint32_t i = (virtualDPPindex+1); i < MAX_DECON_WIN; i++) {
        config[i] = config[i+1];
    }

    /* remove last config */
    config[MAX_DECON_WIN-1].fd_idma[0] = -1;
    config[MAX_DECON_WIN-1].fd_idma[1] = -1;
    config[MAX_DECON_WIN-1].fd_idma[2] = -1;
    config[MAX_DECON_WIN-1].state = config[MAX_DECON_WIN-1].DECON_WIN_STATE_DISABLED;
    config[MAX_DECON_WIN-1].acq_fence = -1;
    config[MAX_DECON_WIN-1].rel_fence = -1;

    /* restore virtual dpp infomation */
    savedVirtualWinConfig.rel_fence = config[virtualDPPindex].rel_fence;
    config[virtualDPPindex] = savedVirtualWinConfig;

    return NO_ERROR;
}
