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

#ifndef ANDROID_EXYNOS_IHWC_H_
#define ANDROID_EXYNOS_IHWC_H_

#include <stdint.h>
#include <sys/types.h>
#include <vector>

#include <utils/Errors.h>
#include <utils/RefBase.h>
#include <binder/IInterface.h>

namespace android {

struct CPUPerfInfo {
    int32_t cpuIDs = -1;
    int32_t min_clock = -1;
};

enum {
    SET_WFD_MODE,
    GET_WFD_MODE,
    GET_WFD_INFO,
    SEND_WFD_COMMAND,
    SET_WFD_OUTPUT_RESOLUTION,
    SET_BOOT_FINISHED,
    SET_VIRTUAL_HPD,
    SET_VDS_GLES_FORMAT,
    GET_EXTERNAL_HDR_CAPA,

    // Below command numbers should be fixed because debugging command is already shared to customers.
    HWC_CONTROL = 12,
    ENABLE_MPP = 15,

    GET_DUMP_LAYER = 100,
    PRINT_MPP_ATTR = 101,
    SET_HWC_DEBUG = 105,
    GET_HWC_DEBUG = 106,
    SET_HWC_FENCE_DEBUG = 107,
    GET_HWC_FENCE_DEBUG = 108,

    GET_CPU_PERF_INFO = 109,
    SET_INTERFACE_DEBUG = 110,

    SET_DISPLAY_MULTI_THREADED_PRESENT = 1010,
};

class IExynosHWCService : public IInterface {
  public:
    DECLARE_META_INTERFACE(ExynosHWCService);
    /*
     * setWFDMode() function sets the WFD operation Mode.
     * It enables / disables the WFD.
     */
    virtual int setWFDMode(unsigned int mode) = 0;
    virtual int getWFDMode() = 0;
    virtual int getWFDInfo(int32_t *state, int32_t *compositionType, int32_t *format,
                           int64_t *usage, int32_t *width, int32_t *height) = 0;
    virtual int sendWFDCommand(int32_t cmd, int32_t ext1, int32_t ext2) = 0;
    virtual int setWFDOutputResolution(unsigned int width, unsigned int height) = 0;
    virtual int setVDSGlesFormat(int format) = 0;
    virtual int getExternalHdrCapabilities() = 0;
    virtual void setBootFinished(void) = 0;
    virtual uint32_t getHWCDebug() = 0;
    virtual int getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *min_clock) = 0;

    /*
    virtual void notifyPSRExit() = 0;
    */
    virtual int32_t setDisplayMultiThreadedPresent(const int32_t& displayId,
                                                   const bool& enable) = 0;
};

/* Native Interface */
class BnExynosHWCService : public BnInterface<IExynosHWCService> {
  public:
    virtual status_t onTransact(uint32_t code,
                                const Parcel &data,
                                Parcel *reply,
                                uint32_t flags = 0) = 0;
};
}  // namespace android
#endif
