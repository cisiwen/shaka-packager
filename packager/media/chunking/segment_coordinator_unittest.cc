// Copyright 2025 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/segment_coordinator.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <packager/chunking_params.h>
#include <packager/macros/status.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_handler_test_base.h>
#include <packager/media/base/stream_info.h>
#include <packager/media/base/scte35_event.h>
#include <packager/media/chunking/chunking_handler.h>
#include <packager/status.h>
#include <packager/status/status_test_util.h>

using ::testing::_;

namespace shaka {
namespace media {
namespace {

const size_t kThreeInputs = 3;
const size_t kThreeOutputs = 3;

const int64_t kTimescale = 90000;

const size_t kStreamIndex = 0;

// Stream indices for different stream types
const size_t kVideoStreamIndex = 0;
const size_t kAudioStreamIndex = 1;
const size_t kTeletextStreamIndex = 2;

}  // namespace

class SegmentCoordinatorTest : public MediaHandlerTestBase {
 protected:
  void SetUpCoordinator(size_t num_inputs, size_t num_outputs) {
    coordinator_ = std::make_shared<SegmentCoordinator>();
    ASSERT_OK(SetUpAndInitializeGraph(coordinator_, num_inputs, num_outputs));
  }

  void MarkAsTeletext(size_t stream_index) {
    coordinator_->MarkAsTeletextStream(stream_index);
  }

  Status DispatchStreamInfo(size_t input_index, StreamType stream_type) {
    std::shared_ptr<StreamInfo> info;
    if (stream_type == StreamType::kStreamVideo) {
      info = GetVideoStreamInfo(kTimescale);
    } else if (stream_type == StreamType::kStreamAudio) {
      info = GetAudioStreamInfo(kTimescale);
    } else {
      info = GetTextStreamInfo(kTimescale);
    }
    auto data = StreamData::FromStreamInfo(kStreamIndex, std::move(info));
    return Input(input_index)->Dispatch(std::move(data));
  }

  Status DispatchSegmentInfo(size_t input_index,
                             int64_t start_time,
                             int64_t duration,
                             int64_t segment_number) {
    auto info = std::make_shared<SegmentInfo>();
    info->start_timestamp = start_time;
    info->duration = duration;
    info->segment_number = segment_number;
    info->is_subsegment = false;
    info->is_encrypted = false;

    auto data = StreamData::FromSegmentInfo(kStreamIndex, std::move(info));
    return Input(input_index)->Dispatch(std::move(data));
  }

  Status DispatchSubsegmentInfo(size_t input_index,
                                int64_t start_time,
                                int64_t duration,
                                int64_t segment_number) {
    auto info = std::make_shared<SegmentInfo>();
    info->start_timestamp = start_time;
    info->duration = duration;
    info->segment_number = segment_number;
    info->is_subsegment = true;
    info->is_encrypted = false;

    auto data = StreamData::FromSegmentInfo(kStreamIndex, std::move(info));
    return Input(input_index)->Dispatch(std::move(data));
  }

  Status DispatchTextSample(size_t input_index,
                            int64_t start_time,
                            int64_t end_time) {
    const char* kNoId = "";
    const char* kNoPayload = "";

    auto sample = GetTextSample(kNoId, start_time, end_time, kNoPayload);
    auto data = StreamData::FromTextSample(kStreamIndex, std::move(sample));
    return Input(input_index)->Dispatch(std::move(data));
  }

  Status FlushAll(std::initializer_list<size_t> inputs) {
    for (auto& input : inputs) {
      RETURN_IF_ERROR(Input(input)->FlushAllDownstreams());
    }
    return Status::OK;
  }

  std::shared_ptr<SegmentCoordinator> coordinator_;
};

// Test 1: ReceivesAndBroadcastsSegmentInfoToTeletext
// Verify that SegmentInfo from video stream is replicated to registered
// teletext streams only.
TEST_F(SegmentCoordinatorTest, ReceivesAndBroadcastsSegmentInfoToTeletext) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);
  MarkAsTeletext(kTeletextStreamIndex);

  const int64_t kSegmentStart = 0;
  const int64_t kSegmentDuration = 4000;
  const int64_t kSegmentNumber = 1;

