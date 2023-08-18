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

#ifndef _EXYNOSDISPLAY_H
#define _EXYNOSDISPLAY_H

#include <fstream>

#include <utils/Vector.h>
#include <utils/KeyedVector.h>
#include <system/graphics.h>

#include "ExynosHWC.h"
#include <hardware/hwcomposer2.h>
#include "ExynosHWCTypes.h"
#include "ExynosHWCHelper.h"
#include "ExynosMPP.h"
#include "ExynosDisplayInterface.h"
#include "ExynosHWCDebug.h"
#include "OneShotTimer.h"

//#include <hardware/exynos/hdrInterface.h>
//#include <hardware/exynos/hdr10pMetaInterface.h>

#ifdef USE_DISPLAY_COLOR_INTERFACE
#include <hardware/exynos/libdisplaycolor.h>
#endif

#define HWC_PRINT_FRAME_NUM 10

#ifndef SECOND_DISPLAY_START_BIT
#define SECOND_DISPLAY_START_BIT 4
#endif

#ifndef DYNAMIC_RECOMP_TIMER_MS
#define DYNAMIC_RECOMP_TIMER_MS 500
#endif

#define LAYER_DUMP_FRAME_CNT_MAX 30
#define LAYER_DUMP_LAYER_CNT_MAX 30
#define ATRACE_FD(fd, w, h)                                                \
    do {                                                                   \
        if (ATRACE_ENABLED()) {                                            \
            char ___traceBuf[1024];                                        \
            snprintf(___traceBuf, 1024, "fd: %d (%dx%d)", (fd), (w), (h)); \
            android::ScopedTrace ___bufTracer(ATRACE_TAG, ___traceBuf);    \
        }                                                                  \
    } while (false)

typedef hwc2_composition_t exynos_composition;

class ExynosLayer;
class ExynosMPP;
class ExynosMPPSource;
class LayerDumpManager;
class LayerDumpThread;

enum rendering_state {
    RENDERING_STATE_NONE = 0,
    RENDERING_STATE_VALIDATED,
    RENDERING_STATE_ACCEPTED_CHANGE,
    RENDERING_STATE_PRESENTED,
    RENDERING_STATE_MAX
};

enum composition_type {
    COMPOSITION_NONE = 0,
    COMPOSITION_CLIENT,
    COMPOSITION_EXYNOS,
    COMPOSITION_MAX
};

enum {
    eDisplayNone = 0x0,
    ePrimaryDisplay = 0x00000001,
    eExternalDisplay = 0x00000002,
    eVirtualDisplay = 0x00000004,
};

enum class hwc_request_state_t {
    SET_CONFIG_STATE_NONE = 0,
    SET_CONFIG_STATE_PENDING,
    SET_CONFIG_STATE_REQUESTED,
};

enum {
    LAYER_DUMP_IDLE,
    LAYER_DUMP_READY,
    LAYER_DUMP_DONE
};

#define NUM_SKIP_STATIC_LAYER 5
struct ExynosFrameInfo {
    uint32_t srcNum;
    exynos_image srcInfo[NUM_SKIP_STATIC_LAYER];
    exynos_image dstInfo[NUM_SKIP_STATIC_LAYER];

    void reset() {
        srcNum = 0;
        memset(srcInfo, 0x0, NUM_SKIP_STATIC_LAYER * sizeof(exynos_image));
        memset(dstInfo, 0x0, NUM_SKIP_STATIC_LAYER * sizeof(exynos_image));
        for (int i = 0; i < NUM_SKIP_STATIC_LAYER; i++) {
            srcInfo[i].acquireFenceFd = -1;
            srcInfo[i].releaseFenceFd = -1;
            dstInfo[i].acquireFenceFd = -1;
            dstInfo[i].releaseFenceFd = -1;
        }
    }
};

struct redering_state_flags_info {
    bool validateFlag = false;
    bool presentFlag = false;
};

struct layerDumpLayerInfo {
    void *planeRawData[4] = {
        nullptr,
    };
    size_t bufferLength[4] = {
        0,
    };
    int32_t /*android_pixel_format_t*/ format = 0;
    int32_t compositionType;
    uint32_t bufferNum = 0;
    uint32_t layerType = 0;
    uint32_t compressionType = COMP_TYPE_NONE;
    uint32_t stride = 0;
    uint32_t vStride = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct layerDumpFrameInfo {
    int32_t layerDumpState = LAYER_DUMP_IDLE;
    int32_t layerDumpCnt = 0;
    layerDumpLayerInfo layerInfo[LAYER_DUMP_LAYER_CNT_MAX];
};

class ExynosSortedLayer : public Vector<ExynosLayer *> {
  public:
    ssize_t remove(const ExynosLayer *item);
    status_t vector_sort();
    static int compare(ExynosLayer *const *lhs, ExynosLayer *const *rhs);
};

class displayTDMInfo {
  public:
    /* Could be extended */
    typedef struct resourceAmount {
        uint32_t totalAmount;
    } resourceAmount_t;
    std::map<tdm_attr_t, resourceAmount_t> mAmount;

    uint32_t initTDMInfo(resourceAmount_t amount, tdm_attr_t attr) {
        mAmount[attr] = amount;
        return 0;
    };

    resourceAmount_t getAvailableAmount(tdm_attr_t attr) {
        return mAmount[attr];
    };
};

class ExynosCompositionInfo : public ExynosMPPSource {
  public:
    struct BufInfo {
        BufInfo(){};
        BufInfo(int w, int h, uint32_t f, uint64_t p_usage, uint64_t c_usage)
            : width(w), height(h), format(f),
              producer_usage(p_usage), consumer_usage(c_usage){};
        int width = 0;
        int height = 0;
        uint32_t format = 0;
        uint64_t producer_usage = 0;
        uint64_t consumer_usage = 0;
        inline bool isValid() { return (width > 0) && (height > 0); };
        inline bool operator==(const BufInfo &other) const {
            return (width == other.width) && (height == other.height) &&
                   (format == other.format) &&
                   (producer_usage == other.producer_usage) &&
                   (consumer_usage == other.consumer_usage);
        };
        inline bool operator!=(const BufInfo &other) const { return !operator==(other); };
    };
    ExynosCompositionInfo() : ExynosCompositionInfo(COMPOSITION_NONE){};
    ExynosCompositionInfo(uint32_t type);
    uint32_t mType;
    bool mHasCompositionLayer;
    int32_t mFirstIndex;
    int32_t mLastIndex;
    buffer_handle_t mTargetBuffer;
    android_dataspace mDataSpace;
    int32_t mAcquireFence;
    int32_t mReleaseFence;
    bool mEnableSkipStatic;
    bool mSkipStaticInitFlag;
    bool mSkipFlag;
    ExynosFrameInfo mSkipSrcInfo;
    exynos_win_config_data mLastWinConfigData;

