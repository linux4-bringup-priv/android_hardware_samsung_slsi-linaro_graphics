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

#ifndef _EXYNOSDEVICE_H
#define _EXYNOSDEVICE_H

#include <unistd.h>
#include <hardware_legacy/uevent.h>

#include <utils/Vector.h>
#include <utils/Trace.h>
#include <utils/Thread.h>
#include <utils/Mutex.h>
#include <utils/Condition.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <cutils/atomic.h>
#include <unordered_map>

#include <thread>
#include <atomic>
#include <utils/Mutex.h>
#include <utils/Condition.h>

#include <hardware/hwcomposer2.h>

#include "ExynosHWC.h"
#include "ExynosHWCModule.h"
#include "ExynosHWCTypes.h"
#include "ExynosHWCHelper.h"
#include "ExynosHWCTypes.h"
#include "ExynosFenceTracer.h"
#include "OneShotTimer.h"

#define MAX_DEV_NAME 128
#define ERROR_LOG_PATH0 "/data/vendor/log/hwc"
#define ERROR_LOG_PATH1 "/data/vendor/log"
#define ERR_LOG_SIZE (1024 * 1024)        // 1MB
#define FENCE_ERR_LOG_SIZE (1024 * 1024)  // 1MB

#ifndef DOZE_VSYNC_PERIOD
#define DOZE_VSYNC_PERIOD 33333333  // 30fps
#endif

#ifndef DYNAMIC_RECOMP_PERIOD
#define DYNAMIC_RECOMP_PERIOD 200000  // 200MS
#endif

#ifndef MAX_SUPPORTED_FPS
#define MAX_SUPPORTED_FPS 120
#endif

#ifdef USE_DQE_INTERFACE
#include <hardware/exynos/dqeInterface.h>
#ifndef DEFAULT_DQE_INTERFACE_XML
#define DEFAULT_DQE_INTERFACE_XML "/vendor/etc/dqe/DQE_coef_data_default.xml"
#endif
#endif

typedef long epic_handle;

#ifndef DRM_DEVICE_PATH
#define DRM_DEVICE_PATH "/dev/dri/card0"
#endif

#ifndef WRITEBACK_CAPTURE_PATH
#define WRITEBACK_CAPTURE_PATH "/data/vendor/log/hwc"
#endif

using namespace android;

class ExynosDevice;
class ExynosDisplay;
class ExynosLayer;
class ExynosResourceManager;
class ExynosDeviceInterface;

class ExynosDevice : public ExynosHotplugHandler, ExynosPanelResetHandler, ExynosFpsChangedCallback {
  public:
    /**
         * TODO : Should be defined as ExynosDisplay type
         * Display list that managed by Device.
         */
    android::Vector<ExynosDisplay *> mDisplays;
    std::map<uint32_t, ExynosDisplay *> mDisplayMap;

    int mNumVirtualDisplay;

    /**
         * Resource manager object that is used to manage HW resources and assign resources to each layers
         */
    ExynosResourceManager *mResourceManager;

    /**
         * Geometry change will be saved by bit map.
         * ex) Display create/destory.
         */
    uint64_t mGeometryChanged;

    /**
         * This is a timer to release epic request
         * after the set time has elapsed.
         */
    OneShotTimer *mEPICOneShotTimer = nullptr;

    bool mIsBootFinished;

    /**
         * Callback informations those are used by SurfaceFlinger.
         * - VsyncCallback: Vsync detect callback.
         * - RefreshCallback: Callback by refresh request from HWC.
         * - HotplugCallback: Hot plug event by new display hardware.
         */

    /** TODO : Array size shuld be checked */
    exynos_callback_info_t mCallbackInfos[HWC2_CALLBACK_SEAMLESS_POSSIBLE + 1];

    /**
         * mDisplayId of display that has the slowest fps.
         * HWC uses vsync of display that has the slowest fps to main vsync.
         */
    uint32_t mVsyncDisplayId;
    uint32_t mVsyncPeriod;

    uint64_t mTimestamp;
    uint32_t mDisplayMode;

    uint32_t mTotalDumpCount;
    bool mIsDumpRequest;
    bool mCanProcessWCG;

    /**
         * This will be initialized with differnt class
         * that inherits ExynosDeviceInterface according to
         * interface type.
         */
    std::unique_ptr<ExynosDeviceInterface> mDeviceInterface;

    uint32_t mVsyncMode = DEFAULT_MODE;

#ifdef USE_DQE_INTERFACE
    dqeInterface *mDqeCoefInterface;
#endif

    // Con/Destructors
    ExynosDevice();
    virtual ~ExynosDevice();

