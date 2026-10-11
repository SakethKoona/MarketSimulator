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
    // Python projects
    bool market = false;          // false: trader (strategy.py); true: population (market.py)
    std::string feed_group = "239.1.1.1";
    int feed_port = 30001;
    int snapshot_port = 30003;
    // C++ flow-model plugin
    int sessions = 8;
    int rate = 5000;              // orders per second target
};

// Writes the C++ adapter project. Returns "" on success or an error message.
std::string generate(const Spec &spec);

// Writes a Python project: strategy.py (trader) or market.py (population),
// plus a copy of <repo>/python/mktsim. No build step.
std::string generate_python(const Spec &spec);

// Writes a C++ flow-model plugin project: src/model.hpp (the user's order
// process) + generated src/flow_adapter.cpp that paces and submits it
// in-process. For load the Python path cannot reach.
std::string generate_flow_model(const Spec &spec);

// Config entry path for the built library (no extension; the exchange adds
// the platform suffix).
std::string library_path(const Spec &spec);

// Runs cmake configure + build in dir/build. Returns the exit status
// (0 = ok); output goes to the terminal.
int build(const Spec &spec);

} // namespace scaffold
