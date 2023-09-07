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

#include <log/log.h>
#include "ExynosHWCHelper.h"
#include "ExynosHWCDebug.h"
#include "ExynosFenceTracer.h"
extern struct exynos_hwc_control exynosHWCControl;
extern uint64_t errorFrameCount;

#if defined(DISABLE_HWC_DEBUG)
#define FT_LOGD(...)
#define FT_LOGI(...)
#else
#define FT_LOGD(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer >= 2)           \
            ALOGD("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }
#define FT_LOGI(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer >= 1)           \
            ALOGD("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }
#endif
#define FT_LOGE(msg, ...)                                \
    {                                                    \
        if (exynosHWCControl.fenceTracer > 0)            \
            ALOGE("[FenceTracer]::" msg, ##__VA_ARGS__); \
    }
ANDROID_SINGLETON_STATIC_INSTANCE(ExynosFenceTracer);

ExynosFenceTracer::ExynosFenceTracer() {
    mFenceInfo.clear();
}

ExynosFenceTracer::~ExynosFenceTracer() {
}

void ExynosFenceTracer::writeFenceInfo(uint32_t fd, hwc_fence_info_t *info,
                                       hwc_fdebug_fence_type type, hwc_fdebug_ip_type ip,
                                       uint32_t direction, bool pendingAllowed) {
    /* Sequnce is ring buffer */
    /* update last flag, sequence */
    info->seq[info->seq_no].isLast = false;
    info->seq_no++;
    if (info->seq_no >= MAX_FENCE_SEQUENCE)
        info->seq_no = 0;
    fenceTrace_t *seq = &info->seq[info->seq_no];
    seq->isLast = true;

    /* direction, type, ip */
    seq->dir = direction;
    seq->type = type;
    seq->ip = ip;
    seq->curFlag = 1;
    info->pendingAllowed = pendingAllowed;

    /* time */
    struct timeval tv;
    gettimeofday(&seq->time, NULL);
    tv = seq->time;
}

void ExynosFenceTracer::changeFenceInfoState(uint32_t fd, const DisplayIdentifier &display,
                                             hwc_fdebug_fence_type type, hwc_fdebug_ip_type ip,
                                             uint32_t direction, bool pendingAllowed) {
    if (!fence_valid(fd))
        return;

    /* init or recevice(from previous) trace info */
    hwc_fence_info_t info;
    if (mFenceInfo.count(fd) == 0) {
        info = {};
    } else
        info = mFenceInfo.at(fd);

    info.displayId = display.id;
    writeFenceInfo(fd, &info, type, ip, direction, pendingAllowed);
    FT_LOGD("FD : %d, direction : %d, type(%d), ip(%d) (%s)", fd, direction, type, ip, __func__);
    mFenceInfo[fd] = info;
}

void ExynosFenceTracer::setFenceInfo(uint32_t fd, const DisplayIdentifier &display,
                                     hwc_fdebug_fence_type type, hwc_fdebug_ip_type ip,
                                     uint32_t direction, bool pendingAllowed) {
    if (!fence_valid(fd))
        return;

    /* init or recevice(from previous) trace info */
    hwc_fence_info_t info;
    if (mFenceInfo.count(fd) == 0) {
        info = {};
    } else {
        info = mFenceInfo.at(fd);
    }

    info.displayId = display.id;
    writeFenceInfo(fd, &info, type, ip, direction, pendingAllowed);
    fenceTrace_t *seq = &info.seq[info.seq_no];

    /* update usage count */
    if ((seq->dir == FENCE_FROM) || (seq->dir == FENCE_DUP)) {
        info.usage++;
    } else if ((seq->dir == FENCE_TO) || (seq->dir == FENCE_CLOSE)) {
        info.usage--;
        if ((seq->dir == FENCE_CLOSE) && (info.usage < 0))
            info.usage = 0;
    } else
        ALOGE("Fence trace : Undefined direction!");

    FT_LOGI("setFenceInfo(%d):: %s, %s, %s, %s usage: %d",
            fd, display.name.c_str(),
            getString(fence_dir_map, seq->dir),
            getString(fence_type_map, seq->type),
            getString(fence_ip_map, seq->ip),
            info.usage);

    seq->usage = info.usage;
    // Fence's usage count shuld be zero at end of frame(present done).
    // This flag means usage count of the fence can be pended over frame.
    if (info.usage == 0)
        info.pendingAllowed = false;

    /* last direction */
    info.last_dir = direction;
    mFenceInfo[fd] = info;
}

void ExynosFenceTracer::printLastFenceInfo(uint32_t fd) {
    struct timeval tv;
    if (!fence_valid(fd))
        return;
    if (mFenceInfo.count(fd) == 0)
        return;

    hwc_fence_info_t info = mFenceInfo.at(fd);
    FT_LOGD("---- Fence FD : %d, Display(%d), usage(%d) ----", fd, info.displayId, info.usage);

    for (int i = 0; i < MAX_FENCE_SEQUENCE; i++) {
        fenceTrace_t *seq = &info.seq[i];
        FT_LOGD("fd(%d) %s(%s)(%s)(cur:%d)(usage:%d)(last:%d)",
                fd, getString(fence_dir_map, seq->dir),
                getString(fence_ip_map, seq->ip), getString(fence_type_map, seq->type),
                seq->curFlag, seq->usage, (int)seq->isLast);

        tv = seq->time;
        struct tm *localTime = getLocalTime(tv);

        FT_LOGD("time:%02d-%02d %02d:%02d:%02d.%03lu(%lu)",
                localTime->tm_mon + 1, localTime->tm_mday,
                localTime->tm_hour, localTime->tm_min,
                localTime->tm_sec, tv.tv_usec / 1000,
                ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)));
    }
}

void ExynosFenceTracer::dumpFenceInfo(int32_t depth) {
    FT_LOGD("Dump fence ++");
    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;

        hwc_fence_info_t info = mFenceInfo.at(i);
        if ((info.usage >= 1 || info.usage <= -1) && (!info.pendingAllowed))
            printLastFenceInfo(i);
    }
    FT_LOGD("Dump fence --");
}

