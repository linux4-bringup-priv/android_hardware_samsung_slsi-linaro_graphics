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

#ifndef EXYNOS_VIRTUALDISPLAY_FB_INTERFACE_MODULE_H
#define EXYNOS_VIRTUALDISPLAY_FB_INTERFACE_MODULE_H

#include "ExynosDisplayFbInterface.h"
#include "ExynosVirtualDisplayFbInterface.h"

class ExynosVirtualDisplayFbInterfaceModule : public ExynosVirtualDisplayFbInterface {
    public:
        ExynosVirtualDisplayFbInterfaceModule();
        virtual ~ExynosVirtualDisplayFbInterfaceModule();
        virtual decon_idma_type getDeconDMAType(
                uint32_t type, uint32_t index) override;

        /* virtual 8K */
        virtual decon_idma_type getSubDeconDMAType(decon_idma_type channel);
        virtual int32_t preProcessForVirtual8K(struct decon_win_config* savedVirtualWinConfig);
        virtual int32_t postProcessForVirtual8K(struct decon_win_config savedVirtualWinConfig);
};
#endif
