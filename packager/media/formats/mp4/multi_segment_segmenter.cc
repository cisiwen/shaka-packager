// Copyright 2014 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp4/multi_segment_segmenter.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <absl/log/check.h>
#include <absl/log/log.h>

#include <packager/file.h>
#include <packager/file/file_closer.h>
#include <packager/macros/status.h>
#include <packager/media/base/aes_cryptor.h>
#include <packager/media/base/aes_encryptor.h>
#include <packager/media/base/buffer_writer.h>
#include <packager/media/base/fourccs.h>
#include <packager/media/base/muxer_options.h>
#include <packager/media/base/muxer_util.h>
#include <packager/media/base/range.h>
#include <packager/media/event/muxer_listener.h>
#include <packager/media/formats/mp4/box_definitions.h>
#include <packager/media/formats/mp4/key_frame_info.h>
#include <packager/media/formats/mp4/segmenter.h>
#include <packager/status.h>

namespace shaka {
namespace media {
namespace mp4 {

MultiSegmentSegmenter::MultiSegmentSegmenter(const MuxerOptions& options,
                                             std::unique_ptr<FileType> ftyp,
                                             std::unique_ptr<Movie> moov)
    : Segmenter(options, std::move(ftyp), std::move(moov)),
      styp_(new SegmentType) {
  // Use the same brands for styp as ftyp.
  styp_->major_brand = Segmenter::ftyp()->major_brand;
  styp_->compatible_brands = Segmenter::ftyp()->compatible_brands;
  // Replace 'cmfc' with 'cmfs' for CMAF segments compatibility.
  std::replace(styp_->compatible_brands.begin(), styp_->compatible_brands.end(),
               FOURCC_cmfc, FOURCC_cmfs);
}

MultiSegmentSegmenter::~MultiSegmentSegmenter() {}

bool MultiSegmentSegmenter::GetInitRange(size_t* offset, size_t* size) {
  VLOG(1) << "MultiSegmentSegmenter outputs init segment: "
          << options().output_file_name;
  return false;
}

bool MultiSegmentSegmenter::GetIndexRange(size_t* offset, size_t* size) {
  VLOG(1) << "MultiSegmentSegmenter does not have index range.";
  return false;
}

std::vector<Range> MultiSegmentSegmenter::GetSegmentRanges() {
  VLOG(1) << "MultiSegmentSegmenter does not have media segment ranges.";
  return std::vector<Range>();
}

Status MultiSegmentSegmenter::DoInitialize() {
  return WriteInitSegment();
}

Status MultiSegmentSegmenter::DoFinalize() {
  // Update init segment with media duration set.
  RETURN_IF_ERROR(WriteInitSegment());
  SetComplete();
  return Status::OK;
}

Status MultiSegmentSegmenter::DoFinalizeSegment(int64_t segment_number) {
  return WriteSegment(segment_number);
}

Status MultiSegmentSegmenter::WriteInitSegment() {
  DCHECK(ftyp());
  DCHECK(moov());
  // Generate the output file with init segment.
  std::unique_ptr<File, FileCloser> file(
      File::Open(options().output_file_name.c_str(), "w"));
  if (!file) {
    return Status(error::FILE_FAILURE,
                  "Cannot open file for write " + options().output_file_name);
  }
  std::unique_ptr<BufferWriter> buffer(new BufferWriter);
  ftyp()->Write(buffer.get());
  moov()->Write(buffer.get());
  return buffer->WriteToFile(file.get());
}

Status MultiSegmentSegmenter::WriteSegment(int64_t segment_number) {
  DCHECK(sidx());
  DCHECK(fragment_buffer());
  DCHECK(styp_);

  DCHECK(!sidx()->references.empty());
  // earliest_presentation_time is the earliest presentation time of any access
  // unit in the reference stream in the first subsegment.
  sidx()->earliest_presentation_time =
      sidx()->references[0].earliest_presentation_time;

  std::unique_ptr<BufferWriter> buffer(new BufferWriter());
  std::unique_ptr<File, FileCloser> file;
  std::string file_name;
  // Per-segment-template output (CMAF's one-file-per-segment model, e.g.
  // video/$Number$.m4s) is built fully in memory below and published in one
  // atomic temp-file-then-rename via File::WriteFileAtomically, rather than
  // opening File::Open(file_name, "w") directly at the final path (this
  // branch's previous approach). Opening directly at the final path
  // creates/truncates the file the instant this function starts, well before
  // any content is written - a live-serving webserver/CDN reading that exact
  // path in that window gets a truncated or empty file, even though the
  // manifest already advertises this segment's full declared duration.
  // Confirmed as a real bug: a live viewer's player reported a segment whose
  // EXTINF declared ~1.92s but decoded to ~22ms; that same segment's later
  // VOD-archived copy (written well after the fact, so never racing a
  // reader) turned out to be a complete, correct 48-frame/1.92s segment - the
  // encoded content itself was never actually broken, only what a concurrent
  // reader could observe mid-write. media_playlist.cc's own WriteToFile
  // already gives playlists this exact guarantee; this extends it to the
  // segment files themselves.
  //
  // The append-to-single-output-file case below (no segment_template - a
  // single continuous file addressed by byte range, not CMAF's model) is
  // left as its own direct, non-atomic append: an atomic swap doesn't apply
  // the same way to appending an already-published file, and this bug is
  // specific to the fresh-file-per-segment path.
  const bool use_segment_template = !options().segment_template.empty();
  if (!use_segment_template) {
    // Append the segment to output file if segment template is not specified.
    file_name = options().output_file_name.c_str();
    file.reset(File::Open(file_name.c_str(), "a"));
    if (!file) {
      return Status(error::FILE_FAILURE, "Cannot open file for append " +
                                             options().output_file_name);
    }
  } else {
    file_name = GetSegmentName(options().segment_template,
                               sidx()->earliest_presentation_time,
                               segment_number, options().bandwidth);
    styp_->Write(buffer.get());
  }

  if (options().mp4_params.generate_sidx_in_media_segments)
    sidx()->Write(buffer.get());

  const size_t segment_header_size = buffer->Size();
  const size_t segment_size = segment_header_size + fragment_buffer()->Size();
  DCHECK_NE(segment_size, 0u);

  const bool encrypted =
      aes128_encryption_config().protection_scheme == kAes128ProtectionScheme;
  if (encrypted) {
    // Encrypt the whole segment (header + fragment) as one CBC stream.
    // Per RFC 8216 §5.2, PKCS7 padding is required.
    buffer->AppendBuffer(*fragment_buffer());
    // AppendBuffer() above copies the accumulated fragment bytes into
    // |buffer|; it does not drain |fragment_buffer()|, so it must be cleared
    // explicitly here or the same bytes get copied again on every subsequent
    // segment, causing unbounded cumulative growth across the whole asset.
    // See https://github.com/shaka-project/shaka-packager/issues/1588.
    fragment_buffer()->Clear();
    AesCbcEncryptor encryptor(kPkcs5Padding, AesCryptor::kUseConstantIv);
    if (!encryptor.InitializeWithIv(aes128_encryption_config().key,
                                    aes128_encryption_config().constant_iv)) {
      return Status(error::ENCRYPTION_FAILURE,
                    "AES-128: failed to initialize encryptor for MP4 segment.");
    }
    std::vector<uint8_t> plaintext(buffer->Buffer(),
                                   buffer->Buffer() + buffer->Size());
    std::vector<uint8_t> ciphertext;
    if (!encryptor.Crypt(plaintext, &ciphertext)) {
      return Status(error::ENCRYPTION_FAILURE,
                    "AES-128: segment encryption failed.");
    }
    buffer->Clear();
    buffer->AppendVector(ciphertext);
  } else {
    // Same reasoning as the encrypted branch above: AppendBuffer() copies
    // rather than drains, so fragment_buffer() needs an explicit Clear().
    // (Previously this branch avoided the copy by writing fragment_buffer()
    // straight to |file| as a second, separate write - no longer possible
    // now that the whole segment must be assembled in |buffer| first for the
    // segment_template case's atomic publish below.)
    buffer->AppendBuffer(*fragment_buffer());
    fragment_buffer()->Clear();
  }

  if (use_segment_template) {
    const std::string contents(reinterpret_cast<const char*>(buffer->Buffer()),
                               buffer->Size());
    if (!File::WriteFileAtomically(file_name.c_str(), contents)) {
      return Status(error::FILE_FAILURE,
                    "Cannot atomically write segment file " + file_name);
    }
  } else {
    RETURN_IF_ERROR(buffer->WriteToFile(file.get()));
    // Close the file, which also does flushing, to make sure the file is
    // written before manifest is updated.
    if (!file.release()->Close()) {
      return Status(
          error::FILE_FAILURE,
          "Cannot close file " + file_name +
              ", possibly file permission issue or running out of disk space.");
    }
  }

  // Only notified once the segment has actually, successfully been written
  // above (either branch) - and only for the non-encrypted case, matching
  // the original code's own scoping (byte offsets into an encrypted segment
  // aren't meaningful the same way for I-frame-only/byte-range addressing).
  if (!encrypted && muxer_listener()) {
    for (const KeyFrameInfo& key_frame_info : key_frame_infos()) {
      muxer_listener()->OnKeyFrame(
          key_frame_info.timestamp,
          segment_header_size + key_frame_info.start_byte_offset,
          key_frame_info.size);
    }
  }

  int64_t segment_duration = 0;
  // ISO/IEC 23009-1:2012: the value shall be identical to sum of the the
  // values of all Subsegment_duration fields in the first ‘sidx’ box.
  for (size_t i = 0; i < sidx()->references.size(); ++i)
    segment_duration += sidx()->references[i].subsegment_duration;

  UpdateProgress(segment_duration);
  if (muxer_listener()) {
    muxer_listener()->OnSampleDurationReady(sample_duration());
    muxer_listener()->OnNewSegment(
        file_name, sidx()->earliest_presentation_time, segment_duration,
        segment_size, segment_number);
  }

  return Status::OK;
}

}  // namespace mp4
}  // namespace media
}  // namespace shaka