    int32_t mWindowIndex;
    compressionInfo_t mCompressionInfo;

    ExynosMPP *mBlendingMPP = nullptr;
    DisplayIdentifier mDisplayIdentifier;

    ExynosFormat mFormat;
    void init(DisplayIdentifier display, ExynosMPP *blendingMPP) {
        mDisplayIdentifier = display;
        mBlendingMPP = blendingMPP;
    };
    void initializeInfos();
    /**
      * Set the target buffer information
      * @param handle the buffer handle
      * @param acquireFence the acquire fence
      * @param dataspace the dataspace of the buffer data
      * @return whether the target buffer has been reallocated
      */
    bool setTargetBuffer(buffer_handle_t handle,
                         int32_t acquireFence, android_dataspace dataspace);
    void setCompressionType(uint32_t compressionType);
    void dump(String8 &result);
    String8 getTypeStr();

  private:
    ExynosFenceTracer &mFenceTracer = ExynosFenceTracer::getInstance();
    BufInfo mBufInfo;
};

struct DisplayControl {
    /** Resource assignment optimization for exynos composition **/
    bool enableExynosCompositionOptimization;
    /** Resource assignment optimization for client composition **/
    bool enableClientCompositionOptimization;
    /** Use G2D as much as possible **/
    bool useMaxG2DSrc;
    /** start m2mMPP before persentDisplay **/
    bool earlyStartMPP;
    /** Adjust display size of the layer having high priority */
    bool adjustDisplayFrame;
    /** setCursorPosition support **/
    bool cursorSupport;
    /** readback support **/
    bool readbackSupport = false;
    bool skipStaticLayers = true;
    bool skipM2mProcessing = true;
    /** Enable multi-thread present **/
    bool multiThreadedPresent = false;
};

typedef struct hiberState {
    FILE *hiberExitFd = NULL;
    bool exitRequested = false;
} hiberState_t;

struct workingVsyncInfo {
    uint32_t vsyncPeriod = 0;
    bool isChanged = false;
    void setVsyncPeriod(uint32_t vsync) {
        if (vsyncPeriod != vsync) {
            isChanged = true;
            vsyncPeriod = vsync;
        }
    };
    void clearChangedFlag() {
        isChanged = false;
    }
};

class ExynosVsyncCallback {
  public:
    void enableVSync(bool enable) {
        mVsyncEnabled = enable;
        resetVsyncTimeStamp();
    };
    bool getVSyncEnabled() { return mVsyncEnabled; };
    void setDesiredVsyncPeriod(uint64_t period) {
        mDesiredVsyncPeriod = period;
        resetVsyncTimeStamp();
    };
    uint64_t getDesiredVsyncPeriod() { return mDesiredVsyncPeriod; };
    uint64_t getVsyncTimeStamp() { return mVsyncTimeStamp; };
    uint64_t getVsyncPeriod() { return mVsyncPeriod; };
    bool Callback(uint64_t timestamp);
    void resetVsyncTimeStamp() { mVsyncTimeStamp = 0; };
    void resetDesiredVsyncPeriod() { mDesiredVsyncPeriod = 0; };

  private:
    bool mVsyncEnabled = false;
    uint64_t mVsyncTimeStamp = 0;
    uint64_t mVsyncPeriod = 0;
    uint64_t mDesiredVsyncPeriod = 0;
};

struct PendingConfigInfo {
    enum configType {
        NO_CONSTRAINTS,
        WITH_CONSTRAINTS
    };
    bool isPending = false;
    hwc2_config_t config = UINT_MAX;
    configType configType = NO_CONSTRAINTS;
    hwc_vsync_period_change_constraints_t constraints;
    hwc_vsync_period_change_timeline_t vsyncAppliedTimeLine;
    void setPendConfig(hwc2_config_t c) {
        isPending = true;
        config = c;
        configType = NO_CONSTRAINTS;
    };
    void setPendConfigWithConstraints(hwc2_config_t c,
                                      hwc_vsync_period_change_constraints_t con,
                                      hwc_vsync_period_change_timeline_t timeline) {
        setPendConfig(c);
        configType = WITH_CONSTRAINTS;
        constraints = con;
        vsyncAppliedTimeLine = timeline;
    };
    void dump(String8 &result) {
        result.appendFormat("PendingConfigInfo::isPending(%d), configType(%d), config(%d)",
                            isPending, configType, config);
    };
};

class ExynosDisplay : public ExynosVsyncHandler {
  public:
    uint32_t mDisplayId;
    uint32_t mType;
    uint32_t mIndex;
    String8 mDeconNodeName;
    uint32_t mXres;
    uint32_t mYres;
    uint32_t mXdpi;
    uint32_t mYdpi;
    uint32_t mVsyncPeriod;
    workingVsyncInfo mWorkingVsyncInfo;
    int32_t mVsyncFd;

    int mPsrMode;
    DisplayInfo mDisplayInfo;

    /* Constructor */
    ExynosDisplay(DisplayIdentifier node);
    /* Destructor */
    virtual ~ExynosDisplay();

    String8 mDisplayName;

    /** State variables */
    bool mPlugState;
    hwc2_power_mode_t mPowerModeState;
    hwc2_vsync_t mVsyncState;
    bool mHasSingleBuffer;

    bool mDisplayConfigPending = false;

    DisplayControl mDisplayControl;

    /**
         * TODO : Should be defined as ExynosLayer type
         * Layer list those sorted by z-order
         */
    ExynosSortedLayer mLayers;

    /**
         * Layer index, target buffer information for GLES.
         */
    ExynosCompositionInfo mClientCompositionInfo;

    /**
         * Layer index, target buffer information for G2D.
         */
    ExynosCompositionInfo mExynosCompositionInfo;

