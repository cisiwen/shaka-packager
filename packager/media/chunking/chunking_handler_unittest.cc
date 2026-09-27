// Copyright 2017 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/chunking/chunking_handler.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <packager/chunking_params.h>
#include <packager/media/base/media_handler.h>
#include <packager/media/base/media_handler_test_base.h>
#include <packager/status.h>
#include <packager/status/status_test_util.h>

using ::testing::_;
using ::testing::ElementsAre;
using ::testing::IsEmpty;

namespace shaka {
namespace media {
namespace {
const size_t kStreamIndex = 0;
const int32_t kTimeScale0 = 800;
const int32_t kTimeScale1 = 1000;
const int64_t kDuration = 300;
const bool kKeyFrame = true;
const bool kIsSubsegment = true;
const bool kEncrypted = true;

}  // namespace

class ChunkingHandlerTest : public MediaHandlerGraphTestBase {
 public:
  void SetUpChunkingHandler(int num_inputs,
                            const ChunkingParams& chunking_params) {
    chunking_handler_.reset(new ChunkingHandler(chunking_params));
    SetUpGraph(num_inputs, num_inputs, chunking_handler_);
    ASSERT_OK(chunking_handler_->Initialize());
  }

  Status Process(std::unique_ptr<StreamData> stream_data) {
    return chunking_handler_->Process(std::move(stream_data));
  }

  Status OnFlushRequest(int stream_index) {
    return chunking_handler_->OnFlushRequest(stream_index);
  }

 protected:
  std::shared_ptr<ChunkingHandler> chunking_handler_;
};

TEST_F(ChunkingHandlerTest, AudioNoSubsegmentsThenFlush) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetAudioStreamInfo(kTimeScale0))));
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(IsStreamInfo(kStreamIndex, kTimeScale0, !kEncrypted, _)));

  for (int i = 0; i < 5; ++i) {
    ClearOutputStreamDataVector();
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kDuration, kDuration, kKeyFrame))));
    // One output stream_data except when i == 3, which also has SegmentInfo.
    if (i == 3) {
      EXPECT_THAT(GetOutputStreamDataVector(),
                  ElementsAre(IsSegmentInfo(kStreamIndex, 0, kDuration * 3,
                                            !kIsSubsegment, !kEncrypted),
                              IsMediaSample(kStreamIndex, i * kDuration,
                                            kDuration, !kEncrypted, _)));
    } else {
      EXPECT_THAT(GetOutputStreamDataVector(),
                  ElementsAre(IsMediaSample(kStreamIndex, i * kDuration,
                                            kDuration, !kEncrypted, _)));
    }
  }

  ClearOutputStreamDataVector();
  ASSERT_OK(OnFlushRequest(kStreamIndex));
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(IsSegmentInfo(kStreamIndex, kDuration * 3, kDuration * 2,
                                !kIsSubsegment, !kEncrypted)));
}

TEST_F(ChunkingHandlerTest, AudioWithSubsegments) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.5;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetAudioStreamInfo(kTimeScale0))));
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kDuration, kDuration, kKeyFrame))));
  }
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale0, !kEncrypted, _),
          IsMediaSample(kStreamIndex, 0, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kDuration, kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kDuration * 2, kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, 2 * kDuration, kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kDuration * 3, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, 3 * kDuration, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, 4 * kDuration, kDuration, !kEncrypted,
                        _)));
}

TEST_F(ChunkingHandlerTest, VideoAndSubsegmentAndNonzeroStart) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.3;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  const int64_t kVideoStartTimestamp = 12345;
  for (int i = 0; i < 6; ++i) {
    // Alternate key frame.
    const bool is_key_frame = (i % 2) == 1;
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, is_key_frame))));
  }
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale1, !kEncrypted, _),
          // The first samples @ kStartTimestamp is discarded - not key frame.
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          // The next segment boundary 13245 / 1000 != 12645 / 1000.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 2, !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          // The subsegment has duration kDuration * 2 since it can only
          // terminate before key frame.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration * 2, kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

// Live in-band SCTE-35 must force an early segment exactly like CueEvent does (see
// ChunkingHandler::OnScte35Event's own doc comment on why audio/video would otherwise stay
// misaligned around a real ad break) - but, unlike CueEvent, must NOT synthesize/dispatch a
// CueEvent downstream: that message reaching this stream's own Muxer would trigger an unrelated
// #EXT-X-PLACEMENT-OPPORTUNITY tag (MuxerListener::OnCueEvent) instead of the proper
// #EXT-X-DATERANGE/CUE-OUT/CUE-IN reporting, which the unmodified kScte35Event passthrough
// already drives independently.
MATCHER_P2(IsScte35EventPassthrough, stream_index, event, "") {
  return arg->stream_index == static_cast<size_t>(stream_index) &&
         arg->stream_data_type == StreamDataType::kScte35Event &&
         arg->scte35_event == event;
}

