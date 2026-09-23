// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CHUNKING_CHUNKING_HANDLER_
#define PACKAGER_MEDIA_CHUNKING_CHUNKING_HANDLER_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <packager/chunking_params.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_sample.h>
#include <packager/media/base/stream_info.h>
#include <packager/media/base/timestamp_util.h>
#include <packager/status.h>

namespace shaka {
namespace media {

/// ChunkingHandler splits the samples into segments / subsegments based on the
/// specified chunking params.
/// This handler is a one-in one-out handler.
/// There can be multiple chunking handler running in different threads or even
/// different processes, we use the "consistent chunking algorithm" to make sure
/// the chunks in different streams are aligned without explicit communcating
/// with each other - which is not efficient and often difficult.
///
/// Consistent Chunking Algorithm:
///  1. Find the consistent chunkable boundary
///  Let the timestamps for video frames be (t1, t2, t3, ...). Then a
///  consistent chunkable boundary is simply the first chunkable boundary after
///  (tk / N) != (tk-1 / N), where '/' denotes integer division, and N is the
///  intended chunk duration.
///  2. Chunk only at the consistent chunkable boundary
///
/// This algorithm will make sure the chunks from different video streams are
/// aligned if they have aligned GoPs.
class ChunkingHandler : public MediaHandler {
 public:
  explicit ChunkingHandler(const ChunkingParams& chunking_params);
  ~ChunkingHandler() override = default;

  // Forces the current segment to end immediately and the next eligible sample to start a fresh
  // one at event_time_in_seconds, exactly like a live SCTE-35 event does (see OnScte35Event) -
  // but callable directly, outside the normal StreamData/Process() graph dispatch. This is how a
  // follower stream (e.g. audio) is driven by SegmentCoordinator to cut at the *actual* timestamp
  // a sync source stream (e.g. video) really cut at, once that's known - which for video, gated
  // on its own next real keyframe, may not be knowable until well after the live SCTE-35 event
  // itself first arrived. Reacting to a stream's own independent, symmetric SCTE-35 handling
  // (OnScte35Event alone) still leaves two unsynchronized decisions that can drift apart under
  // ordinary thread-scheduling jitter - confirmed against multiple real deployment tests where
  // video's own forced-keyframe timing and audio's own independent reaction diverged by anywhere
  // from ~150ms to a full missed GOP. Deriving the follower's cut directly from the sync source's
  // own realized cut removes that independent-decision race entirely.
  Status ForceSegmentBoundaryNow(double event_time_in_seconds);

  // Tells this stream "a live splice just happened somewhere - stop cutting on your own regular
  // periodic grid until told the real cut point" without yet knowing what that point is. For a
  // stream registered as a cue-follower (see SegmentCoordinator::RegisterCueFollower), the real
  // ForceSegmentBoundaryNow call carrying that point doesn't arrive until the sync source's own
  // cue-aligned segment has fully closed - by construction, up to a full segment_duration after
  // this call. Left to its own independent periodic chunking in the meantime (the original,
  // still-default behavior for a cue-follower - see that registration's own doc comment), this
  // stream almost always fits in one more of its own regular cuts before the real correction
  // lands, leaving a small leftover fragment between the two where the sync source only ever
  // needed one clean cut - confirmed live: a reference multi-CDN encoder this was checked
  // against never produces that extra fragment, keeping this stream's own segment count exactly
  // matching the sync source's, splice after splice. Suppressing the regular grid here for the
  // wait removes that fragment: this stream's current segment simply keeps extending (exactly
  // like ForceSegmentBoundaryAt's own pending_forced_boundary_ already does once the real cut is
  // known) until that real cut arrives, cutting exactly once, in one place, like the sync source
  // did. Bounded by suppress_periodic_cuts_'s own doc comment so a lost/late correction can't
  // grow a segment unboundedly.
  void SuppressPeriodicCutsUntilForcedBoundary();

 protected:
  /// @name MediaHandler implementation overrides.
  /// @{
  Status InitializeInternal() override;
  Status Process(std::unique_ptr<StreamData> stream_data) override;
  Status OnFlushRequest(size_t input_stream_index) override;
  /// @}

 private:
  friend class ChunkingHandlerTest;

  ChunkingHandler(const ChunkingHandler&) = delete;
  ChunkingHandler& operator=(const ChunkingHandler&) = delete;

  Status OnStreamInfo(std::shared_ptr<const StreamInfo> info);
  Status OnCueEvent(std::shared_ptr<const CueEvent> event);
  // Live in-band SCTE-35 (parsed from a real MPEG-TS SCTE-35 PID) arrives as this distinct
  // message type, separate from CueEvent (which is exclusive to the ad_cue_generator/
  // CueAlignmentHandler server-side model and never fires for the in-band case). Builds an
  // equivalent CueEvent from it and forwards to OnCueEvent - see chunking_handler.cc's own doc
  // comment on this case for why every stream needs to react to this uniformly, not just
  // whichever stream's own samples happen to cross a segment boundary near the splice point.
  Status OnScte35Event(std::shared_ptr<const SCTE35Event> event);
  Status OnMediaSample(std::shared_ptr<const MediaSample> sample);