  {
    testing::InSequence s;

    // Video stream info passes through
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Audio stream info passes through
    EXPECT_CALL(*Output(kAudioStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Teletext stream info passes through
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Video segment info passes through to video output
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Video segment info is replicated to teletext output
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Flush all streams
    EXPECT_CALL(*Output(kVideoStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kAudioStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletextStreamIndex), OnFlush(kStreamIndex));
  }

  // Dispatch stream info for all streams
  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  ASSERT_OK(DispatchStreamInfo(kAudioStreamIndex, StreamType::kStreamAudio));
  ASSERT_OK(DispatchStreamInfo(kTeletextStreamIndex, StreamType::kStreamText));

  // Video stream dispatches SegmentInfo
  ASSERT_OK(DispatchSegmentInfo(kVideoStreamIndex, kSegmentStart,
                                kSegmentDuration, kSegmentNumber));

  // Flush all streams
  ASSERT_OK(
      FlushAll({kVideoStreamIndex, kAudioStreamIndex, kTeletextStreamIndex}));
}

// Test 2: HandlesMultipleTeletextStreams
// Verify that SegmentInfo is replicated to all registered teletext streams.
TEST_F(SegmentCoordinatorTest, HandlesMultipleTeletextStreams) {
  // Setup: 3 inputs (video, teletext1, teletext2), 3 outputs
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  const size_t kTeletext1Index = 1;
  const size_t kTeletext2Index = 2;

  MarkAsTeletext(kTeletext1Index);
  MarkAsTeletext(kTeletext2Index);

  const int64_t kSegmentStart = 0;
  const int64_t kSegmentDuration = 4000;
  const int64_t kSegmentNumber = 1;

  {
    testing::InSequence s;

    // Stream info for all streams passes through
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kTeletext1Index),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kTeletext2Index),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Video segment info passes through to video output
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Video segment info is replicated to both teletext streams
    EXPECT_CALL(*Output(kTeletext1Index),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));
    EXPECT_CALL(*Output(kTeletext2Index),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Flush all streams
    EXPECT_CALL(*Output(kVideoStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletext1Index), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletext2Index), OnFlush(kStreamIndex));
  }

  // Dispatch stream info
  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  ASSERT_OK(DispatchStreamInfo(kTeletext1Index, StreamType::kStreamText));
  ASSERT_OK(DispatchStreamInfo(kTeletext2Index, StreamType::kStreamText));

  // Video stream dispatches SegmentInfo - should replicate to both teletext
  ASSERT_OK(DispatchSegmentInfo(kVideoStreamIndex, kSegmentStart,
                                kSegmentDuration, kSegmentNumber));

  ASSERT_OK(FlushAll({kVideoStreamIndex, kTeletext1Index, kTeletext2Index}));
}

// Test 3: IgnoresNonSegmentData
// Verify that non-SegmentInfo data types pass through unchanged.
TEST_F(SegmentCoordinatorTest, IgnoresNonSegmentData) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);
  MarkAsTeletext(kTeletextStreamIndex);

  const int64_t kTextSampleStart = 1000;
  const int64_t kTextSampleEnd = 5000;

  {
    testing::InSequence s;

    // Stream info passes through
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Text sample from teletext stream passes through unchanged
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsTextSample(kStreamIndex, _, kTextSampleStart,
                                       kTextSampleEnd)));

    // Flush
    EXPECT_CALL(*Output(kVideoStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletextStreamIndex), OnFlush(kStreamIndex));
  }

  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  ASSERT_OK(DispatchStreamInfo(kTeletextStreamIndex, StreamType::kStreamText));

  // Dispatch text sample - should pass through without replication
  ASSERT_OK(DispatchTextSample(kTeletextStreamIndex, kTextSampleStart,
                               kTextSampleEnd));

  ASSERT_OK(FlushAll({kVideoStreamIndex, kTeletextStreamIndex}));
}

