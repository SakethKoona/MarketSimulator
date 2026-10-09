// mktsim: command-line front end for the matching engine.
//
//   mktsim shell [config]        interactive shell on an in-process exchange
//   mktsim run SCRIPT [config]   run commands from a file ("-" for stdin)
//   mktsim connect [host:port] [SCRIPT|-]
//                                same commands over BOE to a running exchange
//
// Config defaults to configs/default.json found relative to the working
// directory (see Exchange::LoadConfig).
#include "remote_session.hpp"
#include "session.hpp"
#include "shell.hpp"
#include <fstream>
#include <iostream>
#include <unistd.h>

static int usage() {
    std::cerr << "usage:\n"
                 "  mktsim shell [config.json]\n"
                 "  mktsim run SCRIPT|- [config.json]\n"
                 "  mktsim connect [host:port] [SCRIPT|-]\n";
    return 2;
}

static json load_cfg(const char *path) {
    std::vector<std::string> candidates;
    if (path)
        candidates.push_back(path);
    else
        candidates = {"configs/default.json", "../configs/default.json",
                      "../../configs/default.json"};
    for (const auto &p : candidates) {
        std::ifstream f(p);
        if (f)
            return json::parse(f);
    }
    std::string msg = "config not found; tried:";
    for (const auto &p : candidates)
        msg += " " + p;
    throw std::runtime_error(msg);
}

int main(int argc, char **argv) {
    if (argc < 2)
        return usage();
    std::string mode = argv[1];

    try {
        if (mode == "shell") {
            json cfg = load_cfg(argc > 2 ? argv[2] : nullptr);
            LocalSession session(cfg);
            Shell shell(session, std::cout, std::cerr);
            bool tty = isatty(fileno(stdin));
            if (tty)
                std::cout << "mktsim: " << session.Describe()
                          << ". Type help.\n";
            shell.Repl(std::cin, tty);
            return 0;
        }
        if (mode == "run") {
            if (argc < 3)
                return usage();
            json cfg = load_cfg(argc > 3 ? argv[3] : nullptr);
            LocalSession session(cfg);
            Shell shell(session, std::cout, std::cerr);
            std::string path = argv[2];
            int failed;
            if (path == "-") {
                failed = shell.Repl(std::cin, false);
            } else {
                std::ifstream in(path);
                if (!in) {
                    std::cerr << "mktsim: cannot open " << path << "\n";
                    return 2;
                }
                failed = shell.Repl(in, false);
            }
            if (failed)
                std::cerr << "mktsim: " << failed << " command(s) failed\n";
            return failed ? 1 : 0;
        }
        if (mode == "connect") {
            std::string host = "127.0.0.1";
            std::uint16_t port = 30000;
            const char *script = nullptr;
            for (int i = 2; i < argc; i++) {
                std::string a = argv[i];
                if (a == "-" || a.find('/') != std::string::npos ||
                    a.find(".txt") != std::string::npos) {
                    script = argv[i];
                } else {
                    auto c = a.find(':');
                    host = c == std::string::npos ? a : a.substr(0, c);
                    if (c != std::string::npos)
                        port = static_cast<std::uint16_t>(std::stoi(a.substr(c + 1)));
                }
            }
            RemoteSession session(host, port);
            Shell shell(session, std::cout, std::cerr);
            if (!script) {
                bool tty = isatty(fileno(stdin));
                if (tty)
                    std::cout << "mktsim: " << session.Describe()
                              << ". Type help.\n";
                shell.Repl(std::cin, tty);
                return 0;
            }
            int failed;
            if (std::string(script) == "-") {
                failed = shell.Repl(std::cin, false);
            } else {
                std::ifstream in(script);
                if (!in) {
                    std::cerr << "mktsim: cannot open " << script << "\n";
                    return 2;
                }
                failed = shell.Repl(in, false);
            }
            if (failed)
                std::cerr << "mktsim: " << failed << " command(s) failed\n";
            return failed ? 1 : 0;
        }
    } catch (const std::exception &e) {
        std::cerr << "mktsim: " << e.what() << "\n";
        return 1;
    }
    return usage();
}
