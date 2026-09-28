#include <cstdint>
#include <utility>
#include <vector>

#include <anisthesia/media.hpp>
#include <anisthesia/platform.hpp>
#include <anisthesia/player.hpp>

#ifdef _WIN32
#include <anisthesia/win_platform.hpp>
#elif defined(__linux__)
#include <anisthesia/lin_platform.hpp>
#endif

namespace anisthesia {

#if defined(_WIN32) || defined(__linux__)

namespace detail {

#ifdef _WIN32
namespace native = win;
#else
namespace native = lin;
#endif

Result ToResult(native::Result&& result) {
  PlayerId id{.process = result.process.id};
#ifdef _WIN32
  id.window = reinterpret_cast<std::uintptr_t>(result.window.handle);
#endif

  return {std::move(result.player), id, std::move(result.media)};
}

}  // namespace detail

////////////////////////////////////////////////////////////////////////////////

bool GetResults(const std::vector<Player>& players, media_proc_t media_proc,
                std::vector<Result>& results) {
  std::vector<detail::native::Result> native_results;
  const bool success =
      detail::native::GetResults(players, media_proc, native_results);

  for (auto& result : native_results) {
    results.push_back(detail::ToResult(std::move(result)));
  }

  return success;
}

#else

bool GetResults(const std::vector<Player>&, media_proc_t,
                std::vector<Result>&) {
  return false;
}

#endif

}  // namespace anisthesia