    virtual void handleHotplug() override;
    virtual void handlePanelReset();
    virtual void fpsChangedCallback() override;
    void handleHotplugAfterBooting();
    bool isFirstValidate(ExynosDisplay *display);
    bool isLastValidate(ExynosDisplay *display);
    bool isLastPresent(ExynosDisplay *display);

    /**
         * @param display
         */
    ExynosDisplay *getDisplay(uint32_t display) {
        return mDisplayMap[display];
    };

    /**
         * Device Functions for HWC 2.0
         */

    /**
         * Descriptor: HWC2_FUNCTION_CREATE_VIRTUAL_DISPLAY
         * HWC2_PFN_CREATE_VIRTUAL_DISPLAY
         */
    int32_t createVirtualDisplay(
        uint32_t width, uint32_t height, int32_t *format, ExynosDisplay *display);

    /**
         * Descriptor: HWC2_FUNCTION_DESTROY_VIRTUAL_DISPLAY
         * HWC2_PFN_DESTROY_VIRTUAL_DISPLAY
         */
    int32_t destroyVirtualDisplay(
        ExynosDisplay *display);

    /**
         * Descriptor: HWC2_FUNCTION_DUMP
         * HWC2_PFN_DUMP
         */
    void dump(uint32_t *outSize, char *outBuffer);

    /**
         * Descriptor: HWC2_FUNCTION_GET_MAX_VIRTUAL_DISPLAY_COUNT
         * HWC2_PFN_GET_MAX_VIRTUAL_DISPLAY_COUNT
         */
    /* TODO overide check!! */
    uint32_t getMaxVirtualDisplayCount();

    /* Descriptor: HWC2_FUNCTION_GET_LAYER_GENERIC_METADATA_KEY
         */
    void getLayerGenericMetadataKey(uint32_t __unused keyIndex,
                                    uint32_t *__unused outKeyLength, char *__unused outKey, bool *__unused outMandatory);

    /**
         * Descriptor: HWC2_FUNCTION_REGISTER_CALLBACK
         * HWC2_PFN_REGISTER_CALLBACK
         */
    int32_t registerCallback(
        int32_t descriptor, hwc2_callback_data_t callbackData, hwc2_function_pointer_t point);

    void setHWCDebug(unsigned int debug);
    uint32_t getHWCDebug();
    void setHWCFenceDebug(uint32_t ipNum, uint32_t typeNum, uint32_t mode);
    void getHWCFenceDebug();
    void setHWCControl(uint32_t display, uint32_t ctrl, int32_t val);
    void enableMPP(uint32_t physicalType, uint32_t physicalIndex,
                   uint32_t logicalIndex, uint32_t enable);
    void setDisplayMode(uint32_t displayMode);
    bool checkDisplayEnabled(uint32_t displayId);
    bool checkAdditionalConnection();
    void getCapabilities(uint32_t *outCount, int32_t *outCapabilities);
    void setGeometryChanged(uint64_t changedBit) { mGeometryChanged |= changedBit; };
    void clearGeometryChanged();
    void setGeometryFlagForNextFrame();
    bool canSkipValidate();
    bool compareVsyncPeriod();
    void setDumpCount();
    void clearRenderingStateFlags();
    bool wasRenderingStateFlagsCleared();

