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

#include "ExynosDisplayInterface.h"
#include <log/log.h>

ExynosDisplayInterface::~ExynosDisplayInterface() {
}

int32_t ExynosDisplayInterface::getDisplayConfigs(uint32_t *outNumConfigs,
                                                  hwc2_config_t *outConfigs, std::map<uint32_t, displayConfigs_t> &displayConfigs) {
    *outNumConfigs = 1;

    if (outConfigs != NULL)
        outConfigs[0] = 0;

    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayInterface::getDPUConfig(hwc2_config_t *outConfig) {
    /* config will be set at ExynosDisplay's getDPUConfig function */
    *outConfig = 0;
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplayInterface::getColorModes(
    uint32_t *outNumModes,
    int32_t *outModes) {
    *outNumModes = 1;

    if (outModes != NULL) {
        outModes[0] = HAL_COLOR_MODE_NATIVE;
    }
    return HWC2_ERROR_NONE;
}

int32_t ExynosDisplayInterface::getReadbackBufferAttributes(int32_t *outFormat, int32_t *outDataspace) {
    /* readback is not supported */
    return HWC2_ERROR_UNSUPPORTED;
}

int32_t ExynosDisplayInterface::setForcePanic() {
    return 0;
}
