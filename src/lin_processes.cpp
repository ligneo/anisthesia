#include <charconv>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <anisthesia/lin_platform.hpp>
#include <anisthesia/lin_processes.hpp>

namespace anisthesia::lin::detail {

namespace fs = std::filesystem;

bool ParseProcessId(const std::string& str, int& process_id) {
  const auto end = str.data() + str.size();
  const auto [ptr, ec] = std::from_chars(str.data(), end, process_id);
  return ec == std::errc{} && ptr == end;
}

std::string GetCommandName(const fs::path& process_path) {
  std::ifstream file(process_path / "comm");
  std::string name;
  std::getline(file, name);
  return name;
}

std::string GetCommandName(const int process_id) {
  return GetCommandName(fs::path("/proc") / std::to_string(process_id));
}

// A player written in a scripting language runs as its interpreter, e.g.
// "python3.14" for GNOME's Showtime, while the command name is that of the
// script that was started.
bool IsInterpreter(const std::string_view name) {
  constexpr std::string_view prefixes[]{"python", "perl", "ruby"};
  for (const auto prefix : prefixes) {
    if (name.starts_with(prefix))
      return true;
  }
  return name == "node" || name == "gjs";
}

std::string GetProcessName(const fs::path& process_path) {
  // The executable path is only readable for our own processes, which is fine
  // since we cannot read the open files of other users' processes either.
  std::error_code ec;
  const auto executable_path = fs::read_symlink(process_path / "exe", ec);
  if (!ec && !executable_path.empty()) {
    // The kernel marks an executable that was replaced on disk, as happens to
    // players left running through a package upgrade.
    constexpr std::string_view deleted = " (deleted)";
    auto name = executable_path.filename().string();
    if (name.ends_with(deleted))
      name.resize(name.size() - deleted.size());
    if (IsInterpreter(name))
      return GetCommandName(process_path);
    return name;
  }

  // Fall back to the command name, which is truncated to 15 characters
  return GetCommandName(process_path);
}

bool EnumerateProcesses(process_proc_t process_proc) {
  std::error_code ec;
  fs::directory_iterator it("/proc", ec);
  if (ec)
    return false;

  for (const auto& entry : it) {
    Process process;
    if (!ParseProcessId(entry.path().filename().string(), process.id))
      continue;

    process.name = GetProcessName(entry.path());
    if (process.name.empty())
      continue;

    if (!process_proc(process))
      break;
  }

  return true;
}

}  // namespace anisthesia::lin::detail