// Test 4: OnlyBroadcastsToRegisteredStreams
// Verify that SegmentInfo is NOT replicated to non-registered streams.
TEST_F(SegmentCoordinatorTest, OnlyBroadcastsToRegisteredStreams) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  // Only mark kTeletextStreamIndex as teletext
  // kAudioStreamIndex is NOT marked as teletext
  MarkAsTeletext(kTeletextStreamIndex);

  const int64_t kSegmentStart = 0;
  const int64_t kSegmentDuration = 4000;
  const int64_t kSegmentNumber = 1;

  {
    testing::InSequence s;

    // Stream info for all streams
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kAudioStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Video segment info passes through to video output
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Video segment info is replicated ONLY to registered teletext stream
    // Audio stream (not registered) does NOT receive replication
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Flush
    EXPECT_CALL(*Output(kVideoStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kAudioStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletextStreamIndex), OnFlush(kStreamIndex));
  }

  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  ASSERT_OK(DispatchStreamInfo(kAudioStreamIndex, StreamType::kStreamAudio));
  ASSERT_OK(DispatchStreamInfo(kTeletextStreamIndex, StreamType::kStreamText));

  // Video stream dispatches SegmentInfo
  // Should replicate to teletext but NOT to audio
  ASSERT_OK(DispatchSegmentInfo(kVideoStreamIndex, kSegmentStart,
                                kSegmentDuration, kSegmentNumber));

  ASSERT_OK(
      FlushAll({kVideoStreamIndex, kAudioStreamIndex, kTeletextStreamIndex}));
}

// Additional test: Verify subsegments are not replicated
TEST_F(SegmentCoordinatorTest, SubsegmentsNotReplicated) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);
  MarkAsTeletext(kTeletextStreamIndex);

  const int64_t kSegmentStart = 0;
  const int64_t kSegmentDuration = 1000;
  const int64_t kSegmentNumber = 1;

  {
    testing::InSequence s;

    // Stream info
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));
    EXPECT_CALL(*Output(kTeletextStreamIndex),
                OnProcess(IsStreamInfo(kStreamIndex, kTimescale, _, _)));

    // Subsegment info passes through to video output only
    // NOT replicated to teletext
    EXPECT_CALL(*Output(kVideoStreamIndex),
                OnProcess(IsSegmentInfo(kStreamIndex, kSegmentStart,
                                        kSegmentDuration, _, _)));

    // Flush
    EXPECT_CALL(*Output(kVideoStreamIndex), OnFlush(kStreamIndex));
    EXPECT_CALL(*Output(kTeletextStreamIndex), OnFlush(kStreamIndex));
  }

  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  ASSERT_OK(DispatchStreamInfo(kTeletextStreamIndex, StreamType::kStreamText));

  // Dispatch subsegment - should NOT be replicated
  ASSERT_OK(DispatchSubsegmentInfo(kVideoStreamIndex, kSegmentStart,
                                   kSegmentDuration, kSegmentNumber));

  ASSERT_OK(FlushAll({kVideoStreamIndex, kTeletextStreamIndex}));
}

