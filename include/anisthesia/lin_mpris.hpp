#pragma once

#include <functional>
#include <string>

#include <anisthesia/media.hpp>

namespace anisthesia::lin::detail {

// A media player on the session bus, as reported via MPRIS:
// https://specifications.freedesktop.org/mpris-spec/latest/
struct MprisPlayer {
  int process_id = 0;
  MediaState state = MediaState::Unknown;
  media_time_t duration{};
  media_time_t position{};
  std::string title;
  std::string url;
};

using mpris_proc_t = std::function<bool(const MprisPlayer&)>;

bool EnumerateMprisPlayers(mpris_proc_t mpris_proc);

}  // namespace anisthesia::lin::detail
