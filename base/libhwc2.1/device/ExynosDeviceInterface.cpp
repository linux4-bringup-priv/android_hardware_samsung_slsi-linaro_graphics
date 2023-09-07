/*
 * Copyright (C) 2019 The Android Open Source Project
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

#include "ExynosDeviceInterface.h"
#include "ExynosHWCDebug.h"
#include "ExynosMPP.h"
#include "ExynosResourceRestriction.h"
#include <unordered_set>

void ExynosDeviceInterface::printDppRestriction(struct dpp_ch_restriction res) {
    ALOGD("=========================================================");
    ALOGD("id: %d", res.id);
    ALOGD("dpp_restriction");
    ALOGD("src_f_w (%d, %d, %d)", res.restriction.src_f_w.min, res.restriction.src_f_w.max, res.restriction.src_f_w.align);
    ALOGD("src_f_h (%d, %d, %d)", res.restriction.src_f_h.min, res.restriction.src_f_h.max, res.restriction.src_f_h.align);
    ALOGD("src_w (%d, %d, %d)", res.restriction.src_w.min, res.restriction.src_w.max, res.restriction.src_w.align);
    ALOGD("src_h (%d, %d, %d)", res.restriction.src_h.min, res.restriction.src_h.max, res.restriction.src_h.align);
    ALOGD("src_x_align(%d), src_y_align(%d)", res.restriction.src_x_align, res.restriction.src_y_align);

    ALOGD("dst_f_w (%d, %d, %d)", res.restriction.dst_f_w.min, res.restriction.dst_f_w.max, res.restriction.dst_f_w.align);
    ALOGD("dst_f_h (%d, %d, %d)", res.restriction.dst_f_h.min, res.restriction.dst_f_h.max, res.restriction.dst_f_h.align);
    ALOGD("dst_w (%d, %d, %d)", res.restriction.dst_w.min, res.restriction.dst_w.max, res.restriction.dst_w.align);
    ALOGD("dst_h (%d, %d, %d)", res.restriction.dst_h.min, res.restriction.dst_h.max, res.restriction.dst_h.align);
    ALOGD("dst_x_align(%d), dst_y_align(%d)", res.restriction.dst_x_align, res.restriction.dst_y_align);
    ALOGD("src_h_rot_max(%d)", res.restriction.src_h_rot_max);

    ALOGD("blk_w (%d, %d, %d)", res.restriction.blk_w.min, res.restriction.blk_w.max, res.restriction.blk_w.align);
    ALOGD("blk_h (%d, %d, %d)", res.restriction.blk_h.min, res.restriction.blk_h.max, res.restriction.blk_h.align);
    ALOGD("blk_x_align(%d), blk_y_align(%d)", res.restriction.blk_x_align, res.restriction.blk_y_align);

    ALOGD("format cnt: %d", res.restriction.format_cnt);
    for (int i = 0; i < res.restriction.format_cnt; i++) {
        ALOGD("[%d] format: %d (%s)", i, res.restriction.format[i], getFormatStr(res.restriction.format[i]).c_str());
    }

    ALOGD("scale down: %d, up: %d", res.restriction.scale_down, res.restriction.scale_up);
    ALOGD("attr: 0x%lx", res.attr);
}