bool ExynosFenceTracer::fenceWarn(uint32_t threshold) {
    uint32_t cnt = 0, r_cnt = 0;

    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);
        if (info.usage >= 1 || info.usage <= -1)
            cnt++;
    }

    if ((cnt > threshold) || (exynosHWCControl.fenceTracer > 0))
        dumpFenceInfo(0);

    if (r_cnt > threshold)
        ALOGE("Fence leak somewhare!!");

    FT_LOGD("fence hwc : %d, real : %d", cnt, r_cnt);
    return (cnt > threshold) ? true : false;
}

void ExynosFenceTracer::resetFenceCurFlag() {
    FT_LOGD("%s ++", __func__);
    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);

        if (info.usage == 0) {
            hwc_fence_info_t *info_ = &mFenceInfo[i];
            for (int j = 0; j < MAX_FENCE_SEQUENCE; j++)
                info_->seq[j].curFlag = 0;
        } else if (!info.pendingAllowed)
            FT_LOGE("usage mismatched fd %d, usage %d, pending %d", i,
                    info.usage, info.pendingAllowed);
    }
    FT_LOGD("%s --", __func__);
}

void ExynosFenceTracer::printFenceTrace(String8 &saveString, struct tm *localTime) {
    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;

        hwc_fence_info_t info = mFenceInfo.at(i);

        if (info.usage >= 1) {
            saveString.appendFormat("FD hwc : %d, usage %d, pending : %d\n", i, info.usage, (int)info.pendingAllowed);
            for (int j = 0; j < MAX_FENCE_SEQUENCE; j++) {
                fenceTrace_t *seq = &info.seq[j];
                saveString.appendFormat("    %s(%s)(%s)(cur:%d)(usage:%d)(last:%d)",
                                        getString(fence_dir_map, seq->dir),
                                        getString(fence_ip_map, seq->ip), getString(fence_type_map, seq->type),
                                        seq->curFlag, seq->usage, (int)seq->isLast);

                struct timeval tv = seq->time;
                localTime = (struct tm *)localtime((time_t *)&tv.tv_sec);

                saveString.appendFormat(" - time:%02d-%02d %02d:%02d:%02d.%03lu(%lu)\n",
                                        localTime->tm_mon + 1, localTime->tm_mday,
                                        localTime->tm_hour, localTime->tm_min,
                                        localTime->tm_sec, tv.tv_usec / 1000,
                                        ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)));
            }
        }
    }
}

