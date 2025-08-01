// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLUTTER_FLOW_EMBEDDED_VIEWS_H_
#define FLUTTER_FLOW_EMBEDDED_VIEWS_H_

#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include "flutter/display_list/dl_builder.h"
#include "flutter/display_list/geometry/dl_geometry_conversions.h"
#include "flutter/flow/surface_frame.h"
#include "flutter/fml/memory/ref_counted.h"
#include "flutter/fml/raster_thread_merger.h"

#if IMPELLER_SUPPORTS_RENDERING
#include "flutter/impeller/display_list/aiks_context.h"  // nogncheck
#include "flutter/impeller/renderer/context.h"           // nogncheck
#else   // IMPELLER_SUPPORTS_RENDERING
namespace impeller {
class Context;
class AiksContext;
}  // namespace impeller
#endif  // !IMPELLER_SUPPORTS_RENDERING

class GrDirectContext;

namespace flutter {

enum class MutatorType {
  kClipRect,
  kClipRRect,
  kClipRSE,
  kClipPath,
  kTransform,
  kOpacity,
  kBackdropFilter
};

// Represents an image filter mutation.
//
// Should be used for image_filter_layer and backdrop_filter_layer.
class ImageFilterMutation {
 public:
  ImageFilterMutation(std::shared_ptr<DlImageFilter> filter,
                      const DlRect& filter_rect)
      : filter_(std::move(filter)), filter_rect_(filter_rect) {}

  const DlImageFilter& GetFilter() const { return *filter_; }
  const DlRect& GetFilterRect() const { return filter_rect_; }

  bool operator==(const ImageFilterMutation& other) const {
    return *filter_ == *other.filter_ && filter_rect_ == other.filter_rect_;
  }

  bool operator!=(const ImageFilterMutation& other) const {
    return !operator==(other);
  }

 private:
  std::shared_ptr<DlImageFilter> filter_;
  const DlRect filter_rect_;
};

// Stores mutation information like clipping or kTransform.
//
// The `type` indicates the type of the mutation: kClipRect, kTransform and etc.
// Each `type` is paired with an object that supports the mutation. For example,
// if the `type` is kClipRect, `rect()` is used the represent the rect to be
// clipped. One mutation object must only contain one type of mutation.
class Mutator {
 public:
  Mutator(const Mutator& other) : data_(other.data_) {}

  explicit Mutator(const DlRect& rect) : data_(rect) {}
  explicit Mutator(const DlRoundRect& rrect) : data_(rrect) {}
  explicit Mutator(const DlRoundSuperellipse& rrect) : data_(rrect) {}
  explicit Mutator(const DlPath& path) : data_(path) {}
  explicit Mutator(const DlMatrix& matrix) : data_(matrix) {}
  explicit Mutator(const uint8_t& alpha) : data_(alpha) {}
  explicit Mutator(const std::shared_ptr<DlImageFilter>& filter,
                   const DlRect& filter_rect)
      : data_(ImageFilterMutation(filter, filter_rect)) {}

  MutatorType GetType() const {
    return static_cast<MutatorType>(data_.index());
  }

  const DlRect& GetRect() const { return std::get<DlRect>(data_); }
  const DlRoundRect& GetRRect() const { return std::get<DlRoundRect>(data_); }
  const DlRoundSuperellipse& GetRSE() const {
    return std::get<DlRoundSuperellipse>(data_);
  }
  const DlRoundRect GetRSEApproximation() const {
    return GetRSE().ToApproximateRoundRect();
  }
  const DlPath& GetPath() const { return std::get<DlPath>(data_); }
  const DlMatrix& GetMatrix() const { return std::get<DlMatrix>(data_); }
  const ImageFilterMutation& GetFilterMutation() const {
    return std::get<ImageFilterMutation>(data_);
  }
  const uint8_t& GetAlpha() const { return std::get<uint8_t>(data_); }
  float GetAlphaFloat() const { return DlColor::toOpacity(GetAlpha()); }

  bool operator==(const Mutator& other) const { return data_ == other.data_; }

  bool operator!=(const Mutator& other) const { return !operator==(other); }

  bool IsClipType() {
    switch (GetType()) {
      case MutatorType::kClipRect:
      case MutatorType::kClipPath:
      case MutatorType::kClipRRect:
      case MutatorType::kClipRSE:
        return true;
      case MutatorType::kOpacity:
      case MutatorType::kTransform:
      case MutatorType::kBackdropFilter:
        return false;
    }
  }