TEST_F(ChunkingHandlerTest, Scte35Event) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.5;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kVideoStartTimestamp = 12345;
  const double kCueTimeInSeconds =
      static_cast<double>(kVideoStartTimestamp + kDuration) / kTimeScale1;
  // SCTE-35 timestamps are always in 90kHz ticks, independent of this stream's own time_scale_ -
  // see OnScte35Event's own conversion.
  const int64_t kScte35StartTime90k =
      static_cast<int64_t>(kCueTimeInSeconds * 90000);

  auto scte35_event = std::make_shared<SCTE35Event>(
      "cue-id", kScte35StartTime90k, /*duration=*/0);

  for (int i = 0; i < 6; ++i) {
    const bool is_key_frame = true;
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, is_key_frame))));
    if (i == 0) {
      ASSERT_OK(Process(
          StreamData::FromScte35Event(kStreamIndex, scte35_event)));
    }
  }

  // Identical segmentation shape to the CueEvent test above - the SCTE-35 event forces the same
  // early segment - but IsScte35EventPassthrough (the original message, unmodified) appears
  // where IsCueEvent appeared there, and no CueEvent is ever emitted.
  //
  // The event passthrough comes BEFORE the SegmentInfo that closes the segment it interrupted -
  // not after, as it would if the segment closed synchronously the instant the event arrived.
  // The close is deliberately deferred to the next sample actually eligible to start a fresh
  // segment (see ForceSegmentBoundaryAt/pending_forced_boundary_'s own doc comments): closing
  // synchronously would mean every sample arriving before that next eligible one - a live splice
  // point essentially never lands exactly on one - falls into the "discard samples before
  // segment start" branch and is silently lost, up to a full partial GOP of real video per cue.
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kEncrypted, _),
          IsScte35EventPassthrough(kStreamIndex, scte35_event),
          // A new segment is created due to the existance of the SCTE-35 event.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 1,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 2, kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 4, !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

TEST_F(ChunkingHandlerTest, CueEvent) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  chunking_params.subsegment_duration_in_seconds = 0.5;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kVideoStartTimestamp = 12345;
  const double kCueTimeInSeconds =
      static_cast<double>(kVideoStartTimestamp + kDuration) / kTimeScale1;

  auto cue_event = std::make_shared<CueEvent>();
  cue_event->time_in_seconds = kCueTimeInSeconds;

  for (int i = 0; i < 6; ++i) {
    const bool is_key_frame = true;
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, is_key_frame))));
    if (i == 0) {
      ASSERT_OK(Process(StreamData::FromCueEvent(kStreamIndex, cue_event)));
    }
  }

  // The CueEvent comes BEFORE the SegmentInfo that closes the segment it interrupted - see the
  // Scte35Event test's own comment on why the close is deliberately deferred to the next sample
  // actually eligible to start a fresh segment, rather than closing synchronously right here.
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kEncrypted, _),
          IsCueEvent(kStreamIndex, kCueTimeInSeconds),
          // A new segment is created due to the existance of Cue.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 1,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 2, kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp + kDuration,
                        kDuration * 4, !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

