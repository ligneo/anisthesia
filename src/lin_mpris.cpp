#include <anisthesia/lin_mpris.hpp>

#ifdef ANISTHESIA_MPRIS

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <anisthesia/lin_dbus.hpp>
#include <anisthesia/media.hpp>
#include <anisthesia/util.hpp>

namespace anisthesia::lin::detail {

namespace {

constexpr std::string_view kServicePrefix = "org.mpris.MediaPlayer2.";
constexpr auto kObjectPath = "/org/mpris/MediaPlayer2";
constexpr auto kPlayerInterface = "org.mpris.MediaPlayer2.Player";
constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
constexpr auto kBusName = "org.freedesktop.DBus";
constexpr auto kBusPath = "/org/freedesktop/DBus";

// Detection is expected to run periodically, so an unresponsive player must not
// hold it up for long.
constexpr std::chrono::milliseconds kTimeout{100};
// The bus itself is not as slow as a player can be, but it can be busy.
constexpr std::chrono::seconds kBusTimeout{1};

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

// The readers below take a variant and skip it if it holds an unexpected type.

bool ReadVariant(dbus::Reader& reader, std::string& value) {
  std::string_view type;
  if (!reader.ReadSignature(type))
    return false;
  if (type != "s")
    return reader.Skip(type);

  std::string_view str;
  if (!reader.ReadString(str))
    return false;
  value = str;
  return true;
}

// Microseconds should be `x`, but some players send other integer types.
bool ReadVariant(dbus::Reader& reader, media_time_t& value) {
  std::string_view type;
  if (!reader.ReadSignature(type))
    return false;

  std::chrono::microseconds time{};

  if (type == "x" || type == "t") {
    std::uint64_t number = 0;
    if (!reader.ReadUint64(number))
      return false;
    time = std::chrono::microseconds{static_cast<std::int64_t>(number)};
  } else if (type == "i" || type == "u") {
    std::uint32_t number = 0;
    if (!reader.ReadUint32(number))
      return false;
    time = std::chrono::microseconds{type == "i"
                                         ? std::int64_t{static_cast<std::int32_t>(number)}
                                         : std::int64_t{number}};
  } else {
    return reader.Skip(type);
  }

  value = std::chrono::duration_cast<media_time_t>(time);
  return true;
}

// Reads an `a{sv}` dictionary, passing each key to `read_value`, which must
// read or skip the variant that follows it.
template <typename Proc>
bool ReadDictionary(dbus::Reader& reader, Proc read_value) {
  std::size_t end = 0;
  if (!reader.EnterArray(8, end))
    return false;

  while (reader.position() < end) {
    std::string_view key;
    if (!reader.Align(8) || !reader.ReadString(key) || !read_value(key))
      return false;
  }

  return reader.position() == end;
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

bool ReadPlayer(const dbus::Message& reply, MprisPlayer& player) {
  if (reply.type != dbus::MessageType::MethodReturn || reply.signature != "a{sv}")
    return false;

  dbus::Reader reader{reply.body, reply.order};

  auto skip_variant = [&reader] { return reader.Skip("v"); };

  auto read_metadata = [&](const std::string_view key) {
    if (key == "xesam:title")
      return ReadVariant(reader, player.title);
    if (key == "xesam:url")
      return ReadVariant(reader, player.url);
    if (key == "mpris:length")
      return ReadVariant(reader, player.duration);
    return skip_variant();
  };

  auto read_property = [&](const std::string_view key) {
    if (key == "PlaybackStatus") {
      std::string playback_status;
      if (!ReadVariant(reader, playback_status))
        return false;
      player.state = ToMediaState(playback_status);
      return true;
    }
    if (key == "Position")
      return ReadVariant(reader, player.position);
    if (key == "Metadata") {
      std::string_view type;
      if (!reader.ReadSignature(type))
        return false;
      return type == "a{sv}" ? ReadDictionary(reader, read_metadata)
                             : reader.Skip(type);
    }
    return skip_variant();
  };

  return ReadDictionary(reader, read_property);
}

std::vector<std::string> ListPlayerServices(dbus::Connection& bus) {
  const dbus::Call call{.destination = kBusName,
                        .path = kBusPath,
                        .interface = kBusName,
                        .member = "ListNames",
                        .argument = {}};

  std::vector<std::string> services;

  bus.Exchange({&call, 1}, kBusTimeout,
               [&services](std::size_t, const dbus::Message& reply) {
                 if (reply.type != dbus::MessageType::MethodReturn ||
                     reply.signature != "as")
                   return;

                 dbus::Reader reader{reply.body, reply.order};
                 std::size_t end = 0;
                 if (!reader.EnterArray(4, end))
                   return;

                 while (reader.position() < end) {
                   std::string_view name;
                   if (!reader.ReadString(name))
                     return;
                   if (name.starts_with(kServicePrefix))
                     services.emplace_back(name);
                 }
               });

  return services;
}

struct Player {
  std::string service;
  MprisPlayer player;
  bool ok = false;
};

std::vector<Player> FindPlayers(dbus::Connection& bus,
                                const std::vector<std::string>& services) {
  std::vector<dbus::Call> calls;
  calls.reserve(services.size());
  for (const auto& service : services) {
    calls.push_back({.destination = kBusName,
                     .path = kBusPath,
                     .interface = kBusName,
                     .member = "GetConnectionUnixProcessID",
                     .argument = service});
  }

  std::vector<Player> players(services.size());
  for (std::size_t i = 0; i < services.size(); ++i)
    players[i].service = services[i];

  bus.Exchange(calls, kBusTimeout,
               [&players](const std::size_t index, const dbus::Message& reply) {
                 if (reply.type != dbus::MessageType::MethodReturn ||
                     reply.signature != "u")
                   return;

                 dbus::Reader reader{reply.body, reply.order};
                 std::uint32_t process_id = 0;
                 if (reader.ReadUint32(process_id))
                   players[index].player.process_id = static_cast<int>(process_id);
               });

  return players;
}

}  // namespace

////////////////////////////////////////////////////////////////////////////////

bool EnumerateMprisPlayers(const std::vector<int>& process_ids,
                           mpris_proc_t mpris_proc) {
  dbus::Connection bus;
  if (!bus.OpenSession())
    return false;

  auto players = FindPlayers(bus, ListPlayerServices(bus));

  // Other services, including proxies such as playerctld, are not queried
  std::erase_if(players, [&process_ids](const Player& player) {
    const auto process_id = player.player.process_id;
    return std::ranges::find(process_ids, process_id) == process_ids.end() ||
           IsTorBrowser(process_id);
  });

  std::vector<dbus::Call> calls;
  calls.reserve(players.size());
  for (const auto& player : players) {
    calls.push_back({.destination = player.service,
                     .path = kObjectPath,
                     .interface = kPropertiesInterface,
                     .member = "GetAll",
                     .argument = kPlayerInterface});
  }

  bus.Exchange(calls, kTimeout,
               [&players](const std::size_t index, const dbus::Message& reply) {
                 players[index].ok = ReadPlayer(reply, players[index].player);
               });

  for (const auto& player : players) {
    if (player.ok && !mpris_proc(player.player))
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