    virtual bool getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *minClock);

    /* Add EPIC APIs */
    void *mEPICHandle = NULL;
    void (*mEPICInit)();
    epic_handle (*mEPICRequestFcnPtr)(int id);
    void (*mEPICFreeFcnPtr)(epic_handle handle);
    bool (*mEPICAcquireFcnPtr)(epic_handle handle);
    bool (*mEPICReleaseFcnPtr)(epic_handle handle);
    bool (*mEPICAcquireOptionFcnPtr)(epic_handle handle, unsigned int value, unsigned int usec);
    bool (*mEPICAcquireConditional)(epic_handle handle, const char *name, ssize_t size);
    bool (*mEPICFreeConditional)(epic_handle handle, const char *name, ssize_t size);

    bool handleVsyncPeriodChange();
    bool handleVsyncPeriodChangeInternal();
    virtual bool supportPerformaceAssurance() { return false; };
    virtual void performanceAssuranceInternal(); // REQUIRES(mMutex)
    /*
     * This function checks mActiveConfig of each display
     * in order to get fps for performance.
     * However this function uses config instead of mActiveConfig of display
     * if the display is specified as a paramter.
     * Caller should specifies the display and config if it wants to get fps
     * before config is applied to mActiveConfig.
     */
    virtual int getFpsForPerformance(ExynosDisplay *display = nullptr, hwc2_config_t config = 0);
    virtual int getFpsForCapacity(ExynosDisplay *display = nullptr, hwc2_config_t config = 0);

    virtual void setCPUClocksPerCluster(__unused uint32_t fps) { return; };
    virtual void acquireCPUPerfPerCluster(__unused uint32_t fps) { return; }
    virtual void releaseCPUPerfPerCluster() { return; }

    class captureReadbackClass {
      public:
        captureReadbackClass();
        ~captureReadbackClass();
        int32_t allocBuffer(uint32_t format, uint32_t w, uint32_t h);
        buffer_handle_t &getBuffer() { return mBuffer; };
        void saveToFile(const String8 &fileName);

      private:
        buffer_handle_t mBuffer = nullptr;
    };
    void captureScreenWithReadback(uint32_t displayType);
    void cleanupCaptureScreen(void *buffer);
    void signalReadbackDone();
    void clearWaitingReadbackReqDone() {
        mIsWaitingReadbackReqDone = false;
    };
    void setVsyncMode();
    virtual void initDeviceInterface(uint32_t interfaceType);
    uint32_t getDeviceInterfaceType();
    void initDisplays();
    void registerHandlers();
    void registerRestrictions();

    /** APIs for display **/
    virtual int32_t validateDisplay(
        ExynosDisplay *display,
        uint32_t *outNumTypes, uint32_t *outNumRequests);
    int32_t validateAllDisplays(ExynosDisplay *firstDisplay,
                                uint32_t *outNumTypes, uint32_t *outNumRequests);
    int32_t getDeviceValidateInfo(DeviceValidateInfo &info);
    int32_t getDeviceResourceInfo(DeviceResourceInfo &info);

    virtual int32_t presentDisplay(ExynosDisplay *display,
                                   int32_t *outPresentFence);
    int32_t finishFrame();
    int32_t getDevicePresentInfo(DevicePresentInfo &info);
    void invalidate();

    int32_t setColorMode(ExynosDisplay *display, int32_t mode);
    int32_t setColorModeWithRenderIntent(ExynosDisplay *display,
                                         int32_t mode, int32_t intent);
    int32_t getColorModes(ExynosDisplay *display, uint32_t *outNumModes,
                          int32_t *outModes);
    int32_t setPowerMode(ExynosDisplay *display,
                         int32_t /*hwc2_power_mode_t*/ mode);
    int32_t createLayer(ExynosDisplay *display, hwc2_layer_t *outLayer);
    int32_t destroyLayer(ExynosDisplay *display, hwc2_layer_t layer);
    int32_t setColorTransform(ExynosDisplay *display, const float *matrix,
                              int32_t /*android_color_transform_t*/ hint);
    int32_t setClientTarget(ExynosDisplay *display, buffer_handle_t target,
                            int32_t acquireFence, int32_t dataspace);
    int32_t setActiveConfig(ExynosDisplay *display, hwc2_config_t config);
    int32_t setActiveConfigWithConstraints(ExynosDisplay *display, hwc2_config_t config,
                                           hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
                                           hwc_vsync_period_change_timeline_t *outTimeline);

    /** APIs for layer **/
    int32_t setLayerBlendMode(ExynosLayer *layer,
                              int32_t /*hwc2_blend_mode_t*/ mode);
    int32_t setLayerBuffer(ExynosDisplay *display, hwc2_layer_t layer,
                           buffer_handle_t buffer, int32_t acquireFence);
    int32_t setLayerCompositionType(ExynosLayer *layer,
                                    int32_t /*hwc2_composition_t */ type);
    int32_t setLayerDataspace(ExynosDisplay *display, hwc2_layer_t layer,
                              int32_t /*android_dataspace_t*/ dataspace);
    int32_t setLayerDisplayFrame(ExynosLayer *layer, hwc_rect_t frame);
    int32_t setLayerSourceCrop(ExynosLayer *layer, hwc_frect_t crop);
    int32_t setLayerTransform(ExynosLayer *layer,
                              int32_t /*hwc_transform_t*/ transform);
    int32_t setLayerZOrder(ExynosLayer *layer, uint32_t z);
    int32_t printMppsAttr();
    void resetForDestroyClient();

  protected:
    void updateNonPrimaryDisplayList(ExynosDisplay *display);

  protected:
    uint32_t mInterfaceType;
    Mutex mMutex;
    DeviceValidateInfo mDeviceValidateInfo;
    DevicePresentInfo mDevicePresentInfo;

  private:
    Mutex mCaptureMutex;
    Condition mCaptureCondition;
    std::atomic<bool> mIsWaitingReadbackReqDone = false;
    ExynosFenceTracer &mFenceTracer = ExynosFenceTracer::getInstance();
};
#endif  //_EXYNOSDEVICE_H