 private:
  std::variant<DlRect,
               DlRoundRect,
               DlRoundSuperellipse,
               DlPath,
               DlMatrix,
               uint8_t,
               ImageFilterMutation>
      data_;
};  // Mutator

// A stack of mutators that can be applied to an embedded platform view.
//
// The stack may include mutators like transforms and clips, each mutator
// applies to all the mutators that are below it in the stack and to the
// embedded view.
//
// For example consider the following stack: [T1, T2, T3], where T1 is the top
// of the stack and T3 is the bottom of the stack. Applying this mutators stack
// to a platform view P1 will result in T1(T2(T3(P1))).
class MutatorsStack {
 public:
  MutatorsStack() = default;

  void PushClipRect(const DlRect& rect);
  void PushClipRRect(const DlRoundRect& rrect);
  void PushClipRSE(const DlRoundSuperellipse& rrect);
  void PushClipPath(const DlPath& path);
  void PushTransform(const DlMatrix& matrix);
  void PushOpacity(const uint8_t& alpha);
  // `filter_rect` is in global coordinates.
  void PushBackdropFilter(const std::shared_ptr<DlImageFilter>& filter,
                          const DlRect& filter_rect);

  // Removes the `Mutator` on the top of the stack
  // and destroys it.
  void Pop();

  void PopTo(size_t stack_count);

  // Returns a reverse iterator pointing to the top of the stack, which is the
  // mutator that is furtherest from the leaf node.
  const std::vector<std::shared_ptr<Mutator>>::const_reverse_iterator Top()
      const;
  // Returns a reverse iterator pointing to the bottom of the stack, which is
  // the mutator that is closeset from the leaf node.
  const std::vector<std::shared_ptr<Mutator>>::const_reverse_iterator Bottom()
      const;

  // Returns an iterator pointing to the beginning of the mutator vector, which
  // is the mutator that is furtherest from the leaf node.
  const std::vector<std::shared_ptr<Mutator>>::const_iterator Begin() const;

  // Returns an iterator pointing to the end of the mutator vector, which is the
  // mutator that is closest from the leaf node.
  const std::vector<std::shared_ptr<Mutator>>::const_iterator End() const;

  bool is_empty() const { return vector_.empty(); }
  size_t stack_count() const { return vector_.size(); }

