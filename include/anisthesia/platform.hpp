#pragma once

#include <cstdint>
#include <vector>

#include <anisthesia/media.hpp>
#include <anisthesia/player.hpp>

namespace anisthesia {

// Tells a running player apart from others between calls, e.g. two windows of
// the same player. What it holds depends on what the platform can provide.
struct PlayerId {
  std::int64_t process = 0;
  std::uintptr_t window = 0;  // native window handle, if there is one

  bool operator==(const PlayerId&) const = default;
};

struct Result {
  Player player;
  PlayerId id;
  std::vector<Media> media;
};

// Picks the implementation of the current platform, so that applications do
// not need to know about `win` or `lin`. Returns false on other platforms.
bool GetResults(const std::vector<Player>& players, media_proc_t media_proc,
                std::vector<Result>& results);

}  // namespace anisthesia
