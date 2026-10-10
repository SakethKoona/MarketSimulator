#pragma once
// Scaffolds a custom ingress adapter project for the exchange, in the way a
// framework's `create` command lays out a new app: a buildable shared
// library against gateway/include/ingress/api.h where the TCP serving,
// sessions, order-id bookkeeping and report routing are already written,
// and the user fills in two functions, decode() and encode(), in
// src/protocol.hpp to speak their own wire format.
#include <filesystem>
#include <string>

namespace scaffold {

struct Spec {
    std::string name;             // library and project name, e.g. my_ingress
    std::filesystem::path dir;    // project directory to create
    std::filesystem::path repo;   // MarketSimulator checkout (for api.h)
    int port = 30030;             // default TCP port the adapter listens on
};

// Writes the project. Returns "" on success or an error message.
std::string generate(const Spec &spec);

// Config entry path for the built library (no extension; the exchange adds
// the platform suffix).
std::string library_path(const Spec &spec);

// Runs cmake configure + build in dir/build. Returns the exit status
// (0 = ok); output goes to the terminal.
int build(const Spec &spec);

} // namespace scaffold
