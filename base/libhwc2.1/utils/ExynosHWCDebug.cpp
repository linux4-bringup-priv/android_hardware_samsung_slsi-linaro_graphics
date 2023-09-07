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
#include "ExynosHWCDebug.h"
#include "ExynosDisplay.h"
#include <android/sync.h>
#include "exynos_sync.h"

uint32_t mErrLogSize = 0;

extern struct exynos_hwc_control exynosHWCControl;
uint64_t errorFrameCount;

int32_t saveErrorLog(const String8 &errString, DisplayIdentifier &display) {
    int32_t ret = NO_ERROR;
    if (mErrLogSize >= ERR_LOG_SIZE)
        return -1;

    FILE *pFile = NULL;
    char filePath[128];
    sprintf(filePath, "%s/hwc_error_log.txt", ERROR_LOG_PATH0);
    pFile = fopen(filePath, "a");
    if (pFile == NULL) {
        ALOGE("Fail to open file %s/hwc_error_log.txt, error: %s", ERROR_LOG_PATH0, strerror(errno));
        sprintf(filePath, "%s/hwc_error_log.txt", ERROR_LOG_PATH1);
        pFile = fopen(filePath, "a");
    }
    if (pFile == NULL) {
        ALOGE("Fail to open file %s/hwc_error_log.txt, error: %s", ERROR_LOG_PATH1, strerror(errno));
        return -errno;
    }

    mErrLogSize = ftell(pFile);
    if (mErrLogSize >= ERR_LOG_SIZE) {
        if (pFile != NULL)
            fclose(pFile);
        return -1;
    }

    String8 saveString;
    struct timeval tv;
    struct tm *localTime;
    gettimeofday(&tv, NULL);
    localTime = (struct tm *)localtime((time_t *)&tv.tv_sec);

    if (display.id != UINT32_MAX) {
        saveString.appendFormat("%02d-%02d %02d:%02d:%02d.%03lu(%lu) %s %" PRIu64 ": %s\n",
                                localTime->tm_mon + 1, localTime->tm_mday,
                                localTime->tm_hour, localTime->tm_min,
                                localTime->tm_sec, tv.tv_usec / 1000, ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)),
                                display.name.c_str(), errorFrameCount,
                                errString.c_str());
    } else {
        saveString.appendFormat("%02d-%02d %02d:%02d:%02d.%03lu(%lu) : %s\n",
                                localTime->tm_mon + 1, localTime->tm_mday,
                                localTime->tm_hour, localTime->tm_min,
                                localTime->tm_sec, tv.tv_usec / 1000, ((tv.tv_sec * 1000) + (tv.tv_usec / 1000)),
                                errString.c_str());
    }

    if (pFile != NULL) {
        fwrite(saveString.c_str(), 1, saveString.size(), pFile);
        mErrLogSize = (uint32_t)ftell(pFile);
        ret = mErrLogSize;
        fclose(pFile);
    }
    return ret;
}