    /**
         * Geometry change info is described by bit map.
         * This flag is cleared when resource assignment for all displays
         * is done.
         */
    uint64_t mGeometryChanged;

    /**
         * Rendering step information that is seperated by
         * VALIDATED, ACCEPTED_CHANGE, PRESENTED.
         */
    rendering_state mRenderingState;

    /**
         * Rendering step information that is called by client
         */
    rendering_state mHWCRenderingState;

    /*
         * Flag that each rendering step is performed
         * This flags are set in each rendering step.
         * this flags are cleared by setLayer...() APIs.
         */
    redering_state_flags_info mRenderingStateFlags;

    uint64_t mLastModeSwitchTimeStamp;
    uint64_t mLastUpdateTimeStamp;
    uint32_t mDumpCount = 0;
    uint32_t mDumpInitCount = 0;

    /* default DMA for the display */
    decon_idma_type mDefaultDMA;

    /**
         * DECON WIN_CONFIG information.
         */
    exynos_dpu_data mDpuData;

    /**
         * Last win_config data is used as WIN_CONFIG skip decision or debugging.
         */
    exynos_dpu_data mLastDpuData;

    /**
         * Restore release fence from DECON.
         */

    /* Present fence for last(N-1) frame */
    int mLastPresentFence;
    /* Present fence for N-2 frame */
    int mN2PresentFence = -1;

    bool mUseDpu;

    /**
         * Max Window number, It should be set by display module(chip)
         */
    uint32_t mMaxWindowNum;
    uint32_t mWindowNumUsed;
    uint32_t mBaseWindowIndex;

    // Priority
    uint32_t mNumMaxPriorityAllowed;
    int32_t mCursorIndex;

    int32_t mColorTransformHint;

    // HDR capabilities
    std::vector<int32_t> mHdrTypes;
    float mMaxLuminance;
    float mMaxAverageLuminance;
    float mMinLuminance;
    bool mHasHdr10PlusLayer;

    std::map<uint32_t, displayConfigs_t> mDisplayConfigs;
    android::SortedVector<uint32_t> mAssignedWindows;

    // WCG
    android_color_mode_t mColorMode;

    // Skip present frame if there was no validate after power on
    bool mNeedSkipPresent;
    // Skip validate, present frame
    bool mNeedSkipValidatePresent;

    /*
         * flag whether the frame is skipped
         * by specific display (ExynosVirtualDisplay, ExynosExternalDisplay...)
         */
    bool mIsSkipFrame;

    hiberState_t mHiberState;
    std::ofstream mBrightnessOfs;
    uint32_t mMaxBrightness;

    hwc_vsync_period_change_constraints_t mVsyncPeriodChangeConstraints;
    hwc_vsync_period_change_timeline_t mVsyncAppliedTimeLine;
    hwc_request_state_t mConfigRequestState;
    hwc2_config_t mDesiredConfig;

    uint32_t mActiveConfig = 0;
    uint32_t mConfigChangeTimoutCnt = 0;
    exynos_callback_info_t mCallbackInfos[HWC2_CALLBACK_SEAMLESS_POSSIBLE + 1];
    ExynosFpsChangedCallback *mFpsChangedCallback = nullptr;
    bool mIsVsyncDisplay = false;

    hdrInterface *mHdrCoefInterface = NULL;
    hdr10pMetaInterface *mHdr10PMetaInterface = NULL;
    struct HdrTargetInfo mHdrTargetInfo = {0, 0, 0, HDR_BPC_8, HDR_CAPA_INNER};
#ifndef HDR_IF_VER
    struct HdrLayerInfo {
        int dataspace;
        void *static_metadata;
        int static_len;
        void *dynamic_metadata;
        int dynamic_len;
        bool premult_alpha;
        enum HdrBpc bpc;
        enum RenderSource source;
        float *tf_matrix;
        bool bypass;
    };
#endif
    uint32_t getHdrLayerInfo(exynos_image img, enum RenderSource renderSource = REND_ORI, HdrLayerInfo *outHdrLayerInfo = nullptr);
    bool needHdrProcessing(exynos_image &srcImg, exynos_image &dstImg);
    bool mHasHdr10AttrMPP = false;
    bool mHasHdr10PlusAttrMPP = false;
    bool isSupportLibHdrApi();

#ifdef USE_DQE_INTERFACE
    int mDqeParcelFd = -1;
    struct dqeCoef *mDquCoefAddr = NULL;
#endif

#ifdef USE_DISPLAY_COLOR_INTERFACE
    IDisplayColor *mDisplayColorInterface = NULL;
    int mDisplayColorFd = -1;
    int mDisplayColorCoefSize = -1;
    void *mDisplayColorCoefAddr = NULL;
    std::unordered_map<uint32_t, DisplayColorMode> mDisplayColorModes;
    std::unordered_map<uint32_t, std::vector<DisplayRenderIntent>> mDisplayRenderIntents;
    DisplayColorMode mCurrentDisplayColorMode;
    DisplayRenderIntent mCurrentDisplayRenderIntents;
    virtual int32_t searchDisplayColorMode(int32_t mode);
    virtual int32_t searchDisplayRenderIntent(int32_t mode, int32_t intent);
#endif

    bool mHpdStatus;

    bool mUseDynamicRecomp = false;
    dynamic_recomp_mode mDynamicRecompMode = NO_MODE_SWITCH;
    std::optional<OneShotTimer> mDynamicRecompTimer;
    Mutex mDRMutex;

    void initOneShotTimer() {
        mDynamicRecompTimer.emplace(
            std::chrono::milliseconds(DYNAMIC_RECOMP_TIMER_MS), [] {},
            [this] { checkLayersForSettingDR(); });
    };
    virtual void checkLayersForSettingDR(){};
    virtual void checkLayersForRevertingDR(uint64_t &geometryChanged);

    virtual void hotplug();
    virtual bool checkHotplugEventUpdated(bool &hpdStatus);
    virtual void handleHotplugEvent(bool hpdStatus);
    void initDisplay();
    void initCompositionInfo(ExynosCompositionInfo &compositionInfo);
    virtual void init(uint32_t maxWindowNum, ExynosMPP *blendingMPP);

    int getId();

