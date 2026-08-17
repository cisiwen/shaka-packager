// Copyright 2026 Google LLC. All rights reserved.
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include <packager/hls/base/session_index_writer.h>

#include <string>

#include <absl/time/civil_time.h>
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <packager/file.h>

namespace shaka {
namespace hls {

namespace {
const char kMemoryFilePath[] = "memory://session_index.json";
}

TEST(SessionIndexWriterTest, AppendSegmentWritesValidGrowingJson) {
  SessionIndexWriter writer(kMemoryFilePath);

  ASSERT_TRUE(writer.AppendSegment(absl::CivilHour(2026, 8, 14, 15, 0, 0),
                                   "master_2026-08-14T15.m3u8"));
  std::string content1;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &content1));
  nlohmann::json parsed = nlohmann::json::parse(content1);
  ASSERT_EQ(1u, parsed["segments"].size());
  EXPECT_EQ("2026-08-14T15:00:00Z", parsed["segments"][0]["start"]);
  EXPECT_EQ("master_2026-08-14T15.m3u8",
            parsed["segments"][0]["master_playlist"]);

  ASSERT_TRUE(writer.AppendSegment(absl::CivilHour(2026, 8, 14, 16, 0, 0),
                                   "master_2026-08-14T16.m3u8"));
  std::string content2;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &content2));
  parsed = nlohmann::json::parse(content2);
  ASSERT_EQ(2u, parsed["segments"].size());
  EXPECT_EQ("2026-08-14T15:00:00Z", parsed["segments"][0]["start"]);
  EXPECT_EQ("2026-08-14T16:00:00Z", parsed["segments"][1]["start"]);
  EXPECT_EQ("master_2026-08-14T16.m3u8",
            parsed["segments"][1]["master_playlist"]);

  ASSERT_TRUE(writer.AppendSegment(absl::CivilHour(2026, 8, 14, 17, 0, 0),
                                   "master_2026-08-14T17.m3u8"));
  std::string content3;
  ASSERT_TRUE(File::ReadFileToString(kMemoryFilePath, &content3));
  parsed = nlohmann::json::parse(content3);
  ASSERT_EQ(3u, parsed["segments"].size());
  EXPECT_EQ("master_2026-08-14T17.m3u8",
            parsed["segments"][2]["master_playlist"]);
}

}  // namespace hls
}  // namespace shaka
