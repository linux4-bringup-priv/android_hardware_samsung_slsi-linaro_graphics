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

#ifndef ANDROID_EXYNOS_HWC_SERVICE_H_
#define ANDROID_EXYNOS_HWC_SERVICE_H_

#include <utils/Errors.h>
#include <sys/types.h>
#include <log/log.h>
#include <binder/IServiceManager.h>
#include <utils/Singleton.h>
#include <utils/StrongPointer.h>
#include "IExynosHWC.h"
#include "ExynosHWC.h"

namespace android {

class ExynosHWCService
    : public BnExynosHWCService,
      Singleton<ExynosHWCService> {
  public:
    static ExynosHWCService *getExynosHWCService();
    ~ExynosHWCService();
    virtual void setExynosDevice(ExynosDevice *device);

    virtual int setWFDMode(unsigned int mode);
    virtual int getWFDMode();
    virtual int getWFDInfo(int32_t *state, int32_t *compositionType, int32_t *format,
                           int64_t *usage, int32_t *width, int32_t *height);
    virtual int sendWFDCommand(int32_t cmd, int32_t ext1, int32_t ext2);
    virtual int setWFDOutputResolution(unsigned int width, unsigned int height);
    virtual int setVDSGlesFormat(int format);
    virtual int getExternalHdrCapabilities();
    void setBootFinishedCallback(void (*callback)(ExynosDevice *));
    virtual void setBootFinished(void);
    virtual uint32_t getHWCDebug();
    virtual int getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *min_clock);
    virtual int32_t setDisplayMultiThreadedPresent(const int32_t& display_id,
                                                   const bool& enable) override;

    void enableMPP(uint32_t physicalType, uint32_t physicalIndex, uint32_t logicalIndex, uint32_t enable);
    /* Below functions are used only with vndservice call */
    void setHWCDebug(int debug);
    void setHWCFenceDebug(uint32_t ipNum, uint32_t fenceNum, uint32_t mode);
    void getHWCFenceDebug();
    int setHWCCtl(uint32_t display, uint32_t ctrl, int32_t val);
    void setDumpCount(uint32_t dumpCount);
    void setInterfaceDebug(int32_t display, int32_t interface, int32_t value);
    virtual int printMppsAttr();

    virtual status_t onTransact(uint32_t code,
                                const Parcel &data,
                                Parcel *reply,
                                uint32_t flags = 0);

  private:
    friend class Singleton<ExynosHWCService>;
    ExynosHWCService();
    int createServiceLocked();
    ExynosHWCService *mHWCService;
    Mutex mLock;
    ExynosDevice *mExynosDevice;
    void (*bootFinishedCallback)(ExynosDevice *);
};

}  // namespace android
#endif
