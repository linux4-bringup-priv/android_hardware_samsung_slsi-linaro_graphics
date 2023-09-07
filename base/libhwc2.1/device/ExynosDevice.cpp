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

#include <sched.h>
#include <dlfcn.h>

#include <hardware/exynos/ion.h>

#include "ExynosDevice.h"
#include "ExynosDisplay.h"
#include "ExynosLayer.h"
#include "ExynosPrimaryDisplayModule.h"
#include "ExynosResourceManagerModule.h"
#include "ExynosExternalDisplayModule.h"
#include "ExynosVirtualDisplayModule.h"
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"
#include "ExynosDeviceFbInterface.h"
#include "ExynosDeviceDrmInterface.h"
#include <sync/sync.h>
#include <sys/mman.h>
#include <unistd.h>
#include "ExynosGraphicBuffer.h"

#ifdef USES_HWC_CPU_PERF_MODE
#include "CpuPerfInfo.h"
#else
typedef struct perfMap {
    uint32_t m2mCapa;
} perfMap_t;
static std::map<uint32_t, perfMap> perfTable = {
    {30, {24}},
    {60, {8}},
    {120, {4}},
};
#endif

using namespace vendor::graphics;

/**
 * ExynosDevice implementation
 */

class ExynosDevice;

extern void vsync_callback(hwc2_callback_data_t callbackData,
                           hwc2_display_t displayId, int64_t timestamp);

int hwcDebug;
int hwcFenceDebug[FENCE_IP_MAX];
struct exynos_hwc_control exynosHWCControl;
struct update_time_info updateTimeInfo;

bool enableSkipValidate = !(property_get_int32("persist.vendor.debug.disable.skip.validate", 0));

ExynosDevice::ExynosDevice()
    : mGeometryChanged(0),
      mIsBootFinished(false),
      mVsyncDisplayId(getDisplayId(HWC_DISPLAY_PRIMARY, 0)),
      mVsyncPeriod(0),
      mTimestamp(0),
      mDisplayMode(0),
      mTotalDumpCount(0),
      mIsDumpRequest(false),
      mInterfaceType(INTERFACE_TYPE_FB) {
#ifdef ENABLE_FORCE_GPU
    exynosHWCControl.forceGpu = true;
#else
    exynosHWCControl.forceGpu = false;
#endif
#ifdef HWC_DISABLE_WIN_UPDATE
    exynosHWCControl.windowUpdate = false;
#else
    exynosHWCControl.windowUpdate = true;
#endif
    exynosHWCControl.forcePanic = false;
    exynosHWCControl.skipResourceAssign = true;
    exynosHWCControl.multiResolution = true;
    exynosHWCControl.dumpMidBuf = false;
    exynosHWCControl.displayMode = DISPLAY_MODE_NUM;
    exynosHWCControl.skipWinConfig = true;
    exynosHWCControl.skipValidate = true;
    exynosHWCControl.doFenceFileDump = false;
    exynosHWCControl.fenceTracer = 0;
    exynosHWCControl.sysFenceLogging = false;
    exynosHWCControl.usePerfFile = false;

    /* Initialize pre defined format */
    PredefinedFormat::init();
    ExynosMPP::initDefaultMppFormats();

    hwcDebug = 0;

    mInterfaceType = getDeviceInterfaceType();
    ALOGD("%s : interface type(%d)", __func__, mInterfaceType);

    /* This order should not be changed
     * create ExynosResourceManager ->
     * create displays and add them to the list ->
     * initDeviceInterface() ->
     * ExynosResourceManager::updateRestrictions()
     */
    mResourceManager = new ExynosResourceManagerModule();
    DeviceResourceInfo deviceResourceInfo;
    getDeviceResourceInfo(deviceResourceInfo);
    mResourceManager->setDeviceInfo(deviceResourceInfo);

    String8 strDispW;
    String8 strDispH;
    for (size_t i = 0; i < DISPLAY_COUNT; i++) {
        exynos_display_t display_t = AVAILABLE_DISPLAY_UNITS[i];
        ExynosDisplay *exynos_display = NULL;
        DisplayIdentifier node;
        node.index = display_t.index;
        node.type = display_t.type;
        node.id = getDisplayId(node.type, node.index);
        node.name.appendFormat("%s", display_t.display_name);
        node.deconNodeName.appendFormat("%s", display_t.decon_node_name);
        switch (display_t.type) {
        case HWC_DISPLAY_PRIMARY:
            exynos_display = (ExynosDisplay *)(new ExynosPrimaryDisplayModule(node));
            /* Primary display always plugged-in */
            exynos_display->mPlugState = true;
            if (display_t.index == 0) {
                ExynosMPP::mainDisplayWidth = exynos_display->mXres;
                if (ExynosMPP::mainDisplayWidth <= 0) {
                    ExynosMPP::mainDisplayWidth = 1440;
                }
                ExynosMPP::mainDisplayHeight = exynos_display->mYres;
                if (ExynosMPP::mainDisplayHeight <= 0) {
                    ExynosMPP::mainDisplayHeight = 2560;
                }
            }
            strDispW.appendFormat("%d", exynos_display->mXres);
            strDispH.appendFormat("%d", exynos_display->mYres);
            property_set("vendor.hwc.display.w", strDispW.c_str());
            property_set("vendor.hwc.display.h", strDispH.c_str());
            break;
        case HWC_DISPLAY_EXTERNAL:
            exynos_display = (ExynosDisplay *)(new ExynosExternalDisplayModule(node));
            break;
        case HWC_DISPLAY_VIRTUAL:
            exynos_display = (ExynosDisplay *)(new ExynosVirtualDisplayModule(node));
            mNumVirtualDisplay = 0;
            break;
        default:
            ALOGE("Unsupported display type(%d)", display_t.type);
            break;
        }

        if (exynos_display != NULL) {
            mDisplays.add(exynos_display);
            mDisplayMap.insert(std::make_pair(exynos_display->mDisplayId,
                                              exynos_display));
        }
    }

    for (uint32_t i = 0; i < FENCE_IP_ALL; i++)
        hwcFenceDebug[i] = 0;

#ifdef USE_DQE_INTERFACE
    mDqeCoefInterface = new dqeInterface(DEFAULT_DQE_INTERFACE_XML);
#endif

    String8 saveString;
    saveString.appendFormat("ExynosDevice is initialized");
    DisplayIdentifier empty;
    uint32_t errFileSize = saveErrorLog(saveString, empty);
    ALOGI("Initial errlog size: %d bytes\n", errFileSize);

    /* If project use DRM interface,
     * Create and initialize at ExynosDeviceModule */
    if (mInterfaceType == INTERFACE_TYPE_DRM)
        mDeviceInterface = std::make_unique<ExynosDeviceDrmInterface>();
    else
        mDeviceInterface = std::make_unique<ExynosDeviceFbInterface>();

    initDeviceInterface(mInterfaceType);
    registerHandlers();
    registerRestrictions();
    /*
     * Otf MPP could be created by registerRestrictions.
     * initDisplays should be called after registerRestrictions for this case.
     */
    initDisplays();
    mResourceManager->initDisplays(mDisplays, mDisplayMap);
    int ret = mResourceManager->doPreProcessing();
    if (ret)
        ALOGI("ExynosResourceManager::doPreProcessing return fail %d", ret);

    mCanProcessWCG = mResourceManager->deviceSupportWCG();

    /* CPU clock settings */
#ifdef EPIC_LIBRARY_PATH
    mEPICHandle = dlopen(EPIC_LIBRARY_PATH, RTLD_NOW | RTLD_LOCAL);
    if (!mEPICHandle) {
        ALOGE("%s: DLOPEN failed\n", __func__);
        return;
    } else {
        (mEPICInit) =
            (void (*)())dlsym(mEPICHandle, "epic_init");
        (mEPICRequestFcnPtr) =
            (epic_handle(*)(int))dlsym(mEPICHandle, "epic_alloc_request_internal");
        (mEPICFreeFcnPtr) =
            (void (*)(epic_handle))dlsym(mEPICHandle, "epic_free_request_internal");
        (mEPICAcquireFcnPtr) =
            (bool (*)(epic_handle))dlsym(mEPICHandle, "epic_acquire_internal");
        (mEPICReleaseFcnPtr) =
            (bool (*)(epic_handle))dlsym(mEPICHandle, "epic_release_internal");
        (mEPICAcquireOptionFcnPtr) =
            (bool (*)(epic_handle, unsigned, unsigned))dlsym(mEPICHandle, "epic_acquire_option_internal");
        (mEPICAcquireConditional) =
            (bool (*)(epic_handle, const char *, ssize_t))dlsym(mEPICHandle, "epic_acquire_conditional_internal");
        (mEPICFreeConditional) =
            (bool (*)(epic_handle, const char *, ssize_t))dlsym(mEPICHandle, "epic_release_conditional_internal");
    }

    if (!mEPICRequestFcnPtr || !mEPICFreeFcnPtr || !mEPICAcquireFcnPtr || !mEPICReleaseFcnPtr) {
        ALOGE("%s: DLSYM failed\n", __func__);
    }

    if (!mEPICInit)
        ALOGE("%s: DLSYM mEPICInit failed", __func__);
    else
        mEPICInit();

    if (!mEPICAcquireConditional || !mEPICFreeConditional)
        ALOGI("%s: Additional DLSYM failed\n", __func__);
#endif

    if (mInterfaceType == INTERFACE_TYPE_DRM) {
        for (uint32_t i = 0; i < mResourceManager->getM2mMPPSize(); i++) {
            mResourceManager->getM2mMPP(i)->registerBufDestoryedCallback(
                [=](const uint64_t &bufferId) { ExynosDisplayInterface::removeBuffer(bufferId); });
        }
    }

    mResourceManager->initDisplaysTDMInfo();
    handleVsyncPeriodChange();
}

void ExynosDevice::updateNonPrimaryDisplayList(ExynosDisplay *display) {
    auto updateList = [&](std::vector<DisplayIdentifier> &list) {
        auto it = std::find(list.begin(), list.end(),
                            display->mDisplayInfo.displayIdentifier);
        if ((display->mPlugState) &&
            (it == list.end()))
            list.push_back(display->mDisplayInfo.displayIdentifier);

        if ((display->mPlugState == false) &&
            (it != list.end()))
            list.erase(it);
    };
    updateList(mDeviceValidateInfo.nonPrimaryDisplays);
    updateList(mDevicePresentInfo.nonPrimaryDisplays);
}