// Test 6: DrivesCueFollowerOnCueAlignedSegment
// Verify that a registered cue-follower's own ChunkingHandler is driven directly (via
// ForceSegmentBoundaryNow) - cutting an in-progress segment short at the sync source's realized
// cut timestamp - when, and only when, the sync source reports a segment with
// ends_at_forced_boundary set (see that field's own doc comment for why this - not is_cue_aligned,
// which now only matters for HLS reporting - is what actually drives followers). Uses a real
// ChunkingHandler as the follower, wired standalone (FakeInputMediaHandler -> ChunkingHandler ->
// CachingMediaHandler) since MediaHandlerTestBase's own Input()/Output() bookkeeping only supports
// one handler-under-test (the coordinator itself) at a time.
TEST_F(SegmentCoordinatorTest, DrivesCueFollowerOnCueAlignedSegment) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  const int32_t kAudioTimescale = 1000;
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;  // 1000 ticks at kAudioTimescale.

  auto follower_input = std::make_shared<FakeInputMediaHandler>();
  auto follower = std::make_shared<ChunkingHandler>(chunking_params);
  auto follower_output = std::make_shared<CachingMediaHandler>();
  ASSERT_OK(MediaHandler::Chain({follower_input, follower, follower_output}));
  ASSERT_OK(follower->Initialize());

  coordinator_->RegisterCueFollower(kAudioStreamIndex, follower);

  // Follower's own stream info (establishes its time_scale) and three in-progress samples
  // (0-600 ticks accumulated so far) - this segment would naturally keep running to the full
  // 1000 ticks (segment_duration_in_seconds=1) with no outside intervention.
  ASSERT_OK(follower_input->Dispatch(
      StreamData::FromStreamInfo(kStreamIndex, GetAudioStreamInfo(kAudioTimescale))));
  for (int64_t start : {0, 200, 400}) {
    ASSERT_OK(follower_input->Dispatch(
        StreamData::FromMediaSample(kStreamIndex, GetMediaSample(start, 200, true))));
  }
  follower_output->Clear();

  // Video (sync source) reports its very first segment - deliberately consumed here, before the
  // real segment under test below, so has_aligned_followers_at_start_ is already true by then and
  // this test genuinely isolates ends_at_forced_boundary from the separate startup-alignment path
  // (see DrivesCueFollowerOnFirstSegmentEvenWithoutCueAlignment for that one).
  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  auto startup_info = std::make_shared<SegmentInfo>();
  startup_info->start_timestamp = 0;
  startup_info->duration = 1;
  startup_info->segment_number = 1;
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromSegmentInfo(kStreamIndex, startup_info)));
  follower_output->Clear();

  // Video (sync source) reports the segment that itself ends at the real splice boundary, 0.6s in
  // real time - deliberately not a round multiple of the follower's own 1s segment_duration_, to
  // prove the follower is driven by this timestamp specifically, not by its own periodic grid.
  // start_timestamp + duration (not start_timestamp alone) is the target - see
  // ends_at_forced_boundary's own doc comment for why.
  auto video_info = std::make_shared<SegmentInfo>();
  video_info->start_timestamp = 0;
  video_info->duration = static_cast<int64_t>(0.6 * kTimescale);  // video's own timescale
  video_info->segment_number = 2;
  video_info->ends_at_forced_boundary = true;
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromSegmentInfo(kStreamIndex, video_info)));

  // The follower's in-progress segment (started at 0) should already have been force-ended by
  // the call above. Feeding it one more sample - still well before its own natural 1000-tick
  // boundary - should show a fresh segment starting, proving the force landed at 0.6s (600
  // ticks at kAudioTimescale) rather than waiting for the natural schedule.
  ASSERT_OK(follower_input->Dispatch(
      StreamData::FromMediaSample(kStreamIndex, GetMediaSample(700, 200, true))));
  ASSERT_OK(follower_input->FlushAllDownstreams());

  bool found_forced_segment = false;
  for (const auto& data : follower_output->Cache()) {
    if (data->stream_data_type == StreamDataType::kSegmentInfo) {
      const auto& info = data->segment_info;
      if (info->start_timestamp == 0 && info->duration == 600) {
        found_forced_segment = true;
      }
      // No natural (unforced) 1000-tick segment should ever appear - that would mean the force
      // was ignored and the follower just ran its own regular periodic schedule instead.
      EXPECT_NE(info->duration, 1000);
    }
  }
  EXPECT_TRUE(found_forced_segment)
      << "expected a segment forced short (start=0, duration=600) by the sync source's "
      << "cue-aligned cut, matching video's own realized 0.6s splice point";
}

