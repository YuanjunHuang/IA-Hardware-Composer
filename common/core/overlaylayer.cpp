/*
// Copyright (c) 2016 Intel Corporation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
*/

#include "overlaylayer.h"

#include <cmath>

#include <drm_mode.h>
#include <hwctrace.h>

#include "hwcutils.h"

#include <nativebufferhandler.h>

namespace hwcomposer {

OverlayLayer::ImportedBuffer::~ImportedBuffer() {
  if (acquire_fence_ > 0) {
    close(acquire_fence_);
  }
}

OverlayLayer::ImportedBuffer::ImportedBuffer(OverlayBuffer* buffer,
                                             int32_t acquire_fence)
    : acquire_fence_(acquire_fence) {
  buffer_.reset(buffer);
}

void OverlayLayer::SetAcquireFence(int32_t acquire_fence) {
  // Release any existing fence.
  if (imported_buffer_->acquire_fence_ > 0) {
    close(imported_buffer_->acquire_fence_);
  }

  imported_buffer_->acquire_fence_ = acquire_fence;
}

int32_t OverlayLayer::GetAcquireFence() const {
  return imported_buffer_->acquire_fence_;
}

int32_t OverlayLayer::ReleaseAcquireFence() const {
  int32_t fence = imported_buffer_->acquire_fence_;
  imported_buffer_->acquire_fence_ = -1;
  return fence;
}

OverlayBuffer* OverlayLayer::GetBuffer() const {
  return imported_buffer_->buffer_.get();
}

void OverlayLayer::SetBuffer(NativeBufferHandler* buffer_handler,
                             HWCNativeHandle handle, int32_t acquire_fence) {
  OverlayBuffer* buffer = OverlayBuffer::CreateOverlayBuffer();
  buffer->InitializeFromNativeHandle(handle, buffer_handler);
  imported_buffer_.reset(new ImportedBuffer(buffer, acquire_fence));
}

void OverlayLayer::ResetBuffer() {
  imported_buffer_.reset(nullptr);
}

void OverlayLayer::SetBlending(HWCBlending blending) {
  blending_ = blending;
}

void OverlayLayer::SetSourceCrop(const HwcRect<float>& source_crop) {
  source_crop_width_ = static_cast<int>(ceilf(source_crop.right) -
                                        static_cast<int>(source_crop.left));
  source_crop_height_ = static_cast<int>(ceilf(source_crop.bottom) -
                                         static_cast<int>(source_crop.top));
  source_crop_ = source_crop;
}

void OverlayLayer::SetDisplayFrame(const HwcRect<int>& display_frame) {
  display_frame_width_ = display_frame.right - display_frame.left;
  display_frame_height_ = display_frame.bottom - display_frame.top;
  display_frame_ = display_frame;
  surface_damage_ = display_frame_;
  last_surface_damage_ = surface_damage_;
}

void OverlayLayer::ValidateTransform(uint32_t transform,
                                     uint32_t display_transform) {
  if (transform & kTransform90) {
    if (transform & kReflectX) {
      plane_transform_ |= kReflectX;
    }

    if (transform & kReflectY) {
      plane_transform_ |= kReflectY;
    }

    switch (display_transform) {
      case kRotate90:
	plane_transform_ |= kTransform180;
        break;
      case kRotate180:
	plane_transform_ |= kTransform270;
        break;
      case kRotateNone:
	plane_transform_ |= kTransform90;
        break;
      default:
        break;
    }
  } else if (transform & kTransform180) {
    switch (display_transform) {
      case kRotate90:
	plane_transform_ |= kTransform270;
        break;
      case kRotate270:
	plane_transform_ |= kTransform90;
        break;
      case kRotateNone:
	plane_transform_ |= kTransform180;
        break;
      default:
        break;
    }
  } else if (transform & kTransform270) {
    switch (display_transform) {
      case kRotate270:
	plane_transform_ |= kTransform180;
        break;
      case kRotate180:
	plane_transform_ |= kTransform90;
        break;
      case kRotateNone:
	plane_transform_ |= kTransform270;
        break;
      default:
        break;
    }
  } else {
    if (display_transform == kRotate90) {
      if (transform & kReflectX) {
	plane_transform_ |= kReflectX;
      }

      if (transform & kReflectY) {
	plane_transform_ |= kReflectY;
      }

      plane_transform_ |= kTransform90;
    } else {
      switch (display_transform) {
        case kRotate270:
	  plane_transform_ |= kTransform270;
          break;
        case kRotate180:
	  plane_transform_ |= kReflectY;
          break;
        default:
          break;
      }
    }
  }
}

void OverlayLayer::UpdateSurfaceDamage(HwcLayer* layer,
                                       OverlayLayer* previous_layer) {
  if (!gpu_rendered_) {
    surface_damage_ = display_frame_;
    last_surface_damage_ = surface_damage_;
    return;
  }

  if (!previous_layer || (state_ & kClearSurface) ||
      (state_ & kDimensionsChanged) || (transform_ != kIdentity)) {
    surface_damage_ = display_frame_;
    last_surface_damage_ = surface_damage_;
    return;
  }

  const HwcRect<int>& previous = previous_layer->last_surface_damage_;
  const HwcRect<int>& current = layer->GetSurfaceDamage();
  surface_damage_.left = std::min(current.left, previous.left);
  surface_damage_.right = std::max(current.right, previous.right);
  surface_damage_.top = std::min(current.top, previous.top);
  surface_damage_.bottom = std::max(current.bottom, previous.bottom);

  last_surface_damage_ = current;
}

void OverlayLayer::InitializeState(HwcLayer* layer,
                                   NativeBufferHandler* buffer_handler,
                                   OverlayLayer* previous_layer,
                                   uint32_t z_order, uint32_t layer_index,
                                   uint32_t max_height, HWCRotation rotation,
                                   bool handle_constraints) {
  transform_ = layer->GetTransform();
  if (rotation != kRotateNone) {
    ValidateTransform(layer->GetTransform(), rotation);
    // Remove this in case we enable support in future
    // to apply display rotation at pipe level.
    transform_ = plane_transform_;
  } else {
    plane_transform_ = transform_;
  }

  alpha_ = layer->GetAlpha();
  layer_index_ = layer_index;
  z_order_ = z_order;
  source_crop_width_ = layer->GetSourceCropWidth();
  source_crop_height_ = layer->GetSourceCropHeight();
  source_crop_ = layer->GetSourceCrop();
  blending_ = layer->GetBlending();
  SetBuffer(buffer_handler, layer->GetNativeHandle(), layer->GetAcquireFence());
  ValidateForOverlayUsage();
  if (previous_layer) {
    ValidatePreviousFrameState(previous_layer, layer);
  }

  if (layer->HasContentAttributesChanged() ||
      layer->HasVisibleRegionChanged() || layer->HasLayerAttributesChanged()) {
    state_ |= kClearSurface;
  }

  if (!handle_constraints) {
    UpdateSurfaceDamage(layer, previous_layer);
    return;
  }

  int32_t left_constraint = layer->GetLeftConstraint();
  int32_t right_constraint = layer->GetRightConstraint();
  if (left_constraint >= 0 && right_constraint >= 0) {
    if (display_frame_.right > right_constraint) {
      display_frame_.right = right_constraint;
    }

    if (display_frame_.left < left_constraint) {
      display_frame_.left = left_constraint;
    }

    if (display_frame_.right < right_constraint) {
      display_frame_.right =
          std::max(display_frame_.left, display_frame_.right);
    }

    if (display_frame_.left > left_constraint) {
      display_frame_.left = std::min(display_frame_.left, display_frame_.right);
    }

    if (left_constraint > 0) {
      display_frame_.left -= left_constraint;
      display_frame_.right = right_constraint - display_frame_.right;
    }

    display_frame_.bottom =
        std::min(max_height, static_cast<uint32_t>(display_frame_.bottom));
    display_frame_width_ = display_frame_.right - display_frame_.left;
    display_frame_height_ = display_frame_.bottom - display_frame_.top;
    UpdateSurfaceDamage(layer, previous_layer);
    if (gpu_rendered_) {
      surface_damage_.left =
          std::min(surface_damage_.left, display_frame_.left);
      surface_damage_.right =
          std::min(surface_damage_.right, display_frame_.right);
      surface_damage_.top = std::min(surface_damage_.top, display_frame_.top);
      surface_damage_.bottom =
          std::min(surface_damage_.bottom, display_frame_.bottom);
    }
  }

  float lconstraint = (float)layer->GetLeftSourceConstraint();
  float rconstraint = (float)layer->GetRightSourceConstraint();
  if (lconstraint >= 0 && rconstraint >= 0) {
    if (source_crop_.right > rconstraint) {
      source_crop_.right = rconstraint;
    }

    if (source_crop_.left < lconstraint) {
      source_crop_.left = lconstraint;
    }

    if (source_crop_.right < rconstraint) {
      source_crop_.right = std::max(source_crop_.left, source_crop_.right);
    }

    if (source_crop_.left > lconstraint) {
      source_crop_.left = std::min(source_crop_.left, source_crop_.right);
    }

    source_crop_width_ = static_cast<int>(ceilf(source_crop_.right) -
                                          static_cast<int>(source_crop_.left));
    source_crop_height_ = static_cast<int>(ceilf(source_crop_.bottom) -
                                           static_cast<int>(source_crop_.top));
  }
}

void OverlayLayer::InitializeFromHwcLayer(
    HwcLayer* layer, NativeBufferHandler* buffer_handler,
    OverlayLayer* previous_layer, uint32_t z_order, uint32_t layer_index,
    uint32_t max_height, HWCRotation rotation, bool handle_constraints) {
  display_frame_width_ = layer->GetDisplayFrameWidth();
  display_frame_height_ = layer->GetDisplayFrameHeight();
  display_frame_ = layer->GetDisplayFrame();
  InitializeState(layer, buffer_handler, previous_layer, z_order, layer_index,
                  max_height, rotation, handle_constraints);
}

void OverlayLayer::InitializeFromScaledHwcLayer(
    HwcLayer* layer, NativeBufferHandler* buffer_handler,
    OverlayLayer* previous_layer, uint32_t z_order, uint32_t layer_index,
    const HwcRect<int>& display_frame, uint32_t max_height,
    HWCRotation rotation, bool handle_constraints) {
  SetDisplayFrame(display_frame);
  InitializeState(layer, buffer_handler, previous_layer, z_order, layer_index,
                  max_height, rotation, handle_constraints);
}

void OverlayLayer::ValidatePreviousFrameState(OverlayLayer* rhs,
                                              HwcLayer* layer) {
  OverlayBuffer* buffer = imported_buffer_->buffer_.get();
  if (buffer->GetFormat() != rhs->imported_buffer_->buffer_->GetFormat())
    return;

  bool content_changed = false;
  bool rect_changed = layer->HasDisplayRectChanged();
  // We expect cursor plane to support alpha always.
  if (rhs->gpu_rendered_ || (type_ == kLayerCursor)) {
    content_changed = rect_changed || layer->HasContentAttributesChanged() ||
                      layer->HasLayerAttributesChanged() ||
                      layer->HasSourceRectChanged();
  } else {
    // If previous layer was opaque and we have alpha now,
    // let's mark this layer for re-validation. Plane
    // supporting XRGB format might not necessarily support
    // transparent planes. We assume plane supporting
    // ARGB will support XRGB.
    if ((rhs->alpha_ == 0xff) && (alpha_ != rhs->alpha_))
      return;

    if (blending_ != rhs->blending_)
      return;

    if (rect_changed || layer->HasLayerAttributesChanged())
      return;

    if (layer->HasSourceRectChanged()) {
      // If the overall width and height hasn't changed, it
      // shouldn't impact the plane composition results.
      if ((source_crop_width_ != rhs->source_crop_width_) ||
          (source_crop_height_ != rhs->source_crop_height_)) {
        return;
      }
    }
  }

  state_ &= ~kLayerAttributesChanged;
  gpu_rendered_ = rhs->gpu_rendered_;

  if (!rect_changed) {
    state_ &= ~kDimensionsChanged;
  }

  if (!layer->HasVisibleRegionChanged() &&
      !layer->HasSurfaceDamageRegionChanged() &&
      !layer->HasLayerContentChanged() && !content_changed) {
    state_ &= ~kLayerContentChanged;
  }
}

void OverlayLayer::ValidateForOverlayUsage() {
  const std::unique_ptr<OverlayBuffer>& buffer = imported_buffer_->buffer_;
  if (buffer->GetUsage() & kLayerCursor) {
    type_ = kLayerCursor;
  } else if (buffer->IsVideoBuffer()) {
    type_ = kLayerVideo;
  }
}

void OverlayLayer::Dump() {
  DUMPTRACE("OverlayLayer Information Starts. -------------");
  switch (blending_) {
    case HWCBlending::kBlendingNone:
      DUMPTRACE("Blending: kBlendingNone.");
      break;
    case HWCBlending::kBlendingPremult:
      DUMPTRACE("Blending: kBlendingPremult.");
      break;
    case HWCBlending::kBlendingCoverage:
      DUMPTRACE("Blending: kBlendingCoverage.");
      break;
    default:
      break;
  }

  if (transform_ & kReflectX)
    DUMPTRACE("Transform: kReflectX.");
  if (transform_ & kReflectY)
    DUMPTRACE("Transform: kReflectY.");
  if (transform_ & kReflectY)
    DUMPTRACE("Transform: kReflectY.");
  else if (transform_ & kTransform180)
    DUMPTRACE("Transform: kTransform180.");
  else if (transform_ & kTransform270)
    DUMPTRACE("Transform: kTransform270.");
  else
    DUMPTRACE("Transform: kTransform0.");

  DUMPTRACE("Alpha: %u", alpha_);

  DUMPTRACE("SourceWidth: %d", source_crop_width_);
  DUMPTRACE("SourceHeight: %d", source_crop_height_);
  DUMPTRACE("DstWidth: %d", display_frame_width_);
  DUMPTRACE("DstHeight: %d", display_frame_height_);
  DUMPTRACE("AquireFence: %d", imported_buffer_->acquire_fence_);

  imported_buffer_->buffer_->Dump();
}

}  // namespace hwcomposer