    size_t yuvWriteByLines(void *temp, int align_Width, int original_Width, int original_Height, FILE *fp);
    int32_t setCompositionTargetExynosImage(uint32_t targetType, exynos_image *src_img, exynos_image *dst_img);
    int32_t initializeValidateInfos();
    int32_t addClientCompositionLayer(uint32_t layerIndex,
                                      uint32_t *isExynosCompositionChanged = NULL);
    int32_t removeClientCompositionLayer(uint32_t layerIndex);
    bool hasClientComposition();
    int32_t handleSandwitchedExynosCompositionLayer(
        std::vector<int32_t> &highPriLayers, float totalUsedCapa,
        bool &invalidFlag, int32_t &changeFlag);
    int32_t handleNestedClientCompositionLayer(int32_t &changeFlag);
    int32_t addExynosCompositionLayer(uint32_t layerIndex, float totalUsedCapa);

    /**
         * @param *outLayer
         */
    int32_t destroyLayer(hwc2_layer_t outLayer,
                         uint64_t &geometryFlag);

    void destroyLayers();

    /**
         * @param index
         */
    ExynosLayer *checkLayer(hwc2_layer_t addr, bool printError = true);

    virtual void doPreProcessing(DeviceValidateInfo &validateInfo,
                                 uint64_t &geometryChanged);

    /**
         * @param compositionType
         */
    int skipStaticLayers(ExynosCompositionInfo &compositionInfo);
    int handleStaticLayers(ExynosCompositionInfo &compositionInfo);

    int doPostProcessing();

    int doExynosComposition();

    int32_t configureOverlay(ExynosLayer *layer,
                             exynos_win_config_data &cfg, bool hdrException = false);
    virtual int32_t configureOverlay(ExynosCompositionInfo &compositionInfo);

    int32_t configureHandle(ExynosLayer &layer, int fence_fd,
                            exynos_win_config_data &cfg, bool hdrException = false);

    void clearWinConfigData() { mDpuData.reset(); };
    virtual int setWinConfigData(DevicePresentInfo &deviceInfo);

    virtual int setDisplayWinConfigData();

    virtual int32_t validateWinConfigData();

    virtual int deliverWinConfigData(DevicePresentInfo &presentInfo);

    virtual int setReleaseFences();

    virtual bool checkFrameValidation();

    virtual bool is2StepBlendingRequired(exynos_image &src, buffer_handle_t outbuf);

    /**
         * Display Functions for HWC 2.0
         */

    /**
         * Descriptor: HWC2_FUNCTION_ACCEPT_DISPLAY_CHANGES
         * HWC2_PFN_ACCEPT_DISPLAY_CHANGES
         **/
    virtual int32_t acceptDisplayChanges();

    /**
         * Descriptor: HWC2_FUNCTION_CREATE_LAYER
         * HWC2_PFN_CREATE_LAYER
         */
    virtual int32_t createLayer(hwc2_layer_t *outLayer,
                                uint64_t &geometryFlag);

    /**
         * Descriptor: HWC2_FUNCTION_GET_ACTIVE_CONFIG
         * HWC2_PFN_GET_ACTIVE_CONFIG
         */
    virtual int32_t getActiveConfig(hwc2_config_t *outConfig);

    /**
         * Descriptor: HWC2_FUNCTION_GET_CHANGED_COMPOSITION_TYPES
         * HWC2_PFN_GET_CHANGED_COMPOSITION_TYPES
         */
    virtual int32_t getChangedCompositionTypes(
        uint32_t *outNumElements, hwc2_layer_t *outLayers,
        int32_t * /*hwc2_composition_t*/ outTypes);

    /**
         * Descriptor: HWC2_FUNCTION_GET_CLIENT_TARGET_SUPPORT
         * HWC2_PFN_GET_CLIENT_TARGET_SUPPORT
         */
    virtual int32_t getClientTargetSupport(
        uint32_t width, uint32_t height,
        int32_t /*android_pixel_format_t*/ format,
        int32_t /*android_dataspace_t*/ dataspace);

    /**
         * Descriptor: HWC2_FUNCTION_GET_COLOR_MODES
         * HWC2_PFN_GET_COLOR_MODES
         */
    virtual int32_t getColorModes(
        uint32_t *outNumModes, int32_t * /*android_color_mode_t*/ outModes,
        bool canProcessWCG);

    /* getDisplayAttribute(..., config, attribute, outValue)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_ATTRIBUTE
         * HWC2_PFN_GET_DISPLAY_ATTRIBUTE
         */
    virtual int32_t getDisplayAttribute(
        hwc2_config_t config,
        int32_t /*hwc2_attribute_t*/ attribute, int32_t *outValue);

    /* getDisplayConfigs(..., outNumConfigs, outConfigs)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_CONFIGS
         * HWC2_PFN_GET_DISPLAY_CONFIGS
         */
    virtual int32_t getDisplayConfigs(
        uint32_t *outNumConfigs,
        hwc2_config_t *outConfigs);

    /* getDisplayName(..., outSize, outName)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_NAME
         * HWC2_PFN_GET_DISPLAY_NAME
         */
    virtual int32_t getDisplayName(uint32_t *outSize, char *outName);

    /* getDisplayRequests(..., outDisplayRequests, outNumElements, outLayers,
         *     outLayerRequests)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_REQUESTS
         * HWC2_PFN_GET_DISPLAY_REQUESTS
         */
    virtual int32_t getDisplayRequests(
        int32_t * /*hwc2_display_request_t*/ outDisplayRequests,
        uint32_t *outNumElements, hwc2_layer_t *outLayers,
        int32_t * /*hwc2_layer_request_t*/ outLayerRequests);

    /* getDisplayType(..., outType)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_TYPE
         * HWC2_PFN_GET_DISPLAY_TYPE
         */
    virtual int32_t getDisplayType(
        int32_t * /*hwc2_display_type_t*/ outType);
    /* getDozeSupport(..., outSupport)
         * Descriptor: HWC2_FUNCTION_GET_DOZE_SUPPORT
         * HWC2_PFN_GET_DOZE_SUPPORT
         */
    virtual int32_t getDozeSupport(int32_t *outSupport);

    /* getReleaseFences(..., outNumElements, outLayers, outFences)
         * Descriptor: HWC2_FUNCTION_GET_RELEASE_FENCES
         * HWC2_PFN_GET_RELEASE_FENCES
         */
    virtual int32_t getReleaseFences(
        uint32_t *outNumElements,
        hwc2_layer_t *outLayers, int32_t *outFences);

