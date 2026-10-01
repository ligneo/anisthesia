#include <anisthesia/lin_mpris.hpp>

#ifdef ANISTHESIA_MPRIS

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <systemd/sd-bus.h>

#include <anisthesia/media.hpp>
#include <anisthesia/util.hpp>

namespace anisthesia::lin::detail {

namespace {

constexpr std::string_view kServicePrefix = "org.mpris.MediaPlayer2.";
constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";

// Detection is expected to run periodically, so an unresponsive player must not
// hold it up for long.
constexpr std::uint64_t kTimeout = 100'000;  // microseconds

struct BusDeleter {
  void operator()(sd_bus* bus) const { sd_bus_flush_close_unref(bus); }
};
struct MessageDeleter {
  void operator()(sd_bus_message* message) const { sd_bus_message_unref(message); }
};
struct CredsDeleter {
  void operator()(sd_bus_creds* creds) const { sd_bus_creds_unref(creds); }
};
struct StrvDeleter {
  void operator()(char** strv) const {
    for (auto str = strv; *str; ++str)
      std::free(*str);
    std::free(strv);
  }
};

using bus_ptr = std::unique_ptr<sd_bus, BusDeleter>;
using message_ptr = std::unique_ptr<sd_bus_message, MessageDeleter>;
using creds_ptr = std::unique_ptr<sd_bus_creds, CredsDeleter>;
using strv_ptr = std::unique_ptr<char*[], StrvDeleter>;

// Tor Browser is Firefox, down to the process name and the MPRIS service, so
// the Firefox entry would match it and report the title of whatever page is
// open. Only the path tells the two apart: the bundle is extracted into a
// `tor-browser` directory, and torbrowser-launcher keeps that layout under
// `~/.local/share/torbrowser`. Reporting what someone is doing in Tor Browser
// is the last thing this should do, so it is left out rather than made an
// option.
bool IsTorBrowser(const int process_id) {
  std::error_code ec;
  const auto path = std::filesystem::read_symlink(
      "/proc/" + std::to_string(process_id) + "/exe", ec);

  for (const auto& part : path) {
    if (anisthesia::detail::util::EqualStrings(part.string(), "tor-browser") ||
        anisthesia::detail::util::EqualStrings(part.string(), "torbrowser"))
      return true;
  }
  return false;
}

int GetProcessId(sd_bus* bus, const char* service) {
  sd_bus_creds* creds = nullptr;
  if (sd_bus_get_name_creds(bus, service, SD_BUS_CREDS_PID, &creds) < 0)
    return 0;
  const creds_ptr creds_guard{creds};

  pid_t process_id = 0;
  if (sd_bus_creds_get_pid(creds, &process_id) < 0)
    return 0;
  return process_id;
}

// The readers below take a variant and skip it if it holds an unexpected type.

std::string_view PeekVariant(sd_bus_message* message) {
  const char* contents = nullptr;
  if (sd_bus_message_peek_type(message, nullptr, &contents) < 0 || !contents)
    return {};
  return contents;
}

bool ReadVariant(sd_bus_message* message, std::string& value) {
  if (PeekVariant(message) != "s")
    return sd_bus_message_skip(message, "v") >= 0;

  const char* str = nullptr;
  if (sd_bus_message_read(message, "v", "s", &str) < 0)
    return false;
  value = str ? str : "";
  return true;
}

// Microseconds should be `x`, but some players send other integer types.
bool ReadVariant(sd_bus_message* message, media_time_t& value) {
  const auto type = PeekVariant(message);
  std::chrono::microseconds time{};

  if (type == "x" || type == "t") {
    std::int64_t number = 0;
    if (sd_bus_message_read(message, "v", type.data(), &number) < 0)
      return false;
    time = std::chrono::microseconds{number};
  } else if (type == "i" || type == "u") {
    std::uint32_t number = 0;
    if (sd_bus_message_read(message, "v", type.data(), &number) < 0)
      return false;
    time = std::chrono::microseconds{type == "i"
                                         ? static_cast<std::int32_t>(number)
                                         : number};
  } else {
    return sd_bus_message_skip(message, "v") >= 0;
  }

  value = std::chrono::duration_cast<media_time_t>(time);
  return true;
}

// Reads an `a{sv}` dictionary, passing each key to `read_value`, which must
// read or skip the variant that follows it.
template <typename Proc>
bool ReadDictionary(sd_bus_message* message, Proc read_value) {
  if (sd_bus_message_enter_container(message, 'a', "{sv}") <= 0)
    return false;

  int r = 0;
  while ((r = sd_bus_message_enter_container(message, 'e', "sv")) > 0) {
    const char* key = nullptr;
    if (sd_bus_message_read(message, "s", &key) < 0 || !read_value(key))
      return false;
    if (sd_bus_message_exit_container(message) < 0)
      return false;
  }

  return r == 0 && sd_bus_message_exit_container(message) >= 0;
}

MediaState ToMediaState(const std::string_view playback_status) {
  if (playback_status == "Playing")
    return MediaState::Playing;
  if (playback_status == "Paused")
    return MediaState::Paused;
  if (playback_status == "Stopped")
    return MediaState::Stopped;
  return MediaState::Unknown;
}

bool GetPlayer(sd_bus* bus, const char* service, MprisPlayer& player) {
  sd_bus_message* request = nullptr;
  if (sd_bus_message_new_method_call(bus, &request, service, kObjectPath,
                                     "org.freedesktop.DBus.Properties",
                                     "GetAll") < 0)
    return false;
  const message_ptr request_guard{request};

  if (sd_bus_message_append(request, "s", kPlayerInterface) < 0)
    return false;

  sd_bus_message* reply = nullptr;
  if (sd_bus_call(bus, request, kTimeout, nullptr, &reply) < 0)
    return false;
  const message_ptr reply_guard{reply};

  auto read_metadata = [&](const std::string_view key) {
    if (key == "xesam:title")
      return ReadVariant(reply, player.title);
    if (key == "xesam:url")
      return ReadVariant(reply, player.url);
    if (key == "mpris:length")
      return ReadVariant(reply, player.duration);
    return sd_bus_message_skip(reply, "v") >= 0;
  };

  auto read_property = [&](const std::string_view key) {
    if (key == "PlaybackStatus") {
      std::string playback_status;
      if (!ReadVariant(reply, playback_status))
        return false;
      player.state = ToMediaState(playback_status);
      return true;
    }
    if (key == "Position")
      return ReadVariant(reply, player.position);
    if (key == "Metadata") {
      if (PeekVariant(reply) != "a{sv}")
        return sd_bus_message_skip(reply, "v") >= 0;
      return sd_bus_message_enter_container(reply, 'v', "a{sv}") > 0 &&
             ReadDictionary(reply, read_metadata) &&
             sd_bus_message_exit_container(reply) >= 0;
    }
    return sd_bus_message_skip(reply, "v") >= 0;
  };

  return ReadDictionary(reply, read_property);
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////

bool EnumerateMprisPlayers(const std::vector<int>& process_ids,
                           mpris_proc_t mpris_proc) {
  sd_bus* bus = nullptr;
  if (sd_bus_open_user(&bus) < 0)
    return false;
  const bus_ptr bus_guard{bus};

  char** names = nullptr;
  if (sd_bus_list_names(bus, &names, nullptr) < 0)
    return false;
  const strv_ptr names_guard{names};

  for (auto name = names; name && *name; ++name) {
    if (!std::string_view{*name}.starts_with(kServicePrefix))
      continue;

    MprisPlayer player;

    // Other services, including proxies such as playerctld, are not queried
    player.process_id = GetProcessId(bus, *name);
    if (std::ranges::find(process_ids, player.process_id) == process_ids.end())
      continue;
    if (IsTorBrowser(player.process_id))
      continue;

    if (!GetPlayer(bus, *name, player))
      continue;

    if (!mpris_proc(player))
      break;
  }

  return true;
}

}  // namespace anisthesia::lin::detail

#else

namespace anisthesia::lin::detail {

bool EnumerateMprisPlayers(const std::vector<int>&, mpris_proc_t) {
  return false;
}

}  // namespace anisthesia::lin::detail

#endif
