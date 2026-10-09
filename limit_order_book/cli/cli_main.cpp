// mktsim: command-line front end for the matching engine.
//
//   mktsim shell [config]        interactive shell on an in-process exchange
//   mktsim run SCRIPT [config]   run commands from a file ("-" for stdin)
//   mktsim connect [host:port] [SCRIPT|-]
//                                same commands over BOE to a running exchange
//   mktsim tui [feedviz args]    open the feed TUI (clients/feedviz)
//
// Config defaults to configs/default.json found relative to the working
// directory (see Exchange::LoadConfig).
#include "remote_session.hpp"
#include "session.hpp"
#include "shell.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
#include <cstring>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static int usage() {
    std::cerr << "usage:\n"
                 "  mktsim shell [config.json]\n"
                 "  mktsim run SCRIPT|- [config.json]\n"
                 "  mktsim connect [host:port] [SCRIPT|-]\n"
                 "  mktsim tui [feedviz args]\n";
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

// Directory of this executable, or "" if unknown
static std::filesystem::path self_dir() {
    std::string p;
#ifdef __APPLE__
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0)
        p = buf;
#else
    std::error_code ec;
    auto link = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec)
        p = link.string();
#endif
    if (p.empty())
        return {};
    std::error_code ec;
    auto canon = std::filesystem::weakly_canonical(p, ec);
    return (ec ? std::filesystem::path(p) : canon).parent_path();
}

// Finds the feedviz TUI binary: next to this binary's repo, or relative
// to the working directory, or on PATH.
static std::string find_feedviz() {
    namespace fs = std::filesystem;
    const fs::path rel = "clients/feedviz/target/release/feedviz";
    std::vector<fs::path> candidates;
    fs::path here = self_dir();
    if (!here.empty()) {
        // bin/mktsim -> limit_order_book -> repo root
        candidates.push_back(here / ".." / ".." / rel);
        candidates.push_back(here / ".." / rel);
    }
    for (const char *base : {".", "..", "../.."})
        candidates.push_back(fs::path(base) / rel);
    for (const auto &c : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(c, ec))
            return fs::weakly_canonical(c, ec).string();
    }
    return "feedviz"; // let execvp search PATH
}

// mktsim tui [args]: replaces this process with feedviz.
static int run_tui(int argc, char **argv) {
    std::string bin = find_feedviz();
    std::vector<std::string> args{bin};
    bool have_iface = false, replay = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--iface") have_iface = true;
        if (a == "--replay") replay = true;
        args.push_back(a);
    }
#ifdef __APPLE__
    // Same-host multicast on macOS needs the loopback interface, matching
    // "interface": "127.0.0.1" in the config's feed block.
    if (!have_iface && !replay) {
        args.push_back("--iface");
        args.push_back("127.0.0.1");
    }
#endif
    std::vector<char *> cargv;
    for (auto &s : args)
        cargv.push_back(s.data());
    cargv.push_back(nullptr);
    execvp(bin.c_str(), cargv.data());
    std::cerr << "mktsim: cannot run the TUI (" << bin << "): " << std::strerror(errno)
              << "\nBuild it first:\n  cd clients/feedviz && cargo build --release\n";
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2)
        return usage();
    std::string mode = argv[1];
    if (mode == "tui")
        return run_tui(argc, argv);

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
