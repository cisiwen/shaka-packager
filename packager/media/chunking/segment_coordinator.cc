// Copyright 2025 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/segment_coordinator.h>

#include <cstddef>
#include <memory>
#include <utility>

#include <absl/log/log.h>

#include <packager/macros/status.h>
#include <packager/media/base/media_handler.h>
#include <packager/status.h>

namespace shaka {
namespace media {

SegmentCoordinator::SegmentCoordinator() = default;

void SegmentCoordinator::MarkAsTeletextStream(size_t input_stream_index) {
  DVLOG(2) << "SegmentCoordinator: Marking stream " << input_stream_index
           << " as teletext";
  teletext_stream_indices_.insert(input_stream_index);
}

void SegmentCoordinator::RegisterCueFollower(
    size_t input_stream_index, std::shared_ptr<ChunkingHandler> follower) {
  LOG(INFO) << "SegmentCoordinator[" << this << "]: Registering stream "
            << input_stream_index << " as a cue follower, handler="
            << follower.get();
  // Hold this follower's very first segment open (same effect as suppressing it mid-stream ahead
  // of a cue correction - see that method's own doc comment) so OnSegmentInfo's startup alignment
  // below can close it at the sync source's own real first boundary instead of wherever this
  // follower's own independent PTS-modulo grid happens to fall. Safe to call here, at pipeline
  // wiring time: no samples have reached any ChunkingHandler yet.
  follower->SuppressPeriodicCutsUntilForcedBoundary();
  cue_follower_handlers_[input_stream_index] = std::move(follower);
}

void SegmentCoordinator::RegisterScte35ImmediateReceiver(
    std::shared_ptr<ChunkingHandler> handler) {
  LOG(INFO) << "SegmentCoordinator[" << this
            << "]: Registering ChunkingHandler=" << handler.get()
            << " as an immediate SCTE-35 receiver";
  scte35_immediate_receivers_.push_back(std::move(handler));
}

Status SegmentCoordinator::InitializeInternal() {
  // This handler accepts all stream types and passes them through.
  // The number of output streams equals the number of input streams.
  return Status::OK;
}

Status SegmentCoordinator::Process(std::unique_ptr<StreamData> stream_data) {
  const size_t input_stream_index = stream_data->stream_index;
  const StreamDataType stream_data_type = stream_data->stream_data_type;

  DVLOG(3) << "SegmentCoordinator::Process stream_index=" << input_stream_index
           << " type=" << StreamDataTypeToString(stream_data_type);

  // Handle StreamInfo specially too - just to record each stream's own time_scale (see
  // stream_time_scales_'s own doc comment) - always passes through unchanged either way.
  if (stream_data_type == StreamDataType::kStreamInfo) {
    RETURN_IF_ERROR(OnStreamInfo(input_stream_index, stream_data->stream_info));
    return Dispatch(std::move(stream_data));
  }

  // Handle live in-band SCTE-35 events specially - drive the sync source's own ChunkingHandler
  // directly (see RegisterPrimaryChunkingHandler's own doc comment for why this can't just be a
  // second input wired into that ChunkingHandler instance instead). The event's own unmodified
  // pass-through below still reaches this stream's own Muxer unchanged, for
  // #EXT-X-DATERANGE/CUE-OUT/CUE-IN reporting - entirely independent of the direct call just made.
  if (stream_data_type == StreamDataType::kScte35Event) {
    const double event_time_in_seconds =
        static_cast<double>(stream_data->scte35_event->start_time()) /
        90000.0;
    for (auto& receiver : scte35_immediate_receivers_) {
      LOG(INFO) << "SegmentCoordinator[" << this
                << "]: received live SCTE-35 event, driving immediate "
                << "receiver ChunkingHandler=" << receiver.get() << " to "
                << event_time_in_seconds << "s";
      RETURN_IF_ERROR(receiver->ForceSegmentBoundaryNow(event_time_in_seconds));
    }
    // Cue-follower streams (e.g. audio) don't get driven to a cut here at all - their real cut
    // doesn't come until OnSegmentInfo drives them from the sync source's own *realized* cut,
    // below. But left doing nothing until then, a follower keeps its own independent periodic
    // chunking running in the meantime (see RegisterCueFollower's own doc comment) and almost
    // always fits in one more regular cut of its own before that real correction arrives,
    // leaving a small leftover fragment neither cut needed - see
    // SuppressPeriodicCutsUntilForcedBoundary's own doc comment. Telling every follower right
    // now, at the same moment immediate receivers react, closes that gap: it does not cut
    // anything by itself, only pauses this stream's own regular grid until the real correction
    // (or that method's own safety valve) actually does.
    for (auto& entry : cue_follower_handlers_) {
      LOG(INFO) << "SegmentCoordinator[" << this
                << "]: received live SCTE-35 event, suppressing periodic cuts on "
                << "cue-follower stream " << entry.first << " until its real correction arrives";
      entry.second->SuppressPeriodicCutsUntilForcedBoundary();
    }
    return Dispatch(std::move(stream_data));
  }

  // Handle SegmentInfo specially - replicate to teletext streams
  if (stream_data_type == StreamDataType::kSegmentInfo) {
    auto info = std::move(stream_data->segment_info);

    // First, dispatch to the same output stream (pass through)
    RETURN_IF_ERROR(DispatchSegmentInfo(input_stream_index, info));

    // If this is from the intended leader (not teletext, not itself a follower - see
    // IsCueFollowerStream's own doc comment for why followers are excluded here too), consider it
    // for sync-source/teletext-replication/cue-follower-driving purposes.
    if (!IsTeletextStream(input_stream_index) &&
        !IsCueFollowerStream(input_stream_index)) {
      RETURN_IF_ERROR(OnSegmentInfo(input_stream_index, std::move(info)));
    }

    return Status::OK;
  }

  // For all other data types, pass through unchanged
  return Dispatch(std::move(stream_data));
}

Status SegmentCoordinator::OnStreamInfo(size_t input_stream_index,
                                        std::shared_ptr<const StreamInfo> info) {
  if (info) {
    stream_time_scales_[input_stream_index] = info->time_scale();
  }
  return Status::OK;
}

Status SegmentCoordinator::OnSegmentInfo(
    size_t input_stream_index,
    std::shared_ptr<const SegmentInfo> info) {
  // Only replicate full segments, not subsegments
  if (info->is_subsegment) {
    DVLOG(3) << "SegmentCoordinator: Skipping subsegment replication";
    return Status::OK;
  }

  // Nothing downstream cares about the sync source's own boundaries - skip the bookkeeping below
  // entirely. Deliberately checks both registries (not just teletext_stream_indices_, the
  // original condition here) - an audio-only cue-follower setup with no teletext at all would
  // otherwise never even reach the sync-source-determination logic below, silently disabling cue
  // following entirely.
  if (teletext_stream_indices_.empty() && cue_follower_handlers_.empty()) {
    DVLOG(3) << "SegmentCoordinator: No teletext or cue-follower streams "
             << "registered, skipping";
    return Status::OK;
  }

  // Set the sync source to the first non-teletext, non-follower stream that sends SegmentInfo.
  // This ensures we only use one stream (typically video) for alignment, avoiding issues when
  // video and audio have different segment boundaries.
  if (!sync_source_stream_index_.has_value()) {
    sync_source_stream_index_ = input_stream_index;
    LOG(INFO) << "SegmentCoordinator[" << this << "]: Set sync source to stream "
              << input_stream_index;
  }

  // Only proceed for the sync source stream
  if (input_stream_index != sync_source_stream_index_.value()) {
    DVLOG(3) << "SegmentCoordinator: Ignoring SegmentInfo from stream "
             << input_stream_index << " (sync source is stream "
             << sync_source_stream_index_.value() << ")";
    return Status::OK;
  }

  // Update latest boundary for logging
  latest_segment_boundary_ = info->start_timestamp;

  LOG(INFO) << "SegmentCoordinator[" << this
            << "]: Received SegmentInfo from sync source stream "
            << input_stream_index << " boundary=" << info->start_timestamp
            << " duration=" << info->duration
            << " segment_number=" << info->segment_number
            << " is_cue_aligned=" << info->is_cue_aligned
            << " cue_follower_count=" << cue_follower_handlers_.size();

  if (!teletext_stream_indices_.empty()) {
    DVLOG(2) << "SegmentCoordinator: Replicating segment boundary "
             << info->start_timestamp << " to "
             << teletext_stream_indices_.size() << " teletext stream(s)";
    for (size_t teletext_stream_index : teletext_stream_indices_) {
      DVLOG(3) << "SegmentCoordinator: Replicating to teletext stream "
               << teletext_stream_index;
      RETURN_IF_ERROR(DispatchSegmentInfo(teletext_stream_index, info));
    }
  }

  // Drive cue-follower streams directly the instant the sync source's own forced cut actually
  // happens (ends_at_forced_boundary - see that field's own doc comment for why this reacts to
  // the segment that *ends* at the boundary rather than waiting for is_cue_aligned on the *next*
  // segment, which needlessly adds that next segment's own full duration to every follower's
  // correction latency) - OR, once, unconditionally, for the very first segment the sync source
  // ever reports at all. That startup case matters because a follower's own periodic grid (raw
  // PTS modulo segment_duration, anchored at that follower's own arbitrary PTS origin - see
  // ChunkingHandler::OnMediaSample) shares no common reference with the sync source's own grid
  // absent this: confirmed live via a real capture with no SCTE-35 activity at all, where video's
  // and audio's own PROGRAM-DATE-TIME grids sat a stable, non-drifting ~2.4s out of phase for the
  // whole recording, because nothing had ever forced them onto a shared origin. Doing this once at
  // startup, the same way a real forced-boundary segment already does, gives both streams the
  // same absolute grid origin from the very beginning instead of only after the first splice.
  const bool is_startup_alignment = !has_aligned_followers_at_start_;
  has_aligned_followers_at_start_ = true;
  if ((info->ends_at_forced_boundary || is_startup_alignment) &&
      !cue_follower_handlers_.empty()) {
    auto scale_it = stream_time_scales_.find(input_stream_index);
    if (scale_it == stream_time_scales_.end() || scale_it->second <= 0) {
      LOG(WARNING) << "SegmentCoordinator: missing/invalid time_scale for "
                   << "sync source stream " << input_stream_index
                   << " - cannot drive cue-follower streams";
      return Status::OK;
    }
    // Startup alignment targets this (the sync source's very first) segment's own START - see
    // RegisterCueFollower's own doc comment: audio's first segment should start where video's
    // does. A forced boundary targets this segment's own END instead - that's the real splice
    // boundary; this segment's start is just wherever it happened to begin, unrelated to the cue.
    const int64_t target_timestamp = is_startup_alignment
                                          ? info->start_timestamp
                                          : info->start_timestamp + info->duration;
    const double event_time_in_seconds =
        static_cast<double>(target_timestamp) / scale_it->second;
    for (auto& entry : cue_follower_handlers_) {
      LOG(INFO) << "SegmentCoordinator[" << this
                << "]: driving cue-follower stream " << entry.first << " to "
                << event_time_in_seconds << "s (sync source's own realized "
                << (is_startup_alignment ? "startup" : "forced-boundary") << ")";
      RETURN_IF_ERROR(entry.second->ForceSegmentBoundaryNow(event_time_in_seconds));
    }
  }

  return Status::OK;
}

bool SegmentCoordinator::IsTeletextStream(size_t input_stream_index) const {
  return teletext_stream_indices_.count(input_stream_index) > 0;
}

bool SegmentCoordinator::IsCueFollowerStream(size_t input_stream_index) const {
  return cue_follower_handlers_.count(input_stream_index) > 0;
}

}  // namespace media
}  // namespace shaka
