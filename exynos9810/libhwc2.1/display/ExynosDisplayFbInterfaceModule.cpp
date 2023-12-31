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

decon_idma_type getIDMAType(uint32_t type, uint32_t index) {
    if (type == MPP_DPP_VG)
        return (decon_idma_type)((uint32_t)IDMA_VG0 + index);
    else if (type == MPP_DPP_VGFS)
        return (decon_idma_type)((uint32_t)IDMA_VGF0 + index);
    else if (type == MPP_DPP_VGRFS)
        return (decon_idma_type)((uint32_t)IDMA_VGRF0 + index);
    else if (type == MPP_DPP_G) {
        switch (index) {
            case 0:
                return IDMA_G0;
            case 1:
                return IDMA_G1;
            default:
                return MAX_DECON_DMA_TYPE;
        }
    } else
        return MAX_DECON_DMA_TYPE;
}
