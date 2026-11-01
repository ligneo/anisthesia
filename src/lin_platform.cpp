#include <algorithm>
#include <regex>
#include <string>
#include <utility>
#include <vector>

#include <anisthesia/media.hpp>
#include <anisthesia/player.hpp>
#include <anisthesia/util.hpp>

#include <anisthesia/lin_platform.hpp>
#include <anisthesia/lin_processes.hpp>

namespace anisthesia::lin {

namespace detail {

// Applications spawn helper processes that share the same executable (e.g.
// `ipc/thumbfast` of mpv, `Web Content` of Firefox). They may keep files open
// long after the player itself is gone, so they must not be mistaken for it.
// The kernel gives each process its own name, which is what tells them apart.
bool IsHelperProcess(const Process& process) {
  if (process.comm.empty())
    return false;

  // The command name is truncated to 15 characters
  const auto name = process.name.substr(0, process.comm.size());
  return !anisthesia::detail::util::EqualStrings(name, process.comm);
}

// Window classes are not available, so players are told apart by their
// executables only. The patterns are prepared once for all processes, since
// compiling a regular expression costs far more than matching it.
class PlayerMatcher {
public:
  explicit PlayerMatcher(const Player& player) : player_(player) {
    for (const auto& pattern : player.executables) {
      if (!pattern.empty() && pattern.front() == '^')
        regexes_.emplace_back(pattern);
    }
  }

  const Player& player() const { return player_; }

  bool Matches(const Process& process) const {
    for (const auto& regex : regexes_) {
      if (std::regex_match(process.name, regex))
        return true;
    }
    for (const auto& pattern : player_.executables) {
      if (!pattern.empty() &&
          anisthesia::detail::util::EqualStrings(pattern, process.name))
        return true;
    }
    return false;
  }

private:
  const Player& player_;
  std::vector<std::regex> regexes_;
};

}  // namespace detail

////////////////////////////////////////////////////////////////////////////////

bool GetResults(const std::vector<Player>& players, media_proc_t media_proc,
                std::vector<Result>& results) {
  std::vector<detail::PlayerMatcher> matchers;
  matchers.reserve(players.size());
  for (const auto& player : players)
    matchers.emplace_back(player);

  auto process_proc = [&](const Process& process) -> bool {
    for (const auto& matcher : matchers) {
      if (!matcher.Matches(process))
        continue;
      Process player_process = process;
      player_process.comm = detail::GetCommandName(process.id);
      if (!detail::IsHelperProcess(player_process))
        results.push_back({matcher.player(), std::move(player_process), {}});
      break;
    }
    return true;
  };

  if (!detail::EnumerateProcesses(process_proc))
    return false;

  const bool success = detail::ApplyStrategies(media_proc, results);

  // Unlike windows, processes without any media are common (e.g. helper
  // processes of the same application), so they are not worth returning.
  std::erase_if(results, [](const Result& result) {
    return result.media.empty();
  });

  return success;
}

}  // namespace anisthesia::lin