    enum {
        SKIP_ERR_NONE = 0,
        SKIP_ERR_CONFIG_DISABLED,
        SKIP_ERR_FIRST_FRAME,
        SKIP_ERR_GEOMETRY_CHAGNED,
        SKIP_ERR_HAS_CLIENT_COMP,
        SKIP_ERR_SKIP_STATIC_CHANGED,
        SKIP_ERR_HAS_REQUEST,
        SKIP_ERR_DISP_NOT_CONNECTED,
        SKIP_ERR_DISP_NOT_POWER_ON,
        SKIP_ERR_FORCE_VALIDATE
    };
    virtual int32_t canSkipValidate();

    int32_t forceSkipPresentDisplay(int32_t *outPresentFence);

    int32_t handlePresentError(String8 &errString, int32_t *outPresentFence);
    /* presentDisplay(..., outPresentFence)
         * Descriptor: HWC2_FUNCTION_PRESENT_DISPLAY
         * HWC2_PFN_PRESENT_DISPLAY
         */
    virtual int32_t presentDisplay(DevicePresentInfo &presentInfo,
                                   int32_t *outPresentFence);
    virtual int32_t disableReadback();
    void setPresentState();

    /* setActiveConfig(..., config)
         * Descriptor: HWC2_FUNCTION_SET_ACTIVE_CONFIG
         * HWC2_PFN_SET_ACTIVE_CONFIG
         */
    virtual int32_t setActiveConfig(hwc2_config_t config);

    /* setClientTarget(..., target, acquireFence, dataspace)
         * Descriptor: HWC2_FUNCTION_SET_CLIENT_TARGET
         * HWC2_PFN_SET_CLIENT_TARGET
         */
    virtual int32_t setClientTarget(
        buffer_handle_t target,
        int32_t acquireFence, int32_t /*android_dataspace_t*/ dataspace,
        uint64_t &geometryFlag);

    /* setColorTransform(..., matrix, hint)
         * Descriptor: HWC2_FUNCTION_SET_COLOR_TRANSFORM
         * HWC2_PFN_SET_COLOR_TRANSFORM
         */
    virtual int32_t setColorTransform(
        const float *matrix,
        int32_t /*android_color_transform_t*/ hint,
        uint64_t &geometryFlag);

    /* setColorMode(..., mode)
         * Descriptor: HWC2_FUNCTION_SET_COLOR_MODE
         * HWC2_PFN_SET_COLOR_MODE
         */
    virtual int32_t setColorMode(int32_t /*android_color_mode_t*/ mode,
                                 bool canprocessWCG, uint64_t &geometryFlag);

    /* setOutputBuffer(..., buffer, releaseFence)
         * Descriptor: HWC2_FUNCTION_SET_OUTPUT_BUFFER
         * HWC2_PFN_SET_OUTPUT_BUFFER
         */
    virtual int32_t setOutputBuffer(
        buffer_handle_t buffer,
        int32_t releaseFence);

    virtual int clearDisplay();

    /* setPowerMode(..., mode)
         * Descriptor: HWC2_FUNCTION_SET_POWER_MODE
         * HWC2_PFN_SET_POWER_MODE
         */
    virtual int32_t setPowerMode(int32_t /*hwc2_power_mode_t*/ mode,
                                 uint64_t &geometryFlag);

    /* setVsyncEnabled(..., enabled)
         * Descriptor: HWC2_FUNCTION_SET_VSYNC_ENABLED
         * HWC2_PFN_SET_VSYNC_ENABLED
         */
    virtual int32_t setVsyncEnabled(
        int32_t /*hwc2_vsync_t*/ enabled);
    int32_t setVsyncEnabledInternal(
        int32_t /*hwc2_vsync_t*/ enabled);

    int32_t forceSkipValidateDisplay(
        uint32_t *outNumTypes, uint32_t *outNumRequests);

    virtual int32_t preProcessValidate(DeviceValidateInfo &validateInfo,
                                       uint64_t &geometryChanged);
    virtual int32_t postProcessValidate();
    int32_t setValidateState(uint32_t &outNumTypes,
                             uint32_t &outNumRequests,
                             uint64_t &geometryChanged);
    void setForceClient();

    /* getHdrCapabilities(..., outNumTypes, outTypes, outMaxLuminance,
         *     outMaxAverageLuminance, outMinLuminance)
         * Descriptor: HWC2_FUNCTION_GET_HDR_CAPABILITIES
         */
    virtual int32_t getHdrCapabilities(uint32_t *outNumTypes, int32_t * /*android_hdr_t*/ outTypes, float *outMaxLuminance,
                                       float *outMaxAverageLuminance, float *outMinLuminance);

    virtual int32_t getRenderIntents(int32_t mode, uint32_t *outNumIntents,
                                     int32_t * /*android_render_intent_v1_1_t*/ outIntents);
    virtual int32_t setColorModeWithRenderIntent(
        int32_t /*android_color_mode_t*/ mode,
        int32_t /*android_render_intent_v1_1_t */ intent,
        bool canProcessWCG, uint64_t &geometryFlag);

    /* HWC 2.3 APIs */

    /* getDisplayIdentificationData(..., outPort, outDataSize, outData)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_IDENTIFICATION_DATA
         * Parameters:
         *   outPort - the connector to which the display is connected;
         *             pointer will be non-NULL
         *   outDataSize - if outData is NULL, the size in bytes of the data which would
         *       have been returned; if outData is not NULL, the size of outData, which
         *       must not exceed the value stored in outDataSize prior to the call;
         *       pointer will be non-NULL
         *   outData - the EDID 1.3 blob identifying the display
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in
         */
    int32_t getDisplayIdentificationData(uint8_t *outPort,
                                         uint32_t *outDataSize, uint8_t *outData);