// Test 7: DrivesAllImmediateReceiversOnScte35Event
// Verify that EVERY registered immediate receiver (video's AND audio's ChunkingHandler, not just
// one "primary") is driven directly (via ForceSegmentBoundaryNow) - cutting an in-progress segment
// short at the live SCTE-35 event's own timestamp - when a single kScte35Event arrives on the
// coordinator, and that the event still passes through unchanged to this stream's own output (for
// Muxer/HLS DATERANGE/CUE-OUT reporting). This is what closes almost the entire alignment gap on
// each stream's own, immediately - see RegisterScte35ImmediateReceiver's own doc comment for why a
// single "primary" receiver alone left a full extra segment_duration of latency on every follower.
TEST_F(SegmentCoordinatorTest, DrivesAllImmediateReceiversOnScte35Event) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;  // 90000 ticks at kTimescale.

  auto MakeReceiver = [&]() {
    auto input = std::make_shared<FakeInputMediaHandler>();
    auto handler = std::make_shared<ChunkingHandler>(chunking_params);
    auto output = std::make_shared<CachingMediaHandler>();
    return std::make_tuple(input, handler, output);
  };

  auto [video_input, video_handler, video_output] = MakeReceiver();
  auto [audio_input, audio_handler, audio_output] = MakeReceiver();
  ASSERT_OK(MediaHandler::Chain({video_input, video_handler, video_output}));
  ASSERT_OK(MediaHandler::Chain({audio_input, audio_handler, audio_output}));
  ASSERT_OK(video_handler->Initialize());
  ASSERT_OK(audio_handler->Initialize());

  coordinator_->RegisterScte35ImmediateReceiver(video_handler);
  coordinator_->RegisterScte35ImmediateReceiver(audio_handler);

  // Both receivers' own stream info and three in-progress samples (0-54000 ticks accumulated so
  // far, matching the SCTE-35 event's own 54000-tick timestamp below) - each segment would
  // naturally keep running to the full 90000 ticks with no intervention.
  for (auto* input : {video_input.get(), audio_input.get()}) {
    ASSERT_OK(input->Dispatch(
        StreamData::FromStreamInfo(kStreamIndex, GetVideoStreamInfo(kTimescale))));
    for (int64_t start : {0, 18000, 36000}) {
      ASSERT_OK(input->Dispatch(
          StreamData::FromMediaSample(kStreamIndex, GetMediaSample(start, 18000, true))));
    }
  }
  video_output->Clear();
  audio_output->Clear();

  // A single live SCTE-35 event arrives on the video stream's own input, at 0.6s (54000 ticks at
  // 90kHz) - deliberately not a round multiple of either receiver's own 1s segment_duration_, to
  // prove both are driven by this timestamp specifically, not by their own periodic grids.
  EXPECT_CALL(*Output(kVideoStreamIndex), OnProcess(_));  // the passthrough scte35 event itself
  auto scte35_event =
      std::make_shared<SCTE35Event>("event-1", /*start_time=*/54000, /*duration=*/1350000);
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromScte35Event(kStreamIndex, scte35_event)));

  // Feeding one more sample to each - still well before the natural 90000-tick boundary - should
  // show a fresh segment starting on BOTH, proving the force landed at 54000 ticks on each
  // independently rather than waiting for their natural schedule.
  for (auto* input : {video_input.get(), audio_input.get()}) {
    ASSERT_OK(input->Dispatch(
        StreamData::FromMediaSample(kStreamIndex, GetMediaSample(60000, 18000, true))));
    ASSERT_OK(input->FlushAllDownstreams());
  }

  for (auto* output : {video_output.get(), audio_output.get()}) {
    bool found_forced_segment = false;
    for (const auto& data : output->Cache()) {
      if (data->stream_data_type == StreamDataType::kSegmentInfo) {
        const auto& info = data->segment_info;
        if (info->start_timestamp == 0 && info->duration == 54000) {
          found_forced_segment = true;
        }
        // No natural (unforced) 90000-tick segment should ever appear - that would mean the force
        // was ignored and this receiver just ran its own regular periodic schedule instead.
        EXPECT_NE(info->duration, 90000);
      }
    }
    EXPECT_TRUE(found_forced_segment)
        << "expected a segment forced short (start=0, duration=54000) by the live SCTE-35 event's "
        << "own timestamp, on every registered immediate receiver";
  }
}

