/*
 * Copyright (C) 2018 The Android Open Source Project
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

#define LOG_TAG "hwc-resource-manager"

#include "resourcemanager.h"

#include <cutils/properties.h>
#include <log/log.h>
#include <sstream>
#include <string>

namespace android {

ResourceManager::ResourceManager() : num_displays_(0) {
}

int ResourceManager::Init() {
  char path_pattern[PROPERTY_VALUE_MAX];
  // Could be a valid path or it can have at the end of it the wildcard %
  // which means that it will try open all devices until an error is met.
  int path_len = property_get("vendor.hwc.drm.device", path_pattern, "/dev/dri/card%");
  int ret = 0;
  if (path_pattern[path_len - 1] != '%') {
    ret = AddDrmDevice(std::string(path_pattern));
  } else {
    path_pattern[path_len - 1] = '\0';
    for (int idx = 0; (!ret || ret == -EEXIST); ++idx) {
      std::ostringstream path;
      path << path_pattern << idx;
      ALOGD("%s %s trying..", __func__, path.str().c_str());
      ret = AddDrmDevice(path.str());
    }
  }

  if (!num_displays_) {
    ALOGE("Failed to initialize any displays");
    return ret ? -EINVAL : ret;
  }
  return ret;
}

int ResourceManager::AddDrmDevice(std::string path) {
  std::unique_ptr<DrmDevice> drm = std::make_unique<DrmDevice>();
  int displays_added, ret;
  std::tie(ret, displays_added) = drm->Init(path.c_str(), num_displays_);
  if (ret)
    return ret;
  drms_.push_back(std::move(drm));
  num_displays_ += displays_added;
  return ret;
}

DrmConnector *ResourceManager::AvailableWritebackConnector(int display) {
  DrmDevice *drm_device = GetDrmDevice(display);
  DrmConnector *writeback_conn = NULL;
  if (drm_device) {
    writeback_conn = drm_device->AvailableWritebackConnector(display);
    if (writeback_conn)
      return writeback_conn;
  }
  for (auto &drm : drms_) {
    if (drm.get() == drm_device)
      continue;
    writeback_conn = drm->AvailableWritebackConnector(display);
    if (writeback_conn)
      return writeback_conn;
  }
  return writeback_conn;
}

DrmDevice *ResourceManager::GetDrmDevice(int display) {
  for (auto &drm : drms_) {
    if (drm->HandlesDisplay(display))
      return drm.get();
  }
  return NULL;
}
}  // namespace android
