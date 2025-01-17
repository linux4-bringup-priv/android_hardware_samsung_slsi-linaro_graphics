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
    for (int i=0; i < MAX_DECON_DMA_TYPE; i++){
        if((IDMA_CHANNEL_MAP[i].type == type) &&
           (IDMA_CHANNEL_MAP[i].index == index))
            return IDMA_CHANNEL_MAP[i].channel;
    }
    return MAX_DECON_DMA_TYPE;
}