  Status EndSegmentIfStarted();
  Status EndSubsegmentIfStarted() const;

  // Shared by OnCueEvent and OnScte35Event - the actual "start fresh after this timestamp"
  // effect, factored out so OnScte35Event can reuse it without OnCueEvent's own
  // DispatchCueEvent side effect (see OnScte35Event's own doc comment in the .cc for why that
  // specifically must not happen for a live in-band SCTE-35 event).
  void ForceSegmentBoundaryAt(double event_time_in_seconds);

  bool IsSubsegmentEnabled() {
    return subsegment_duration_ > 0 &&
           subsegment_duration_ != segment_duration_;
  }

  const ChunkingParams chunking_params_;

  // Segment and subsegment duration in stream's time scale.
  int64_t segment_duration_ = 0;
  int64_t subsegment_duration_ = 0;

  // Segment number that keeps monotically increasing.
  // Set to start_segment_number in constructor.
  int64_t segment_number_ = 1;

  // Current segment index, useful to determine where to do chunking.
  int64_t current_segment_index_ = -1;

  // Current subsegment index, useful to determine where to do chunking.
  int64_t current_subsegment_index_ = -1;

  std::optional<int64_t> segment_start_time_;
  std::optional<int64_t> subsegment_start_time_;
  int64_t max_segment_time_ = 0;
  int32_t time_scale_ = 0;

  // The offset is applied to sample timestamps so a full segment is generated
  // after cue points.
  int64_t cue_offset_ = 0;

  // Marks exactly one segment - the one immediately following a cue-forced boundary - as
  // is_cue_aligned in its own SegmentInfo (see that field's own doc comment). next_ is set the
  // instant a boundary is forced (ForceSegmentBoundaryAt); current_ latches it the instant the
  // *next* segment actually starts (OnMediaSample) so EndSegmentIfStarted can stamp the right
  // outgoing SegmentInfo, then both are consumed/reset so a later, unrelated segment never
  // inherits the flag.
  bool next_segment_cue_aligned_ = false;
  bool current_segment_cue_aligned_ = false;

  // Set by ForceSegmentBoundaryAt to mean "cut unconditionally at the next sample eligible to
  // start a segment" (a real keyframe, when segment_sap_aligned - the default). Deliberately a
  // separate flag from segment_start_time_: an earlier version of this used
  // segment_start_time_ = std::nullopt itself as the "force" signal, which meant every sample
  // that arrived while still waiting for that next keyframe (a live splice point almost never
  // lands exactly on one) fell into OnMediaSample's "discard samples before segment start"
  // branch below and was silently lost - up to a full partial GOP's worth of real video per cue,
  // confirmed via direct ffprobe frame-level analysis comparing the raw pre-Packager stream
  // (continuous) against Packager's own packaged output (a real gap at every cue-forced
  // boundary). segment_start_time_ now stays exactly as a normal periodic boundary leaves it -
  // still valid, still describing the segment that's logically about to close - so every
  // in-between sample keeps flowing into it (correctly extending its own duration) instead of
  // vanishing, right up until the real keyframe finally arrives and both this flag and
  // segment_start_time_ are consumed/reset together, atomically, exactly as
  // IsNewSegmentIndex(...) already does for an ordinary (non-cue) boundary.
  bool pending_forced_boundary_ = false;

  // Set by SuppressPeriodicCutsUntilForcedBoundary, cleared the moment a forced boundary is
  // finally consumed (same instant pending_forced_boundary_ is cleared) - see that method's own
  // doc comment for why this exists. While true, OnMediaSample's periodic (IsNewSegmentIndex)
  // check is skipped entirely; pending_forced_boundary_'s own unconditional cut still applies
  // normally, so this never blocks the real, eventual correction - only this stream's own
  // interim periodic cuts. suppress_periodic_cuts_started_at_ bounds how long that skip can
  // last: if a correction is lost or badly delayed, forcing the next periodic cut through anyway
  // after kMaxSuppressSeconds (a generous multiple of a normal wait) trades a possible one-time
  // small fragment for never growing a segment unboundedly.
  bool suppress_periodic_cuts_ = false;
  std::optional<int64_t> suppress_periodic_cuts_started_at_;

  // Unwraps 33-bit PTS/DTS timestamps to 64-bit monotonically increasing
  // values, handling wrap-around at 2^33. This ensures SegmentInfo timestamps
  // are always increasing even when input timestamps wrap around.
  PtsUnwrapper pts_unwrapper_;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CHUNKING_CHUNKING_HANDLER_
