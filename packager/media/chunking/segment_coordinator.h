// Copyright 2025 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_CHUNKING_SEGMENT_COORDINATOR_H_
#define PACKAGER_MEDIA_CHUNKING_SEGMENT_COORDINATOR_H_

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <packager/media/base/media_handler.h>
#include <packager/media/chunking/chunking_handler.h>
#include <packager/status.h>

namespace shaka {
namespace media {

/// SegmentCoordinator is a N-to-N media handler that coordinates segment
/// boundaries across different stream types. All streams (video, audio, text)
/// go through the same coordinator instance, similar to CueAlignmentHandler.
///
/// It receives SegmentInfo events from video/audio streams (emitted by
/// ChunkingHandler) and replicates them to registered teletext streams,
/// ensuring that teletext segments align with media segments.
///
/// This handler is designed to solve the alignment problem where teletext
/// segments use mathematical calculation (segment_start + segment_duration)
/// while video/audio use actual keyframe timestamps, causing misalignment
/// especially with:
/// - Arbitrary input timestamps (not divisible by segment duration)
/// - MPEG-TS timestamp wrap-around scenarios
/// - Sources with offset keyframe timestamps
///
/// The coordinator only replicates SegmentInfo to teletext streams (cc_index >=
/// 0). Other text formats (WebVTT files, TTML) and video/audio streams pass
/// through unchanged.
///
/// A second, distinct mechanism (RegisterCueFollower) drives a registered follower stream's own
/// ChunkingHandler directly - via a plain method call (ChunkingHandler::ForceSegmentBoundaryNow),
/// not the StreamData/Process() graph - whenever the sync source reports a *cue-aligned* segment
/// (SegmentInfo::is_cue_aligned), i.e. one that resulted from an actual live SCTE-35/CueEvent
/// splice. Unlike teletext replication (every segment, for a stream with no independent chunking
/// of its own), a cue-follower keeps doing its own normal periodic chunking otherwise; this only
/// intervenes at the moment the sync source *actually* cuts at a splice point, driving the
/// follower to the sync source's own realized cut timestamp instead of trusting the follower's
/// own independent reaction to the same raw event to land in the same place - see
/// RegisterCueFollower's own doc comment for why that independent-decision race is a real,
/// measured problem this removes rather than narrows.
///
/// Pipeline placement (all streams go through the same coordinator instance):
///   Video/Audio → CueAligner → SegmentCoordinator → ChunkingHandler → ...
///   Teletext    → CueAligner → SegmentCoordinator → CcStreamFilter →
///   TextChunker → ...
///
/// When ChunkingHandler emits SegmentInfo, it flows back through the
/// coordinator, which then broadcasts it to all registered teletext stream
/// indices.
class SegmentCoordinator : public MediaHandler {
 public:
  SegmentCoordinator();
  ~SegmentCoordinator() override = default;

  /// Mark a stream as a teletext stream that should receive segment boundaries.
  /// This should be called during pipeline setup before processing begins.
  /// @param input_stream_index The input stream index for the teletext stream.
  void MarkAsTeletextStream(size_t input_stream_index);

  /// Registers follower's own ChunkingHandler instance to be driven directly (via
  /// ChunkingHandler::ForceSegmentBoundaryNow, a plain method call outside the normal
  /// StreamData/Process() graph) whenever the sync source stream (see sync_source_stream_index_)
  /// reports a cue-aligned SegmentInfo (see that field's own doc comment) - i.e. whenever the sync
  /// source *actually* cuts a segment at a live SCTE-35/CueEvent splice point - AND once,
  /// unconditionally, the very first time the sync source reports any SegmentInfo at all (see
  /// OnSegmentInfo's own doc comment on has_aligned_followers_at_start_).
  ///
  /// Unlike MarkAsTeletextStream/teletext replication (every single segment boundary, used to let
  /// a stream with no independent chunking of its own follow the sync source unconditionally),
  /// this only fires at cue points (plus that one startup alignment), and follower is expected to
  /// keep doing its own normal independent periodic chunking the rest of the time. The cue-point
  /// case exists purely to remove the *cue-triggered* segmentation race between two independent
  /// ChunkingHandler instances each reacting to their own copy of the same live SCTE-35 event,
  /// which real deployment testing showed drifting apart by anywhere from ~150ms to a full missed
  /// GOP (video's own forced-keyframe timing isn't deterministic, and two independent decisions
  /// reacting to the "same" event on separate threads can still land in different places).
  /// Deriving follower's cut directly from the sync source's own *realized* cut timestamp, once
  /// it's actually known, removes that race entirely rather than narrowing it.
  ///
  /// Immediately suppresses follower's own periodic cuts (ForceSegmentBoundaryNow's own doc
  /// comment on SuppressPeriodicCutsUntilForcedBoundary) so its very first segment stays open,
  /// same as it would mid-stream waiting for a cue correction, until the startup alignment above
  /// closes it at the sync source's own real first boundary instead of wherever follower's own
  /// independent PTS-modulo grid happened to fall - see OnSegmentInfo's own doc comment for why
  /// that grid is otherwise permanently out of phase with the sync source's, confirmed live via
  /// two tracks whose PROGRAM-DATE-TIME grids sat a stable ~2.4s apart for an entire capture with
  /// no splice ever firing to correct it.
  ///
  /// This should be called during pipeline setup before processing begins, same as
  /// MarkAsTeletextStream.
  /// @param input_stream_index The input stream index for the follower stream (e.g. audio).
  /// @param follower The follower stream's own ChunkingHandler instance.
  void RegisterCueFollower(size_t input_stream_index,
                            std::shared_ptr<ChunkingHandler> follower);

