// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_HLS_BASE_SESSION_INDEX_WRITER_H_
#define PACKAGER_HLS_BASE_SESSION_INDEX_WRITER_H_

#include <string>
#include <vector>

#include <absl/time/civil_time.h>

namespace shaka {
namespace hls {

/// Writes a top-level JSON "session index" listing every hourly HLS master
/// playlist produced by HlsParams::rotate_manifest_hourly, so a DVR-style
/// tool can browse a whole live session as a sequence of hour-long
/// recordings. Not independently thread-safe -- callers (SimpleHlsNotifier)
/// must hold their own lock.
class SessionIndexWriter {
 public:
  /// @param output_path is where the JSON index file is written.
  explicit SessionIndexWriter(const std::string& output_path);
  virtual ~SessionIndexWriter();

  /// Appends one hourly entry and atomically rewrites the entire index file
  /// from the full in-memory entry list -- never a partial/append-only
  /// on-disk mutation, so a crash or concurrent reader mid-write never
  /// observes a corrupt/half-written file.
  /// @param hour_start is the UTC hour this master playlist covers.
  /// @param master_playlist_path is the (relative) path of the hourly
  ///        master playlist, e.g. "master_2026-08-14T15.m3u8".
  /// @return true on success, false otherwise.
  virtual bool AppendSegment(absl::CivilHour hour_start,
                            const std::string& master_playlist_path);

 private:
  struct Entry {
    std::string start;
    std::string master_playlist;
  };

  SessionIndexWriter(const SessionIndexWriter&) = delete;
  SessionIndexWriter& operator=(const SessionIndexWriter&) = delete;

  const std::string output_path_;
  std::vector<Entry> entries_;
};

}  // namespace hls
}  // namespace shaka

#endif  // PACKAGER_HLS_BASE_SESSION_INDEX_WRITER_H_