void ExynosDevice::handleHotplugAfterBooting() {
    bool hpdStatus = false;
    uint32_t displayIndex = UINT32_MAX;
    {
        Mutex::Autolock lock(mMutex);

        for (size_t i = 0; i < mDisplays.size(); i++) {
            if (mDisplays[i] == nullptr)
                continue;
            if (mDisplays[i]->mType != HWC_DISPLAY_EXTERNAL)
                continue;
            hpdStatus = mDisplays[i]->mDisplayInterface->readHotplugStatus();
            if (hpdStatus) {
                mDisplays[i]->handleHotplugEvent(hpdStatus);
                displayIndex = i;
                updateNonPrimaryDisplayList(mDisplays[i]);
                break;
            }
        }
        handleVsyncPeriodChangeInternal();
    }
    if (displayIndex != UINT32_MAX) {
        mDisplays[displayIndex]->hotplug();
        mDisplays[displayIndex]->invalidate();
    }
}

void ExynosDevice::handleHotplug() {
    bool hpdStatus = false;
    uint32_t displayIndex = UINT32_MAX;
    {
        Mutex::Autolock lock(mMutex);

        for (size_t i = 0; i < mDisplays.size(); i++) {
            if (mDisplays[i] == nullptr)
                continue;
            if (mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL)
                continue;
            if (mDisplays[i]->checkHotplugEventUpdated(hpdStatus)) {
                mDisplays[i]->handleHotplugEvent(hpdStatus);
                displayIndex = i;
                if (mDisplays[i]->mType == HWC_DISPLAY_EXTERNAL) {
                    updateNonPrimaryDisplayList(mDisplays[i]);
                    if (!hpdStatus) {
                        handleVsyncPeriodChangeInternal();
                    }
                }
                break;
            }
        }
    }
    if (displayIndex != UINT32_MAX) {
        if (mDisplays[displayIndex]->mType == HWC_DISPLAY_EXTERNAL) {
            for (size_t i = 0; i < mDisplays.size(); i++) {
                if (mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL && mDisplays[i]->mUseDpu) {
                    Mutex::Autolock lock(mMutex);
                    ExynosVirtualDisplay *virtualDisplay = (ExynosVirtualDisplay *)mDisplays[i];
                    virtualDisplay->setExternalPlugState(hpdStatus, mGeometryChanged);
                    if (hpdStatus && virtualDisplay->mPlugState)
                        virtualDisplay->destroyVirtualDisplay();
                    break;
                }
            }
        }

        mDisplays[displayIndex]->hotplug();
        mDisplays[displayIndex]->invalidate();
    }
}

void ExynosDevice::handlePanelReset() {
    int status;
    hwc2_power_mode_t prevModeState;
    hwc2_vsync_t prevVsyncState;

    prevModeState = mDisplays[0]->mPowerModeState;
    prevVsyncState = mDisplays[0]->mVsyncState;

    ALOGI("Powering off primary");
    status = mDisplays[0]->setPowerMode(HWC_POWER_MODE_OFF, mGeometryChanged);
    if (status != HWC2_ERROR_NONE) {
        ALOGE("power-off on primary failed with error = %d", status);
    }
    // Set PreModeState, recovery back to previous status.
    ALOGI("Restoring power mode on primary");
    status = mDisplays[0]->setPowerMode(prevModeState, mGeometryChanged);

    if (status != HWC2_ERROR_NONE) {
        ALOGE("Setting power mode on primary failed with error = %d", status);
    }
    //Driver doesn't restore vsync state and HWC should set Vsync State again.
    status = mDisplays[0]->setVsyncEnabled(prevVsyncState);
    if (status != NO_ERROR) {
        ALOGE("enabling vsync failed for primary with error = %d", status);
    }
}

bool ExynosDevice::handleVsyncPeriodChange() {
    Mutex::Autolock lock(mMutex);
    return handleVsyncPeriodChangeInternal();
}

bool ExynosDevice::handleVsyncPeriodChangeInternal() {
    bool vsyncChanged = false;
    if (compareVsyncPeriod()) {
        /*
         * m2mMPP capacity can be chaged in performanceAssuranceInternal
         * so resource reassignment is required
         * We don't need invalidate
         * New capacity would be applied in the next frame update
         */
        setGeometryChanged(GEOMETRY_DEVICE_FPS_CHANGED);
        vsyncChanged = true;
    }
    performanceAssuranceInternal();
    return vsyncChanged;
}

void ExynosDevice::fpsChangedCallback() {
    handleVsyncPeriodChangeInternal();
}

uint32_t ExynosDevice::getDeviceInterfaceType() {
    if (access(DRM_DEVICE_PATH, F_OK) == NO_ERROR)
        return INTERFACE_TYPE_DRM;
    else
        return INTERFACE_TYPE_FB;
}

void ExynosDevice::initDeviceInterface(uint32_t interfaceType) {
    void *deviceData = nullptr;
    size_t deviceDataSize = 0;

    mDeviceInterface->init(deviceData, deviceDataSize);
    if (deviceDataSize)
        deviceData = (void *)malloc(deviceDataSize);
    mDeviceInterface->init(deviceData, deviceDataSize);

    DeviceToDisplayInterface initData;
    if (mInterfaceType == INTERFACE_TYPE_DRM) {
        for (uint32_t i = 0; i < mResourceManager->getOtfMPPs().size(); i++) {
            ExynosMPP *cur = mResourceManager->getOtfMPPWithChannel(i);
            if (cur == nullptr)
                HWC_LOGE_NODISP("getOtfMPPWithChannel fail, ch(%d)", i);
            initData.exynosMPPsForChannel.push_back(cur);
        }
    }
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        mDisplays[i]->initDisplayInterface(interfaceType,
                                           deviceData, deviceDataSize);
        mDisplays[i]->mDisplayInterface->setDeviceToDisplayInterface(initData);
    }

    free(deviceData);

    ExynosDisplay *primaryDisplay = (ExynosDisplay *)getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    if (primaryDisplay != NULL)
        mDeviceInterface->setPrimaryDisplayFd(primaryDisplay->mDisplayInterface->getDisplayFd());
}

ExynosDevice::~ExynosDevice() {
    ExynosDisplay *primary_display = getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));

    delete primary_display;
    delete mResourceManager;

    if (mEPICHandle != NULL) {
        dlclose(mEPICHandle);
    }
}

bool ExynosDevice::isFirstValidate(ExynosDisplay *display) {
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        /*
         * Do not skip when mDisplays[i] is same with display
         * If this condition is skipped
         * display can be checked with the first display
         * even though it was already validated.
         * This can be happend when the first validated display
         * is powered off or disconnected.
         */

        if ((mDisplays[i]->mType != HWC_DISPLAY_VIRTUAL) &&
            (mDisplays[i]->mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_OFF))
            continue;

        if (mDisplays[i]->mPlugState == false)
            continue;

        /* exynos9810 specific source code */
        if (mDisplays[i]->mType == HWC_DISPLAY_EXTERNAL) {
            ExynosExternalDisplay *extDisp = (ExynosExternalDisplay *)mDisplays[i];
            if (extDisp->mBlanked == true)
                continue;
        }

        if (mDisplays[i]->mRenderingStateFlags.validateFlag) {
            HDEBUGLOGD(eDebugResourceManager, "\t%s is not first validate, %s is validated",
                       display->mDisplayName.c_str(), mDisplays[i]->mDisplayName.c_str());
            return false;
        }
    }

    HDEBUGLOGD(eDebugResourceManager, "\t%s is the first validate", display->mDisplayName.c_str());
    return true;
}

bool ExynosDevice::isLastValidate(ExynosDisplay *display) {
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        if (mDisplays[i] == display)
            continue;

        if ((mDisplays[i]->mType != HWC_DISPLAY_VIRTUAL) &&
            (mDisplays[i]->mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_OFF))
            continue;

        if (mDisplays[i]->mPlugState == false)
            continue;

        /* exynos9810 specific source code */
        if (mDisplays[i]->mType == HWC_DISPLAY_EXTERNAL) {
            ExynosExternalDisplay *extDisp = (ExynosExternalDisplay *)mDisplays[i];
            if (extDisp->mBlanked == true)
                continue;
        }

        if (mDisplays[i]->mRenderingStateFlags.validateFlag == false) {
            HDEBUGLOGD(eDebugResourceManager, "\t%s is not last validate, %s is not validated",
                       display->mDisplayName.c_str(), mDisplays[i]->mDisplayName.c_str());
            return false;
        }
    }
    HDEBUGLOGD(eDebugResourceManager, "\t%s is the last validate", display->mDisplayName.c_str());
    return true;
}

bool ExynosDevice::isLastPresent(ExynosDisplay *display) {
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        if (mDisplays[i] == display)
            continue;

        if ((mDisplays[i]->mType != HWC_DISPLAY_VIRTUAL) &&
            (mDisplays[i]->mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_OFF))
            continue;

        if (mDisplays[i]->mPlugState == false)
            continue;

        /* exynos9810 specific source code */
        if (mDisplays[i]->mType == HWC_DISPLAY_EXTERNAL) {
            ExynosExternalDisplay *extDisp = (ExynosExternalDisplay *)mDisplays[i];
            if (extDisp->mBlanked == true)
                continue;
        }

        if (mDisplays[i]->mRenderingStateFlags.presentFlag == false) {
            HDEBUGLOGD(eDebugResourceManager, "\t%s is not last present, %s is not presented",
                       display->mDisplayName.c_str(), mDisplays[i]->mDisplayName.c_str());
            return false;
        }
    }
    HDEBUGLOGD(eDebugResourceManager, "\t%s is the last present", display->mDisplayName.c_str());
    return true;
}

/**
 * Device Functions for HWC 2.0
 */

int32_t ExynosDevice::createVirtualDisplay(
    uint32_t width, uint32_t height, int32_t * /*android_pixel_format_t*/ format, ExynosDisplay *display) {
    Mutex::Autolock lock(mMutex);

    ((ExynosVirtualDisplay *)display)->createVirtualDisplay(width, height, format);
    updateNonPrimaryDisplayList(display);

    handleVsyncPeriodChangeInternal();

    return HWC2_ERROR_NONE;
}

