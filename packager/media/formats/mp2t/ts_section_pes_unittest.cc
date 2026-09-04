// Copyright 2025 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/media/formats/mp2t/ts_section_pes.h>

#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include <packager/media/base/timestamp.h>
#include <packager/media/formats/mp2t/es_parser.h>

namespace shaka {
namespace media {
namespace mp2t {
namespace {

// Records every Parse() call it receives, standing in for a real ES parser
// (e.g. EsParserSCTE35) so tests can observe exactly when TsSectionPes hands
// data off, without needing a real codec-specific parser.
class RecordingEsParser : public EsParser {
 public:
  RecordingEsParser() : EsParser(/*pid=*/0) {}

  bool Parse(const uint8_t* buf, int size, int64_t pts, int64_t dts) override {
    parse_count++;
    last_size = size;
    last_pts = pts;
    return true;
  }
  bool Flush() override { return true; }
  void Reset() override {}

  int parse_count = 0;
  int last_size = 0;
  int64_t last_pts = kNoTimestamp;
};

// Builds a raw section carrying no real PES framing at all - matching exactly
// how live in-band SCTE-35 is sent by AndroidStreamer's own TsMuxer.writeScte35
// (see MpegTsStreamSink.kt/Scte35.kt): a bare pointer_field byte followed by a
// self-length-prefixed section, with no 00 00 01 start code anywhere. The two
// bytes at what would be a real PES's PES_packet_length field position
// (protocol_version=0x00 and the following reserved-bits byte) are
// deliberately both zero here, mirroring the real encoder's own fixed layout.
std::vector<uint8_t> BuildRawSectionNoRealPes() {
  return {
      0x00,        // pointer_field
      0xFC,        // table_id (splice_info_section)
      0x30, 0x0A,  // section_syntax_indicator/reserved + section_length
      0x00,        // protocol_version - this is raw_pes[4] once pushed
      0x00,        // encrypted_packet+algorithm+pts_adjustment bit32 - raw_pes[5]
      0x00, 0x00, 0x00, 0x00,  // pts_adjustment low 32 bits
      0x00,        // cw_index
  };
}

// Builds a real PES packet with the genuine 00 00 01 start code and
// PES_packet_length=0 (unbounded - the normal encoding for a live video PES,
// per TsMuxer.kt's own "PES_packet_length: 0 means unbounded" comment),
// followed by a minimal PTS-only header and a small payload.
std::vector<uint8_t> BuildRealPesUnboundedSize() {
  return {
      0x00, 0x00, 0x01,  // packet_start_code_prefix
      0xE0,              // stream_id (video)
      0x00, 0x00,        // PES_packet_length = 0 (unbounded)
      0x80,              // marker bits
      0x80,              // PTS_DTS_flags = '10' (PTS only)
      0x05,              // PES_header_data_length
      0x21, 0x00, 0x01, 0x00, 0x01,  // PTS field (dummy but well-formed marker bits)
      0xAA, 0xBB, 0xCC,  // a few bytes of "payload"
  };
}

}  // namespace

TEST(TsSectionPesTest, RawSectionEmitsImmediatelyWithoutNextPacket) {
  auto recorder = std::make_unique<RecordingEsParser>();
  RecordingEsParser* recorder_ptr = recorder.get();
  TsSectionPes section(std::move(recorder));

  std::vector<uint8_t> data = BuildRawSectionNoRealPes();
  // A single Parse() call, payload_unit_start_indicator=true, exactly as a
  // live in-band SCTE-35 packet arrives - one full section per TS packet.
  ASSERT_TRUE(section.Parse(/*payload_unit_start_indicator=*/true, data.data(),
                             static_cast<int>(data.size()), /*reference_pts=*/12345));

  // Must have been handed to the underlying ES parser already - not deferred
  // until some future packet arrives on this PID.
  EXPECT_EQ(recorder_ptr->parse_count, 1);
  // ParseInternal's own "not a real PES packet" branch passes everything
  // after the leading pointer_field byte through, using reference_pts_ as the
  // timestamp (see ts_section_pes.cc), so the recorded size is one less than
  // the pushed data and the pts matches what was passed in as reference_pts.
  EXPECT_EQ(recorder_ptr->last_size, static_cast<int>(data.size()) - 1);
  EXPECT_EQ(recorder_ptr->last_pts, 12345);
}

TEST(TsSectionPesTest, RawSectionSecondEventAlsoEmitsImmediately) {
  // Regression test for the actual bug: before the fix, a raw section's
  // PES_packet_length bytes (here always 0, since they're really
  // protocol_version + reserved bits) were misread as "unbounded PES, wait
  // for the next packet before emitting" - meaning event N only ever emitted
  // once event N+1 arrived. Verify two independent packets each emit on
  // their own, with no dependency on a third packet ever showing up.
  auto recorder = std::make_unique<RecordingEsParser>();
  RecordingEsParser* recorder_ptr = recorder.get();
  TsSectionPes section(std::move(recorder));

  std::vector<uint8_t> first = BuildRawSectionNoRealPes();
  ASSERT_TRUE(section.Parse(true, first.data(), static_cast<int>(first.size()), 1000));
  EXPECT_EQ(recorder_ptr->parse_count, 1);

  std::vector<uint8_t> second = BuildRawSectionNoRealPes();
  ASSERT_TRUE(section.Parse(true, second.data(), static_cast<int>(second.size()), 2000));
  // The second packet must NOT be required to make the first one emit (it
  // already did, above) - and the second one emits on its own too, without a
  // third packet ever needing to arrive.
  EXPECT_EQ(recorder_ptr->parse_count, 2);
  EXPECT_EQ(recorder_ptr->last_pts, 2000);
}

TEST(TsSectionPesTest, RealUnboundedPesStillWaitsForNextPacket) {
  // Confirms the fix doesn't change existing behavior for genuine PES-framed
  // streams (video/audio) with PES_packet_length=0 (unbounded) - those must
  // still defer until the next packet's payload_unit_start_indicator forces
  // emission, exactly as before.
  auto recorder = std::make_unique<RecordingEsParser>();
  RecordingEsParser* recorder_ptr = recorder.get();
  TsSectionPes section(std::move(recorder));

  std::vector<uint8_t> first = BuildRealPesUnboundedSize();
  ASSERT_TRUE(section.Parse(true, first.data(), static_cast<int>(first.size()), 1000));
  EXPECT_EQ(recorder_ptr->parse_count, 0)
      << "an unbounded-size real PES packet must wait for the next packet, same as before";

  std::vector<uint8_t> second = BuildRealPesUnboundedSize();
  ASSERT_TRUE(section.Parse(true, second.data(), static_cast<int>(second.size()), 2000));
  // Arrival of the next PUSI=1 packet force-emits the first one.
  EXPECT_EQ(recorder_ptr->parse_count, 1);
}

}  // namespace mp2t
}  // namespace media
}  // namespace shaka