    /* getDisplayCapabilities(..., outCapabilities)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_CAPABILITIES
         * Parameters:
         *   outNumCapabilities - if outCapabilities was nullptr, returns the number of capabilities
         *       if outCapabilities was not nullptr, returns the number of capabilities stored in
         *       outCapabilities, which must not exceed the value stored in outNumCapabilities prior
         *       to the call; pointer will be non-NULL
         *   outCapabilities - a list of supported capabilities.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in
         */
    /* Capabilities
           Invalid = HWC2_CAPABILITY_INVALID,
           SidebandStream = HWC2_CAPABILITY_SIDEBAND_STREAM,
           SkipClientColorTransform = HWC2_CAPABILITY_SKIP_CLIENT_COLOR_TRANSFORM,
           PresentFenceIsNotReliable = HWC2_CAPABILITY_PRESENT_FENCE_IS_NOT_RELIABLE,
           SkipValidate = HWC2_CAPABILITY_SKIP_VALIDATE,
        */
    int32_t getDisplayCapabilities(uint32_t *outNumCapabilities,
                                   uint32_t *outCapabilities);

    /* getDisplayBrightnessSupport(displayToken)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_BRIGHTNESS_SUPPORT
         * Parameters:
         *   outSupport - whether the display supports operations.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY when the display is invalid.
         */
    int32_t getDisplayBrightnessSupport(bool *outSupport);

    /* setDisplayBrightness(displayToken, brightnesss)
         * Descriptor: HWC2_FUNCTION_SET_DISPLAY_BRIGHTNESS
         * Parameters:
         *   brightness - a number between 0.0f (minimum brightness) and 1.0f (maximum brightness), or
         *          -1.0f to turn the backlight off.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY   when the display is invalid, or
         *   HWC2_ERROR_UNSUPPORTED   when brightness operations are not supported, or
         *   HWC2_ERROR_BAD_PARAMETER when the brightness is invalid, or
         *   HWC2_ERROR_NO_RESOURCES  when the brightness cannot be applied.
         */
    int32_t setDisplayBrightness(float brightness);

    /* getDisplayConnectionType(..., outType)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_CONNECTION_TYPE
         * Optional for all HWC2 devices
         *
         * Returns whether the given physical display is internal or external.
         *
         * Parameters:
         * outType - the connection type of the display; pointer will be non-NULL
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         * HWC2_ERROR_BAD_DISPLAY when the display is invalid or virtual.
         */
    int32_t getDisplayConnectionType(uint32_t *outType);

    /* getDisplayVsyncPeriod(..., outVsyncPeriods)
         * Descriptor: HWC2_FUNCTION_GET_DISPLAY_VSYNC_PERIOD
         * Required for HWC2 devices for composer 2.4
         *
         * Retrieves which vsync period the display is currently using.
         *
         * If no display configuration is currently active, this function must
         * return BAD_CONFIG. If a vsync period is about to change due to a
         * setActiveConfigWithConstraints call, this function must return the current vsync period
         * until the change has taken place.
         *
         * Parameters:
         *     outVsyncPeriod - the current vsync period of the display.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in
         *   HWC2_ERROR_BAD_CONFIG - no configuration is currently active
         */
    virtual int32_t getDisplayVsyncPeriod(hwc2_vsync_period_t *__unused outVsyncPeriod);

    /* setActiveConfigWithConstraints(...,
         *                                config,
         *                                vsyncPeriodChangeConstraints,
         *                                outTimeline)
         * Descriptor: HWC2_FUNCTION_SET_ACTIVE_CONFIG_WITH_CONSTRAINTS
         * Required for HWC2 devices for composer 2.4
         *
         * Sets the active configuration and the refresh rate for this display.
         * If the new config shares the same config group as the current config,
         * only the vsync period shall change.
         * Upon returning, the given display configuration, except vsync period, must be active and
         * remain so until either this function is called again or the display is disconnected.
         * When the display starts to refresh at the new vsync period, onVsync_2_4 callback must be
         * called with the new vsync period.
         *
         * Parameters:
         *     config - the new display configuration.
         *     vsyncPeriodChangeConstraints - constraints required for changing vsync period:
         *                                    desiredTimeNanos - the time in CLOCK_MONOTONIC after
         *                                                       which the vsync period may change
         *                                                       (i.e., the vsync period must not change
         *                                                       before this time).
         *                                    seamlessRequired - if true, requires that the vsync period
         *                                                       change must happen seamlessly without
         *                                                       a noticeable visual artifact.
         *                                                       When the conditions change and it may be
         *                                                       possible to change the vsync period
         *                                                       seamlessly, HWC2_CALLBACK_SEAMLESS_POSSIBLE
         *                                                       callback must be called to indicate that
         *                                                       caller should retry.
         *     outTimeline - the timeline for the vsync period change.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in.
         *   HWC2_ERROR_BAD_CONFIG - an invalid configuration handle passed in.
         *   HWC2_ERROR_SEAMLESS_NOT_ALLOWED - when seamlessRequired was true but config provided doesn't
         *                                 share the same config group as the current config.
         *   HWC2_ERROR_SEAMLESS_NOT_POSSIBLE - when seamlessRequired was true but the display cannot
         *                                      achieve the vsync period change without a noticeable
         *                                      visual artifact.
         */
    virtual int32_t setActiveConfigWithConstraints(hwc2_config_t __unused config,
                                                   hwc_vsync_period_change_constraints_t *__unused vsyncPeriodChangeConstraints,
                                                   hwc_vsync_period_change_timeline_t *__unused outTimeline,
                                                   bool needUpdateTimeline = true);

    /* setBootDisplayConfig(..., config)
         * Descriptor: HWC2_FUNCTION_SET_BOOT_DISPLAY_CONFIG
         * Optional for HWC2 devices
         *
         * Sets the display config in which the device boots.
         * If the device is unable to boot in this config for any reason (example HDMI display
         * changed), the implementation should try to find a config which matches the resolution
         * and refresh-rate of this config. If no such config exists, the implementation's
         * preferred display config should be used.
         *
         * See also:
         *     getPreferredBootDisplayConfig
         *
         * Parameters:
         *     config - is the new boot time config for the display.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *     HWC2_ERROR_BAD_DISPLAY - when the display is invalid
         *     HWC2_ERROR_BAD_CONFIG - when the configuration is invalid
         *     HWC2_ERROR_UNSUPPORTED - when the display does not support boot display config
         */
    virtual int32_t setBootDisplayConfig(int32_t config);

    /* clearBootDisplayConfig(...)
         * Descriptor: HWC2_FUNCTION_CLEAR_BOOT_DISPLAY_CONFIG
         * Optional for HWC2 devices
         *
         * Clears the boot display config.
         * The device should boot in the implementation's preferred display config.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *     HWC2_ERROR_BAD_DISPLAY - when the display is invalid
         *     HWC2_ERROR_UNSUPPORTED - when the display does not support boot display config
         */
    virtual int32_t clearBootDisplayConfig();