/**
 * @param *display
 * @return int32_t
 */
int32_t ExynosDevice::destroyVirtualDisplay(ExynosDisplay *display) {
    Mutex::Autolock lock(mMutex);

    ((ExynosVirtualDisplay *)display)->destroyVirtualDisplay();
    mResourceManager->reloadResourceForHWFC();
    mResourceManager->setTargetDisplayLuminance(0, 100);
    mResourceManager->setTargetDisplayDevice(0);

    updateNonPrimaryDisplayList(display);

    handleVsyncPeriodChangeInternal();

    return HWC2_ERROR_NONE;
}

void ExynosDevice::initDisplays() {
    Mutex::Autolock lock(mMutex);
    uint32_t maxWinNum = mResourceManager->getOtfMPPs().size() -
                         mResourceManager->mVirtualMPPNum;
    for (auto display : mDisplays) {
        display->init(maxWinNum,
                      mResourceManager->getExynosMPPForBlending(display));
    }
}

void ExynosDevice::registerHandlers() {
    /* This order should not be changed
     * registerVsyncHandler() ->
     * registerHotplugHandler()
     */
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        // Register vsync handler to DeviceInterface
        struct ExynosDisplayHandler temp;
        temp.handle = (ExynosVsyncHandler *)mDisplays[i];
        temp.mVsyncFd = mDisplays[i]->mDisplayInterface->getVsyncFd();
        mDeviceInterface->registerVsyncHandler(temp, mDisplays[i]->mDisplayId);

        // Register vsync handler to each DisplayInterface
        mDisplays[i]->mDisplayInterface->registerVsyncHandler(mDisplays[i]);

        // Register display fps changed callback
        mDisplays[i]->registerFpsChangedCallback(this);
    }
    mDeviceInterface->registerHotplugHandler(this);
    mDeviceInterface->registerPanelResetHandler(this);
}

void ExynosDevice::registerRestrictions() {
    struct dpp_restrictions_info_v2 *restrictions = NULL;
    int32_t ret = 0;
    uint32_t otfMPPSize = mResourceManager->getOtfMPPSize() + 1;
    ret = mDeviceInterface->getRestrictions(restrictions, otfMPPSize);
    if (ret == NO_ERROR) {
        mResourceManager->makeDPURestrictions(restrictions,
                                              mInterfaceType != INTERFACE_TYPE_DRM);
        /*
         * Update otfMPP feature table.
         * Feature table is not used to update otfMPP feature
         * in drm interface.
         * makeDPURestrictions() updates feature directly
         * in drm interface.
         */
        if (mInterfaceType != INTERFACE_TYPE_DRM)
            mResourceManager->updateFeatureTable(restrictions);
        mResourceManager->makeM2MRestrictions();
    } else {
        mResourceManager->updateRestrictions();
    }
    /*
     * This should be called after updateFeatureTable
     * because it uses feature_table updated by updateFeatureTable
     */
    mResourceManager->updateMPPFeature((ret != NO_ERROR) || (mInterfaceType != INTERFACE_TYPE_DRM));

    /* It's implmented in each module */
    mResourceManager->setVirtualOtfMPPsRestrictions();
    for (size_t i = 0; i < mDisplays.size(); i++)
        mResourceManager->checkAttrMPP(mDisplays[i]);
    /* Checking whether WCG is supported or not
     * should be called after updating attribute of MPPs
     */
    mResourceManager->updateSupportWCG();
    mCanProcessWCG = mResourceManager->deviceSupportWCG();
}

void ExynosDevice::dump(uint32_t *outSize, char *outBuffer) {
    Mutex::Autolock lock(mMutex);

    if (outSize == NULL) {
        ALOGE("%s:: outSize is null", __func__);
        return;
    }

    android::String8 result;
    result.append("\n\n");

    struct tm *localTime = (struct tm *)localtime((time_t *)&updateTimeInfo.lastUeventTime.tv_sec);
    result.appendFormat("lastUeventTime(%02d:%02d:%02d.%03lu) lastTimestamp(%" PRIu64 ")\n",
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, updateTimeInfo.lastUeventTime.tv_usec / 1000, mTimestamp);

    localTime = (struct tm *)localtime((time_t *)&updateTimeInfo.lastEnableVsyncTime.tv_sec);
    result.appendFormat("lastEnableVsyncTime(%02d:%02d:%02d.%03lu)\n",
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, updateTimeInfo.lastEnableVsyncTime.tv_usec / 1000);

    localTime = (struct tm *)localtime((time_t *)&updateTimeInfo.lastDisableVsyncTime.tv_sec);
    result.appendFormat("lastDisableVsyncTime(%02d:%02d:%02d.%03lu)\n",
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, updateTimeInfo.lastDisableVsyncTime.tv_usec / 1000);

    localTime = (struct tm *)localtime((time_t *)&updateTimeInfo.lastValidateTime.tv_sec);
    result.appendFormat("lastValidateTime(%02d:%02d:%02d.%03lu)\n",
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, updateTimeInfo.lastValidateTime.tv_usec / 1000);

    localTime = (struct tm *)localtime((time_t *)&updateTimeInfo.lastPresentTime.tv_sec);
    result.appendFormat("lastPresentTime(%02d:%02d:%02d.%03lu)\n",
                        localTime->tm_hour, localTime->tm_min,
                        localTime->tm_sec, updateTimeInfo.lastPresentTime.tv_usec / 1000);

    for (size_t i = 0; i < mDisplays.size(); i++) {
        ExynosDisplay *display = mDisplays[i];
        if (display->mPlugState == true)
            display->dump(result);
    }

    if (outBuffer == NULL) {
        *outSize = (uint32_t)result.length();
    } else {
        if (*outSize == 0) {
            ALOGE("%s:: outSize is 0", __func__);
            return;
        }
        size_t copySize = static_cast<size_t>(*outSize);
        if (copySize > result.size())
            copySize = result.size();
        ALOGI("HWC dump:: resultSize(%zu), outSize(%d), copySize(%zu)", result.size(), *outSize, copySize);
        strlcpy(outBuffer, result.c_str(), copySize);
    }

    return;
}

uint32_t ExynosDevice::getMaxVirtualDisplayCount() {
#ifdef USES_VIRTUAL_DISPLAY
    return 1;
#else
    return 0;
#endif
}

int32_t ExynosDevice::registerCallback(
    int32_t descriptor, hwc2_callback_data_t callbackData,
    hwc2_function_pointer_t point) {
    if (descriptor < 0 || descriptor > HWC2_CALLBACK_SEAMLESS_POSSIBLE)
        return HWC2_ERROR_BAD_PARAMETER;

    mCallbackInfos[descriptor].callbackData = callbackData;
    mCallbackInfos[descriptor].funcPointer = point;

    for (size_t i = 0; i < mDisplays.size(); i++) {
        mDisplays[i]->mCallbackInfos[descriptor].callbackData = callbackData;
        mDisplays[i]->mCallbackInfos[descriptor].funcPointer = point;
    }

    /* Call hotplug callback for primary display*/
    if (descriptor == HWC2_CALLBACK_HOTPLUG) {
        HWC2_PFN_HOTPLUG callbackFunc =
            (HWC2_PFN_HOTPLUG)mCallbackInfos[descriptor].funcPointer;
        if (callbackFunc != NULL) {
            callbackFunc(callbackData, getDisplayId(HWC_DISPLAY_PRIMARY, 0), HWC2_CONNECTION_CONNECTED);
            ExynosDisplay *exynosDisplay = getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 0));
            exynosDisplay->mHpdStatus = true;
#if defined(USES_DUAL_DISPLAY)
            callbackFunc(callbackData, getDisplayId(HWC_DISPLAY_PRIMARY, 1), HWC2_CONNECTION_CONNECTED);
            ExynosDisplay *exynosDisplay = getDisplay(getDisplayId(HWC_DISPLAY_PRIMARY, 1));
            exynosDisplay->mHpdStatus = true;
#endif
        }
    }

    return HWC2_ERROR_NONE;
}

void ExynosDevice::invalidate() {
    HWC2_PFN_REFRESH callbackFunc =
        (HWC2_PFN_REFRESH)mCallbackInfos[HWC2_CALLBACK_REFRESH].funcPointer;
    if (callbackFunc != NULL)
        callbackFunc(mCallbackInfos[HWC2_CALLBACK_REFRESH].callbackData,
                     getDisplayId(HWC_DISPLAY_PRIMARY, 0));
    else
        ALOGE("%s:: refresh callback is not registered", __func__);
}

void ExynosDevice::setHWCDebug(unsigned int debug) {
    Mutex::Autolock lock(mMutex);
    hwcDebug = debug;
}

uint32_t ExynosDevice::getHWCDebug() {
    Mutex::Autolock lock(mMutex);
    return hwcDebug;
}

void ExynosDevice::setHWCFenceDebug(uint32_t typeNum, uint32_t ipNum, uint32_t mode) {
    Mutex::Autolock lock(mMutex);

    if (typeNum > FENCE_TYPE_ALL || ipNum > FENCE_IP_ALL || mode > 1) {
        ALOGE("%s:: input is not valid type(%u), IP(%u), mode(%d)", __func__, typeNum, ipNum, mode);
        return;
    }

    uint32_t value = 0;

    if (typeNum == FENCE_TYPE_ALL)
        value = (1 << FENCE_TYPE_ALL) - 1;
    else
        value = 1 << typeNum;

    if (ipNum == FENCE_IP_ALL) {
        for (uint32_t i = 0; i < FENCE_IP_ALL; i++) {
            if (mode)
                hwcFenceDebug[i] |= value;
            else
                hwcFenceDebug[i] &= (~value);
        }
    } else {
        if (mode)
            hwcFenceDebug[ipNum] |= value;
        else
            hwcFenceDebug[ipNum] &= (~value);
    }
}

void ExynosDevice::getHWCFenceDebug() {
    Mutex::Autolock lock(mMutex);

    for (uint32_t i = 0; i < FENCE_IP_ALL; i++)
        ALOGE("[HWCFenceDebug] IP_Number(%d) : Debug(%x)", i, hwcFenceDebug[i]);
}

