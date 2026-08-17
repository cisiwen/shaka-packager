// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/hls/base/session_index_writer.h>

#include <absl/log/log.h>
#include <absl/strings/str_format.h>
#include <nlohmann/json.hpp>

#include <packager/file.h>

namespace shaka {
namespace hls {

namespace {
std::string FormatIso8601Hour(absl::CivilHour hour) {
  return absl::StrFormat("%04d-%02d-%02dT%02d:00:00Z", hour.year(),
                        hour.month(), hour.day(), hour.hour());
}
}  // namespace

SessionIndexWriter::SessionIndexWriter(const std::string& output_path)
    : output_path_(output_path) {}

SessionIndexWriter::~SessionIndexWriter() {}

bool SessionIndexWriter::AppendSegment(
    absl::CivilHour hour_start,
    const std::string& master_playlist_path) {
  entries_.push_back({FormatIso8601Hour(hour_start), master_playlist_path});

  nlohmann::json segments = nlohmann::json::array();
  for (const auto& entry : entries_) {
    segments.push_back(
        {{"start", entry.start}, {"master_playlist", entry.master_playlist}});
  }
  nlohmann::json root;
  root["segments"] = segments;

  if (!File::WriteFileAtomically(output_path_.c_str(), root.dump(2))) {
    LOG(ERROR) << "Failed to write session index to: " << output_path_;
    return false;
  }
  return true;
}

}  // namespace hls
}  // namespace shaka