// Regression test for a real production bug: a live in-band SCTE-35 event essentially never
// lands exactly on a real keyframe (the encoder forces one "right at the splice point", but that
// keyframe arrives some time after the event is parsed - up to a full GOP later). Every
// non-keyframe sample in between used to be silently discarded (an earlier version of
// ForceSegmentBoundaryAt used segment_start_time_ = std::nullopt itself as the "force a cut"
// signal, which fell straight into OnMediaSample's "discard samples before segment start"
// branch) - confirmed live via direct frame-level ffprobe analysis: Packager's own packaged
// output had a real ~0.4s hole of missing video frames at every cue-forced boundary, even though
// the exact same frames were present, cleanly spaced, in the raw stream Packager received.
// This asserts every one of those in-between samples is dispatched, not dropped, and correctly
// extends the segment that's about to close (rather than being lost) right up until the real
// next keyframe finally arrives and starts the new one.
TEST_F(ChunkingHandlerTest, Scte35EventDoesNotDropNonKeyframeSamplesBeforeNextKeyframe) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kVideoStartTimestamp = 12345;
  // Lands mid-GOP, well before the next real keyframe (samples 1-3 below) - exactly the case
  // that used to lose samples.
  const double kCueTimeInSeconds =
      static_cast<double>(kVideoStartTimestamp + kDuration / 2) / kTimeScale1;
  auto scte35_event = std::make_shared<SCTE35Event>(
      "cue-id", static_cast<int64_t>(kCueTimeInSeconds * 90000),
      /*duration=*/0);

  // Sample 0: real key frame, starts the segment. 1-3: non-key frames arriving after the cue but
  // before the next real key frame - must all be dispatched, not discarded. 4: the next real key
  // frame, finally closes the interrupted segment and opens a new one. 5: trails into it.
  const bool kIsKeyFrame[] = {true, false, false, false, true, false};
  for (int i = 0; i < 6; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kVideoStartTimestamp + i * kDuration,
                                     kDuration, kIsKeyFrame[i]))));
    if (i == 0) {
      ASSERT_OK(Process(
          StreamData::FromScte35Event(kStreamIndex, scte35_event)));
    }
  }

  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kVideoStartTimestamp, kDuration,
                        !kEncrypted, _),
          IsScte35EventPassthrough(kStreamIndex, scte35_event),
          // Samples 1-3: non-key frames between the cue and the next real key frame - the exact
          // samples that used to be silently discarded. All three must still be dispatched.
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 1,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 2,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 3,
                        kDuration, !kEncrypted, _),
          // The interrupted segment finally closes on sample 4 (the next real key frame) -
          // its duration correctly extends to cover samples 1-3 instead of losing them.
          IsSegmentInfo(kStreamIndex, kVideoStartTimestamp, kDuration * 4,
                        !kIsSubsegment, !kEncrypted),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 4,
                        kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kVideoStartTimestamp + kDuration * 5,
                        kDuration, !kEncrypted, _)));
}

// Regression test for a cue-follower stream (e.g. audio, driven by SegmentCoordinator - see
// RegisterCueFollower's own doc comment): a real correction (ForceSegmentBoundaryNow, called from
// OnSegmentInfo once the sync source's own realized cut is known) doesn't arrive until up to a
// full segment_duration after the raw event - and every sample is trivially is_key_frame()==true
// for a stream like this, so left alone it almost always fits in one more of its own regular
// periodic cuts before that correction lands, leaving a small leftover fragment neither cut
// needed (confirmed live against a reference multi-CDN encoder, which never produces that extra
// fragment - its own audio segment count matches video's exactly, splice after splice).
// SuppressPeriodicCutsUntilForcedBoundary - called as soon as the raw event arrives, before the
// real correction's timestamp is even known - closes that gap: this asserts every sample in
// between keeps extending the current segment (not triggering its own periodic cut), and the
// eventual correction produces exactly one cut, not two.
TEST_F(ChunkingHandlerTest, SuppressPeriodicCutsUntilForcedBoundaryProducesExactlyOneCut) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kStartTimestamp = 12345;
  // Sample 0 opens the segment normally.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kStartTimestamp, kDuration, kKeyFrame))));

  // The raw event arrives - suppress this stream's own periodic grid before its real correction
  // (the sync source's realized cut timestamp) is even known.
  chunking_handler_->SuppressPeriodicCutsUntilForcedBoundary();

  // Samples 1-4 span two of this stream's own regular segment_duration_-sized grid lines (every
  // AAC-like sample here is a "key frame", so without suppression at least one of these would
  // ordinarily cut on its own). None should - the segment stays open, extending through all of
  // them.
  for (int i = 1; i <= 4; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kStartTimestamp + kDuration * i,
                                     kDuration, kKeyFrame))));
  }

  // The real correction finally arrives, driven by the sync source's own realized cut.
  const double kCorrectionTimeInSeconds =
      static_cast<double>(kStartTimestamp + kDuration * 5) / kTimeScale1;
  ASSERT_OK(chunking_handler_->ForceSegmentBoundaryNow(kCorrectionTimeInSeconds));

  // The very next sample is this stream's own next eligible one (again, trivially a "key frame")
  // - it finally closes the long-suppressed segment and opens a new one, in exactly one cut.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kStartTimestamp + kDuration * 5,
                                   kDuration, kKeyFrame))));

  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kStartTimestamp, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 1, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 2, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 3, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 4, kDuration,
                        !kEncrypted, _),
          // Exactly one SegmentInfo - covering samples 0-4 in a single segment - not two.
          IsSegmentInfo(kStreamIndex, kStartTimestamp, kDuration * 5, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 5, kDuration,
                        !kEncrypted, _)));
}