void ExynosDevice::setHWCControl(uint32_t display, uint32_t ctrl, int32_t val) {
    ExynosDisplay *exynosDisplay = NULL;

    /* ctrls that should be done without mutex lock */
    if (ctrl == HWC_CTL_CAPTURE_READBACK) {
        /*
         * captureScreenWithReadback() returns after present is called
         * present needs mutex lock so here we should not lock mutex
         */
        captureScreenWithReadback(HWC_DISPLAY_PRIMARY);
        return;
    }

    /* ctrls that should be done with mutex lock */
    Mutex::Autolock lock(mMutex);
    switch (ctrl) {
    case HWC_CTL_FORCE_GPU:
        ALOGI("%s::HWC_CTL_FORCE_GPU on/off=%d", __func__, val);
        exynosHWCControl.forceGpu = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_WINDOW_UPDATE:
        ALOGI("%s::HWC_CTL_WINDOW_UPDATE on/off=%d", __func__, val);
        exynosHWCControl.windowUpdate = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_FORCE_PANIC:
        ALOGI("%s::HWC_CTL_FORCE_PANIC on/off=%d", __func__, val);
        exynosHWCControl.forcePanic = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        break;
    case HWC_CTL_SKIP_STATIC:
        exynosDisplay = (ExynosDisplay *)getDisplay(display);
        if (exynosDisplay == nullptr)
            return;
        ALOGI("%s::%s HWC_CTL_SKIP_STATIC on/off=%d", __func__,
              exynosDisplay->mDisplayName.c_str(), val);
        exynosDisplay->mDisplayControl.skipStaticLayers =
            static_cast<bool>(val);
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        break;
    case HWC_CTL_SKIP_M2M_PROCESSING:
        exynosDisplay = (ExynosDisplay *)getDisplay(display);
        if (exynosDisplay == nullptr)
            return;
        ALOGI("%s::%s HWC_CTL_SKIP_M2M_PROCESSING on/off=%d", __func__,
              exynosDisplay->mDisplayName.c_str(), val);
        exynosDisplay->mDisplayControl.skipM2mProcessing = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        break;
    case HWC_CTL_SKIP_RESOURCE_ASSIGN:
        ALOGI("%s::HWC_CTL_SKIP_RESOURCE_ASSIGN on/off=%d", __func__, val);
        exynosHWCControl.skipResourceAssign = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_SKIP_VALIDATE:
        ALOGI("%s::HWC_CTL_SKIP_VALIDATE on/off=%d", __func__, val);
        exynosHWCControl.skipValidate = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_DUMP_MID_BUF:
        ALOGI("%s::HWC_CTL_DUMP_MID_BUF on/off=%d", __func__, val);
        exynosHWCControl.dumpMidBuf = (unsigned int)val;
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_DISPLAY_MODE:
        ALOGI("%s::HWC_CTL_DISPLAY_MODE mode=%d", __func__, val);
        setDisplayMode((uint32_t)val);
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_ENABLE_EXYNOSCOMPOSITION_OPT:
    case HWC_CTL_USE_MAX_G2D_SRC:
    case HWC_CTL_ENABLE_EARLY_START_MPP:
        exynosDisplay = (ExynosDisplay *)getDisplay(display);
        if (exynosDisplay == NULL) {
            for (uint32_t i = 0; i < mDisplays.size(); i++) {
                mDisplays[i]->setHWCControl(ctrl, val);
            }
        } else {
            exynosDisplay->setHWCControl(ctrl, val);
        }
        setGeometryChanged(GEOMETRY_DEVICE_CONFIG_CHANGED);
        invalidate();
        break;
    case HWC_CTL_DYNAMIC_RECOMP:
        ALOGI("%s::HWC_CTL_DYNAMIC_RECOMP on/off = %d", __func__, val);
        exynosDisplay = (ExynosDisplay *)getDisplay(display);
        if (exynosDisplay == nullptr) {
            ALOGI("%s::can not get ExynosDisplay", __func__);
            break;
        }
        switch (val) {
        case 0:
            exynosDisplay->mUseDynamicRecomp = false;
            if (exynosDisplay->mDynamicRecompTimer)
                exynosDisplay->mDynamicRecompTimer->stop();
            break;
        case 1:
            exynosDisplay->mUseDynamicRecomp = true;
            if (!exynosDisplay->mDynamicRecompTimer)
                exynosDisplay->initOneShotTimer();
            if (exynosDisplay->mDynamicRecompTimer)
                exynosDisplay->mDynamicRecompTimer->start();
            break;
        default:
            break;
        }
        if ((exynosDisplay->mPowerModeState == HWC2_POWER_MODE_ON) ||
            (exynosDisplay->mPowerModeState == HWC2_POWER_MODE_DOZE)) {
            setGeometryChanged(GEOMETRY_DISPLAY_DYNAMIC_RECOMPOSITION);
            exynosDisplay->invalidate();
        }
        break;

    case HWC_CTL_ADJUST_DYNAMIC_RECOMP_TIMER:
        ALOGI("%s::HWC_CTL_ADJUST_DYNAMIC_RECOMP_TIMER passed = %d", __func__, val);
        exynosDisplay = (ExynosDisplay *)getDisplay(display);
        if (exynosDisplay == nullptr) {
            ALOGI("%s::can not get ExynosDisplay", __func__);
            break;
        }
        if (val <= 0) {
            ALOGI("%s::invalid value(%d) is passed", __func__, val);
            break;
        } else {
            if ((exynosDisplay->mDynamicRecompTimer) && (exynosDisplay->mUseDynamicRecomp == true))
                exynosDisplay->mDynamicRecompTimer->setInterval(
                    std::chrono::milliseconds(val));
            else
                ALOGI("%s::[%s] DynamicRecomposition is not enabled yet", __func__, exynosDisplay->mDisplayName.c_str());
        }
        break;

    case HWC_CTL_ENABLE_FENCE_TRACER:
        ALOGI("%s::HWC_CTL_ENABLE_FENCE_TRACER on/off=%d", __func__, val);
        exynosHWCControl.fenceTracer = (unsigned int)val;
        break;
    case HWC_CTL_SYS_FENCE_LOGGING:
        ALOGI("%s::HWC_CTL_SYS_FENCE_LOGGING on/off=%d", __func__, val);
        exynosHWCControl.sysFenceLogging = (unsigned int)val;
        break;
    case HWC_CTL_DO_FENCE_FILE_DUMP:
        ALOGI("%s::HWC_CTL_DO_FENCE_FILE_DUMP on/off=%d", __func__, val);
        exynosHWCControl.doFenceFileDump = (unsigned int)val;
        break;
    case HWC_CTL_USE_PERF_FILE:
        ALOGI("%s::HWC_CTL_USE_PERF_FILE on/off=%d", __func__, val);
        exynosHWCControl.usePerfFile = (unsigned int)val;
        break;
    default:
        ALOGE("%s: unsupported HWC_CTL (%d)", __func__, ctrl);
        break;
    }
}

void ExynosDevice::enableMPP(uint32_t physicalType, uint32_t physicalIndex,
                             uint32_t logicalIndex, uint32_t enable) {
    Mutex::Autolock lock(mMutex);
    ExynosResourceManager::enableMPP(physicalType, physicalIndex, logicalIndex,
                                     enable);
}

void ExynosDevice::setDisplayMode(uint32_t displayMode) {
    exynosHWCControl.displayMode = displayMode;
}

bool ExynosDevice::checkDisplayEnabled(uint32_t displayId) {
    ExynosDisplay *display = getDisplay(displayId);

    if (!display)
        return false;
    else
        return display->isEnabled();
}

bool ExynosDevice::checkAdditionalConnection() {
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        switch (mDisplays[i]->mType) {
        case HWC_DISPLAY_PRIMARY:
            break;
        case HWC_DISPLAY_EXTERNAL:
        case HWC_DISPLAY_VIRTUAL:
            if (mDisplays[i]->mPlugState)
                return true;
            break;
        default:
            break;
        }
    }
    return false;
}

void ExynosDevice::getCapabilities(uint32_t *outCount, int32_t *outCapabilities) {
    uint32_t capabilityNum = 0;

    ALOGD("skipValidate enable: %d", enableSkipValidate);

    if (enableSkipValidate)
        capabilityNum++;

    if (outCapabilities == NULL) {
        *outCount = capabilityNum;
        return;
    }
    if (capabilityNum != *outCount) {
        ALOGE("%s:: invalid outCount(%d), should be(%d)", __func__, *outCount, capabilityNum);
        return;
    }

    uint32_t index = 0;

    if (enableSkipValidate)
        outCapabilities[index++] = HWC2_CAPABILITY_SKIP_VALIDATE;

    return;
}

void ExynosDevice::clearGeometryChanged() {
    mGeometryChanged = 0;
    for (auto display : mDisplays) {
        display->clearGeometryChanged();
    }
}

void ExynosDevice::setGeometryFlagForNextFrame() {
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        if ((mDisplays[i]->mType != HWC_DISPLAY_VIRTUAL) &&
            (mDisplays[i]->mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_OFF))
            continue;

        if (mDisplays[i]->mPlugState == false)
            continue;

        /* exynos9810 specific source code */
        if (mDisplays[i]->mType == HWC_DISPLAY_EXTERNAL) {
            ExynosExternalDisplay *extDisp = (ExynosExternalDisplay *)mDisplays[i];
            if (extDisp->mBlanked == true)
                continue;
        }
        /*
         * Resource assignment information was initialized during skipping frames
         * so resource assignment for the first displayed frame after skipping
         * frames should not be skipped
         *
         * mGeometryChanged might been cleared if this was last present.
         * Set geometry flag not to skip validate this display in next frame.
         */
        if ((mDisplays[i]->mIsSkipFrame) ||
            (mDisplays[i]->mNeedSkipPresent) ||
            (mDisplays[i]->mNeedSkipValidatePresent)) {
            setGeometryChanged(GEOMETRY_DISPLAY_FRAME_SKIPPED);
        }

        /*
         * Working vsync period can be changed after commit
         * Resource should be reassigned in this case
         */
        if (mDisplays[i]->mWorkingVsyncInfo.isChanged) {
            setGeometryChanged(GEOMETRY_DISPLAY_WORKING_VSYNC_CHANGED);
            mDisplays[i]->mWorkingVsyncInfo.clearChangedFlag();
        }
    }
}