    /* getPreferredBootDisplayConfig(..., config*)
         * Descriptor: HWC2_FUNCTION_GET_PREFERRED_DISPLAY_CONFIG
         * Optional for HWC2 devices
         *
         * Returns the implementation's preferred display config.
         * This is display config used by the implementation at boot time, if the boot
         * display config has not been requested yet, or if it has been previously cleared.
         *
         * See also:
         *     setBootDisplayConfig
         *
         * Parameters:
         *     outConfig - is the implementation's preferred display config
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *     HWC2_ERROR_BAD_DISPLAY - when the display is invalid
         *     HWC2_ERROR_BAD_CONFIG - when the configuration is invalid
         *     HWC2_ERROR_UNSUPPORTED - when the display does not support boot display config
         */
    int32_t getPreferredBootDisplayConfig(int32_t* outConfig);

    virtual int32_t getPreferredDisplayConfigInternal(int32_t *outConfig);

    /* setAutoLowLatencyMode(displayToken, on)
         * Descriptor: HWC2_FUNCTION_SET_AUTO_LOW_LATENCY_MODE
         * Optional for HWC2 devices
         *
         * setAutoLowLatencyMode requests that the display goes into low latency mode. If the display
         * is connected via HDMI 2.1, then Auto Low Latency Mode should be triggered. If the display is
         * internally connected, then a custom low latency mode should be triggered (if available).
         *
         * Parameters:
         *   on - indicates whether to turn low latency mode on (=true) or off (=false)
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - when the display is invalid, or
         *   HWC2_ERROR_UNSUPPORTED - when the display does not support any low latency mode
         */
    int32_t setAutoLowLatencyMode(bool __unused on);

    /* getSupportedContentTypes(..., outSupportedContentTypes)
         * Descriptor: HWC2_FUNCTION_GET_SUPPORTED_CONTENT_TYPES
         * Optional for HWC2 devices
         *
         * getSupportedContentTypes returns a list of supported content types
         * (as described in the definition of ContentType above).
         * This list must not change after initialization.
         *
         * Parameters:
         *   outNumSupportedContentTypes - if outSupportedContentTypes was nullptr, returns the number
         *       of supported content types; if outSupportedContentTypes was not nullptr, returns the
         *       number of capabilities stored in outSupportedContentTypes, which must not exceed the
         *       value stored in outNumSupportedContentTypes prior to the call; pointer will be non-NULL
         *   outSupportedContentTypes - a list of supported content types.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in
         */
    int32_t getSupportedContentTypes(uint32_t *__unused outNumSupportedContentTypes,
                                     uint32_t *__unused outSupportedContentTypes);

    /* setContentType(displayToken, contentType)
         * Descriptor: HWC2_FUNCTION_SET_CONTENT_TYPE
         * Optional for HWC2 devices
         *
         * setContentType instructs the display that the content being shown is of the given contentType
         * (one of GRAPHICS, PHOTO, CINEMA, GAME).
         *
         * According to the HDMI 1.4 specification, supporting all content types is optional. Whether
         * the display supports a given content type is reported by getSupportedContentTypes.
         *
         * Parameters:
         *   contentType - the type of content that is currently being shown on the display
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - when the display is invalid, or
         *   HWC2_ERROR_UNSUPPORTED - when the given content type is a valid content type, but is not
         *                            supported on this display, or
         *   HWC2_ERROR_BAD_PARAMETER - when the given content type is invalid
         */
    int32_t setContentType(int32_t /* hwc2_content_type_t */ __unused contentType);

    /* getClientTargetProperty(..., outClientTargetProperty)
         * Descriptor: HWC2_FUNCTION_GET_CLIENT_TARGET_PROPERTY
         * Optional for HWC2 devices
         *
         * Retrieves the client target properties for which the hardware composer
         * requests after the last call to validateDisplay. The client must set the
         * properties of the client target to match the returned values.
         * When this API is implemented, if client composition is needed, the hardware
         * composer must return meaningful client target property with dataspace not
         * setting to UNKNOWN.
         * When the returned dataspace is set to UNKNOWN, it means hardware composer
         * requests nothing, the client must ignore the returned client target property
         * structrue.
         *
         * Parameters:
         *   outClientTargetProperty - the client target properties that hardware
         *       composer requests. If dataspace field is set to UNKNOWN, it means
         *       the hardware composer requests nothing, the client must ignore the
         *       returned client target property structure.
         *
         * Returns HWC2_ERROR_NONE or one of the following errors:
         *   HWC2_ERROR_BAD_DISPLAY - an invalid display handle was passed in
         *   HWC2_ERROR_NOT_VALIDATED - validateDisplay has not been called for this
         *       display
         */
    int32_t getClientTargetProperty(hwc_client_target_property_t *outClientTargetProperty);

    /* setActiveConfig MISCs */
    virtual int32_t setActiveConfigInternal(hwc2_config_t config);
    virtual int32_t getActiveConfigInternal(hwc2_config_t *outConfig);
    bool isBadConfig(hwc2_config_t config);
    virtual bool needChangeConfig(hwc2_config_t __unused config);
    int32_t updateInternalDisplayConfigVariables(hwc2_config_t config, bool updateVsync = true);
    int32_t resetConfigRequestState();
    int32_t updateConfigRequestAppliedTime();
    int32_t updateVsyncAppliedTimeLine(int64_t actualChangeTime);
    virtual int32_t getDisplayVsyncPeriodInternal(hwc2_vsync_period_t *outVsyncPeriod);
    int32_t getDisplayVsyncTimestamp(uint64_t *outVsyncTimestamp);
    virtual int32_t doDisplayConfigPostProcess();
    int32_t getConfigAppliedTime(const uint64_t desiredTime,
                                 const uint64_t actualChangeTime,
                                 int64_t &appliedTime, int64_t &refreshTime);

    /* TODO : TBD */
    int32_t setCursorPositionAsync(uint32_t x_pos, uint32_t y_pos);

