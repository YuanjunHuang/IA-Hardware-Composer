/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "grallocbufferhandler.h"

#include <xf86drmMode.h>
#include <xf86drm.h>

#include <hardware/hardware.h>
#include <hardware/hwcomposer.h>
#include <ui/GraphicBuffer.h>
#include <cutils/native_handle.h>
#include "commondrmutils.h"

#include <cros_gralloc_handle.h>
#include <cros_gralloc_helpers.h>

#include <hwcdefs.h>
#include <hwctrace.h>
#include "drmhwcgralloc.h"
#include "commondrmutils.h"
#include "utils_android.h"

namespace hwcomposer {

// static
NativeBufferHandler *NativeBufferHandler::CreateInstance(uint32_t fd) {
  GrallocBufferHandler *handler = new GrallocBufferHandler(fd);
  if (!handler)
    return NULL;

  if (!handler->Init()) {
    ETRACE("Failed to initialize GralocBufferHandlers.");
    delete handler;
    return NULL;
  }
  return handler;
}

GrallocBufferHandler::GrallocBufferHandler(uint32_t fd) : fd_(fd) {
}

GrallocBufferHandler::~GrallocBufferHandler() {
}

bool GrallocBufferHandler::Init() {
  int ret = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                          (const hw_module_t **)&gralloc_);
  if (ret) {
    ETRACE("Failed to open gralloc module");
    return false;
  }
  return true;
}

bool GrallocBufferHandler::CreateBuffer(uint32_t w, uint32_t h, int format,
                                        HWCNativeHandle *handle,
                                        uint32_t layer_type) {
  return CreateGraphicsBuffer(w, h, format, handle, layer_type);
}

bool GrallocBufferHandler::ReleaseBuffer(HWCNativeHandle handle) {
  return ReleaseGraphicsBuffer(handle, fd_);
}

void GrallocBufferHandler::DestroyHandle(HWCNativeHandle handle) {
  DestroyBufferHandle(handle);
}

bool GrallocBufferHandler::ImportBuffer(HWCNativeHandle handle) {
  return ImportGraphicsBuffer(handle, fd_);
}

uint32_t GrallocBufferHandler::GetTotalPlanes(HWCNativeHandle handle) {
  auto gr_handle = (struct cros_gralloc_handle *)handle->handle_;
  if (!gr_handle) {
    ETRACE("could not find gralloc drm handle");
    return false;
  }

  return drm_bo_get_num_planes(gr_handle->format);
}

void GrallocBufferHandler::CopyHandle(HWCNativeHandle source,
                                      HWCNativeHandle *target) {
  CopyBufferHandle(source, target);
}

void *GrallocBufferHandler::Map(HWCNativeHandle /*handle*/, uint32_t /*x*/,
                                uint32_t /*y*/, uint32_t /*width*/,
                                uint32_t /*height*/, uint32_t * /*stride*/,
                                void ** /*map_data*/, size_t /*plane*/) {
  return NULL;
}

int32_t GrallocBufferHandler::UnMap(HWCNativeHandle /*handle*/,
                                 void * /*map_data*/) {
}

}  // namespace hwcomposer