bool ExynosDevice::canSkipValidate() {
    /*
     * This should be called by presentDisplay()
     * when presentDisplay() is called without validateDisplay() call
     */

    int ret = 0;
    if ((exynosHWCControl.skipValidate == false) ||
        (mGeometryChanged != 0)) {
        HDEBUGLOGD(eDebugSkipValidate,
                   "skipValidate(%d), mGeometryChanged(0x%" PRIx64 ")",
                   exynosHWCControl.skipValidate, mGeometryChanged);
        return false;
    }

    getDeviceValidateInfo(mDeviceValidateInfo);

    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        /*
         * Check all displays.
         * Resource assignment can have problem if validateDisplay is skipped
         * on only some displays.
         * All display's validateDisplay should be skipped or all display's validateDisplay
         * should not be skipped.
         */
        if (mDisplays[i]->mPlugState) {
            /*
             * presentDisplay is called without validateDisplay.
             * Call functions that should be called in validateDiplay
             */
            getDeviceValidateInfo(mDeviceValidateInfo);
            mDisplays[i]->doPreProcessing(mDeviceValidateInfo, mGeometryChanged);

            if ((ret = mDisplays[i]->canSkipValidate()) != NO_ERROR) {
                HDEBUGLOGD(eDebugSkipValidate, "Display[%d] can't skip validate (%d), renderingState(%d), geometryChanged(0x%" PRIx64 ")",
                           mDisplays[i]->mDisplayId, ret,
                           mDisplays[i]->mRenderingState, mGeometryChanged);
                return false;
            } else {
                HDEBUGLOGD(eDebugSkipValidate, "Display[%d] can skip validate (%d), renderingState(%d), geometryChanged(0x%" PRIx64 ")",
                           mDisplays[i]->mDisplayId, ret,
                           mDisplays[i]->mRenderingState, mGeometryChanged);
            }
        }
    }

    if (mGeometryChanged != 0) {
        HDEBUGLOGD(eDebugSkipValidate, "mGeometryChanged(0x%" PRIx64 ") is changed",
                   mGeometryChanged);
        /* validateDisplay() should be called */
        return false;
    }
    return true;
}

bool ExynosDevice::compareVsyncPeriod() {
    uint32_t prevVsyncPeriod = mVsyncPeriod;
    uint32_t mExtDisplayId = getDisplayId(HWC_DISPLAY_EXTERNAL, 0);

    if (mVsyncMode == DEFAULT_MODE) {
        if (checkDisplayEnabled(mExtDisplayId))
            mVsyncDisplayId = mExtDisplayId;
        else
            mVsyncDisplayId = getDisplayId(HWC_DISPLAY_PRIMARY, 0);
    } else {
        std::map<uint32_t, uint32_t> displayFps;
        displayFps.clear();

        for (size_t i = mDisplays.size(); i > 0; i--) {
            if (mDisplays[i - 1] == nullptr)
                continue;
            if (mDisplays[i - 1]->mType == HWC_DISPLAY_VIRTUAL)
                continue;
            if (mDisplays[i - 1]->mPlugState == false)
                continue;
            if ((mDisplays[i - 1]->mPowerModeState == HWC2_POWER_MODE_OFF) ||
                (mDisplays[i - 1]->mPowerModeState == HWC2_POWER_MODE_DOZE_SUSPEND))
                continue;
            else if (mDisplays[i - 1]->mPowerModeState == HWC2_POWER_MODE_DOZE)
                displayFps.insert(std::make_pair(DOZE_VSYNC_PERIOD / 100000, mDisplays[i - 1]->mDisplayId));
            else
                displayFps.insert(std::make_pair(mDisplays[i - 1]->mVsyncPeriod / 100000, mDisplays[i - 1]->mDisplayId));
        }

        if (displayFps.size() == 0) {
            mVsyncDisplayId = getDisplayId(HWC_DISPLAY_PRIMARY, 0);
            mVsyncPeriod = getDisplay(mVsyncDisplayId)->mVsyncPeriod;
        } else {
            if (mVsyncMode == HIGHEST_MODE) {
                std::map<uint32_t, uint32_t>::iterator k = displayFps.begin();
                mVsyncDisplayId = k->second;
                ALOGI("[%s] set the highest(mode:%d) vsync %d of display(%d)", __func__, mVsyncMode, k->first, k->second);
            } else if (mVsyncMode == LOWEST_MODE) {
                std::map<uint32_t, uint32_t>::iterator k = --displayFps.end();
                mVsyncDisplayId = k->second;
                ALOGI("[%s] set the lowest(mode:%d) vsync %d of display(%d)", __func__, mVsyncMode, k->first, k->second);
            }
        }
    }

    for (auto display : mDisplays) {
        if (display->getId() == mVsyncDisplayId) {
            display->mIsVsyncDisplay = true;
            mVsyncPeriod = display->mVsyncPeriod;
        } else {
            display->mIsVsyncDisplay = false;
        }
    }

    if (prevVsyncPeriod != mVsyncPeriod)
        return true;
    return false;
}

void ExynosDevice::setDumpCount() {
    for (size_t i = 0; i < mDisplays.size(); i++) {
        if (mDisplays[i]->mPlugState == true)
            mDisplays[i]->setDumpCount(mTotalDumpCount);
    }
}

void ExynosDevice::clearRenderingStateFlags() {
    for (size_t i = 0; i < mDisplays.size(); i++) {
        mDisplays[i]->mRenderingStateFlags.validateFlag = false;
        mDisplays[i]->mRenderingStateFlags.presentFlag = false;
    }
}

bool ExynosDevice::wasRenderingStateFlagsCleared() {
    for (size_t i = 0; i < mDisplays.size(); i++) {
        if (mDisplays[i]->mRenderingStateFlags.validateFlag ||
            mDisplays[i]->mRenderingStateFlags.presentFlag)
            return false;
    }
    return true;
}

int32_t ExynosDevice::validateDisplay(
    ExynosDisplay *display,
    uint32_t *outNumTypes, uint32_t *outNumRequests) {
    Mutex::Autolock lock(mMutex);

    gettimeofday(&updateTimeInfo.lastValidateTime, NULL);
    if (outNumTypes == nullptr || outNumRequests == nullptr)
        return HWC2_ERROR_BAD_PARAMETER;

    funcReturnCallback retCallback([&]() {
        display->mHWCRenderingState = RENDERING_STATE_VALIDATED;
    });

    int32_t ret = HWC2_ERROR_NONE;
    if ((display->mPlugState == false) || (display->isFrameSkipPowerState())) {
        display->mNeedSkipValidatePresent = true;
        ret = display->forceSkipValidateDisplay(outNumTypes, outNumRequests);
        return ret;
    }

    if (isFirstValidate(display)) {
        /*
         * Validate all of displays
         */
        ret = validateAllDisplays(display, outNumTypes, outNumRequests);
    } else {
        bool needRefresh = false;
        if (display->mRenderingStateFlags.validateFlag) {
            HDEBUGLOGD(eDebugResourceManager, "%s is already validated",
                       display->mDisplayName.c_str());
            /*
             * HWC doesn't skip this frame in validate, present time.
             * However this display was already validated in first validate time
             * so it doesn't call validateDisplay() here.
             */
            display->mNeedSkipValidatePresent = false;
        } else {
            HDEBUGLOGD(eDebugResourceManager, "%s is power on after first validate",
                       display->mDisplayName.c_str());
            /*
             * This display was not validated in first validate time.
             * It means that power is on after first validate time.
             * Skip validate and present in this frame.
             */
            display->mNeedSkipValidatePresent = true;
            needRefresh = true;
        }

        /*
         * mRenderingState could be changed to RENDERING_STATE_NONE
         * if presentDisplay() was called by presentOrValidate()
         * before this validate call
         * Restore mRenderingState to RENDERING_STATE_VALIDATED
         * because display was already validated
         */
        ret = display->setValidateState(*outNumTypes, *outNumRequests,
                                        mGeometryChanged);
        if (needRefresh)
            invalidate();
    }

    return ret;
}

int32_t ExynosDevice::getDeviceValidateInfo(DeviceValidateInfo &info) {
    /* Find otfMPP that can handle YUV image */
    ExynosMPP *mpp = nullptr;
    exynos_image img;
    img.exynosFormat = ExynosMPP::defaultMppDstUncompYuvFormat;
    auto otfMPPs = ExynosResourceManager::getOtfMPPs();
    auto mpp_it = std::find_if(otfMPPs.begin(), otfMPPs.end(),
                               [&img](auto m) { return m->isSrcFormatSupported(img); });
    mpp = mpp_it == otfMPPs.end() ? nullptr : *mpp_it;
    if (mpp) {
        info.srcSizeRestriction =
            mpp->getSrcSizeRestrictions(RESTRICTION_YUV);
        info.dstSizeRestriction =
            mpp->getDstSizeRestrictions(RESTRICTION_YUV);
    }
    info.useCameraException = mResourceManager->useCameraException();

    info.hasUnstartedDisplay = false;
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        info.hasUnstartedDisplay |= mDisplays[i]->checkDisplayUnstarted();
    }

    return NO_ERROR;
}

int32_t ExynosDevice::getDeviceResourceInfo(DeviceResourceInfo &info) {
    info.displayMode = mDisplayMode;
    return NO_ERROR;
}

int32_t ExynosDevice::getDevicePresentInfo(DevicePresentInfo &info) {
    info.vsyncMode = mVsyncMode;
    info.isBootFinished = mIsBootFinished;
    return NO_ERROR;
}

