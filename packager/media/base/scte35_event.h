// Copyright 2017 Google Inc. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_MEDIA_BASE_SCTE35_SAMPLE_H_
#define PACKAGER_MEDIA_BASE_SCTE35_SAMPLE_H_

#include <stdint.h>

#include <string>
#include <vector>


namespace shaka {
namespace media {

class SCTE35Event {
 public:
  SCTE35Event(const std::string& id,
             int64_t start_time,
             int64_t duration,
             uint32_t splice_event_id = 0);

  const std::string& id() const { return id_; }
  int64_t start_time() const { return start_time_; }
  int64_t duration() const { return duration_; }
  int64_t EndTime() const;

  // The real SCTE-35 splice_event_id from the splice_insert() command, used to
  // correlate a cue-in/return command with the cue-out it belongs to. Distinct
  // from id(), which carries either the raw hex-encoded section (cue-out) or a
  // fixed placeholder string (cue-in) -- neither of which is a usable, unique,
  // stable identifier on its own.
  uint32_t splice_event_id() const { return splice_event_id_; }

  int32_t sub_stream_index() const { return sub_stream_index_; }
  void set_sub_stream_index(int32_t idx) { sub_stream_index_ = idx; }

 private:
  // Allow the compiler generated copy constructor and assignment operator
  // intentionally. Since the text data is typically small, the performance
  // impact is minimal.

  const std::string id_;
  const int64_t start_time_ = 0;
  const int64_t duration_ = 0;
  const uint32_t splice_event_id_ = 0;
  int32_t sub_stream_index_ = -1;
};

}  // namespace media
}  // namespace shaka

#endif  // PACKAGER_MEDIA_BASE_SCTE35_SAMPLE_H_
// // Scte35Event represents cuepoint markers in input streams. It will be used
// // to represent out of band cuepoint markers too.
// struct Scte35Event {
//   std::string id;
//   // Segmentation type id from SCTE35 segmentation descriptor.
//   int type = 0;
//   double start_time_in_seconds = 0;
//   double duration_in_seconds = 0;
//   std::string cue_data;
// };