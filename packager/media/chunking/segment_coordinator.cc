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

  // Drive cue-follower streams directly, only for a cue-aligned segment (i.e. only when the sync
  // source actually just cut a segment at a live splice point - see RegisterCueFollower's own
  // doc comment for why *every* segment isn't relevant here the way it is for teletext).
  if (info->is_cue_aligned && !cue_follower_handlers_.empty()) {
    auto scale_it = stream_time_scales_.find(input_stream_index);
    if (scale_it == stream_time_scales_.end() || scale_it->second <= 0) {
      LOG(WARNING) << "SegmentCoordinator: missing/invalid time_scale for "
                   << "sync source stream " << input_stream_index
                   << " - cannot drive cue-follower streams";
      return Status::OK;
    }
    const double event_time_in_seconds =
        static_cast<double>(info->start_timestamp) / scale_it->second;
    for (auto& entry : cue_follower_handlers_) {
      LOG(INFO) << "SegmentCoordinator[" << this
                << "]: driving cue-follower stream " << entry.first << " to "
                << event_time_in_seconds << "s (sync source's own realized cut)";
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