int32_t ExynosDevice::validateAllDisplays(ExynosDisplay *firstDisplay,
                                          uint32_t *outNumTypes, uint32_t *outNumRequests) {
    int32_t ret = HWC2_ERROR_NONE;

    HDEBUGLOGD(eDebugResourceManager, "This is first validate");

    if ((exynosHWCControl.displayMode < DISPLAY_MODE_NUM) &&
        (mDisplayMode != exynosHWCControl.displayMode)) {
        setGeometryChanged(GEOMETRY_DEVICE_DISP_MODE_CHAGED);
        mDisplayMode = exynosHWCControl.displayMode;
    }

    setVsyncMode();

    HDEBUGLOGD(eDebugResourceManager, "Validate all of displays ++++++++++++++++++++++++++++++++");

    auto skip_display = [=](ExynosDisplay *display) -> bool {
        /*
         * No skip validation if SurfaceFlinger calls validateDisplay
         * even though plug state is false.
         * HWC should return valid outNumTypes and outNumRequests if
         * SurfaceFlinger calls validateDisplay.
         */
        if (display == firstDisplay)
            return false;
        if ((display->mType != HWC_DISPLAY_VIRTUAL) &&
            (display->mPowerModeState == (hwc2_power_mode_t)HWC_POWER_MODE_OFF))
            return true;
        if (display->mPlugState == false)
            return true;
        /* exynos9810 specific source code */
        if (display->mType == HWC_DISPLAY_EXTERNAL) {
            ExynosExternalDisplay *extDisp = (ExynosExternalDisplay *)display;
            if (extDisp->mBlanked == true)
                return true;
        }
        if ((display->mNeedSkipPresent) &&
            (display->mRenderingStateFlags.presentFlag == true)) {
            /*
             * Present was already skipped after power is turned on.
             * Surfaceflinger will not call present() again in this frame.
             * So validateDisplays() should not validate this display
             * and should not start m2mMPP.
             */
            HDEBUGLOGD(eDebugResourceManager,
                       "%s:: validate is skipped in validateDisplays (mNeedSkipPresent is set)",
                       display->mDisplayName.c_str());
            return true;
        }

        return false;
    };

    getDeviceValidateInfo(mDeviceValidateInfo);
    mResourceManager->applyEnableMPPRequests();

    /* preprocessing display for validate */
    for (int32_t i = (mDisplays.size() - 1); i >= 0; i--) {
        if (skip_display(mDisplays[i]))
            continue;
        /* No skips validate and present */
        mDisplays[i]->mNeedSkipValidatePresent = false;
        mDisplays[i]->preProcessValidate(mDeviceValidateInfo, mGeometryChanged);
        if ((mDisplays[i]->mType == HWC_DISPLAY_VIRTUAL) &&
            !(mDisplays[i]->mUseDpu)) {
            ExynosVirtualDisplay *virtualDisplay = (ExynosVirtualDisplay *)mDisplays[i];
            if (virtualDisplay->mNeedReloadResourceForHWFC) {
                mResourceManager->reloadResourceForHWFC();
                mResourceManager->setTargetDisplayLuminance(
                    virtualDisplay->mMinTargetLuminance,
                    virtualDisplay->mMaxTargetLuminance);
                mResourceManager->setTargetDisplayDevice(
                    virtualDisplay->mSinkDeviceType);
                virtualDisplay->mNeedReloadResourceForHWFC = false;
            }
        }
    }

    if ((ret = mResourceManager->checkExceptionScenario(mGeometryChanged)) != NO_ERROR)
        HWC_LOGE_NODISP("checkExceptionScenario error ret(%d)", ret);

    if (exynosHWCControl.skipResourceAssign == 0) {
        /* Set any flag to mGeometryChanged */
        setGeometryChanged(GEOMETRY_DEVICE_SCENARIO_CHANGED);
    }
    if (!enableSkipValidate && checkAdditionalConnection()) {
        /* Set any flag to mGeometryChanged */
        setGeometryChanged(0x10);
    }

    HDEBUGLOGD(eDebugResourceManager | eDebugSkipResourceAssign,
               "%s:: mGeometryChanged(0x%" PRIx64 ")", __func__, mGeometryChanged);

    if (mGeometryChanged) {
        if ((ret = mResourceManager->prepareResources()) != NO_ERROR) {
            HWC_LOGE_NODISP("%s:: prepareResources() error (%d)",
                            __func__, ret);
            return HWC_HAL_ERROR_INVAL;
        }
    }

    for (int32_t i = (mDisplays.size() - 1); i >= 0; i--) {
        ExynosDisplay *display = mDisplays[i];
        if (skip_display(display))
            continue;
        int32_t displayRet = NO_ERROR;

        if (display->mLayers.size() == 0)
            ALOGI("%s:: %s validateDisplay layer size is 0",
                  __func__, display->mDisplayName.c_str());

        if (mGeometryChanged && !(display->mIsSkipFrame)) {
            if ((displayRet = mResourceManager->assignResource(display)) != NO_ERROR) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: assignResource() fail, error(%d)",
                         __func__, displayRet);
            } else {
                float assignedCapacity =
                    mResourceManager->getAssignedCapacity(MPP_G2D);
                if (assignedCapacity >
                    (mResourceManager->getM2MCapa(MPP_G2D) * MPP_CAPA_OVER_THRESHOLD)) {
                    HWC_LOGE(display->mDisplayInfo.displayIdentifier, "Assigned capacity for exynos composition "
                                                                      "is over restriction (%f)",
                             assignedCapacity);
                    displayRet = -1;
                }
            }
        }

        /*
         * HWC should update performanceInfo even if assignResource is skipped
         * HWC excludes the layer from performance calculation
         * if there is no buffer update. (using ExynosMPP::canSkipProcessing())
         * Therefore performanceInfo should be calculated again if only the buffer is updated.
         */
        if ((displayRet == NO_ERROR) &&
            ((displayRet = mResourceManager->deliverPerformanceInfo(display)) != NO_ERROR)) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: deliverPerformanceInfo() error (%d)",
                     __func__, displayRet);
        }

        if ((displayRet == NO_ERROR) &&
            ((displayRet = display->postProcessValidate() != NO_ERROR))) {
            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s:: postProcessValidate() error (%d)",
                     __func__, displayRet);
        }

        if (displayRet != NO_ERROR) {
            String8 errString;
            errString.appendFormat("%s:: validate fail for display[%s] firstValidate(%d), ret(%d)",
                                   __func__, display->mDisplayName.c_str(), display == firstDisplay ? 1 : 0, displayRet);
            display->printDebugInfos(errString);
            display->mDisplayInterface->setForcePanic();

            HWC_LOGE(display->mDisplayInfo.displayIdentifier, "%s", errString.c_str());
            display->setGeometryChanged(GEOMETRY_ERROR_CASE, mGeometryChanged);
            display->setForceClient();
            mResourceManager->resetAssignedResources(display, true);
            mResourceManager->assignCompositionTarget(display,
                                                      COMPOSITION_CLIENT);
            mResourceManager->assignWindow(display);
        }

        if (display == firstDisplay) {
            /* Update ret only if display is the first display */
            ret = display->setValidateState(*outNumTypes, *outNumRequests,
                                            mGeometryChanged);
        } else {
            uint32_t tmpOutNumTypes = 0;
            uint32_t tmpNumRequests = 0;
            display->setValidateState(tmpOutNumTypes, tmpNumRequests,
                                      mGeometryChanged);
        }

        if (mIsDumpRequest && isLastValidate(display)) {
            mIsDumpRequest = false;
            setDumpCount();
        }
    }
    HDEBUGLOGD(eDebugResourceManager, "Validate all of displays ----------------------------------");
    return ret;
}

int32_t ExynosDevice::presentDisplay(ExynosDisplay *display,
                                     int32_t *outPresentFence) {
    ATRACE_CALL();
    Mutex::Autolock lock(mMutex);

    if (display == NULL || display == nullptr) {
        ALOGE("%s: There is no display", __func__);
        return HWC2_ERROR_BAD_DISPLAY;
    }

    funcReturnCallback retCallback([&]() {
        display->mHWCRenderingState = RENDERING_STATE_PRESENTED;
    });

    String8 errString;
    /* It is called except for not validted and unplug case */
    auto presentPostProcessing = [&]() -> int32_t {
        if (display->mDpuData.enable_readback) {
            signalReadbackDone();
            display->disableReadback();
        }
        display->setPresentState();

        int32_t ret = HWC2_ERROR_NONE;
        if (!mFenceTracer.validateFences(
                display->mDisplayInfo.displayIdentifier)) {
            errString.appendFormat("%s:: validate fence failed", __func__);
            display->handlePresentError(errString, outPresentFence);
            setGeometryChanged(GEOMETRY_ERROR_CASE);
            ret = HWC_HAL_ERROR_INVAL;
        }

        if (isLastPresent(display)) {
            finishFrame();
        }
        return ret;
    };

    HDEBUGLOGD(eDebugResourceManager, "%s: renderingState(%d)",
               display->mDisplayName.c_str(), display->mRenderingState);
    int32_t ret = 0;
    if (display->mHWCRenderingState == RENDERING_STATE_VALIDATED) {
        ALOGI("%s:: acceptDisplayChanges was not called",
              display->mDisplayName.c_str());
        if (display->acceptDisplayChanges() != HWC2_ERROR_NONE) {
            ALOGE("%s:: acceptDisplayChanges is failed",
                  display->mDisplayName.c_str());
        }
    }

    if ((display->mPlugState == false) || (display->isFrameSkipPowerState())) {
        display->mNeedSkipValidatePresent = true;
        if (wasRenderingStateFlagsCleared()) {
            return display->forceSkipPresentDisplay(outPresentFence);
        }
    }

    if ((display->mNeedSkipPresent) || (display->mNeedSkipValidatePresent)) {
        ret = display->handleSkipPresent(outPresentFence);
        if (ret != HWC2_ERROR_NOT_VALIDATED) {
            /*
             * Condition that composer service would not call
             * validate and present again in this frame
             */
            presentPostProcessing();
        }
        /*
         * else means that composer service would call
         * validate and present
         */
        invalidate();
        return ret;
    }

    auto handleErr = [&]() -> int32_t {
        errString.appendFormat("device mGeometryChanged(%" PRIx64 ")\n",
                               mGeometryChanged);
        display->handlePresentError(errString, outPresentFence);
        presentPostProcessing();
        setGeometryChanged(GEOMETRY_ERROR_CASE);
        return HWC_HAL_ERROR_INVAL;
    };

    if (display->mRenderingState != RENDERING_STATE_ACCEPTED_CHANGE) {
        /*
         * presentDisplay() can be called before validateDisplay()
         * when HWC2_CAPABILITY_SKIP_VALIDATE is supported
         */
        if (!enableSkipValidate) {
            errString.appendFormat("%s:: Skip validate is not supported. "
                                   "Invalid rendering state : %d\n",
                                   __func__, display->mRenderingState);
            return handleErr();
        }

        if ((display->mRenderingState != RENDERING_STATE_NONE) &&
            (display->mRenderingState != RENDERING_STATE_PRESENTED) &&
            /* validated by first validate */
            (display->mRenderingState != RENDERING_STATE_VALIDATED)) {
            errString.appendFormat("%s:: %s display invalid rendering state : %d\n",
                                   __func__, display->mDisplayName.c_str(), display->mRenderingState);
            return handleErr();
        }
        if (canSkipValidate() == false) {
            HDEBUGLOGD(eDebugSkipValidate, "%s display need validate",
                       display->mDisplayName.c_str());
            display->mRenderingState = RENDERING_STATE_NONE;
            return HWC2_ERROR_NOT_VALIDATED;
        } else {
            HDEBUGLOGD(eDebugSkipValidate, "%s display validate is skipped",
                       display->mDisplayName.c_str());
        }
        /*
         * Check HDR10+ layers > HDR10+ IPs
         * If ture, HDR10+ layers are changed to HDR10 layers
         */
        if (display->mHasHdr10PlusLayer)
            mResourceManager->processHdr10plusToHdr10(display);

        /*
         * ExynosMPP needs acquire fence so acqurie fence should be set
         * before startPostProcessing().
         * If validate was not skipped, it was set in validate.
         */
        if (!display->mRenderingStateFlags.validateFlag)
            display->setSrcAcquireFences();

        /*
         * startPostProcessing() might be performed in first validate
         * if validateFlag is true
         */
        if ((display->mDisplayControl.earlyStartMPP == true) &&
            (display->mRenderingStateFlags.validateFlag == false)) {
            /*
             * HWC should update performanceInfo when validate is skipped
             * HWC excludes the layer from performance calculation
             * if there is no buffer update. (using ExynosMPP::canSkipProcessing())
             * Therefore performanceInfo should be calculated again if the buffer is updated.
             */
            if ((ret = mResourceManager->deliverPerformanceInfo(display)) != NO_ERROR) {
                HWC_LOGE(display->mDisplayInfo.displayIdentifier, "deliverPerformanceInfo() error (%d) "
                                                                  "in validateSkip case",
                         ret);
            }
            if ((ret = display->startPostProcessing()) != NO_ERROR) {
                errString.appendFormat("%s:: %s display startPostProcessing error : %d\n",
                                       __func__, display->mDisplayName.c_str(), ret);
                return handleErr();
            }
        }
    }
#ifdef USES_HWC_CPU_PERF_MODE
    if (isLastPresent(display))
        acquireCPUPerfPerCluster(round((double)1000000000 / (display->mVsyncPeriod)));
#endif

    display->clearWinConfigData();
#ifdef USE_DQE_INTERFACE
    if (display->needDqeSetting() &&
        (mDqeCoefInterface != nullptr) &&
        (display->mDquCoefAddr != nullptr)) {
        if (mDqeCoefInterface->getDqeCoef(display->mDquCoefAddr) == NO_ERROR)
            display->setDqeCoef(display->mDqeParcelFd);
    }
#endif
    getDevicePresentInfo(mDevicePresentInfo);
    ret = display->presentDisplay(mDevicePresentInfo, outPresentFence);
    if (ret != HWC2_ERROR_NONE) {
        errString.appendFormat("%s:: %s display present error : %d\n", __func__,
                               display->mDisplayName.c_str(), ret);
        return handleErr();
    }
    ret = presentPostProcessing();
    return ret;
}

