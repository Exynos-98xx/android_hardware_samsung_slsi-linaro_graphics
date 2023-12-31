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

#ifndef EXYNOS_DEVICE_MODULE_H
#define EXYNOS_DEVICE_MODULE_H

#include "ExynosDevice.h"
#include "CpuPerfInfo.h"

class ExynosDeviceModule : public ExynosDevice {
    public:
        ExynosDeviceModule();
        virtual ~ExynosDeviceModule();
        virtual bool supportPerformaceAssurance() {
            /* TODO : This feature support only 120hz display */
#ifdef USES_HWC_CPU_PERF_MODE
            return true;
#else
            return false;
#endif
        };
        virtual void setCPUClocksPerCluster(uint32_t fps);
};

#endif
