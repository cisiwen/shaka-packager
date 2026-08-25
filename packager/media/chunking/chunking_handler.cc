// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/chunking_handler.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/chunking_params.h>
#include <packager/macros/status.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_sample.h>
#include <packager/media/base/stream_info.h>
#include <packager/status.h>

namespace shaka {
namespace media {
namespace {
const size_t kStreamIndex = 0;

bool IsNewSegmentIndex(int64_t new_index, int64_t current_index) {
  return new_index != current_index &&
         // Index is calculated from pts, which could decrease. We do not expect
         // it to decrease by more than one segment though, which could happen
         // only if there is a big overlap in the timeline, in which case, we
         // will create a new segment and leave it to the player to handle it.
         new_index != current_index - 1;
}

}  // namespace

ChunkingHandler::ChunkingHandler(const ChunkingParams& chunking_params)
    : chunking_params_(chunking_params) {
  CHECK_NE(chunking_params.segment_duration_in_seconds, 0u);
  segment_number_ = chunking_params.start_segment_number;
}

Status ChunkingHandler::InitializeInternal() {
  if (num_input_streams() != 1 || next_output_stream_index() != 1) {
    return Status(error::INVALID_ARGUMENT,
                  "Expects exactly one input and one output.");
  }
  return Status::OK;
}

Status ChunkingHandler::Process(std::unique_ptr<StreamData> stream_data) {
  switch (stream_data->stream_data_type) {
    case StreamDataType::kStreamInfo:
      return OnStreamInfo(std::move(stream_data->stream_info));
    case StreamDataType::kCueEvent:
      return OnCueEvent(std::move(stream_data->cue_event));
    case StreamDataType::kScte35Event:
      LOG(INFO) << "ChunkingHandler[" << this << "]: received kScte35Event, id="
                << stream_data->scte35_event->id() << " start_time="
                << stream_data->scte35_event->start_time();
      RETURN_IF_ERROR(OnScte35Event(stream_data->scte35_event));
      // The original message still needs to reach this stream's own Muxer unchanged - that's
      // what actually reports the event to the HLS/DASH notifier for DATERANGE/CUE-OUT
      // purposes, entirely independent of the segmentation reaction just performed above.
      return Dispatch(std::move(stream_data));
    case StreamDataType::kSegmentInfo:
      VLOG(3) << "Droppping existing segment info.";
      return Status::OK;
    case StreamDataType::kMediaSample:
      return OnMediaSample(std::move(stream_data->media_sample));
    default:
      VLOG(3) << "Stream data type "
              << static_cast<int>(stream_data->stream_data_type) << " ignored.";
      return Dispatch(std::move(stream_data));
  }
}

Status ChunkingHandler::OnFlushRequest(size_t /*input_stream_index*/) {
  RETURN_IF_ERROR(EndSegmentIfStarted());
  return FlushDownstream(kStreamIndex);
}

Status ChunkingHandler::OnStreamInfo(std::shared_ptr<const StreamInfo> info) {
  time_scale_ = info->time_scale();
  segment_duration_ =
      chunking_params_.segment_duration_in_seconds * time_scale_;
  subsegment_duration_ =
      chunking_params_.subsegment_duration_in_seconds * time_scale_;
  return DispatchStreamInfo(kStreamIndex, std::move(info));
}

Status ChunkingHandler::OnCueEvent(std::shared_ptr<const CueEvent> event) {
  RETURN_IF_ERROR(EndSegmentIfStarted());
  const double event_time_in_seconds = event->time_in_seconds;
  RETURN_IF_ERROR(DispatchCueEvent(kStreamIndex, std::move(event)));
  ForceSegmentBoundaryAt(event_time_in_seconds);
  return Status::OK;
}

void ChunkingHandler::ForceSegmentBoundaryAt(double event_time_in_seconds) {
  // Force start new segment after cue event.
  segment_start_time_ = std::nullopt;
  // |cue_offset_| will be applied to sample timestamp so the segment after cue
  // point have duration ~= |segment_duration_|.
  cue_offset_ = event_time_in_seconds * time_scale_;
  // Marks the segment that's about to start (whenever OnMediaSample next actually starts one) so
  // its eventual SegmentInfo carries is_cue_aligned - see that field's own doc comment.
  next_segment_cue_aligned_ = true;
}

// Live in-band SCTE-35 (parsed from a real MPEG-TS SCTE-35 PID) is a fundamentally different
// message type from CueEvent, which is exclusive to the ad_cue_generator/CueAlignmentHandler
// server-side model (a pre-planned, known-in-advance cue schedule using a shared SyncPointQueue -
// see that handler's own doc comment) and never fires for genuinely live, unplanned SCTE-35
// arriving inline in the transport stream. Before this, a kScte35Event message reached this
// handler completely unhandled (falling through to the default case, a plain pass-through to
// Muxer, which reports it to the HLS/DASH notifier purely for DATERANGE/CUE-OUT/CUE-IN reporting
// purposes) - meaning segmentation itself stayed entirely blind to every live splice point.
//
// That mattered because each stream in a program (video, audio, ...) runs its own independent
// ChunkingHandler instance with no direct communication between them (see this class's own doc
// comment on the "Consistent Chunking Algorithm" it otherwise relies on for cross-stream
// alignment) - and without an explicit trigger, only a stream whose own samples happen to cross a
// segment_duration_ grid boundary near the splice point gets an accurate cut there. In practice
// that was only ever true for video, and only because something upstream (e.g. the live encoder)
// forces an extra keyframe right at the splice point, incidentally shifting which grid bucket its
// very next sample lands in - a video-specific side effect, not a deliberate mechanism. A stream
// with no equivalent forcing mechanism (audio - real AAC/ADTS samples carry no keyframe concept,
// so nothing about a real splice point ever shifts its own segmentation) just continues on its
// regular periodic schedule regardless, so its own visible DATERANGE/CUE-OUT tag lags the true
// splice point by up to a full segment duration - confirmed against a real recording where a
// declared 15.000s ad break's video segments closed in 14.840s while audio's own segments,
// unaware anything had happened, ran a full extra ~1.92s segment past that before catching up.
//
// Reacting to this with the same underlying "force a fresh segment right after this timestamp"
// effect OnCueEvent uses (ForceSegmentBoundaryAt) closes that gap uniformly, for every stream,
// using the exact mechanism this class already trusts for forcing an aligned post-cue segment. It
// changes nothing about *whether* a given stream is allowed to cut early - segment_sap_aligned
// keyframe gating for video is completely unaffected, so video still only actually cuts on its
// own next real keyframe, exactly as before. What changes is that every stream now has an
// explicit, deliberate trigger at the true splice point instead of relying on video's own side
// effect - and since audio's samples are already unconditionally is_key_frame()==true (see
// es_parser_audio.cc), audio can now cut on the very next sample after this arrives, closing
// almost the entire gap on its own.
//
// Deliberately does NOT go through OnCueEvent itself (which would synthesize and
// DispatchCueEvent a CueEvent downstream) - that message reaching this stream's own Muxer would
// trigger MuxerListener::OnCueEvent, which for HLS output means
// SimpleHlsNotifier::NotifyCueEvent -> MediaPlaylist::AddPlacementOpportunity(): an entirely
// unrelated #EXT-X-PLACEMENT-OPPORTUNITY tag with nothing to do with this SCTE-35 event's own
// proper #EXT-X-DATERANGE/CUE-OUT/CUE-IN reporting (which happens independently, via this same
// kScte35Event message's unmodified pass-through to Muxer in Process() above) - and, worse, if
// output_file_template_ is set, Muxer::Process's own kCueEvent handling also finalizes and
// reinitializes the whole muxer. Calling ForceSegmentBoundaryNow directly gets the identical
// segmentation effect with neither side effect.
//
// This alone still leaves this stream's own reaction as an independent decision, racing whatever
// the sync source stream (e.g. video, gated on its own real keyframe) actually does - confirmed
// against real deployment tests as inconsistent, from ~150ms to a full missed GOP off. See
// ForceSegmentBoundaryNow's own doc comment for the second half of the fix: SegmentCoordinator
// drives follower streams directly from the sync source's *realized* cut instead of trusting
// each stream's own independent reaction to the raw event to land in the same place.
Status ChunkingHandler::OnScte35Event(std::shared_ptr<const SCTE35Event> event) {
  // SCTE-35 timestamps are in 90kHz ticks regardless of this stream's own time_scale_ - same
  // conversion Muxer::Process uses for its own OnSCTE35Event notification.
  return ForceSegmentBoundaryNow(static_cast<double>(event->start_time()) / 90000.0);
}

Status ChunkingHandler::ForceSegmentBoundaryNow(double event_time_in_seconds) {
  LOG(INFO) << "ChunkingHandler[" << this << "]: ForceSegmentBoundaryNow("
            << event_time_in_seconds << "s), segment_start_time_="
            << (segment_start_time_ ? *segment_start_time_ : -1)
            << " max_segment_time_=" << max_segment_time_;
  RETURN_IF_ERROR(EndSegmentIfStarted());
  ForceSegmentBoundaryAt(event_time_in_seconds);
  return Status::OK;
}

Status ChunkingHandler::OnMediaSample(
    std::shared_ptr<const MediaSample> sample) {
  DCHECK_GT(time_scale_, 0) << "kStreamInfo should arrive before kMediaSample";

  const int64_t timestamp = sample->pts();

  bool started_new_segment = false;
  const bool can_start_new_segment =
      sample->is_key_frame() || !chunking_params_.segment_sap_aligned;
  if (can_start_new_segment) {
    const int64_t segment_index =
        timestamp < cue_offset_ ? 0
                                : (timestamp - cue_offset_) / segment_duration_;
    if (!segment_start_time_ ||
        IsNewSegmentIndex(segment_index, current_segment_index_)) {
      current_segment_index_ = segment_index;
      // Reset subsegment index.
      current_subsegment_index_ = 0;

      RETURN_IF_ERROR(EndSegmentIfStarted());
      segment_start_time_ = timestamp;
      subsegment_start_time_ = timestamp;
      max_segment_time_ = timestamp + sample->duration();
      started_new_segment = true;
      // Latches whatever ForceSegmentBoundaryAt set for the segment that just ended (consumed by
      // EndSegmentIfStarted just above, using the *previous* value) onto the segment starting
      // right now, so it's this one - not the one that just ended - that gets marked
      // is_cue_aligned when it's eventually this segment's own turn to end.
      current_segment_cue_aligned_ = next_segment_cue_aligned_;
      next_segment_cue_aligned_ = false;
    }
  }

  // This handles the LL-DASH case.
  // On each media sample, which is the basis for a chunk,
  // we must increment the current_subsegment_index_
  // in order to hit FinalizeSegment() within Segmenter.
  if (!started_new_segment && chunking_params_.low_latency_dash_mode) {
    current_subsegment_index_++;

    RETURN_IF_ERROR(EndSubsegmentIfStarted());
    subsegment_start_time_ = timestamp;
  }

  // Here, a subsegment refers to a fragment that is within a segment.
  // This fragment size can be set with the 'fragment_duration' cmd arg.
  // This is NOT for the LL-DASH case.
  if (!started_new_segment && IsSubsegmentEnabled() &&
      !chunking_params_.low_latency_dash_mode) {
    const bool can_start_new_subsegment =
        sample->is_key_frame() || !chunking_params_.subsegment_sap_aligned;
    if (can_start_new_subsegment) {
      const int64_t subsegment_index =
          (timestamp - segment_start_time_.value()) / subsegment_duration_;
      if (IsNewSegmentIndex(subsegment_index, current_subsegment_index_)) {
        current_subsegment_index_ = subsegment_index;

        RETURN_IF_ERROR(EndSubsegmentIfStarted());
        subsegment_start_time_ = timestamp;
      }
    }
  }

  VLOG(3) << "Sample ts: " << timestamp << " "
          << " duration: " << sample->duration() << " scale: " << time_scale_
          << (segment_start_time_ ? " dispatch " : " discard ");
  if (!segment_start_time_) {
    DCHECK(!subsegment_start_time_);
    // Discard samples before segment start. If the segment has started,
    // |segment_start_time_| won't be null.
    return Status::OK;
  }

  segment_start_time_ = std::min(segment_start_time_.value(), timestamp);
  subsegment_start_time_ = std::min(subsegment_start_time_.value(), timestamp);
  max_segment_time_ =
      std::max(max_segment_time_, timestamp + sample->duration());
  return DispatchMediaSample(kStreamIndex, std::move(sample));
}

Status ChunkingHandler::EndSegmentIfStarted() {
  if (!segment_start_time_)
    return Status::OK;

  // Unwrap timestamps to produce monotonically increasing SegmentInfo
  // timestamps even when input PTS wraps around at 2^33
  int64_t unwrapped_start = pts_unwrapper_.Unwrap(segment_start_time_.value());
  int64_t unwrapped_max = pts_unwrapper_.Unwrap(max_segment_time_);

  auto segment_info = std::make_shared<SegmentInfo>();
  segment_info->start_timestamp = unwrapped_start;
  segment_info->duration = unwrapped_max - unwrapped_start;
  segment_info->segment_number = segment_number_++;
  segment_info->is_cue_aligned = current_segment_cue_aligned_;
  current_segment_cue_aligned_ = false;

  LOG(INFO) << "ChunkingHandler[" << this << "]: EndSegmentIfStarted segment_number="
            << segment_info->segment_number << " start=" << unwrapped_start
            << " duration=" << segment_info->duration
            << " is_cue_aligned=" << segment_info->is_cue_aligned;

  DVLOG(2) << "ChunkingHandler: Segment " << segment_info->segment_number
           << " start=" << unwrapped_start
           << " duration=" << segment_info->duration
           << " (wrapped: start=" << segment_start_time_.value()
           << " max=" << max_segment_time_ << ")";

  if (chunking_params_.low_latency_dash_mode) {
    segment_info->is_chunk = true;
    segment_info->is_final_chunk_in_seg = true;
  }

  return DispatchSegmentInfo(kStreamIndex, std::move(segment_info));
}

Status ChunkingHandler::EndSubsegmentIfStarted() const {
  if (!subsegment_start_time_)
    return Status::OK;

  auto subsegment_info = std::make_shared<SegmentInfo>();
  subsegment_info->start_timestamp = subsegment_start_time_.value();
  subsegment_info->duration =
      max_segment_time_ - subsegment_start_time_.value();
  subsegment_info->is_subsegment = true;
  if (chunking_params_.low_latency_dash_mode)
    subsegment_info->is_chunk = true;
  return DispatchSegmentInfo(kStreamIndex, std::move(subsegment_info));
}

}  // namespace media
}  // namespace shaka
