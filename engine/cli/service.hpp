#pragma once
// mktsim up / down / status / logs: lifecycle of the exchange_server daemon.
//
// Paths are shared with exchange_server --daemon so both tools agree:
//   ${XDG_STATE_HOME:-$HOME/.local/state}/mktsim/exchange.pid
//   ${XDG_STATE_HOME:-$HOME/.local/state}/mktsim/exchange.log
// `up` also writes exchange.json (the config actually used, after --flow)
// and exchange.meta (config path and start time) next to them.
#include <filesystem>
#include <string>

namespace service {

std::filesystem::path state_dir();
std::filesystem::path pid_file();
std::filesystem::path log_file();

// Directory of the running mktsim binary ("" if unknown) and the repo root
// derived from it (engine/bin/mktsim -> repo).
std::filesystem::path self_dir();
std::filesystem::path repo_root();

// Finds exchange_server: CMake or engine-Makefile build dirs, then PATH.
std::string find_exchange_server();

int cmd_init(int argc, char **argv);   // mktsim init: interactive config builder
int cmd_up(int argc, char **argv);     // mktsim up [config] [--flow ...] [-i|-y]
int cmd_down(int argc, char **argv);   // mktsim down
int cmd_status(int argc, char **argv); // mktsim status
int cmd_logs(int argc, char **argv);   // mktsim logs [-f] [-n N]

} // namespace service