  bool operator==(const MutatorsStack& other) const {
    if (vector_.size() != other.vector_.size()) {
      return false;
    }
    for (size_t i = 0; i < vector_.size(); i++) {
      if (*vector_[i] != *other.vector_[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator==(const std::vector<Mutator>& other) const {
    if (vector_.size() != other.size()) {
      return false;
    }
    for (size_t i = 0; i < vector_.size(); i++) {
      if (*vector_[i] != other[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const MutatorsStack& other) const {
    return !operator==(other);
  }

  bool operator!=(const std::vector<Mutator>& other) const {
    return !operator==(other);
  }

 private:
  std::vector<std::shared_ptr<Mutator>> vector_;
};  // MutatorsStack

class EmbeddedViewParams {
 public:
  EmbeddedViewParams() = default;

  EmbeddedViewParams(DlMatrix matrix,
                     DlSize size_points,
                     MutatorsStack mutators_stack)
      : matrix_(matrix),
        size_points_(size_points),
        mutators_stack_(std::move(mutators_stack)) {
    final_bounding_rect_ =
        DlRect::MakeSize(size_points).TransformAndClipBounds(matrix);
  }

  // The transformation Matrix corresponding to the sum of all the
  // transformations in the platform view's mutator stack.
  const DlMatrix& transformMatrix() const { return matrix_; };
  // The original size of the platform view before any mutation matrix is
  // applied.
  const DlSize& sizePoints() const { return size_points_; };
  // The mutators stack contains the detailed step by step mutations for this
  // platform view.
  const MutatorsStack& mutatorsStack() const { return mutators_stack_; };
  // The bounding rect of the platform view after applying all the mutations.
  //
  // Clippings are ignored.
  const DlRect& finalBoundingRect() const { return final_bounding_rect_; }

  // Pushes the stored DlImageFilter object to the mutators stack.
  //
  // `filter_rect` is in global coordinates.
  void PushImageFilter(const std::shared_ptr<DlImageFilter>& filter,
                       const DlRect& filter_rect) {
    mutators_stack_.PushBackdropFilter(filter, filter_rect);
  }

  bool operator==(const EmbeddedViewParams& other) const {
    return size_points_ == other.size_points_ &&
           mutators_stack_ == other.mutators_stack_ &&
           final_bounding_rect_ == other.final_bounding_rect_ &&
           matrix_ == other.matrix_;
  }

 private:
  DlMatrix matrix_;
  DlSize size_points_;
  MutatorsStack mutators_stack_;
  DlRect final_bounding_rect_;
};

enum class PostPrerollResult {
  // Frame has successfully rasterized.
  kSuccess,
  // Frame is submitted twice. This is currently only used when
  // thread configuration change occurs.
  kResubmitFrame,
  // Frame is dropped and a new frame with the same layer tree is
  // attempted. This is currently only used when thread configuration
  // change occurs.
  kSkipAndRetryFrame
};

// The |EmbedderViewSlice| represents the details of recording all of
// the layer tree rendering operations that appear before, after
// and between the embedded views.
class EmbedderViewSlice {
 public:
  virtual ~EmbedderViewSlice() = default;
  virtual DlCanvas* canvas() = 0;
  virtual void end_recording() = 0;
  virtual const DlRegion& getRegion() const = 0;
  DlRegion region(const DlRect& query) const {
    DlRegion rquery = DlRegion(DlIRect::RoundOut(query));
    return DlRegion::MakeIntersection(getRegion(), rquery);
  }

  virtual void render_into(DlCanvas* canvas) = 0;
};

class DisplayListEmbedderViewSlice : public EmbedderViewSlice {
 public:
  explicit DisplayListEmbedderViewSlice(DlRect view_bounds);
  ~DisplayListEmbedderViewSlice() override = default;

  DlCanvas* canvas() override;
  void end_recording() override;
  const DlRegion& getRegion() const override;

  void render_into(DlCanvas* canvas) override;
  void dispatch(DlOpReceiver& receiver);
  bool is_empty();
  bool recording_ended();

 private:
  std::unique_ptr<DisplayListBuilder> builder_;
  sk_sp<DisplayList> display_list_;
};

// Facilitates embedding of platform views within the flow layer tree.
//
// Used on iOS, Android (hybrid composite mode), and on embedded platforms
// that provide a system compositor as part of the project arguments.
//
// There are two kinds of "view IDs" in the context of ExternalViewEmbedder, and
// specific names are used to avoid ambiguation:
//
// * ExternalViewEmbedder composites a stack of layers. Each layer's content
//   might be from Flutter widgets, or a platform view, which displays platform
//   native components. Each platform view is labeled by a view ID, which
//   corresponds to the ID from `PlatformViewsRegistry.getNextPlatformViewId`
//   from the framework. In the context of `ExternalViewEmbedder`, this ID is
//   called platform_view_id.
// * The layers are compositied into a single rectangular surface, displayed by
//   taking up an entire native window or part of a window. Each such surface
//   is labeled by a view ID, which corresponds to `FlutterView.viewID` from
//   dart:ui. In the context of `ExternalViewEmbedder`, this ID is called
//   flutter_view_id.
//
// The lifecycle of drawing a frame using ExternalViewEmbedder is:
//
//   1. At the start of a frame, call |BeginFrame|, then |SetUsedThisFrame| to
//      true.
//   2. For each view to be drawn, call |PrepareFlutterView|, then
//   |SubmitFlutterView|.
//   3. At the end of a frame, if |GetUsedThisFrame| is true, call |EndFrame|.
class ExternalViewEmbedder {
  // TODO(cyanglaz): Make embedder own the `EmbeddedViewParams`.

 public:
  ExternalViewEmbedder() = default;

  virtual ~ExternalViewEmbedder() = default;

  // Deallocate the resources for displaying a view.
  //
  // This method must be called when a view is removed from the engine.
  //
  // When the ExternalViewEmbedder is requested to draw an unrecognized view, it
  // implicitly allocates necessary resources. These resources must be
  // explicitly deallocated.
  virtual void CollectView(int64_t view_id);

  // Usually, the root canvas is not owned by the view embedder. However, if
  // the view embedder wants to provide a canvas to the rasterizer, it may
  // return one here. This canvas takes priority over the canvas materialized
  // from the on-screen render target.
  virtual DlCanvas* GetRootCanvas() = 0;

  // Call this in-lieu of |SubmitFlutterView| to clear pre-roll state and
  // sets the stage for the next pre-roll.
  virtual void CancelFrame() = 0;

  // Indicates the beginning of a frame.
  //
  // The `raster_thread_merger` will be null if |SupportsDynamicThreadMerging|
  // returns false.
  virtual void BeginFrame(
      GrDirectContext* context,
      const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) = 0;

  virtual void PrerollCompositeEmbeddedView(
      int64_t platform_view_id,
      std::unique_ptr<EmbeddedViewParams> params) = 0;

  // This needs to get called after |Preroll| finishes on the layer tree.
  // Returns kResubmitFrame if the frame needs to be processed again, this is
  // after it does any requisite tasks needed to bring itself to a valid state.
  // Returns kSuccess if the view embedder is already in a valid state.
  virtual PostPrerollResult PostPrerollAction(
      const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) {
    return PostPrerollResult::kSuccess;
  }

  // Must be called on the UI thread.
  virtual DlCanvas* CompositeEmbeddedView(int64_t platform_view_id) = 0;

  // Prepare for a view to be drawn.
  virtual void PrepareFlutterView(DlISize frame_size,
                                  double device_pixel_ratio) = 0;

  // Submits the content stored since |PrepareFlutterView| to the specified
  // Flutter view.
  //
  // Implementers must submit the frame by calling frame.Submit().
  //
  // This method can mutate the root canvas before submitting the frame.
  //
  // It can also allocate frames for overlay surfaces to compose hybrid views.
  virtual void SubmitFlutterView(
      int64_t flutter_view_id,
      GrDirectContext* context,
      const std::shared_ptr<impeller::AiksContext>& aiks_context,
      std::unique_ptr<SurfaceFrame> frame);

  // This method provides the embedder a way to do additional tasks after
  // |SubmitFrame|. For example, merge task runners if `should_resubmit_frame`
  // is true.
  //
  // For example on the iOS embedder, threads are merged in this call.
  // A new frame on the platform thread starts immediately. If the GPU thread
  // still has some task running, there could be two frames being rendered
  // concurrently, which causes undefined behaviors.
  //
  // The `raster_thread_merger` will be null if |SupportsDynamicThreadMerging|
  // returns false.
  virtual void EndFrame(
      bool should_resubmit_frame,
      const fml::RefPtr<fml::RasterThreadMerger>& raster_thread_merger) {}

  // Whether the embedder should support dynamic thread merging.
  //
  // Returning `true` results a |RasterThreadMerger| instance to be created.
  // * See also |BegineFrame| and |EndFrame| for getting the
  // |RasterThreadMerger| instance.
  virtual bool SupportsDynamicThreadMerging();

  // Called when the rasterizer is being torn down.
  // This method provides a way to release resources associated with the current
  // embedder.
  virtual void Teardown();

  // Change the flag about whether it is used in this frame, it will be set to
  // true when 'BeginFrame' and false when 'EndFrame'.
  void SetUsedThisFrame(bool used_this_frame) {
    used_this_frame_ = used_this_frame;
  }

  // Whether it is used in this frame, returns true between 'BeginFrame' and
  // 'EndFrame', otherwise returns false.
  bool GetUsedThisFrame() const { return used_this_frame_; }

  // Pushes the platform view id of a visited platform view to a list of
  // visited platform views.
  virtual void PushVisitedPlatformView(int64_t platform_view_id) {}

  // Pushes a DlImageFilter object to each platform view within a list of
  // visited platform views.
  //
  // `filter_rect` is in global coordinates.
  //
  // See also: |PushVisitedPlatformView| for pushing platform view ids to the
  // visited platform views list.
  virtual void PushFilterToVisitedPlatformViews(
      const std::shared_ptr<DlImageFilter>& filter,
      const DlRect& filter_rect) {}

 private:
  bool used_this_frame_ = false;

  FML_DISALLOW_COPY_AND_ASSIGN(ExternalViewEmbedder);

};  // ExternalViewEmbedder

}  // namespace flutter

#endif  // FLUTTER_FLOW_EMBEDDED_VIEWS_H_