int32_t ExynosDevice::finishFrame() {
    int32_t ret = 0;
    if ((ret = mResourceManager->finishAssignResourceWork()) != NO_ERROR)
        HWC_LOGE_NODISP("%s:: finishAssignResourceWork() error (%d)",
                        __func__, ret);

    /*
     * geomegryChanged flag should be cleared in last present
     * not in last validate
     * so that present can return HWC2_ERROR_NOT_VALIDATED
     * if geometry is changed in current frame
     * even if present is called after
     * all of display are validated in first validate time
     */
    clearGeometryChanged();

    setGeometryFlagForNextFrame();

    clearRenderingStateFlags();

    // Reset current frame flags for Fence Tracer
    mFenceTracer.resetFenceCurFlag();

    /*
     * Those function calls could set geomegryChanged flag
     * if MPP enable status is changed so it should be called after
     * clearGeometryChanged()
     */
    mResourceManager->applyEnableMPPRequests();
    uint64_t exceptionChanged = 0;
    if ((ret = mResourceManager->checkExceptionScenario(exceptionChanged)) != NO_ERROR)
        HWC_LOGE_NODISP("checkExceptionScenario error ret(%d)", ret);
    setGeometryChanged(exceptionChanged);

    if (exceptionChanged)
        invalidate();

    return ret;
}

int ExynosDevice::getFpsForCapacity(ExynosDisplay *display, hwc2_config_t config) {
    ExynosDisplay *vsyncDisplay = getDisplay(mVsyncDisplayId);
    if ((vsyncDisplay == nullptr) || (mVsyncPeriod == 0)) {
        HWC_LOGE_NODISP("mVsyncDisplayId(%d) or mVsyncPeriod(%d) is invalid",
                        mVsyncDisplayId, mVsyncPeriod);
        return 0;
    }
    return (int)((float)1000000000 / mVsyncPeriod + 0.5);
}

int ExynosDevice::getFpsForPerformance(ExynosDisplay *display, hwc2_config_t config) {
    int fps_sum = 0;

#ifdef USES_HWC_CPU_PERF_MODE
    uint32_t cfgId = 0;
    /* fps accumulation */
    for (uint32_t i = 0; i < mDisplays.size(); i++) {
        ExynosDisplay *d = mDisplays[i];
        if (d->isEnabled()) {
            if (d == display)
                cfgId = config;
            else
                cfgId = d->mActiveConfig;
            auto it = d->mDisplayConfigs.find(cfgId);
            if (it != d->mDisplayConfigs.end()) {
                uint32_t vsyncPeriod = it->second.vsyncPeriod;
                fps_sum += (int)(1000000000 / vsyncPeriod);
            }
        }
    }

    const auto its = perfTable.find(fps_sum);
    if (its == perfTable.end()) {
        /* TODO procssing if searching failed */
        fps_sum = kDefaultDispFps;  // default
    }

    if (fps_sum > MAX_SUPPORTED_FPS)
        fps_sum = MAX_SUPPORTED_FPS;

#endif

    return fps_sum;
}

void ExynosDevice::performanceAssuranceInternal() {
    int fpsForCapacity = getFpsForCapacity();
    auto it = perfTable.find(fpsForCapacity);
    if (it == perfTable.end()) {
        ALOGI("Use default fps instead of %d for setting capacity", fpsForCapacity);
        fpsForCapacity = kDefaultDispFps;
    }
    HDEBUGLOGD(eDebugCapacity, "Set m2mMPP capacity(%d) for fps(%d)",
               perfTable[fpsForCapacity].m2mCapa, fpsForCapacity);
    for (int m2mType = MPP_DPP_NUM; m2mType < MPP_P_TYPE_MAX; m2mType++)
        mResourceManager->setM2MCapa(m2mType, perfTable[fpsForCapacity].m2mCapa);

#ifdef USES_HWC_CPU_PERF_MODE
    /* Find out in module which is support or not */
    if (!supportPerformaceAssurance())
        return;

    int fps = getFpsForPerformance();
    if ((it = perfTable.find(fps)) == perfTable.end()) {
        ALOGI("Use default fps instead of %d for setting performance", fps);
        fps = kDefaultDispFps;
    }
    /* Affinity settings */
    cpu_set_t mask;
    CPU_ZERO(&mask);  // Clear mask
    ALOGI("Set Affinity config for fps(%d) : cpuIDs : %d", fps, perfTable[fps].cpuIDs);
    for (int cpu_no = 0; cpu_no < 32; cpu_no++) {
        if (perfTable[fps].cpuIDs & (1 << cpu_no)) {
            CPU_SET(cpu_no, &mask);
            ALOGI("Set Affinity CPU ID : %d", cpu_no);
        }
    }
    sched_setaffinity(getpid(), sizeof(cpu_set_t), &mask);

    ALOGI("Set affinity HWC : %d", getpid());

    /* TODO cluster modification in module */
    setCPUClocksPerCluster(fps);

    setGeometryChanged(GEOMETRY_MPP_CONFIG_CHANGED);
#endif

    return;
}

bool ExynosDevice::getCPUPerfInfo(int display, int config, int32_t *cpuIDs, int32_t *minClock) {
    Mutex::Autolock lock(mMutex);

    ExynosDisplay *_display = mDisplays[display];
    if (_display == NULL)
        return false;

    /* Find out in module which is support or not */
    if (!supportPerformaceAssurance() ||
        _display->mDisplayConfigs.empty() ||
        _display->mDisplayConfigs.size() <= 1)
        return false;

#ifdef USES_HWC_CPU_PERF_MODE
    int fps = getFpsForPerformance();
    *cpuIDs = perfTable[fps].cpuIDs;
#else
    *cpuIDs = 0xff;
#endif

    /* SurfaceFlinger not control CPU clock */
    *minClock = 0;
    ALOGI("CPU perf : Display(%d), Config(%d), Affinity(%d)", display, config, *cpuIDs);

    return true;
}

void ExynosDevice::getLayerGenericMetadataKey(uint32_t __unused keyIndex,
                                              uint32_t *outKeyLength, char *__unused outKey, bool *__unused outMandatory) {
    *outKeyLength = 0;
    return;
}

ExynosDevice::captureReadbackClass::captureReadbackClass() {
}

ExynosDevice::captureReadbackClass::~captureReadbackClass() {
    ExynosGraphicBufferMapper &gMapper(ExynosGraphicBufferMapper::get());
    if (mBuffer != nullptr)
        gMapper.freeBuffer(mBuffer);
}

int32_t ExynosDevice::captureReadbackClass::allocBuffer(
    uint32_t format, uint32_t w, uint32_t h) {
    ExynosGraphicBufferAllocator &gAllocator(ExynosGraphicBufferAllocator::get());

    uint32_t dstStride = 0;
    uint64_t usage = static_cast<uint64_t>(GRALLOC1_CONSUMER_USAGE_HWCOMPOSER |
                                           GRALLOC1_CONSUMER_USAGE_CPU_READ_OFTEN);

    status_t error = NO_ERROR;
    error = gAllocator.allocate(w, h, format, 1, usage, &mBuffer, &dstStride, "HWC");
    if ((error != NO_ERROR) || (mBuffer == nullptr)) {
        ALOGE("failed to allocate destination buffer(%dx%d): %d",
              w, h, error);
        return static_cast<int32_t>(error);
    }
    return NO_ERROR;
}