  /// Registers a stream's own ChunkingHandler instance (video's AND audio's - every stream that
  /// isn't itself the SCTE-35/text stream) to be driven directly (via
  /// ChunkingHandler::ForceSegmentBoundaryNow, a plain method call, not the StreamData/Process()
  /// graph) *immediately* whenever a live in-band SCTE-35 event arrives on this coordinator from
  /// the SCTE-35 stream's own chain - every registered receiver gets the exact same raw event at
  /// the exact same moment.
  ///
  /// This exists because ChunkingHandler::InitializeInternal requires exactly one input stream, so
  /// the SCTE-35 stream (whose only chain membership is here, in SegmentCoordinator, alongside
  /// video/audio/teletext) cannot simply be wired as a second input into video's/audio's own
  /// ChunkingHandler the way video/audio/teletext all fan into this coordinator. Calling
  /// ForceSegmentBoundaryNow directly, the same pattern RegisterCueFollower already uses, sidesteps
  /// that constraint entirely - see this class's own Process() override.
  ///
  /// Without this, no stream's own ChunkingHandler ever learns about the live splice point at all
  /// (only the Muxer does, via the SCTE-35 event's separate unmodified pass-through, purely for
  /// #EXT-X-DATERANGE/CUE-OUT/CUE-IN reporting) - so SegmentInfo::is_cue_aligned never becomes true
  /// for anyone, which in turn means RegisterCueFollower's own driving logic (gated on
  /// is_cue_aligned) never fires either. This is the missing link between a raw live SCTE-35 event
  /// and the rest of the alignment fix.
  ///
  /// Registering ALL streams here (not just the sync source) matters because RegisterCueFollower's
  /// own correction only fires once the sync source's *entire* cue-aligned segment has finished and
  /// its SegmentInfo has been dispatched - which is, by construction, a full segment_duration after
  /// the actual cut happened (confirmed against a real deployment: a consistent ~1 segment_duration
  /// lag on every cue, replacing the pre-fix problem's ~150ms-1.9s *variance* with an almost equally
  /// large but now *deterministic* delay - not the improvement intended). Giving every stream this
  /// same immediate reaction closes almost the entire gap on each stream's own, independently,
  /// right when the event arrives - audio in particular has no keyframe constraint at all (every
  /// sample is trivially is_key_frame()==true, see es_parser_audio.cc), so its own immediate
  /// reaction can cut on the very next sample. RegisterCueFollower's correction still runs
  /// afterward as a secondary tightening pass, now starting from an already-close position instead
  /// of a several-seconds-stale one.
  ///
  /// This should be called during pipeline setup before processing begins, same as
  /// MarkAsTeletextStream/RegisterCueFollower.
  /// @param handler The stream's own ChunkingHandler instance (video's or audio's).
  void RegisterScte35ImmediateReceiver(std::shared_ptr<ChunkingHandler> handler);

 protected:
  /// @name MediaHandler implementation overrides.
  /// @{
  Status InitializeInternal() override;
  Status Process(std::unique_ptr<StreamData> stream_data) override;
  /// @}

 private:
  SegmentCoordinator(const SegmentCoordinator&) = delete;
  SegmentCoordinator& operator=(const SegmentCoordinator&) = delete;