// Regression test for a real bug found live: RegisterCueFollower calls
// SuppressPeriodicCutsUntilForcedBoundary at pipeline wiring time, before this stream's very
// first-ever sample - unlike the test above, where suppression is applied to an already-open
// segment (the live SCTE-35/cue-event case). !segment_start_time_ (this stream's first-ever
// sample) also enters OnMediaSample's cut-handling block, but nothing has actually been cut yet -
// it's just the moment the (suppressed) first segment starts accumulating. An earlier version of
// that block cleared suppress_periodic_cuts_ unconditionally on every entry into it, including
// this one - silently undoing the suppression before the real correction ever arrived, so the very
// next periodic grid crossing cut early anyway (confirmed live: a real capture produced two short
// audio segments summing to exactly one segment_duration_, instead of the one full segment this
// test asserts).
TEST_F(ChunkingHandlerTest, SuppressBeforeFirstSampleProducesExactlyOneCut) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  // Suppressed before any sample at all has arrived - matching RegisterCueFollower's real usage.
  chunking_handler_->SuppressPeriodicCutsUntilForcedBoundary();

  // Deliberately small and close to 0, unlike the test above's kStartTimestamp=12345: suppression
  // applied before any sample means suppress_periodic_cuts_started_at_ baselines at
  // max_segment_time_'s own default (0), not some already-elapsed stream position - a starting
  // timestamp as large as 12345 would immediately exceed kMaxSuppressSegments * segment_duration_
  // (3000 here) measured from that 0 baseline, firing the safety valve on sample 0 itself and
  // masking the real bug this test exists to catch.
  const int64_t kStartTimestamp = 100;
  // Sample 0 opens the (suppressed) segment for the first time ever.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kStartTimestamp, kDuration, kKeyFrame))));

  // Samples 1-4 span two of this stream's own regular segment_duration_-sized grid lines. None
  // should cut on their own - the segment must stay open, extending through all of them, exactly
  // as the already-open-segment case above does.
  for (int i = 1; i <= 4; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(kStartTimestamp + kDuration * i,
                                     kDuration, kKeyFrame))));
  }

  // The real correction finally arrives, driven by the sync source's own realized cut.
  const double kCorrectionTimeInSeconds =
      static_cast<double>(kStartTimestamp + kDuration * 5) / kTimeScale1;
  ASSERT_OK(chunking_handler_->ForceSegmentBoundaryNow(kCorrectionTimeInSeconds));

  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kStartTimestamp + kDuration * 5,
                                   kDuration, kKeyFrame))));

  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kStartTimestamp, kDuration, !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 1, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 2, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 3, kDuration,
                        !kEncrypted, _),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 4, kDuration,
                        !kEncrypted, _),
          // Exactly one SegmentInfo - covering samples 0-4 in a single segment - not two.
          IsSegmentInfo(kStreamIndex, kStartTimestamp, kDuration * 5, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, kStartTimestamp + kDuration * 5, kDuration,
                        !kEncrypted, _)));
}