void ExynosDevice::captureReadbackClass::saveToFile(const String8 &fileName) {
    if (mBuffer == nullptr) {
        ALOGE("%s:: buffer is null", __func__);
        return;
    }

    char filePath[MAX_DEV_NAME] = {0};
    time_t curTime = time(NULL);
    struct tm *tm = localtime(&curTime);
    ExynosGraphicBufferMeta gmeta(mBuffer);

    snprintf(filePath, MAX_DEV_NAME,
             "%s/capture_format%d_%dx%d_%04d-%02d-%02d_%02d_%02d_%02d.raw",
             WRITEBACK_CAPTURE_PATH, gmeta.format, gmeta.stride, gmeta.vstride,
             tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    FILE *fp = fopen(filePath, "w");
    if (fp) {
        ExynosGraphicBufferMeta gmeta(mBuffer);
        uint32_t writeSize =
            gmeta.stride * gmeta.vstride * formatToBpp(gmeta.format) / 8;
        void *writebackData = mmap(0, writeSize,
                                   PROT_READ | PROT_WRITE, MAP_SHARED, gmeta.fd, 0);
        if (writebackData != MAP_FAILED && writebackData != NULL) {
            size_t result = fwrite(writebackData, writeSize, 1, fp);
            munmap(writebackData, writeSize);
            ALOGD("Success to write %zu data, size(%d)", result, writeSize);
        } else {
            ALOGE("Fail to mmap");
        }
        fclose(fp);
    } else {
        ALOGE("Fail to open %s", filePath);
    }
}

void ExynosDevice::signalReadbackDone() {
    if (mIsWaitingReadbackReqDone) {
        Mutex::Autolock lock(mCaptureMutex);
        mCaptureCondition.signal();
    }
}

void ExynosDevice::captureScreenWithReadback(uint32_t displayType) {
    ExynosDisplay *display = getDisplay(displayType);
    if (display == nullptr) {
        ALOGE("There is no display(%d)", displayType);
        return;
    }

    funcReturnCallback retCallback([&]() {
        clearWaitingReadbackReqDone();
    });

    int32_t outFormat;
    int32_t outDataspace;
    int32_t ret = 0;
    if ((ret = display->getReadbackBufferAttributes(
             &outFormat, &outDataspace)) != HWC2_ERROR_NONE) {
        ALOGE("getReadbackBufferAttributes fail, ret(%d)", ret);
        return;
    }

    captureReadbackClass captureClass;
    if ((ret = captureClass.allocBuffer(outFormat, display->mXres, display->mYres)) != NO_ERROR) {
        return;
    }

    mIsWaitingReadbackReqDone = true;

    if (display->setReadbackBuffer(captureClass.getBuffer(), -1) != HWC2_ERROR_NONE) {
        ALOGE("setReadbackBuffer fail");
        return;
    }

    /* Update screen */
    invalidate();

    /* Wait for handling readback */
    uint32_t waitPeriod = display->mVsyncPeriod * 3;
    {
        Mutex::Autolock lock(mCaptureMutex);
        status_t err = mCaptureCondition.waitRelative(
            mCaptureMutex, us2ns(waitPeriod));
        if (err == TIMED_OUT) {
            ALOGE("timeout, readback is not requested");
            return;
        } else if (err != NO_ERROR) {
            ALOGE("error waiting for readback request: %s (%d)", strerror(-err), err);
            return;
        } else {
            ALOGD("readback request is done");
        }
    }

    int32_t fence = -1;
    if (display->getReadbackBufferFence(&fence) != HWC2_ERROR_NONE) {
        ALOGE("getReadbackBufferFence fail");
        return;
    }
    if (sync_wait(fence, 1000) < 0) {
        ALOGE("sync wait error, fence(%d)", fence);
    }
    hwcFdClose(fence);

    String8 fileName;
    time_t curTime = time(NULL);
    struct tm *tm = localtime(&curTime);
    fileName.appendFormat("capture_format%d_%dx%d_%04d-%02d-%02d_%02d_%02d_%02d.raw",
                          outFormat, display->mXres, display->mYres,
                          tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                          tm->tm_hour, tm->tm_min, tm->tm_sec);
    captureClass.saveToFile(fileName);
}

void ExynosDevice::setVsyncMode() {
    int mode = DEFAULT_MODE;
    char value[256];

    /* value is matched to LOWEST_MODE, HIGHEST_MODE or etc. */
    property_get("vendor.hwc.exynos.vsync_mode", value, "0");
    mode = atoi(value);
    if (mVsyncMode != mode) {
        mVsyncMode = mode;
        handleVsyncPeriodChange();
    }
}

int32_t ExynosDevice::setLayerBlendMode(ExynosLayer *layer,
                                        int32_t /*hwc2_blend_mode_t*/ mode) {
    Mutex::Autolock lock(mMutex);
    int32_t ret = layer->setLayerBlendMode(mode, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerBuffer(ExynosDisplay *display,
                                     hwc2_layer_t layer, buffer_handle_t buffer, int32_t acquireFence) {
    Mutex::Autolock lock(mMutex);

    if (display->mPlugState == false)
        buffer = NULL;
    display->requestHiberExit();

    ExynosLayer *exynosLayer = display->checkLayer(layer);
    if (exynosLayer == nullptr)
        return HWC2_ERROR_BAD_LAYER;

    int32_t ret = exynosLayer->setLayerBuffer(buffer, acquireFence,
                                              mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerCompositionType(ExynosLayer *layer,
                                              int32_t /*hwc2_composition_t*/ type) {
    Mutex::Autolock lock(mMutex);
    int32_t ret = layer->setLayerCompositionType(type, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerDataspace(ExynosDisplay *display,
                                        hwc2_layer_t layer, int32_t /*android_dataspace_t*/ dataspace) {
    Mutex::Autolock lock(mMutex);

    ExynosLayer *exynosLayer = display->checkLayer(layer);
    if (exynosLayer == nullptr)
        return HWC2_ERROR_BAD_LAYER;

    int32_t ret = exynosLayer->setLayerDataspace(dataspace, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerDisplayFrame(ExynosLayer *layer, hwc_rect_t frame) {
    Mutex::Autolock lock(mMutex);
    clearRenderingStateFlags();
    int32_t ret = layer->setLayerDisplayFrame(frame, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerSourceCrop(ExynosLayer *layer, hwc_frect_t crop) {
    Mutex::Autolock lock(mMutex);
    int32_t ret = layer->setLayerSourceCrop(crop, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerTransform(ExynosLayer *layer, int32_t /*hwc_transform_t*/ transform) {
    Mutex::Autolock lock(mMutex);
    int32_t ret = layer->setLayerTransform(transform, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setLayerZOrder(ExynosLayer *layer, uint32_t z) {
    Mutex::Autolock lock(mMutex);
    int32_t ret = layer->setLayerZOrder(z, mGeometryChanged);
    return ret;
}

int32_t ExynosDevice::setColorMode(ExynosDisplay *display, int32_t mode) {
    Mutex::Autolock lock(mMutex);
    return display->setColorMode(mode, mCanProcessWCG, mGeometryChanged);
}

int32_t ExynosDevice::setColorModeWithRenderIntent(ExynosDisplay *display, int32_t mode, int32_t intent) {
    Mutex::Autolock lock(mMutex);
    return display->setColorModeWithRenderIntent(mode, intent, mCanProcessWCG,
                                                 mGeometryChanged);
}

int32_t ExynosDevice::getColorModes(ExynosDisplay *display, uint32_t *outNumModes, int32_t *outModes) {
    return display->getColorModes(outNumModes, outModes, mCanProcessWCG);
}

int32_t ExynosDevice::setPowerMode(ExynosDisplay *display, int32_t /*hwc2_power_mode_t*/ mode) {
    Mutex::Autolock lock(mMutex);

    if (!display)
        return HWC2_ERROR_BAD_DISPLAY;
    if (mode == HWC_POWER_MODE_OFF) {
        /*
         * present will be skipped when display is power off
         * set present flag and clear flags hear
         */
        display->mRenderingStateFlags.presentFlag = true;
        if (isLastPresent(display))
            finishFrame();
    }

    int32_t ret = display->setPowerMode(mode, mGeometryChanged);
    handleVsyncPeriodChangeInternal();
    return ret;
}

int32_t ExynosDevice::createLayer(ExynosDisplay *display, hwc2_layer_t *outLayer) {
    Mutex::Autolock lock(mMutex);
    return display->createLayer(outLayer, mGeometryChanged);
}

int32_t ExynosDevice::destroyLayer(ExynosDisplay *display, hwc2_layer_t layer) {
    Mutex::Autolock lock(mMutex);
    if (display->checkLayer(layer) == nullptr)
        return HWC2_ERROR_BAD_LAYER;

    return display->destroyLayer(layer, mGeometryChanged);
}

int32_t ExynosDevice::setColorTransform(ExynosDisplay *display,
                                        const float *matrix, int32_t /*android_color_transform_t*/ hint) {
    Mutex::Autolock lock(mMutex);
    return display->setColorTransform(matrix, hint, mGeometryChanged);
}

int32_t ExynosDevice::setClientTarget(ExynosDisplay *display,
                                      buffer_handle_t target, int32_t acquireFence, int32_t dataspace) {
    Mutex::Autolock lock(mMutex);
    return display->setClientTarget(target, acquireFence, dataspace,
                                    mGeometryChanged);
}

int32_t ExynosDevice::setActiveConfig(ExynosDisplay *display, hwc2_config_t config) {
    Mutex::Autolock lock(mMutex);
    return display->setActiveConfig(config);
}

int32_t ExynosDevice::setActiveConfigWithConstraints(ExynosDisplay *display,
                                                     hwc2_config_t config,
                                                     hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
                                                     hwc_vsync_period_change_timeline_t *outTimeline) {
    Mutex::Autolock lock(mMutex);
    return display->setActiveConfigWithConstraints(config, vsyncPeriodChangeConstraints,
                                                   outTimeline);
}

int32_t ExynosDevice::printMppsAttr() {
    int32_t res = mResourceManager->printMppsAttr();
    return res;
}

void ExynosDevice::resetForDestroyClient() {
    for (auto display : mDisplays) {
        display->resetForDestroyClient();
    }
}