void ExynosFenceTracer::printLeakFds() {
    int cnt = 1;
    String8 errStringPlus;
    String8 errStringMinus;

    errStringPlus.appendFormat("Leak Fds (1) :\n");

    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);
        if (info.usage >= 1) {
            errStringPlus.appendFormat("%d,", i);
            if (cnt++ % 10 == 0)
                errStringPlus.appendFormat("\n");
        }
    }
    FT_LOGI("%s", errStringPlus.c_str());

    errStringMinus.appendFormat("Leak Fds (-1) :\n");

    cnt = 1;
    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);
        if (info.usage < 0) {
            errStringMinus.appendFormat("%d,", i);
            if (cnt++ % 10 == 0)
                errStringMinus.appendFormat("\n");
        }
    }
    FT_LOGI("%s", errStringMinus.c_str());
}

bool ExynosFenceTracer::validateFencePerFrame(const DisplayIdentifier &display) {
    bool ret = true;

    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);
        if (info.displayId != display.id)
            continue;
        if ((info.usage >= 1 || info.usage <= -1) &&
            (!info.pendingAllowed) && (!info.leaking)) {
            ret = false;
        }
    }

    if (!ret) {
        int priv = exynosHWCControl.fenceTracer;
        exynosHWCControl.fenceTracer = 3;
        dumpNCheckLeak(0);
        exynosHWCControl.fenceTracer = priv;
    }

    return ret;
}

void ExynosFenceTracer::dumpNCheckLeak(int32_t depth) {
    FT_LOGD("Dump leaking fence ++");
    for (auto it = mFenceInfo.begin(); it != mFenceInfo.end(); ++it) {
        uint32_t i = it->first;
        if (mFenceInfo.count(i) == 0)
            continue;
        hwc_fence_info_t info = mFenceInfo.at(i);
        if ((info.usage >= 1 || info.usage <= -1) && (!info.pendingAllowed))
            // leak is occured in this frame first
            if (!info.leaking) {
                info.leaking = true;
                printLastFenceInfo(i);
            }
    }

    int priv = exynosHWCControl.fenceTracer;
    exynosHWCControl.fenceTracer = 3;
    printLeakFds();
    exynosHWCControl.fenceTracer = priv;

    FT_LOGD("Dump leaking fence --");
}

int ExynosFenceTracer::hwc_dup(int fd, const DisplayIdentifier &display,
                               hwc_fdebug_fence_type type, hwc_fdebug_ip_type ip, bool pendingAllowed) {
    int dup_fd = -1;

    if (fd >= 3)
        dup_fd = dup(fd);
    else if (fd == -1) {
        HDEBUGLOGD(eDebugFence, "%s : Fd is -1", __func__);
    } else {
        ALOGW("%s : Fd:%d is less than 3", __func__, fd);
        hwc_print_stack();
    }

    if ((dup_fd < 3) && (dup_fd != -1)) {
        ALOGW("%s : Dupulicated Fd:%d is less than 3 : %d", __func__, fd, dup_fd);
        hwc_print_stack();
    }

    setFenceInfo(dup_fd, display, type, ip, FENCE_FROM, pendingAllowed);
    FT_LOGD("duplicated %d from %d", dup_fd, fd);

    return dup_fd;
}

