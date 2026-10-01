#include <algorithm>
#include <vector>

#include <anisthesia/media.hpp>

#include <anisthesia/lin_mpris.hpp>
#include <anisthesia/lin_open_files.hpp>
#include <anisthesia/lin_platform.hpp>

namespace anisthesia::lin::detail {

class Strategist {
public:
  Strategist(Result& result, media_proc_t media_proc,
             const std::vector<MprisPlayer>& mpris_players)
      : result_(result), media_proc_(media_proc), mpris_players_(mpris_players) {}

  bool ApplyStrategies();

private:
  bool AddMedia(const MediaInfo media_information);

  bool ApplyOpenFilesStrategy();
  bool ApplyMediaControlStrategy();

  Result& result_;
  media_proc_t media_proc_;
  const std::vector<MprisPlayer>& mpris_players_;
};

////////////////////////////////////////////////////////////////////////////////

bool Strategist::ApplyStrategies() {
  bool success = false;

  for (const auto strategy : result_.player.strategies) {
    switch (strategy) {
      case Strategy::OpenFiles:
        success |= ApplyOpenFilesStrategy();
        break;
      case Strategy::MediaControl:
        success |= ApplyMediaControlStrategy();
        break;
      // There is no generic way to read window titles on Wayland. Web
      // browsers report what they play via media control instead.
      case Strategy::WindowTitle:
      case Strategy::UiAutomation:
        break;
    }
  }

  return success;
}

bool ApplyStrategies(media_proc_t media_proc, std::vector<Result>& results) {
  bool success = false;

  // Players are listed once for all processes, and only if any may need them
  std::vector<int> process_ids;
  for (const auto& result : results) {
    const auto& strategies = result.player.strategies;
    if (std::ranges::find(strategies, Strategy::MediaControl) != strategies.end())
      process_ids.push_back(result.process.id);
  }

  std::vector<MprisPlayer> mpris_players;
  if (!process_ids.empty()) {
    EnumerateMprisPlayers(process_ids, [&mpris_players](const MprisPlayer& player) {
      mpris_players.push_back(player);
      return true;
    });
  }

  for (auto& result : results) {
    Strategist strategist(result, media_proc, mpris_players);
    success |= strategist.ApplyStrategies();
  }

  return success;
}

////////////////////////////////////////////////////////////////////////////////

bool Strategist::ApplyOpenFilesStrategy() {
  bool success = false;

  auto open_files_proc = [this, &success](const OpenFile& open_file) -> bool {
    success |= AddMedia({MediaInfoType::File, open_file.path});
    return true;
  };

  EnumerateOpenFiles(result_.process.id, open_files_proc);

  return success;
}

bool Strategist::ApplyMediaControlStrategy() {
  bool success = false;

  for (const auto& player : mpris_players_) {
    if (player.process_id != result_.process.id)
      continue;
    // Stopped players keep reporting what they played last
    if (player.state == MediaState::Stopped)
      continue;
    // A page title alone could be any page; only the address tells which site
    // it is from. Chromium leaves it out.
    if (result_.player.type == PlayerType::WebBrowser && player.url.empty())
      continue;

    Media media;
    media.state = player.state;
    media.duration = player.duration;
    media.position = player.position;

    for (const auto type : {MediaInfoType::Title, MediaInfoType::Url}) {
      MediaInfo information{type, type == MediaInfoType::Title ? player.title
                                                               : player.url};
      if (!information.value.empty() && media_proc_(information))
        media.information.push_back(std::move(information));
    }

    if (media.information.empty())
      continue;

    result_.media.push_back(std::move(media));
    success = true;
  }

  return success;
}

////////////////////////////////////////////////////////////////////////////////

bool Strategist::AddMedia(const MediaInfo media_information) {
  if (media_information.value.empty())
    return false;

  if (!media_proc_(media_information))
    return false;

  Media media{};
  media.information.push_back(media_information);
  result_.media.push_back(std::move(media));

  return true;
}

}  // namespace anisthesia::lin::detail
