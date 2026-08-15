// Copyright 2016 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef PACKAGER_HLS_BASE_SIMPLE_HLS_NOTIFIER_H_
#define PACKAGER_HLS_BASE_SIMPLE_HLS_NOTIFIER_H_

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <absl/synchronization/mutex.h>
#include <absl/time/civil_time.h>
#include <absl/time/time.h>

#include <packager/cea_caption.h>
#include <packager/hls/base/hls_notifier.h>
#include <packager/hls/base/master_playlist.h>
#include <packager/hls/base/media_playlist.h>
#include <packager/hls/base/session_index_writer.h>
#include <packager/hls_params.h>
#include <packager/macros/classes.h>
#include <packager/mpd/base/media_info.pb.h>

namespace shaka {
namespace hls {

/// The encryption info last applied to a stream's MediaPlaylist, kept
/// around so it can be replayed onto a freshly rotated MediaPlaylist (see
/// HlsParams::rotate_manifest_hourly) -- a rotated instance starts with no
/// EXT-X-KEY entries otherwise, silently dropping encryption signaling for
/// the new hour.
struct EncryptionInfoParams {
  MediaPlaylist::EncryptionMethod method = MediaPlaylist::EncryptionMethod::kNone;
  std::string uri;
  std::string key_id;
  std::string iv;
  std::string key_format;
  std::string key_format_versions;
};

/// For testing.
/// Creates MediaPlaylist. Mock this and return mock MediaPlaylist.
class MediaPlaylistFactory {
 public:
  virtual ~MediaPlaylistFactory();
  virtual std::unique_ptr<MediaPlaylist> Create(const HlsParams& hls_params,
                                                const std::string& file_name,
                                                const std::string& name,
                                                const std::string& group_id,
                                                bool is_rotation = false);
};

/// This is thread safe.
class SimpleHlsNotifier : public HlsNotifier {
 public:
  /// @param hls_params contains parameters for setting up the notifier.
  explicit SimpleHlsNotifier(const HlsParams& hls_params);
  ~SimpleHlsNotifier() override;

  /// @name HlsNotifier implemetation overrides.
  /// @{
  bool Init() override;
  bool NotifyNewStream(const MediaInfo& media_info,
                       const std::string& playlist_name,
                       const std::string& stream_name,
                       const std::string& group_id,
                       uint32_t* stream_id) override;
  bool NotifySampleDuration(uint32_t stream_id,
                            int32_t sample_duration) override;
  bool NotifyNewSegment(uint32_t stream_id,
                        const std::string& segment_name,
                        int64_t start_time,
                        int64_t duration,
                        uint64_t start_byte_offset,
                        uint64_t size) override;
  bool NotifyKeyFrame(uint32_t stream_id,
                      int64_t timestamp,
                      uint64_t start_byte_offset,
                      uint64_t size) override;
  bool NotifyCueEvent(uint32_t container_id, int64_t timestamp) override;
  bool NotifySCTE35Event(int64_t timestamp, int64_t duration, const std::string& cue_data,
                        uint32_t splice_event_id) override;
  bool NotifyEncryptionUpdate(
      uint32_t stream_id,
      const std::vector<uint8_t>& key_id,
      const std::vector<uint8_t>& system_id,
      const std::vector<uint8_t>& iv,
      const std::vector<uint8_t>& protection_system_specific_data) override;

  bool NotifyEndOfStream() override;

  bool Flush() override;
  /// }@

 protected:
  const absl::Time& reference_time() const { return reference_time_; }

 private:
  friend class SimpleHlsNotifierTest;

  struct StreamEntry {
    std::unique_ptr<MediaPlaylist> media_playlist;
    MediaPlaylist::EncryptionMethod encryption_method;
    // The playlist's relative path before any hour suffix is applied --
    // needed to rebuild the next hour's rotated file name. Empty/unused
    // unless hls_params().rotate_manifest_hourly is true.
    std::string base_playlist_path;
    // Last-applied encryption info, if any, so it can be replayed onto a
    // freshly rotated MediaPlaylist.
    std::optional<EncryptionInfoParams> last_encryption_info;
  };

  // Rotates media/master playlists to fresh, hour-suffixed files if the
  // UTC hour (per RotationNow()) has advanced since the last check, and no
  // stream currently has an open SCTE-35 ad break (rotation is deferred,
  // retried on the next segment, until the break closes). No-op unless
  // hls_params().rotate_manifest_hourly is true. Called at the top of
  // NotifyNewSegment(), under |lock_|.
  void RotateManifestsIfNeeded();
  // "Now" for rotation purposes: real wall clock, unless
  // hls_params().manifest_rotation_test_interval_seconds is set (test-only),
  // in which case elapsed time is dilated so a rotation happens roughly
  // every N real seconds instead of on a genuine UTC hour boundary.
  absl::Time RotationNow() const;

  std::string master_playlist_dir_;
  int32_t target_duration_ = 0;
  bool end_stream = false;

  std::unique_ptr<MediaPlaylistFactory> media_playlist_factory_;
  std::unique_ptr<MasterPlaylist> master_playlist_;

  // Maps to unique_ptr because StreamEntry also holds unique_ptr
  std::map<uint32_t, std::unique_ptr<StreamEntry>> stream_map_;
  std::list<MediaPlaylist*> media_playlists_;

  uint32_t sequence_number_ = 0;

  absl::Mutex lock_;
  absl::Time reference_time_ = absl::InfinitePast();

  // -- Hourly manifest rotation state (HlsParams::rotate_manifest_hourly) --
  absl::CivilHour current_hour_;
  std::string master_playlist_base_file_name_;
  std::string default_audio_language_;
  std::string default_text_language_;
  std::vector<CeaCaption> default_closed_captions_;
  absl::Time rotation_start_time_ = absl::InfinitePast();
  std::unique_ptr<SessionIndexWriter> session_index_writer_;

  DISALLOW_COPY_AND_ASSIGN(SimpleHlsNotifier);
};

}  // namespace hls
}  // namespace shaka

#endif  // PACKAGER_HLS_BASE_SIMPLE_HLS_NOTIFIER_H_