// Test 8: DrivesCueFollowerOnFirstSegmentEvenWithoutCueAlignment
// Verify the new startup-alignment behavior: the sync source's very first SegmentInfo drives a
// registered cue-follower directly (via ForceSegmentBoundaryNow), exactly like a cue-aligned one
// already does, even though is_cue_aligned is false - since no real SCTE-35 splice has happened
// yet. Without this, a follower's own independent periodic grid (raw PTS modulo segment_duration)
// shares no common origin with the sync source's, and the two sit at a fixed, non-drifting phase
// offset from each other for as long as no splice occurs to correct it (confirmed against a real
// capture with no SCTE-35 activity at all, showing a stable ~2.4s PROGRAM-DATE-TIME offset between
// video and audio for the whole recording). This closes that gap by treating stream start the same
// way an actual splice is already treated.
TEST_F(SegmentCoordinatorTest, DrivesCueFollowerOnFirstSegmentEvenWithoutCueAlignment) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  const int32_t kAudioTimescale = 1000;
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;  // 1000 ticks at kAudioTimescale.

  auto follower_input = std::make_shared<FakeInputMediaHandler>();
  auto follower = std::make_shared<ChunkingHandler>(chunking_params);
  auto follower_output = std::make_shared<CachingMediaHandler>();
  ASSERT_OK(MediaHandler::Chain({follower_input, follower, follower_output}));
  ASSERT_OK(follower->Initialize());

  // RegisterCueFollower itself (not any later event) is what suppresses this follower's periodic
  // cuts from the very start - registering happens before any of the samples below are dispatched.
  coordinator_->RegisterCueFollower(kAudioStreamIndex, follower);

  // Follower's own stream info and three in-progress samples (0-600 ticks accumulated so far) -
  // this segment would naturally keep running to the full 1000 ticks (segment_duration_in_seconds
  // =1) with no outside intervention. Because RegisterCueFollower already suppressed this
  // follower's periodic cuts, it must not cut here even once samples cross what would otherwise be
  // its own natural 1000-tick boundary - proven below by feeding a sample well past that point.
  ASSERT_OK(follower_input->Dispatch(
      StreamData::FromStreamInfo(kStreamIndex, GetAudioStreamInfo(kAudioTimescale))));
  for (int64_t start : {0, 200, 400}) {
    ASSERT_OK(follower_input->Dispatch(
        StreamData::FromMediaSample(kStreamIndex, GetMediaSample(start, 200, true))));
  }
  follower_output->Clear();

  // Video (sync source) reports its very first segment - NOT cue-aligned, since no SCTE-35 splice
  // has happened - whose own realized cut lands at 0.6s in real time. Deliberately not a round
  // multiple of the follower's own 1s segment_duration_, to prove the follower is driven by this
  // timestamp specifically, not by its own periodic grid.
  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));
  auto video_info = std::make_shared<SegmentInfo>();
  video_info->start_timestamp = static_cast<int64_t>(0.6 * kTimescale);  // video's own timescale
  video_info->duration = 1;
  video_info->segment_number = 1;
  video_info->is_cue_aligned = false;
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromSegmentInfo(kStreamIndex, video_info)));

  // The follower's in-progress segment (started at 0) should already have been force-ended by the
  // call above. Feeding it one more sample - still well before its own natural 1000-tick boundary -
  // should show a fresh segment starting, proving the force landed at 0.6s (600 ticks at
  // kAudioTimescale) rather than waiting for the natural schedule.
  ASSERT_OK(follower_input->Dispatch(
      StreamData::FromMediaSample(kStreamIndex, GetMediaSample(700, 200, true))));
  ASSERT_OK(follower_input->FlushAllDownstreams());

  bool found_forced_segment = false;
  for (const auto& data : follower_output->Cache()) {
    if (data->stream_data_type == StreamDataType::kSegmentInfo) {
      const auto& info = data->segment_info;
      if (info->start_timestamp == 0 && info->duration == 600) {
        found_forced_segment = true;
      }
      // No natural (unforced) 1000-tick segment should ever appear - that would mean the
      // suppression/startup alignment was ignored and the follower just ran its own regular
      // periodic schedule instead.
      EXPECT_NE(info->duration, 1000);
    }
  }
  EXPECT_TRUE(found_forced_segment)
      << "expected a segment forced short (start=0, duration=600) by the sync source's first "
      << "segment, even though it was not cue-aligned";
}