    int32_t getReadbackBufferAttributes(int32_t * /*android_pixel_format_t*/ outFormat,
                                        int32_t * /*android_dataspace_t*/ outDataspace);
    int32_t setReadbackBuffer(buffer_handle_t buffer, int32_t releaseFence);
    void setReadbackBufferInternal(buffer_handle_t buffer, int32_t releaseFence);
    int32_t getReadbackBufferFence(int32_t *outFence);

    void dump(String8 &result);

    virtual int32_t startPostProcessing();

    void dumpConfig(const exynos_win_config_data &c);
    void dumpConfig(String8 &result, const exynos_win_config_data &c);
    void printConfig(exynos_win_config_data &c);

    unsigned int getLayerRegion(ExynosLayer *layer,
                                hwc_rect &rect_area, uint32_t regionType);
    int canApplyWindowUpdate(const exynos_dpu_data &lastConfigsData,
                             const exynos_dpu_data &newConfigsData,
                             uint32_t index);
    int mergeDamageRect(hwc_rect &merge_rect, hwc_rect &damage_rect);
    int setWindowUpdate(const hwc_rect &merge_rect);
    bool windowUpdateExceptions();
    int handleWindowUpdate();

    virtual void waitPreviousFrameDone(int fence);

    /* For debugging */
    bool validateExynosCompositionLayer();
    void printDebugInfos(String8 &reason);

    bool checkConfigChanged(const exynos_dpu_data &lastConfigsData,
                            const exynos_dpu_data &newConfigsData);

    uint32_t getRestrictionIndex(const ExynosFormat &format);
    void closeFences();
    void closeFencesForSkipFrame(rendering_state renderingState);

    int32_t getLayerCompositionTypeForValidationType(uint32_t layerIndex);
    void setHWCControl(uint32_t ctrl, int32_t val);
    void setGeometryChanged(uint64_t changedBit, uint64_t &outGeometryChanged);
    void clearGeometryChanged();

    void increaseMPPDstBufIndex();
    virtual void initDisplayInterface(uint32_t interfaceType,
                                      void *deviceData, size_t &deviceDataSize);
    void getDumpLayer();
    void dumpLayers();
    void setDumpCount(uint32_t dumpCount);
    void writeDumpData(int32_t frameNo, int32_t layerNo,
                       layerDumpFrameInfo *frameInfo, layerDumpLayerInfo *layerInfo);
    /* Override for each display's meaning of 'enabled state'
         * Primary : Power on, this function overrided in primary display module
         * Exteranal : Plug-in, default */
    virtual bool isEnabled() { return mPlugState; };

    virtual void requestHiberExit();
    virtual void initHiberState();

    /* getDisplayPreAssignBit support mIndex up to 1.
           It supports only dual LCD and 2 external displays */
    inline uint32_t getDisplayPreAssignBit() {
        uint32_t type = SECOND_DISPLAY_START_BIT * mIndex + mType;
        return 1 << type;
    }

    hdrInterface *createHdrInterfaceInstance();
    hdr10pMetaInterface *createHdr10PMetaInterfaceInstance();

    virtual int32_t getDisplayMultiThreadedPresentSupport(bool& outSupport);

#ifdef USE_DQE_INTERFACE
    bool needDqeSetting();
    void setDqeCoef(int fd);
#endif
    virtual int32_t updateColorConversionInfo();
    virtual int32_t getDisplayInfo(DisplayInfo &dispInfo);
    virtual int32_t setPerformanceSetting() { return NO_ERROR; };
    int32_t handleSkipPresent(int32_t *outPresentFence);
    void setSrcAcquireFences();
    void invalidate();
    virtual bool checkDisplayUnstarted() { return false; };
    void registerFpsChangedCallback(ExynosFpsChangedCallback *callback) {
        mFpsChangedCallback = callback;
    };
    bool isFrameSkipPowerState() {
        if ((mPowerModeState == HWC2_POWER_MODE_OFF) ||
            (mPowerModeState == HWC2_POWER_MODE_DOZE_SUSPEND))
            return true;
        return false;
    };
    virtual void resetForDestroyClient();

  protected:
    Mutex mDisplayMutex;
    ExynosVsyncCallback mVsyncCallback;
    ExynosFenceTracer &mFenceTracer = ExynosFenceTracer::getInstance();
    PendingConfigInfo mPendConfigInfo;
    virtual bool getHDRException(ExynosLayer *layer,
                                 DevicePresentInfo &deviceInfo);
    virtual bool getHDRException() { return false; };
    int32_t setColorConversionInfo(ExynosMPP *otfMPP);

  public:
    /**
         * This will be initialized with differnt class
         * that inherits ExynosDisplayInterface according to
         * interface type.
         */
    std::unique_ptr<ExynosDisplayInterface> mDisplayInterface;
    /* Interface of ExynosVsyncHandler */
    virtual void handleVsync(uint64_t timestamp) override;

    int32_t checkValidationConfigConstraints(hwc2_config_t config,
                                             hwc_vsync_period_change_constraints_t *vsyncPeriodChangeConstraints,
                                             hwc_vsync_period_change_timeline_t *outTimeline);

  private:
    bool skipStaticLayerChanged(ExynosCompositionInfo &compositionInfo);
    LayerDumpManager *mLayerDumpManager = nullptr;

  public:
    std::map<uint32_t, displayTDMInfo> mDisplayTDMInfo;
};

class LayerDumpManager {
  public:
    LayerDumpManager();
    LayerDumpManager(ExynosDisplay *display);
    ~LayerDumpManager();
    int32_t getDumpFrameIndex();
    int32_t getDumpMaxIndex();
    layerDumpFrameInfo *getLayerDumpFrameInfo(uint32_t idx);
    void setCount(uint32_t cnt);
    void run(uint32_t cnt);
    void stop();
    void wait();
    bool isRunning();
    void triggerDumpFrame();
    void loop();
    enum class ThreadState {
        STOPPED = 0,
        RUN = 1,
    };

  private:
    int32_t mDumpFrameIndex; // GUARDED_BY(mMutex)
    int32_t mDumpMaxIndex; // GUARDED_BY(mMutex)
    ExynosDisplay *mDisplay;
    std::mutex mMutex;
    ThreadState mState = ThreadState::STOPPED; // GUARDED_BY(mMutex)
    layerDumpFrameInfo mLayerDumpInfo[LAYER_DUMP_FRAME_CNT_MAX];
    std::thread mThread;
};

#endif  //_EXYNOSDISPLAY_H
