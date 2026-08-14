// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef PACKAGER_MEDIA_FORMATS_MP2T_TS_SECTION_H_
#define PACKAGER_MEDIA_FORMATS_MP2T_TS_SECTION_H_

#include <cstdint>

namespace shaka {
namespace media {
namespace mp2t {

class TsSection {
 public:
  // From ISO/IEC 13818-1 or ITU H.222 spec: Table 2-3 - PID table.
  enum SpecialPid {
    kPidPat = 0x0,
    kPidCat = 0x1,
    kPidTsdt = 0x2,
    kPidNullPacket = 0x1fff,
    kPidMax = 0x1fff,
  };

  virtual ~TsSection() {}

  // Parse the data bytes of the TS packet.
  // |reference_pts| is the most recently known real media PTS from any
  // audio/video stream in the program (90kHz units, see
  // Mp2tMediaParser::biggest_pts_), used by sections that carry no PES
  // timestamp of their own (e.g. a raw SCTE-35 section, as opposed to a
  // PES-wrapped one) as a real "current stream time" reference instead of a
  // placeholder.
  // Return true if parsing is successful.
  virtual bool Parse(bool payload_unit_start_indicator,
                     const uint8_t* buf,
                     int size,
                     int64_t reference_pts) = 0;

  // Process bytes that have not been processed yet (pending buffers in the
  // pipe). Flush might thus results in frame emission, as an example.
  virtual bool Flush() = 0;

  // Reset the state of the parser to its initial state.
  virtual void Reset() = 0;
};

}  // namespace mp2t
}  // namespace media
}  // namespace shaka

#endif