// Test 9: DoesNotRepeatStartupAlignmentOnLaterNonCueAlignedSegments
// Verify the startup alignment above is one-shot: a second, later SegmentInfo from the sync
// source that is still not cue-aligned must NOT drive the follower again - only the very first
// segment (startup) and genuine cue-aligned segments (splices) should ever force the follower.
// Otherwise every ordinary periodic segment the sync source reports would keep yanking the
// follower back onto the sync source's own grid, defeating the point of the follower doing its
// own independent periodic chunking in steady state.
TEST_F(SegmentCoordinatorTest, DoesNotRepeatStartupAlignmentOnLaterNonCueAlignedSegments) {
  SetUpCoordinator(kThreeInputs, kThreeOutputs);

  const int32_t kAudioTimescale = 1000;
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;  // 1000 ticks at kAudioTimescale.

  auto follower_input = std::make_shared<FakeInputMediaHandler>();
  auto follower = std::make_shared<ChunkingHandler>(chunking_params);
  auto follower_output = std::make_shared<CachingMediaHandler>();
  ASSERT_OK(MediaHandler::Chain({follower_input, follower, follower_output}));
  ASSERT_OK(follower->Initialize());

  coordinator_->RegisterCueFollower(kAudioStreamIndex, follower);

  ASSERT_OK(follower_input->Dispatch(
      StreamData::FromStreamInfo(kStreamIndex, GetAudioStreamInfo(kAudioTimescale))));

  ASSERT_OK(DispatchStreamInfo(kVideoStreamIndex, StreamType::kStreamVideo));

  // First video segment (startup alignment) drives the follower to 0.6s, as in the previous test.
  auto first_video_info = std::make_shared<SegmentInfo>();
  first_video_info->start_timestamp = static_cast<int64_t>(0.6 * kTimescale);
  first_video_info->duration = 1;
  first_video_info->segment_number = 1;
  first_video_info->is_cue_aligned = false;
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromSegmentInfo(kStreamIndex, first_video_info)));

  // Follower is now anchored at 600 ticks. Feed it samples for a full natural segment
  // (600 -> 1600 ticks) so it cuts on its own regular grid, then clear the cache so only what
  // happens next is being asserted on.
  for (int64_t start : {600, 800, 1000, 1200, 1400}) {
    ASSERT_OK(follower_input->Dispatch(
        StreamData::FromMediaSample(kStreamIndex, GetMediaSample(start, 200, true))));
  }
  follower_output->Clear();

  // A second, later video segment arrives - still not cue-aligned (an ordinary periodic segment,
  // not a splice) - at a timestamp that does NOT line up with the follower's own current grid.
  // This must NOT force the follower again.
  auto second_video_info = std::make_shared<SegmentInfo>();
  second_video_info->start_timestamp = static_cast<int64_t>(2.3 * kTimescale);
  second_video_info->duration = 1;
  second_video_info->segment_number = 2;
  second_video_info->is_cue_aligned = false;
  ASSERT_OK(Input(kVideoStreamIndex)
                ->Dispatch(StreamData::FromSegmentInfo(kStreamIndex, second_video_info)));

  // Feed samples through the follower's own next natural boundary (1600 ticks, i.e. 600 + 1000).
  // If the second video segment above had wrongly re-forced it, this would show a segment ending
  // at some timestamp derived from 2300 ticks instead of the follower's own 1600-tick grid.
  for (int64_t start : {1600, 1800}) {
    ASSERT_OK(follower_input->Dispatch(
        StreamData::FromMediaSample(kStreamIndex, GetMediaSample(start, 200, true))));
  }
  ASSERT_OK(follower_input->FlushAllDownstreams());

  bool found_natural_segment = false;
  for (const auto& data : follower_output->Cache()) {
    if (data->stream_data_type == StreamDataType::kSegmentInfo) {
      const auto& info = data->segment_info;
      // The follower's own natural grid, anchored at 600 from the startup alignment, cuts next at
      // 600 + 1000 = 1600.
      if (info->start_timestamp == 600 && info->duration == 1000) {
        found_natural_segment = true;
      }
      // A duration of 1700 (2300-600) would mean the second, non-cue-aligned segment wrongly
      // re-forced the follower.
      EXPECT_NE(info->duration, 1700);
    }
  }
  EXPECT_TRUE(found_natural_segment)
      << "expected the follower to keep running its own natural grid (600 -> 1600) undisturbed "
      << "by the second, non-cue-aligned video segment";
}

}  // namespace media
}  // namespace shaka
