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

#include <algorithm>

#include "displayplanemanager.h"

#include "displayplane.h"
#include "drm/drmplane.h"
#include "factory.h"
#include "hwctrace.h"
#include "nativesurface.h"
#include "overlaylayer.h"

#include "hwcutils.h"

namespace hwcomposer {

DisplayPlaneManager::DisplayPlaneManager(DisplayPlaneHandler *plane_handler,
                                         ResourceManager *resource_manager)
    : plane_handler_(plane_handler),
      resource_manager_(resource_manager),
      cursor_plane_(nullptr),
      width_(0),
      height_(0),
      total_overlays_(0),
      display_transform_(kIdentity),
      release_surfaces_(false) {
}

DisplayPlaneManager::~DisplayPlaneManager() {
}

void DisplayPlaneManager::ResizeOverlays() {
  if (!overlay_planes_.empty()) {
    total_overlays_ = overlay_planes_.size();
    if (total_overlays_ > 1) {
      cursor_plane_ = overlay_planes_.back().get();
      // If this is a universal plane, let's not restrict it to
      // cursor usage only.
      if (cursor_plane_->IsUniversal()) {
        cursor_plane_ = NULL;
      } else {
        total_overlays_--;
      }
    }
  }
}

bool DisplayPlaneManager::Initialize(uint32_t width, uint32_t height) {
  width_ = width;
  height_ = height;
  bool status = plane_handler_->PopulatePlanes(overlay_planes_);
  ResizeOverlays();
  return status;
}

void DisplayPlaneManager::ResetPlanes(drmModeAtomicReqPtr pset) {
  for (auto j = overlay_planes_.begin(); j < overlay_planes_.end(); j++) {
    if (!j->get()->InUse()) {
      DrmPlane *drmplane = (DrmPlane *)(j->get());
      drmplane->Disable(pset);
    }
  }
}

bool DisplayPlaneManager::ValidateLayers(
    std::vector<OverlayLayer> &layers, int add_index, bool disable_overlay,
    bool *commit_checked, bool *re_validation_needed,
    DisplayPlaneStateList &composition,
    DisplayPlaneStateList &previous_composition,
    std::vector<NativeSurface *> &mark_later) {
  CTRACE();

  if (add_index <= 0) {
    if (!previous_composition.empty()) {
      for (DisplayPlaneState &plane : previous_composition) {
        MarkSurfacesForRecycling(&plane, mark_later, true);
      }
    }

    if (!composition.empty()) {
      for (DisplayPlaneState &plane : previous_composition) {
        MarkSurfacesForRecycling(&plane, mark_later, true);
      }

      DisplayPlaneStateList().swap(composition);
    }

    if (add_index <= 0) {
      ISURFACETRACE("Full validation being performed. \n");
    }
  }

  std::vector<OverlayPlane> commit_planes;

  for (DisplayPlaneState &temp : composition) {
    commit_planes.emplace_back(
        OverlayPlane(temp.GetDisplayPlane(), temp.GetOverlayLayer()));
  }

  size_t video_layers = 0;
  for (size_t lindex = add_index; lindex < layers.size(); lindex++) {
    if (layers[lindex].IsVideoLayer())
      video_layers++;
  }

  // In case we are forcing GPU composition for all layers and using a single
  // plane.
  if (disable_overlay) {
    if (!video_layers) {
      ISURFACETRACE("Forcing GPU For all layers %d %d %d %d \n",
                    disable_overlay, composition.empty(), add_index <= 0,
                    layers.size());
      ForceGpuForAllLayers(commit_planes, composition, layers, mark_later,
                           false);
    } else {
      ISURFACETRACE("Forcing VPP For all layers %d %d %d %d \n",
                    disable_overlay, composition.empty(), add_index <= 0,
                    layers.size());
      ForceVppForAllLayers(commit_planes, composition, layers, add_index,
                           mark_later, false);
    }

    *re_validation_needed = false;
    *commit_checked = true;
    return true;
  }

  auto overlay_begin = overlay_planes_.begin();
  if (!composition.empty()) {
    overlay_begin = overlay_planes_.begin() + composition.size();
  }

  // Let's mark all planes as free to be used.
  for (auto j = overlay_begin; j != overlay_planes_.end(); ++j) {
    j->get()->SetInUse(false);
  }

  size_t avail_planes = overlay_planes_.size() - composition.size();
  if (!(overlay_planes_[overlay_planes_.size() - 1]->IsUniversal()))
    avail_planes--;
  // If video layers is more than available planes
  // We are going to force all the layers bo be composited by VA path
  // cursor layer should not be handle by VPP
  if (video_layers >= avail_planes && video_layers > 0) {
    ForceVppForAllLayers(commit_planes, composition, layers, add_index,
                         mark_later, false);
    *re_validation_needed = false;
    *commit_checked = true;
    return true;
  }

  std::vector<OverlayLayer *> cursor_layers;
  auto layer_begin = layers.begin();
  auto layer_end = layers.end();
  bool validate_final_layers = false;
  bool test_commit_done = false;
  OverlayLayer *previous_layer = NULL;

  if (add_index > 0) {
    layer_begin = layers.begin() + add_index;
  }

  if (layer_begin != layer_end) {
    auto overlay_end = overlay_planes_.end();
    if (cursor_plane_) {
      overlay_end = overlay_planes_.end() - 1;
    }

    // Handle layers for overlays.
    auto j = overlay_begin;
    while (j < overlay_end) {
      DisplayPlane *plane = j->get();
      j++;
      if (previous_layer && !composition.empty()) {
        DisplayPlaneState &last_plane = composition.back();
        if (last_plane.NeedsOffScreenComposition()) {
          ValidateForDisplayScaling(composition.back(), commit_planes);
        }
      }

      // Let's break in case we have already mapped all our
      // layers.
      if (layer_begin == layer_end)
        break;

      // Handle remaining overlay planes.
      for (auto i = layer_begin; i != layer_end; ++i) {
        OverlayLayer *layer = &(*(i));
        ++layer_begin;
        // Ignore cursor layer as it will handled separately.
        if (layer->IsCursorLayer()) {
          cursor_layers.emplace_back(layer);
          continue;
        }

        bool prefer_seperate_plane = layer->PreferSeparatePlane();
        if (!prefer_seperate_plane && previous_layer) {
          prefer_seperate_plane = previous_layer->PreferSeparatePlane();
        }

        // Previous layer should not be used anywhere below, so can be
        // safely reset to current layer.
        previous_layer = layer;

        commit_planes.emplace_back(OverlayPlane(plane, layer));
        // If we are able to composite buffer with the given plane, lets use
        // it.
        bool fall_back = FallbacktoGPU(plane, layer, commit_planes);
        test_commit_done = true;
        bool force_separate = false;
        if (fall_back && !prefer_seperate_plane && !composition.empty()) {
          force_separate =
              ForceSeparatePlane(layers, composition.back(), layer);
        }

        if (!fall_back || prefer_seperate_plane || force_separate) {
          if (validate_final_layers)
            validate_final_layers = fall_back;

          composition.emplace_back(plane, layer, this, layer->GetZorder(),
                                   display_transform_);
          ISURFACETRACE(
              "Added Layer for direct Scanout: layer index: %d "
              "validate_final_layers: %d force_separate: %d fall_back: %d \n",
              layer->GetZorder(), validate_final_layers, force_separate,
              fall_back);
          plane->SetInUse(true);
          DisplayPlaneState &last_plane = composition.back();
          if (layer->IsVideoLayer()) {
            last_plane.SetVideoPlane(true);
          }
          if (fall_back) {
            if (!validate_final_layers)
              validate_final_layers = !(last_plane.GetOffScreenTarget());

            ResetPlaneTarget(last_plane, commit_planes.back());
          }

          break;
        } else {
          if (composition.empty()) {
            composition.emplace_back(plane, layer, this, layer->GetZorder(),
                                     display_transform_);
            ISURFACETRACE("Added Layer: %d %d validate_final_layers: %d  \n",
                          layer->GetZorder(), composition.size(),
                          validate_final_layers);
            DisplayPlaneState &last_plane = composition.back();
            ResetPlaneTarget(last_plane, commit_planes.back());
            validate_final_layers = true;
            if (display_transform_ != kIdentity) {
              // If DisplayTransform is not supported, let's check if
              // we can fallback to GPU rotation for this plane.
              if (last_plane.GetRotationType() ==
                  DisplayPlaneState::RotationType::kDisplayRotation) {
                last_plane.SetRotationType(
                    DisplayPlaneState::RotationType::kGPURotation, false);

                // Check if we can rotate using Display plane.
                if (FallbacktoGPU(last_plane.GetDisplayPlane(),
                                  last_plane.GetOffScreenTarget()->GetLayer(),
                                  commit_planes)) {
                  last_plane.SetRotationType(
                      DisplayPlaneState::RotationType::kGPURotation, true);
                } else {
                  validate_final_layers = false;
                }
              }
            }

            break;
          } else {
            commit_planes.pop_back();
            ISURFACETRACE("Added Layer: %d %d validate_final_layers: %d  \n",
                          layer->GetZorder(), composition.size(),
                          validate_final_layers);
            j--;
            plane = j->get();
            composition.back().AddLayer(layer);
            while (SquashPlanesAsNeeded(layers, composition, commit_planes,
                                        mark_later, &validate_final_layers)) {
              j--;
              plane = j->get();
            }

            DisplayPlaneState &last_plane = composition.back();
            if (!validate_final_layers)
              validate_final_layers = !(last_plane.GetOffScreenTarget());
            ResetPlaneTarget(last_plane, commit_planes.back());
            break;
          }
        }
      }

      /* NeedSquash
           * 1) last plane is video plane, but has source layer left unsigned,
         then need to squash due to
              can't add left layers to video plane, that will make it go to 3D
           * 2) Same reason if all planes are signed, but sitll left video
         planes
           */
      if (j == overlay_end) {
        bool needsquash =
            composition.back().IsVideoPlane() && (layer_begin != layer_end);
        if (!needsquash) {
          auto temp_iter = layer_begin;
          while (temp_iter != layer_end) {
            if ((*temp_iter).IsVideoLayer()) {
              needsquash = true;
              break;
            }
            temp_iter++;
          }
        }
        if (needsquash) {
          // squash no video plane and return the
          ITRACE("ValidateLayers Squash non video planes need");
          size_t squashed_planes =
              SquashNonVideoPlanes(layers, composition, commit_planes,
                                   mark_later, &validate_final_layers);
          j -= squashed_planes;
        }
      }
    }

    if ((layer_begin != layer_end) && (!composition.empty())) {
      bool is_video = composition.back().IsVideoPlane();
      previous_layer = NULL;
      DisplayPlaneState &last_plane = composition.back();
      // We dont have any additional planes. Pre composite remaining layers
      // to the last overlay plane.
      for (auto i = layer_begin; i != layer_end; ++i) {
        previous_layer = &(*(i));
        // Ignore cursor layer as it will handled separately.
        if (previous_layer->IsCursorLayer()) {
          cursor_layers.emplace_back(previous_layer);
          previous_layer = NULL;
          continue;
        }
        ISURFACETRACE("Added Layer: %d \n", previous_layer->GetZorder());
        last_plane.AddLayer(previous_layer);
      }

      if (composition.back().NeedsOffScreenComposition()) {
        while (SquashPlanesAsNeeded(layers, composition, commit_planes,
                                    mark_later, &validate_final_layers)) {
          continue;
        }
        DisplayPlaneState &squashed_plane = composition.back();
        // In this case we need to fallback to 3Dcomposition till Media
        // backend adds support for multiple layers.
        bool force_buffer = false;
        if (is_video && squashed_plane.GetSourceLayers().size() > 1 &&
            squashed_plane.GetOffScreenTarget()) {
          MarkSurfacesForRecycling(&squashed_plane, mark_later, true);
          force_buffer = true;
        }

        if (force_buffer || squashed_plane.NeedsSurfaceAllocation()) {
          ResetPlaneTarget(squashed_plane, commit_planes.back());
          validate_final_layers = true;
        }

        if (previous_layer) {
          squashed_plane.UsePlaneScalar(false);
        }

        commit_planes.back().layer = squashed_plane.GetOverlayLayer();
      }
    }
  }

  if (!cursor_layers.empty()) {
    ValidateCursorLayer(layers, commit_planes, cursor_layers, mark_later,
                        composition, &validate_final_layers, &test_commit_done,
                        false);

    if (validate_final_layers && add_index > 0 &&
        (composition.size() == (overlay_planes_.size() - 1))) {
      // If commit failed here and we are doing incremental validation,
      // something might be wrong with other layer+plane combinations.
      // Let's ensure DisplayQueue, checks final combination again and
      // request full validation if needed.
      *commit_checked = false;
      return true;
    }
  }

  if (composition.empty()) {
    *re_validation_needed = false;
    *commit_checked = true;
    return true;
  }

  if (validate_final_layers) {
    ValidateFinalLayers(commit_planes, composition, layers, mark_later, false,
                        add_index);
    test_commit_done = true;
  }

  bool render_layers = false;
  FinalizeValidation(composition, commit_planes, &render_layers,
                     re_validation_needed);
  *commit_checked = test_commit_done;
  return render_layers;
}

DisplayPlaneState *DisplayPlaneManager::GetLastUsedOverlay(
    DisplayPlaneStateList &composition) {
  CTRACE();

  DisplayPlaneState *last_plane = NULL;
  size_t size = composition.size();
  for (size_t i = size; i > 0; i--) {
    DisplayPlaneState &plane = composition.at(i - 1);
    if (cursor_plane_ && (cursor_plane_ == plane.GetDisplayPlane()) &&
        (!cursor_plane_->IsUniversal()))
      continue;

    last_plane = &plane;
    break;
  }

  return last_plane;
}

void DisplayPlaneManager::PreparePlaneForCursor(
    DisplayPlaneState *plane, std::vector<NativeSurface *> &mark_later,
    bool *validate_final_layers, bool reset_buffer, bool recycle_resources) {
  NativeSurface *surface = NULL;
  if (reset_buffer) {
    MarkSurfacesForRecycling(plane, mark_later, recycle_resources);
    surface = NULL;
  } else {
    surface = plane->GetOffScreenTarget();
  }

  if (!surface) {
    SetOffScreenPlaneTarget(*plane);
    *validate_final_layers = true;
  }
}

void DisplayPlaneManager::ValidateCursorLayer(
    std::vector<OverlayLayer> &all_layers,
    std::vector<OverlayPlane> &commit_planes,
    std::vector<OverlayLayer *> &cursor_layers,
    std::vector<NativeSurface *> &mark_later,
    DisplayPlaneStateList &composition, bool *validate_final_layers,
    bool *test_commit_done, bool recycle_resources) {
  CTRACE();
  if (cursor_layers.empty()) {
    return;
  }

  DisplayPlaneState *last_plane = GetLastUsedOverlay(composition);
  bool is_video = false;
  if (last_plane)
    is_video = last_plane->IsVideoPlane();

  uint32_t total_size = cursor_layers.size();
  uint32_t cursor_index = 0;
  auto overlay_end = overlay_planes_.end();
  auto overlay_begin = overlay_end - 1;
  if (total_size > 1 || !cursor_plane_) {
    overlay_begin = overlay_planes_.begin() + composition.size();
  }

  for (auto j = overlay_begin; j < overlay_end; ++j) {
    if (cursor_index == total_size)
      break;

    DisplayPlane *plane = j->get();
    if (plane->InUse()) {
      ITRACE("Trying to use a plane for cursor which is already in use. \n");
    }

    OverlayLayer *cursor_layer = cursor_layers.at(cursor_index);
    commit_planes.emplace_back(OverlayPlane(plane, cursor_layer));
    bool fall_back = FallbacktoGPU(plane, cursor_layer, commit_planes);
    *test_commit_done = true;

    // Lets ensure we fall back to GPU composition in case
    // cursor layer cannot be scanned out directly.
    if (fall_back && !is_video && last_plane) {
      commit_planes.pop_back();
      cursor_layer->SetLayerComposition(OverlayLayer::kGpu);
      ISURFACETRACE("Added CursorLayer: %d \n", cursor_layer->GetZorder());
      last_plane->AddLayer(cursor_layer);
      while (SquashPlanesAsNeeded(all_layers, composition, commit_planes,
                                  mark_later, validate_final_layers)) {
        continue;
      }

      last_plane = GetLastUsedOverlay(composition);
      if (last_plane == NULL)
        break;
      bool reset_overlay = false;
      if (!last_plane->GetOffScreenTarget())
        reset_overlay = true;

      PreparePlaneForCursor(last_plane, mark_later, validate_final_layers,
                            last_plane->IsVideoPlane(), recycle_resources);

      if (reset_overlay) {
        // Layer for the plane should have changed, reset commit planes.
        std::vector<OverlayPlane>().swap(commit_planes);
        for (DisplayPlaneState &temp : composition) {
          commit_planes.emplace_back(
              OverlayPlane(temp.GetDisplayPlane(), temp.GetOverlayLayer()));
        }
      }

      last_plane->UsePlaneScalar(false);
    } else {
      composition.emplace_back(plane, cursor_layer, this,
                               cursor_layer->GetZorder(), display_transform_);
      ISURFACETRACE("Added CursorLayer for direct scanout: %d \n",
                    cursor_layer->GetZorder());
      plane->SetInUse(true);
      if (fall_back) {
        DisplayPlaneState &temp = composition.back();
        SetOffScreenPlaneTarget(temp);
        cursor_layer->SetLayerComposition(OverlayLayer::kGpu);
        *validate_final_layers = true;
      } else {
        cursor_layer->SetLayerComposition(OverlayLayer::kDisplay);
        *validate_final_layers = false;
      }

      last_plane = GetLastUsedOverlay(composition);
      if (last_plane)
        is_video = last_plane->IsVideoPlane();
    }

    cursor_index++;
  }

  // We dont have any additional planes. Pre composite remaining cursor layers
  // to the last overlay plane.
  OverlayLayer *last_layer = NULL;
  if (!last_plane && (cursor_index < total_size))
    last_plane = GetLastUsedOverlay(composition);

  uint32_t i = cursor_index;
  while (last_plane && i < total_size) {
    OverlayLayer *cursor_layer = cursor_layers.at(i++);
    ISURFACETRACE("Added CursorLayer: %d \n", cursor_layer->GetZorder());
    last_plane->AddLayer(cursor_layer);
    cursor_layer->SetLayerComposition(OverlayLayer::kGpu);
    last_layer = cursor_layer;
    while (SquashPlanesAsNeeded(all_layers, composition, commit_planes,
                                mark_later, validate_final_layers)) {
      continue;
    }

    last_plane = GetLastUsedOverlay(composition);
  }

  if (last_layer && last_plane) {
    PreparePlaneForCursor(last_plane, mark_later, validate_final_layers,
                          last_plane->IsVideoPlane(), recycle_resources);
    last_plane->UsePlaneScalar(false);
  }
}

void DisplayPlaneManager::ValidateForDisplayTransform(
    DisplayPlaneState &last_plane,
    const std::vector<OverlayPlane> &commit_planes) {
  if (display_transform_ != kIdentity) {
    // No need for any check if we are relying on rotation during
    // 3D Composition pass.
    DisplayPlaneState::RotationType original_rotation =
        last_plane.GetRotationType();
    if (last_plane.RevalidationType() &
        DisplayPlaneState::ReValidationType::kRotation) {
      uint32_t validation_done = DisplayPlaneState::ReValidationType::kRotation;
      last_plane.SetRotationType(
          DisplayPlaneState::RotationType::kDisplayRotation, false);
      // Ensure Rotation doesn't impact the results.
      if (FallbacktoGPU(last_plane.GetDisplayPlane(),
                        last_plane.GetOffScreenTarget()->GetLayer(),
                        commit_planes)) {
        last_plane.SetRotationType(
            DisplayPlaneState::RotationType::kGPURotation, false);
      }

      last_plane.RevalidationDone(validation_done);
    }

    if (original_rotation != last_plane.GetRotationType()) {
      last_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
    }
  }
}

void DisplayPlaneManager::ValidateForDownScaling(
    DisplayPlaneState &last_plane,
    const std::vector<OverlayPlane> &commit_planes) {
#ifdef ENABLE_DOWNSCALING
  uint32_t original_downscaling_factor = last_plane.GetDownScalingFactor();
  if (last_plane.RevalidationType() &
      DisplayPlaneState::ReValidationType::kDownScaling) {
    last_plane.SetDisplayDownScalingFactor(1, false);
    if (!last_plane.IsUsingPlaneScalar() && last_plane.CanUseGPUDownScaling()) {
      last_plane.SetDisplayDownScalingFactor(4, false);
      if (!plane_handler_->TestCommit(commit_planes)) {
        last_plane.SetDisplayDownScalingFactor(1, false);
      }
    }

    uint32_t validation_done =
        DisplayPlaneState::ReValidationType::kDownScaling;
    last_plane.RevalidationDone(validation_done);
  }

  if (original_downscaling_factor != last_plane.GetDownScalingFactor()) {
    last_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
  }
#else
  HWC_UNUSED(commit_planes);
  HWC_UNUSED(last_plane);
#endif
}

void DisplayPlaneManager::ValidateForDisplayScaling(
    DisplayPlaneState &last_plane, std::vector<OverlayPlane> &commit_planes) {
  last_plane.ValidateReValidation();
  if (!(last_plane.RevalidationType() &
        DisplayPlaneState::ReValidationType::kUpScalar)) {
    return;
  }

  last_plane.RevalidationDone(DisplayPlaneState::ReValidationType::kUpScalar);

  bool old_state = last_plane.IsUsingPlaneScalar();
  if (old_state) {
    last_plane.UsePlaneScalar(false, false);
  }

  if (!last_plane.CanUseDisplayUpScaling()) {
    // If we used plane scalar, clear surfaces.
    if (old_state) {
      last_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
    }

    return;
  }

  // TODO: Scalars are limited in HW. Determine scaling ratio
  // which would really benefit vs doing it in GPU side.

  // Display frame and Source rect are different, let's check if
  // we can take advantage of scalars attached to this plane.
  if (last_plane.IsVideoPlane())
    last_plane.UsePlaneScalar(false, false);
  else
    last_plane.UsePlaneScalar(true, false);

  OverlayPlane &last_overlay_plane = commit_planes.back();
  last_overlay_plane.layer = last_plane.GetOverlayLayer();

  bool fall_back =
      FallbacktoGPU(last_plane.GetDisplayPlane(),
                    last_plane.GetOffScreenTarget()->GetLayer(), commit_planes);
  if (fall_back) {
    last_plane.UsePlaneScalar(false, false);
  }

  if (old_state != last_plane.IsUsingPlaneScalar()) {
    last_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
  }
}

void DisplayPlaneManager::ResetPlaneTarget(DisplayPlaneState &plane,
                                           OverlayPlane &overlay_plane) {
  if (plane.NeedsSurfaceAllocation()) {
    SetOffScreenPlaneTarget(plane);
  }

  overlay_plane.layer = plane.GetOverlayLayer();
}

void DisplayPlaneManager::ReleaseUnreservedPlanes(
    std::vector<uint32_t> &reserved_planes) {
  uint32_t plane_index = 0;
  for (std::vector<std::unique_ptr<DisplayPlane>>::iterator iter =
           overlay_planes_.begin();
       iter != overlay_planes_.end();) {
    if (std::find(reserved_planes.begin(), reserved_planes.end(),
                  plane_index) != reserved_planes.end())
      iter++;
    else {
      iter = overlay_planes_.erase(iter);
    }
    plane_index++;
  }
  ResizeOverlays();
}

void DisplayPlaneManager::SetOffScreenPlaneTarget(DisplayPlaneState &plane) {
  if (plane.NeedsSurfaceAllocation()) {
    EnsureOffScreenTarget(plane);
  }

  // Case where we have just one layer which needs to be composited using
  // GPU.
  plane.ForceGPURendering();
}

void DisplayPlaneManager::ReleaseAllOffScreenTargets() {
  CTRACE();
  std::vector<std::unique_ptr<NativeSurface>>().swap(surfaces_);
}

void DisplayPlaneManager::ReleaseFreeOffScreenTargets(bool forced) {
  if (!release_surfaces_ && !forced)
    return;

  std::vector<std::unique_ptr<NativeSurface>> surfaces;
  for (auto &fb : surfaces_) {
    if (fb->IsOnScreen()) {
      surfaces.emplace_back(fb.release());
    }
  }

  surfaces.swap(surfaces_);
  release_surfaces_ = false;
}

void DisplayPlaneManager::SetDisplayTransform(uint32_t transform) {
  display_transform_ = transform;
}

void DisplayPlaneManager::EnsureOffScreenTarget(DisplayPlaneState &plane) {
  NativeSurface *surface = NULL;
  // We only use media formats when video compostion for 1 layer
  int dest_x = plane.GetDisplayFrame().left;
  int dest_w = plane.GetDisplayFrame().right - dest_x;

  bool video_separate =
      plane.IsVideoPlane() && (plane.GetSourceLayers().size() == 1);
  uint32_t preferred_format = 0;
  uint32_t usage = hwcomposer::kLayerNormal;
  if (video_separate && !(dest_w % 2 || dest_x % 2)) {
    preferred_format = plane.GetDisplayPlane()->GetPreferredVideoFormat();
  } else {
    preferred_format = plane.GetDisplayPlane()->GetPreferredFormat();
  }

  uint64_t preferred_modifier =
      plane.GetDisplayPlane()->GetPreferredFormatModifier();
  if (plane.IsVideoPlane())
    preferred_modifier = 0;
  for (auto &srf : surfaces_) {
    if (srf->GetSurfaceAge() == -1) {
      OverlayBuffer *layer_buffer = srf->GetLayer()->GetBuffer();
      if (!layer_buffer)
        continue;
      uint32_t surface_format = layer_buffer->GetFormat();
      if ((preferred_format == surface_format) &&
          (preferred_modifier == srf->GetModifier())) {
        surface = srf.get();
        break;
      }
    }
  }

  if (!surface) {
    NativeSurface *new_surface = NULL;
    if (video_separate) {
      new_surface = CreateVideoSurface(width_, height_);
      usage = hwcomposer::kLayerVideo;
    } else {
      new_surface = Create3DSurface(width_, height_);
    }

    bool modifer_succeeded = false;
    new_surface->Init(resource_manager_, preferred_format, usage,
                      preferred_modifier, &modifer_succeeded);
    if (video_separate)
      new_surface->GetLayer()->SetVideoLayer(true);

    if (modifer_succeeded) {
      plane.GetDisplayPlane()->PreferredFormatModifierValidated();
    } else {
      plane.GetDisplayPlane()->BlackListPreferredFormatModifier();
    }

    surfaces_.emplace_back(std::move(new_surface));
    surface = surfaces_.back().get();
  }

  surface->SetPlaneTarget(plane);
  plane.SetOffScreenTarget(surface);
}

void DisplayPlaneManager::ValidateFinalLayers(
    std::vector<OverlayPlane> &commit_planes,
    DisplayPlaneStateList &composition, std::vector<OverlayLayer> &layers,
    std::vector<NativeSurface *> &mark_later, bool recycle_resources,
    size_t add_index) {
  bool has_video = false;
  for (DisplayPlaneState &plane : composition) {
    if (plane.NeedsOffScreenComposition() && !plane.GetOffScreenTarget()) {
      EnsureOffScreenTarget(plane);
    }
    if (!has_video && plane.IsVideoPlane())
      has_video = true;
  }

  // If this combination fails just fall back to 3D for all layers.
  if (!plane_handler_->TestCommit(commit_planes)) {
    if (!has_video)
      ForceGpuForAllLayers(commit_planes, composition, layers, mark_later,
                           recycle_resources);
    else
      ForceVppForAllLayers(commit_planes, composition, layers, add_index,
                           mark_later, false);
  }
}

bool DisplayPlaneManager::FallbacktoGPU(
    DisplayPlane *target_plane, OverlayLayer *layer,
    const std::vector<OverlayPlane> &commit_planes) const {
  // SolidColor can't be scanout directly
  if (layer->IsSolidColor())
    return true;
  // For Video, we always want to support Display Composition.
  if (layer->IsVideoLayer()) {
    layer->SupportedDisplayComposition(OverlayLayer::kAll);
  } else {
    layer->SupportedDisplayComposition(OverlayLayer::kGpu);
  }

  if (!target_plane->ValidateLayer(layer))
    return true;

  OverlayBuffer *layer_buffer = layer->GetBuffer();
  if (!layer_buffer)
    return true;

  if (layer_buffer->GetFb() == 0) {
    return true;
  }

  // TODO(kalyank): Take relevant factors into consideration to determine if
  // Plane Composition makes sense. i.e. layer size etc
  if (!plane_handler_->TestCommit(commit_planes)) {
    return true;
  }

  layer->SupportedDisplayComposition(OverlayLayer::kAll);
  return false;
}

bool DisplayPlaneManager::CheckPlaneFormat(uint32_t format) {
  return overlay_planes_.at(0)->IsSupportedFormat(format);
}

void DisplayPlaneManager::ForceVppForAllLayers(
    std::vector<OverlayPlane> &commit_planes,
    DisplayPlaneStateList &composition, std::vector<OverlayLayer> &layers,
    size_t add_index, std::vector<NativeSurface *> &mark_later,
    bool recycle_resources) {
  auto layer_begin = layers.begin() + add_index;
  // all planes already assigned, let's reset them into one plane VPP
  if (composition.size() >= overlay_planes_.size()) {
    layer_begin = layers.begin();
    for (DisplayPlaneState &plane : composition) {
      MarkSurfacesForRecycling(&plane, mark_later, recycle_resources);
    }
    DisplayPlaneStateList().swap(composition);
    std::vector<OverlayPlane>().swap(commit_planes);
    auto overlay_begin = overlay_planes_.begin();
    // Let's mark all planes as free to be used.
    for (auto j = overlay_begin; j < overlay_planes_.end(); ++j) {
      j->get()->SetInUse(false);
    }
  }

  auto layer_end = layers.end();
  OverlayLayer *primary_layer = &(*(layer_begin));
  DisplayPlane *current_plane = overlay_planes_.at(composition.size()).get();
  composition.emplace_back(current_plane, primary_layer, this,
                           primary_layer->GetZorder(), display_transform_);
  DisplayPlaneState &last_plane = composition.back();
  last_plane.ForceGPURendering();
  layer_begin++;
  ISURFACETRACE("Added layer in ForceGpuForAllLayers: %d \n",
                primary_layer->GetZorder());

  for (auto i = layer_begin; i != layer_end; ++i) {
    ISURFACETRACE("Added layer in ForceGpuForAllLayers: %d \n", i->GetZorder());
    last_plane.AddLayer(&(*(i)));
    i->SetLayerComposition(OverlayLayer::kGpu);
  }
  last_plane.SetVideoPlane(true);
  EnsureOffScreenTarget(last_plane);
  current_plane->SetInUse(true);

  commit_planes.emplace_back(
      OverlayPlane(last_plane.GetDisplayPlane(), last_plane.GetOverlayLayer()));
  // Check for Any display transform to be applied.
  ValidateForDisplayTransform(last_plane, commit_planes);
  // Check for any change to scalar usage.
  ValidateForDisplayScaling(last_plane, commit_planes);
  // Check for Downscaling.
  ValidateForDownScaling(last_plane, commit_planes);
  // Reset andy Scanout validation state.
  uint32_t validation_done = DisplayPlaneState::ReValidationType::kScanout;
  last_plane.RevalidationDone(validation_done);
}

void DisplayPlaneManager::ForceGpuForAllLayers(
    std::vector<OverlayPlane> &commit_planes,
    DisplayPlaneStateList &composition, std::vector<OverlayLayer> &layers,
    std::vector<NativeSurface *> &mark_later, bool recycle_resources) {
  // Let's mark all planes as free to be used.
  for (auto j = overlay_planes_.begin(); j != overlay_planes_.end(); ++j) {
    j->get()->SetInUse(false);
  }

  if (!composition.empty()) {
    for (DisplayPlaneState &plane : composition) {
      MarkSurfacesForRecycling(&plane, mark_later, recycle_resources);
    }
  }

  auto layer_begin = layers.begin();
  auto layer_end = layers.end();
  DisplayPlaneStateList().swap(composition);
  std::vector<OverlayPlane>().swap(commit_planes);
  OverlayLayer *primary_layer = &(*(layers.begin()));
  DisplayPlane *current_plane = overlay_planes_.at(0).get();

  composition.emplace_back(current_plane, primary_layer, this,
                           primary_layer->GetZorder(), display_transform_);
  DisplayPlaneState &last_plane = composition.back();
  last_plane.ForceGPURendering();
  layer_begin++;
  ISURFACETRACE("Added layer in ForceGpuForAllLayers: %d \n",
                primary_layer->GetZorder());

  for (auto i = layer_begin; i != layer_end; ++i) {
    ISURFACETRACE("Added layer in ForceGpuForAllLayers: %d \n", i->GetZorder());
    last_plane.AddLayer(&(*(i)));
    i->SetLayerComposition(OverlayLayer::kGpu);
  }

  EnsureOffScreenTarget(last_plane);
  current_plane->SetInUse(true);
  commit_planes.emplace_back(
      OverlayPlane(last_plane.GetDisplayPlane(), last_plane.GetOverlayLayer()));
  // Check for Any display transform to be applied.
  ValidateForDisplayTransform(last_plane, commit_planes);
  // Check for any change to scalar usage.
  ValidateForDisplayScaling(last_plane, commit_planes);
  // Check for Downscaling.
  ValidateForDownScaling(last_plane, commit_planes);
  // Reset andy Scanout validation state.
  uint32_t validation_done = DisplayPlaneState::ReValidationType::kScanout;
  last_plane.RevalidationDone(validation_done);
}

void DisplayPlaneManager::ReleasedSurfaces() {
  release_surfaces_ = true;
}

void DisplayPlaneManager::MarkSurfacesForRecycling(
    DisplayPlaneState *plane, std::vector<NativeSurface *> &mark_later,
    bool recycle_resources, bool reset_plane_surfaces) {
  const std::vector<NativeSurface *> &surfaces = plane->GetSurfaces();
  if (!surfaces.empty()) {
    release_surfaces_ = true;
    size_t size = surfaces.size();
    if (recycle_resources) {
      // Make sure we don't mark current on-screen surface or
      // one in flight. These surfaces will be added as part of
      // mark_later to be recycled later.
      for (uint32_t i = 0; i < size; i++) {
        NativeSurface *surface = surfaces.at(i);
        if (surface->GetSurfaceAge() >= 0 && surface->IsOnScreen()) {
          mark_later.emplace_back(surface);
        } else {
          surface->SetSurfaceAge(-1);
        }
      }
    } else {
      for (uint32_t i = 0; i < size; i++) {
        NativeSurface *surface = surfaces.at(i);
        surface->SetSurfaceAge(-1);
      }
    }

    if (reset_plane_surfaces)
      plane->ReleaseSurfaces();
  }
}

bool DisplayPlaneManager::ReValidatePlanes(
    DisplayPlaneStateList &composition, std::vector<OverlayLayer> &layers,
    std::vector<NativeSurface *> &mark_later, bool *request_full_validation,
    bool needs_revalidation_checks, bool re_validate_commit) {
  ISURFACETRACE(
      "ReValidatePlanes called needs_revalidation_checks %d re_validate_commit "
      "%d  \n",
      needs_revalidation_checks, re_validate_commit);
  // Let's first check the current combination works.
  *request_full_validation = false;
  bool render = false;
  bool reset_composition_region = false;
  std::vector<OverlayPlane> commit_planes;
  for (DisplayPlaneState &temp : composition) {
    commit_planes.emplace_back(
        OverlayPlane(temp.GetDisplayPlane(), temp.GetOverlayLayer()));

    if (temp.Scanout()) {
      continue;
    }

    render = true;
  }

  if (re_validate_commit) {
    // If this combination fails just fall back to full validation.
    if (!plane_handler_->TestCommit(commit_planes)) {
      ISURFACETRACE(
          "ReValidatePlanes Test commit failed. Forcing full validation. \n");
      *request_full_validation = true;
      return render;
    }
  }

  if (!needs_revalidation_checks) {
    return render;
  }

  uint32_t index = 0;

  for (DisplayPlaneState &last_plane : composition) {
    if (!last_plane.NeedsOffScreenComposition()) {
      index++;
      reset_composition_region = false;
      continue;
    }

    if (reset_composition_region) {
      last_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
    }

    reset_composition_region = false;
    uint32_t revalidation_type = last_plane.RevalidationType();

    if (!revalidation_type) {
      render = true;
      index++;
      continue;
    }

    uint32_t validation_done = DisplayPlaneState::ReValidationType::kScanout;
    if (revalidation_type & DisplayPlaneState::ReValidationType::kScanout) {
      const std::vector<size_t> &source_layers = last_plane.GetSourceLayers();
      bool uses_scalar = last_plane.IsUsingPlaneScalar();
      // Store current layer to re-set in case commit fails.
      const OverlayLayer *current_layer = last_plane.GetOverlayLayer();
      OverlayLayer *layer = &(layers.at(source_layers.at(0)));
      last_plane.SetOverlayLayer(layer);
      // Disable GPU Rendering.
      last_plane.DisableGPURendering();
      if (uses_scalar)
        last_plane.UsePlaneScalar(false, false);

      layer->SetLayerComposition(OverlayLayer::kDisplay);

      commit_planes.at(index).layer = last_plane.GetOverlayLayer();

      // If this combination fails just fall back to original state.
      if (FallbacktoGPU(last_plane.GetDisplayPlane(), layer, commit_planes)) {
        // Reset to old state.
        last_plane.ForceGPURendering();
        layer->SetLayerComposition(OverlayLayer::kGpu);
        last_plane.SetOverlayLayer(current_layer);
        commit_planes.at(index).layer = last_plane.GetOverlayLayer();
        if (uses_scalar)
          last_plane.UsePlaneScalar(true, false);
      } else {
        ISURFACETRACE("ReValidatePlanes called: moving to scan \n");
        MarkSurfacesForRecycling(&last_plane, mark_later, true);
        last_plane.SetOverlayLayer(layer);
        reset_composition_region = true;
      }
    }

    render = true;
    index++;
    if (revalidation_type & DisplayPlaneState::ReValidationType::kUpScalar) {
      ValidateForDisplayScaling(last_plane, commit_planes);
      validation_done |= DisplayPlaneState::ReValidationType::kUpScalar;
    }

    if (revalidation_type & DisplayPlaneState::ReValidationType::kRotation) {
      validation_done |= DisplayPlaneState::ReValidationType::kRotation;
      // Save old rotation type.
      DisplayPlaneState::RotationType old_type = last_plane.GetRotationType();
      DisplayPlaneState::RotationType new_type = old_type;
      if (old_type == DisplayPlaneState::RotationType::kGPURotation) {
        last_plane.SetRotationType(
            DisplayPlaneState::RotationType::kDisplayRotation, false);
      } else if (re_validate_commit) {
        // We should have already done a full commit check above.
        // As their is no state change we can avoid another test
        // commit here.
        last_plane.RevalidationDone(validation_done);
        continue;
      }

      // Check if we can rotate using Display plane.
      EnsureOffScreenTarget(last_plane);
      if (FallbacktoGPU(last_plane.GetDisplayPlane(),
                        last_plane.GetOffScreenTarget()->GetLayer(),
                        commit_planes)) {
        new_type = DisplayPlaneState::RotationType::kGPURotation;
      }

      if (old_type != new_type) {
        // Set new rotation type. Clear surfaces in case type has changed.
        last_plane.SetRotationType(new_type, true);
      }
    }

    if (revalidation_type & DisplayPlaneState::ReValidationType::kDownScaling) {
      validation_done |= DisplayPlaneState::ReValidationType::kDownScaling;
      // Make sure we are not handling upscaling.
      if (last_plane.IsUsingPlaneScalar()) {
        ITRACE(
            "We are using upscaling and also trying to validate for "
            "downscaling \n");
        if (last_plane.GetDownScalingFactor() > 1)
          last_plane.SetDisplayDownScalingFactor(1, true);
      } else {
        // Check for Downscaling.
        ValidateForDownScaling(last_plane, commit_planes);
      }
    }

    last_plane.RevalidationDone(validation_done);
  }

  return render;
}

void DisplayPlaneManager::FinalizeValidation(
    DisplayPlaneStateList &composition,
    std::vector<OverlayPlane> &commit_planes, bool *render_layers,
    bool *re_validation_needed) {
  bool re_validation = false;
  bool needs_gpu = false;
  for (DisplayPlaneState &plane : composition) {
    if (plane.NeedsOffScreenComposition()) {
      plane.RefreshSurfaces(NativeSurface::kFullClear);
      plane.ValidateReValidation();
      // Check for Any display transform to be applied.
      ValidateForDisplayTransform(plane, commit_planes);

      // Check for Downscaling.
      ValidateForDownScaling(plane, commit_planes);

      if (!needs_gpu) {
        needs_gpu = !plane.IsSurfaceRecycled();
      }

      if (plane.RevalidationType() !=
          DisplayPlaneState::ReValidationType::kNone) {
        re_validation = true;
      }
    }
  }

  if (re_validation_needed)
    *re_validation_needed = re_validation;

  if (render_layers)
    *render_layers = needs_gpu;
}

size_t DisplayPlaneManager::SquashNonVideoPlanes(
    const std::vector<OverlayLayer> &layers, DisplayPlaneStateList &composition,
    std::vector<OverlayPlane> &commit_planes,
    std::vector<NativeSurface *> &mark_later, bool *validate_final_layers) {
  size_t composition_index = composition.size() - 1;
  size_t squashed_count = 0;

  while (composition_index > 0) {
    DisplayPlaneState &last_plane = composition.at(composition_index);
    DisplayPlaneState &scanout_plane = composition.at(composition_index - 1);

    if (!last_plane.IsVideoPlane() && !scanout_plane.IsVideoPlane()) {
      ISURFACETRACE("Squasing non video planes. \n");
      const std::vector<size_t> &new_layers = last_plane.GetSourceLayers();
      for (const size_t &index : new_layers) {
        scanout_plane.AddLayer(&(layers.at(index)));
      }

      scanout_plane.RefreshSurfaces(NativeSurface::kFullClear, true);
      last_plane.GetDisplayPlane()->SetInUse(false);
      MarkSurfacesForRecycling(&last_plane, mark_later, true);
      size_t top = composition.size() - 1;

      while (top > composition_index) {
        DisplayPlaneState &temp = composition.at(top);
        DisplayPlaneState &temp1 = composition.at(top - 1);
        temp.SetDisplayPlane(temp1.GetDisplayPlane());
        top--;
      }
      composition.erase(composition.begin() + composition_index);
      squashed_count++;
      if (scanout_plane.NeedsSurfaceAllocation()) {
        SetOffScreenPlaneTarget(scanout_plane);
        *validate_final_layers = true;
      }
    }
    composition_index--;
  }

  if (!commit_planes.empty() && squashed_count) {
    // Layer for the plane should have changed, reset commit planes.
    std::vector<OverlayPlane>().swap(commit_planes);
    for (DisplayPlaneState &temp : composition) {
      commit_planes.emplace_back(
          OverlayPlane(temp.GetDisplayPlane(), temp.GetOverlayLayer()));
    }
  }

  return squashed_count;
}

bool DisplayPlaneManager::SquashPlanesAsNeeded(
    const std::vector<OverlayLayer> &layers, DisplayPlaneStateList &composition,
    std::vector<OverlayPlane> &commit_planes,
    std::vector<NativeSurface *> &mark_later, bool *validate_final_layers) {
  bool status = false;
  if (composition.size() > 1) {
    DisplayPlaneState &last_plane = composition.back();
    DisplayPlaneState &scanout_plane = composition.at(composition.size() - 2);
    ISURFACETRACE(
        "ANALAYZE scanout_plane: scanout_plane.NeedsOffScreenComposition() %d "
        "scanout_plane.IsCursorPlane() %d scanout_plane.IsVideoPlane() %d  \n",
        scanout_plane.NeedsOffScreenComposition(),
        scanout_plane.IsCursorPlane(), scanout_plane.IsVideoPlane());

    ISURFACETRACE(
        "ANALAYZE last_plane: scanout_plane.NeedsOffScreenComposition() %d "
        "scanout_plane.IsCursorPlane() %d scanout_plane.IsVideoPlane() %d  \n",
        scanout_plane.NeedsOffScreenComposition(),
        scanout_plane.IsCursorPlane(), scanout_plane.IsVideoPlane());

    if (!scanout_plane.IsCursorPlane() && !scanout_plane.IsVideoPlane()) {
      ISURFACETRACE("ANALAYZE AnalyseOverlap: %d \n",
                    AnalyseOverlap(scanout_plane.GetDisplayFrame(),
                                   last_plane.GetDisplayFrame()));
      ISURFACETRACE("ANALAYZE Scanout Display Rect %d %d %d %d \n",
                    scanout_plane.GetDisplayFrame().left,
                    scanout_plane.GetDisplayFrame().top,
                    scanout_plane.GetDisplayFrame().right,
                    scanout_plane.GetDisplayFrame().bottom);
      ISURFACETRACE("ANALAYZE Last offscreen plane rect %d %d %d %d \n",
                    last_plane.GetDisplayFrame().left,
                    last_plane.GetDisplayFrame().top,
                    last_plane.GetDisplayFrame().right,
                    last_plane.GetDisplayFrame().bottom);
    }
    const HwcRect<int> &display_frame = scanout_plane.GetDisplayFrame();
    const HwcRect<int> &target_frame = last_plane.GetDisplayFrame();
    if (!scanout_plane.IsCursorPlane() && !scanout_plane.IsVideoPlane() &&
        (AnalyseOverlap(display_frame, target_frame) != kOutside)) {
      if (!ForceSeparatePlane(layers, last_plane, NULL)) {
        ISURFACETRACE("Squasing planes. \n");
        const std::vector<size_t> &new_layers = last_plane.GetSourceLayers();
        for (const size_t &index : new_layers) {
          scanout_plane.AddLayer(&(layers.at(index)));
        }

        scanout_plane.RefreshSurfaces(NativeSurface::kFullClear, true);

        last_plane.GetDisplayPlane()->SetInUse(false);
        MarkSurfacesForRecycling(&last_plane, mark_later, true);
        composition.pop_back();
        status = true;

        DisplayPlaneState &squashed_plane = composition.back();
        if (squashed_plane.NeedsSurfaceAllocation()) {
          SetOffScreenPlaneTarget(squashed_plane);
          *validate_final_layers = true;
        }

        if (!commit_planes.empty()) {
          // Layer for the plane should have changed, reset commit planes.
          std::vector<OverlayPlane>().swap(commit_planes);
          for (DisplayPlaneState &temp : composition) {
            commit_planes.emplace_back(
                OverlayPlane(temp.GetDisplayPlane(), temp.GetOverlayLayer()));
          }
        }
      }
    }
  }

  return status;
}

bool DisplayPlaneManager::ForceSeparatePlane(
    const std::vector<OverlayLayer> &layers,
    const DisplayPlaneState &last_plane, const OverlayLayer *target_layer) {
  const std::vector<size_t> &new_layers = last_plane.GetSourceLayers();
  const HwcRect<int> &display_frame = last_plane.GetDisplayFrame();
  HwcRect<int> target_display_frame = display_frame;
  uint32_t total_width = 0;
  uint32_t total_height = 0;

  if (!target_layer) {
    if (!(last_plane.IsVideoPlane() || last_plane.IsCursorPlane()))
      return false;
    else
      return true;
  }

  total_width = target_layer->GetDisplayFrameWidth();
  total_height = target_layer->GetDisplayFrameHeight();
  target_display_frame = target_layer->GetDisplayFrame();
  CalculateRect(display_frame, target_display_frame);

  bool force_separate = false;
  for (const size_t &index : new_layers) {
    const OverlayLayer &layer = layers.at(index);
    total_width = std::max(total_width, layer.GetDisplayFrameWidth());
    total_height = std::max(total_height, layer.GetDisplayFrameHeight());
  }

  uint32_t target_width =
      target_display_frame.right - target_display_frame.left;
  uint32_t target_height =
      target_display_frame.bottom - target_display_frame.top;
  if ((total_width != target_width) || (total_height != target_height)) {
    force_separate = true;
  }

  return force_separate;
}

}  // namespace hwcomposer