  /// Handle incoming SegmentInfo from video/audio streams and replicate to
  /// teletext streams.
  /// @param input_stream_index The input stream index that sent the
  /// SegmentInfo.
  /// @param info The segment information to process.
  /// @return Status of the operation.
  Status OnSegmentInfo(size_t input_stream_index,
                       std::shared_ptr<const SegmentInfo> info);

  /// Check if a stream index corresponds to a teletext stream.
  /// @param input_stream_index The input stream index to check.
  /// @return true if the stream is a teletext stream.
  bool IsTeletextStream(size_t input_stream_index) const;

  /// Check if a stream index corresponds to a registered cue-follower stream (see
  /// RegisterCueFollower). Excluded from ever becoming the sync source, same reasoning as
  /// IsTeletextStream - without this, a follower stream (e.g. audio) emitting its own SegmentInfo
  /// before the intended sync source (e.g. video) does, at startup, could otherwise win the "first
  /// stream to report SegmentInfo becomes the sync source" race and make the sync source a
  /// follower of itself, silently disabling the whole mechanism for the real leader.
  /// @param input_stream_index The input stream index to check.
  /// @return true if the stream is a registered cue-follower stream.
  bool IsCueFollowerStream(size_t input_stream_index) const;

  /// Records each stream's own time_scale as its StreamInfo passes through - needed to convert
  /// the sync source's SegmentInfo::start_timestamp (in the sync source's own time_scale) into
  /// real seconds for RegisterCueFollower's ChunkingHandler::ForceSegmentBoundaryNow calls, which
  /// take seconds regardless of any particular stream's own time_scale.
  Status OnStreamInfo(size_t input_stream_index,
                      std::shared_ptr<const StreamInfo> info);

  /// Latest segment boundary timestamp from video/audio streams.
  /// Used for logging and debugging purposes.
  int64_t latest_segment_boundary_ = 0;

  /// Set of input stream indices that are teletext streams and should receive
  /// replicated SegmentInfo from video/audio streams.
  std::set<size_t> teletext_stream_indices_;

  /// Follower streams' own ChunkingHandler instances, driven directly at cue points - see
  /// RegisterCueFollower's own doc comment.
  std::map<size_t, std::shared_ptr<ChunkingHandler>> cue_follower_handlers_;

  /// Every stream's own ChunkingHandler instance (video's and audio's), driven directly and
  /// immediately on every live SCTE-35 event this coordinator observes from the SCTE-35 stream's
  /// own chain - see RegisterScte35ImmediateReceiver's own doc comment.
  std::vector<std::shared_ptr<ChunkingHandler>> scte35_immediate_receivers_;

  /// Each stream's own time_scale, keyed by input_stream_index - see OnStreamInfo.
  std::map<size_t, int32_t> stream_time_scales_;

  /// The stream index that acts as the sync source for segment boundaries.
  /// Only SegmentInfo from this stream is replicated to teletext streams.
  /// This is set to the first non-teletext stream that sends SegmentInfo,
  /// ensuring consistent alignment even when video and audio have different
  /// segment boundaries.
  std::optional<size_t> sync_source_stream_index_;

  /// One-shot latch: false until the sync source's very first SegmentInfo has been used to drive
  /// cue-follower streams (see OnSegmentInfo). Without this, a follower's own periodic chunking
  /// grid (raw PTS modulo segment_duration, anchored at that follower's own arbitrary PTS origin
  /// - see ChunkingHandler::OnMediaSample) shares no common reference with the sync source's own
  /// grid, so absent an actual splice to force the two into alignment, they simply sit at
  /// whatever relative phase their independent PTS origins happen to produce, forever - confirmed
  /// live via a real capture with no SCTE-35 activity at all, where video's and audio's own
  /// PROGRAM-DATE-TIME grids sat a stable, non-drifting ~2.4s out of phase for the whole
  /// recording. Treating the sync source's first-ever segment the same way a cue-aligned one is
  /// already treated - driving every registered follower to that segment's own realized start,
  /// exactly once - gives both streams the same absolute grid origin from the very start of the
  /// stream, with no new "global clock" concept needed: it reuses the same
  /// ForceSegmentBoundaryNow/cue_offset_ re-anchoring already shipped and tested for live splices.
  bool has_aligned_followers_at_start_ = false;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_CHUNKING_SEGMENT_COORDINATOR_H_
