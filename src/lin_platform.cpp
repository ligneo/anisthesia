#include <algorithm>
#include <regex>
#include <string>
#include <vector>

#include <anisthesia/media.hpp>
#include <anisthesia/player.hpp>
#include <anisthesia/util.hpp>

#include <anisthesia/lin_platform.hpp>
#include <anisthesia/lin_processes.hpp>

namespace anisthesia::lin {

namespace detail {

bool IsPlayerProcess(const Process& process, const Player& player) {
  auto check_pattern = [](const std::string& pattern, const std::string& str) {
    if (pattern.empty())
      return false;
    if (pattern.front() == '^' && std::regex_match(str, std::regex(pattern)))
      return true;
    return anisthesia::detail::util::EqualStrings(pattern, str);
  };

  // Window classes are not available, so we can only check executables
  for (const auto& pattern : player.executables) {
    if (check_pattern(pattern, process.name))
      return true;
  }
  return false;
}

}  // namespace detail

////////////////////////////////////////////////////////////////////////////////

bool GetResults(const std::vector<Player>& players, media_proc_t media_proc,
                std::vector<Result>& results) {
  auto process_proc = [&](const Process& process) -> bool {
    for (const auto& player : players) {
      if (detail::IsPlayerProcess(process, player)) {
        results.push_back({player, process, {}});
        break;
      }
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