int ExynosFenceTracer::fence_close(int fence, const DisplayIdentifier &displayIdentifier,
                                   hwc_fdebug_fence_type type, hwc_fdebug_ip_type ip, const char *debug_str) {
    FT_LOGI("%s:: fence_close(%d)", debug_str, fence);
    setFenceInfo(fence, displayIdentifier, type, ip, FENCE_CLOSE);
    return hwcFdClose(fence);
}

bool ExynosFenceTracer::fence_valid(int fence) {
    if (fence == -1) {
        HDEBUGLOGD(eDebugFence, "%s : fence is -1", __func__);
        return false;
    } else if (fence < 3) {
        ALOGW("%s : fence (fd:%d) is less than 3", __func__, fence);
        hwc_print_stack();
        return true;
    }
    return true;
}

bool ExynosFenceTracer::validateFences(const DisplayIdentifier &display) {
    if (!(validateFencePerFrame(display)) && fenceWarn(MAX_FENCE_THRESHOLD)) {
        String8 errString;
        errString.appendFormat("Per frame fence leak!\n");
        ALOGE("%s", errString.c_str());
        saveFenceTrace();
        return false;
    }

    if (exynosHWCControl.doFenceFileDump) {
        ALOGE("Fence file dump !");
        if (mFenceLogSize != 0)
            ALOGE("Fence file not empty!");
        saveFenceTrace();
        exynosHWCControl.doFenceFileDump = false;
    }

    return true;
}

int32_t ExynosFenceTracer::saveFenceTrace() {
    int32_t ret = NO_ERROR;
    FILE *pFile = NULL;
    char filePath[128];
    String8 saveString;

    if (mFenceLogSize >= FENCE_ERR_LOG_SIZE)
        return -1;

    sprintf(filePath, "%s/hwc_fence_state.txt", ERROR_LOG_PATH0);
    pFile = fopen(filePath, "a");

    if (pFile == NULL) {
        ALOGE("Fail to open file %s/hwc_fence_state.txt, error: %s", ERROR_LOG_PATH0, strerror(errno));
        sprintf(filePath, "%s/hwc_fence_state.txt", ERROR_LOG_PATH1);
        pFile = fopen(filePath, "a");
    }
    if (pFile == NULL) {
        ALOGE("Fail to open file %s, error: %s", ERROR_LOG_PATH1, strerror(errno));
        return -errno;
    }

    mFenceLogSize = ftell(pFile);
    if (mFenceLogSize >= FENCE_ERR_LOG_SIZE) {
        fclose(pFile);
        return -1;
    }

    struct timeval tv;
    struct tm *localTime;
    gettimeofday(&tv, NULL);
    localTime = (struct tm *)localtime((time_t *)&tv.tv_sec);

    saveString.appendFormat("\nerrFrameNumber: %" PRIu64 " time:%02d-%02d %02d:%02d:%02d.%03lu(%lu)\n",
                            errorFrameCount,
                            localTime->tm_mon + 1, localTime->tm_mday,
                            localTime->tm_hour, localTime->tm_min,
                            localTime->tm_sec, tv.tv_usec / 1000,
                            ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)));

    printFenceTrace(saveString, localTime);

    if (pFile != NULL) {
        fwrite(saveString.c_str(), 1, saveString.size(), pFile);
        mFenceLogSize = (uint32_t)ftell(pFile);
        ret = mFenceLogSize;
        fclose(pFile);
    }

    return ret;
}

hwc_fdebug_ip_type_t ExynosFenceTracer::getM2MIPFenceType(uint32_t physicalType) {
    if (physicalType == MPP_MSC)
        return FENCE_IP_MSC;
    else if (physicalType == MPP_G2D)
        return FENCE_IP_MSC;

    return FENCE_IP_UNDEFINED;
}
