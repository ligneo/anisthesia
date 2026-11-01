#pragma once

#include <functional>
#include <string>

namespace anisthesia::lin {

struct Process;

namespace detail {

using process_proc_t = std::function<bool(const Process&)>;

// The command name of the processes is left empty, as it is only needed for
// the few that turn out to be players.
bool EnumerateProcesses(process_proc_t process_proc);

std::string GetCommandName(int process_id);

}  // namespace detail

}  // namespace anisthesia::lin