// Regression test for a real bug found live in the fix above: a cue-follower's correction can
// arrive up to ~1 segment_duration after the timestamp it was actually requested for (see
// ForceSegmentBoundaryNow's own doc comment), so by the time it finally triggers a cut, the
// periodic grid anchor (cue_offset_) computed from that *requested* timestamp can already be
// most of the way through its own grid cell - meaning the very next sample immediately crosses
// into the next periodic segment_index anyway, producing an unwanted second, tiny segment right
// after the one the correction was supposed to produce on its own. This asserts a correction
// requested for one timestamp but not actually landing until a later sample re-anchors the grid
// to that sample's own real timestamp, so the freshly-opened segment gets a full, fresh
// segment_duration before its own next periodic cut - not an almost-immediate second one.
TEST_F(ChunkingHandlerTest, ForcedBoundaryReanchorsGridToActualCutNotRequestedTime) {
  ChunkingParams chunking_params;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));
  ClearOutputStreamDataVector();

  const int64_t kStartTimestamp = 12345;
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kStartTimestamp, kDuration, kKeyFrame))));
  chunking_handler_->SuppressPeriodicCutsUntilForcedBoundary();

  // Requested for 12.400s - but (as in a real cue-follower correction) the actual next eligible
  // sample doesn't arrive until well after that, at 13345 below.
  ASSERT_OK(chunking_handler_->ForceSegmentBoundaryNow(12.400));

  // This sample is late enough (13345 - 12400 = 945, 94.5% of one segment_duration_) that,
  // without re-anchoring, the segment_index grid it lands in has almost no room left - closes
  // the suppressed segment and opens a new one.
  const int64_t kForcedCutTimestamp = 13345;
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kForcedCutTimestamp, kDuration, kKeyFrame))));

  // Only 300 ticks later - on the stale (unanchored) grid this alone already crosses into the
  // next segment_index (945 + 300 = 1245 > 1000) and would wrongly cut again immediately. With
  // re-anchoring, the grid restarted fresh at kForcedCutTimestamp, so this is nowhere near a
  // boundary - must just extend the still-open segment.
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kForcedCutTimestamp + kDuration, kDuration, kKeyFrame))));

  // A full segment_duration_ after the real cut (not the originally-requested time) - this
  // finally does cross the (correctly re-anchored) grid, closing that segment normally.
  const int64_t kNextRealBoundary = kForcedCutTimestamp + 1000;
  ASSERT_OK(Process(StreamData::FromMediaSample(
      kStreamIndex, GetMediaSample(kNextRealBoundary, kDuration, kKeyFrame))));

  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsMediaSample(kStreamIndex, kStartTimestamp, kDuration, !kEncrypted, _),
          // The forced cut: closes the suppressed segment (duration only spans what was
          // actually dispatched into it - just sample 0 here), opens a new one.
          IsSegmentInfo(kStreamIndex, kStartTimestamp, kDuration, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, kForcedCutTimestamp, kDuration, !kEncrypted, _),
          // The very next sample must NOT produce a second SegmentInfo here - it just extends
          // the segment the forced cut just opened.
          IsMediaSample(kStreamIndex, kForcedCutTimestamp + kDuration, kDuration,
                        !kEncrypted, _),
          // Only once a full segment_duration_ has genuinely passed since the real cut (not the
          // originally-requested time) does this segment finally close, normally - covering
          // both of its own samples (2 * kDuration), not cut short by the stale grid.
          IsSegmentInfo(kStreamIndex, kForcedCutTimestamp, kDuration * 2, !kIsSubsegment,
                        !kEncrypted),
          IsMediaSample(kStreamIndex, kNextRealBoundary, kDuration, !kEncrypted, _)));
}

TEST_F(ChunkingHandlerTest, LowLatencyDash) {
  ChunkingParams chunking_params;
  chunking_params.low_latency_dash_mode = true;
  chunking_params.segment_duration_in_seconds = 1;
  SetUpChunkingHandler(1, chunking_params);

  // Each completed segment will contain 2 chunks
  const int64_t kChunkDurationInMs = 500;
  const int64_t kSegmentDurationInMs = 1000;

  ASSERT_OK(Process(StreamData::FromStreamInfo(
      kStreamIndex, GetVideoStreamInfo(kTimeScale1))));

  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(Process(StreamData::FromMediaSample(
        kStreamIndex, GetMediaSample(i * kChunkDurationInMs, kChunkDurationInMs,
                                     kKeyFrame))));
  }

  // NOTE: Each MediaSample will create a chunk, dispatching SegmentInfo
  EXPECT_THAT(
      GetOutputStreamDataVector(),
      ElementsAre(
          IsStreamInfo(kStreamIndex, kTimeScale1, !kEncrypted, _),
          // Chunk 1 for segment 1
          IsMediaSample(kStreamIndex, 0, kChunkDurationInMs, !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, kChunkDurationInMs, kIsSubsegment,
                        !kEncrypted),
          // Chunk 2 for segment 1
          IsMediaSample(kStreamIndex, kChunkDurationInMs, kChunkDurationInMs,
                        !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, 0, 2 * kChunkDurationInMs, !kIsSubsegment,
                        !kEncrypted),
          // Chunk 1 for segment 2
          IsMediaSample(kStreamIndex, kSegmentDurationInMs, kChunkDurationInMs,
                        !kEncrypted, _),
          IsSegmentInfo(kStreamIndex, kSegmentDurationInMs, kChunkDurationInMs,
                        kIsSubsegment, !kEncrypted),
          // Chunk 2 for segment 2
          IsMediaSample(kStreamIndex, kSegmentDurationInMs + kChunkDurationInMs,
                        kChunkDurationInMs, !kEncrypted, _)));
}

}  // namespace media
}  // namespace shaka
