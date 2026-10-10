#include "service.hpp"
#include "nlohmann/json.hpp"
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace service {

/* ---------------- paths ---------------- */

fs::path state_dir() {
    if (const char *x = std::getenv("XDG_STATE_HOME"); x && *x)
        return fs::path(x) / "mktsim";
    const char *home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local" / "state" / "mktsim";
}
fs::path pid_file() { return state_dir() / "exchange.pid"; }
fs::path log_file() { return state_dir() / "exchange.log"; }
static fs::path meta_file() { return state_dir() / "exchange.meta"; }
static fs::path derived_cfg() { return state_dir() / "exchange.json"; }

fs::path self_dir() {
    std::string p;
#ifdef __APPLE__
    char buf[4096];
    uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n) == 0)
        p = buf;
#else
    std::error_code link_ec;
    auto link = fs::read_symlink("/proc/self/exe", link_ec);
    if (!link_ec)
        p = link.string();
#endif
    if (p.empty())
        return {};
    std::error_code ec;
    auto canon = fs::weakly_canonical(p, ec);
    return (ec ? fs::path(p) : canon).parent_path();
}

fs::path repo_root() {
    fs::path here = self_dir();
    std::error_code ec;
    // engine/bin/mktsim or build/engine/mktsim: two levels up either way
    if (!here.empty()) {
        fs::path r = fs::weakly_canonical(here / ".." / "..", ec);
        if (!ec && fs::exists(r / "configs" / "default.json", ec))
            return r;
    }
    for (const char *base : {".", "..", "../.."}) {
        fs::path r = fs::weakly_canonical(fs::path(base), ec);
        if (!ec && fs::exists(r / "configs" / "default.json", ec))
            return r;
    }
    return {};
}

std::string find_exchange_server() {
    std::error_code ec;
    fs::path root = repo_root();
    if (!root.empty()) {
        for (const char *rel : {"build/gateway/exchange_server",
                                "gateway/build/exchange_server"}) {
            fs::path c = root / rel;
            if (fs::is_regular_file(c, ec))
                return c.string();
        }
    }
    return "exchange_server"; // PATH
}

/* ---------------- helpers ---------------- */

static bool pid_alive(pid_t pid) {
    if (pid <= 0)
        return false;
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

static pid_t read_pid() {
    std::ifstream in(pid_file());
    long v = 0;
    if (!(in >> v))
        return 0;
    return static_cast<pid_t>(v);
}

static std::string read_last_line(const fs::path &p) {
    std::ifstream in(p);
    std::string line, last;
    while (std::getline(in, line))
        if (!line.empty())
            last = line;
    return last;
}

static std::string fmt_duration(std::chrono::seconds s) {
    long t = s.count();
    std::ostringstream os;
    if (t >= 86400) os << t / 86400 << "d ";
    if (t >= 3600) os << (t % 86400) / 3600 << "h ";
    if (t >= 60) os << (t % 3600) / 60 << "m ";
    os << t % 60 << "s";
    return os.str();
}

static int usage_up() {
    std::cerr << "usage: mktsim up [config.json] [--flow calm|busy|load|off]\n";
    return 2;
}

// Resolves a config path: explicit, else configs/default.json in the repo.
static fs::path resolve_config(const char *given) {
    std::error_code ec;
    if (given) {
        fs::path p = given;
        if (fs::is_regular_file(p, ec))
            return fs::weakly_canonical(p, ec);
        return {};
    }
    fs::path root = repo_root();
    if (!root.empty())
        return root / "configs" / "default.json";
    return {};
}

/* ---------------- up ---------------- */

int cmd_up(int argc, char **argv) {
    const char *cfg_arg = nullptr;
    std::string flow;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--flow") {
            if (i + 1 >= argc)
                return usage_up();
            flow = argv[++i];
            if (flow != "calm" && flow != "busy" && flow != "load" && flow != "off")
                return usage_up();
        } else if (!a.empty() && a[0] == '-') {
            return usage_up();
        } else {
            cfg_arg = argv[i];
        }
    }

    pid_t pid = read_pid();
    if (pid_alive(pid)) {
        std::cout << "exchange already running (pid " << pid << "); use "
                     "`mktsim status` or `mktsim down`\n";
        return 0;
    }

    fs::path cfg = resolve_config(cfg_arg);
    if (cfg.empty()) {
        std::cerr << "mktsim up: config not found"
                  << (cfg_arg ? std::string(": ") + cfg_arg : "") << "\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(state_dir(), ec);

    // --flow: write a derived config with the flowgen ingress entry adjusted
    fs::path use_cfg = cfg;
    if (!flow.empty()) {
        std::ifstream in(cfg);
        json c = json::parse(in);
        json &ingress = c["ingress"];
        if (!ingress.is_array())
            ingress = json::array();
        bool found = false;
        for (auto &e : ingress) {
            if (e.value("type", "") == "flowgen") {
                found = true;
                e["enabled"] = (flow != "off");
                if (flow != "off")
                    e["profile"] = flow;
            }
        }
        if (!found && flow != "off")
            ingress.push_back({{"type", "flowgen"}, {"enabled", true},
                               {"profile", flow}, {"sessions", 2}});
        std::ofstream out(derived_cfg());
        out << c.dump(4) << "\n";
        use_cfg = derived_cfg();
    }

    std::string bin = find_exchange_server();
    fs::path root = repo_root();
    fs::path pidf = pid_file(), logf = log_file();

    pid_t child = ::fork();
    if (child < 0) {
        std::cerr << "mktsim up: fork failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (child == 0) {
        // Relative paths in the config (plugins) resolve against the repo
        if (!root.empty())
            (void)::chdir(root.c_str());
        std::string cfgs = use_cfg.string(), pids = pidf.string(), logs = logf.string();
        std::vector<std::string> args{bin, cfgs, "--daemon", "--pidfile", pids,
                                      "--logfile", logs};
        std::vector<char *> cargv;
        for (auto &s : args)
            cargv.push_back(s.data());
        cargv.push_back(nullptr);
        execvp(bin.c_str(), cargv.data());
        std::cerr << "mktsim up: cannot run " << bin << ": " << std::strerror(errno)
                  << "\nBuild it first: make (repo root) or cd gateway && make\n";
        _exit(127);
    }

    int st = 0;
    ::waitpid(child, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        std::cerr << "mktsim up: exchange_server failed to start"
                  << (WIFEXITED(st) ? " (exit " + std::to_string(WEXITSTATUS(st)) + ")" : "")
                  << "\n";
        std::string last = read_last_line(logf);
        if (!last.empty())
            std::cerr << "  log: " << last << "\n";
        return 1;
    }

    // The daemon writes its pid file once it is up
    for (int i = 0; i < 60; i++) {
        pid = read_pid();
        if (pid_alive(pid))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!pid_alive(pid)) {
        std::cerr << "mktsim up: no live pid in " << pidf << " after 3s\n";
        std::string last = read_last_line(logf);
        if (!last.empty())
            std::cerr << "  log: " << last << "\n";
        return 1;
    }

    {
        std::ofstream meta(meta_file());
        meta << "config=" << cfg.string() << "\n"
             << "used=" << use_cfg.string() << "\n"
             << "flow=" << (flow.empty() ? "config" : flow) << "\n"
             << "started="
             << std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count()
             << "\n";
    }

    std::cout << "exchange up (pid " << pid << ")\n"
              << "  config: " << cfg.string()
              << (flow.empty() ? "" : "  (flow " + flow + ")") << "\n"
              << "  log:    " << logf.string() << "\n"
              << "  next:   mktsim tui · mktsim connect · mktsim status · mktsim down\n";
    return 0;
}

/* ---------------- down ---------------- */

int cmd_down(int, char **) {
    pid_t pid = read_pid();
    if (!pid_alive(pid)) {
        std::cout << "exchange is not running\n";
        std::error_code ec;
        fs::remove(pid_file(), ec);
        return 0;
    }
    if (::kill(pid, SIGTERM) != 0) {
        std::cerr << "mktsim down: cannot signal pid " << pid << ": "
                  << std::strerror(errno) << "\n";
        return 1;
    }
    for (int i = 0; i < 200; i++) { // up to 10s for a graceful stop
        if (!pid_alive(pid)) {
            std::cout << "exchange stopped (pid " << pid << ")\n";
            std::error_code ec;
            fs::remove(pid_file(), ec);
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::cerr << "mktsim down: pid " << pid
              << " did not exit within 10s; `kill -9 " << pid << "` if it is stuck\n";
    return 1;
}

/* ---------------- status ---------------- */

int cmd_status(int, char **) {
    pid_t pid = read_pid();
    bool alive = pid_alive(pid);
    if (!alive) {
        std::cout << "exchange: not running\n";
        if (pid)
            std::cout << "  stale pid file " << pid_file() << " (pid " << pid << ")\n";
        std::string last = read_last_line(log_file());
        if (!last.empty())
            std::cout << "  last log: " << last << "\n";
        return 1;
    }

    std::cout << "exchange: running (pid " << pid << ")\n";

    // uptime from the meta file, else the pid file's mtime
    std::chrono::seconds up{0};
    std::string cfg, flow;
    {
        std::ifstream meta(meta_file());
        std::string line;
        long started = 0;
        while (std::getline(meta, line)) {
            if (line.rfind("config=", 0) == 0) cfg = line.substr(7);
            else if (line.rfind("flow=", 0) == 0) flow = line.substr(5);
            else if (line.rfind("started=", 0) == 0) started = std::atol(line.c_str() + 8);
        }
        long now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
        if (started > 0 && now >= started) {
            up = std::chrono::seconds(now - started);
        } else {
            struct stat sb{};
            if (::stat(pid_file().c_str(), &sb) == 0 && now >= sb.st_mtime)
                up = std::chrono::seconds(now - sb.st_mtime);
        }
    }
    std::cout << "  uptime: " << fmt_duration(up) << "\n";
    if (!cfg.empty())
        std::cout << "  config: " << cfg << (flow.empty() || flow == "config" ? "" : "  (flow " + flow + ")") << "\n";

    // Ports from the config actually in use
    std::error_code ec;
    fs::path used = fs::exists(derived_cfg(), ec) && flow != "config" && !flow.empty()
                        ? derived_cfg()
                        : fs::path(cfg);
    if (!used.empty() && fs::exists(used, ec)) {
        try {
            std::ifstream in(used);
            json c = json::parse(in);
            std::ostringstream ports;
            if (c.contains("ingress")) {
                for (auto &e : c["ingress"]) {
                    if (!e.value("enabled", true))
                        continue;
                    if (e.contains("port")) {
                        ports << "  " << e.value("type", "?") << " :" << e["port"].get<int>();
                    } else if (e.contains("config") && e["config"].contains("port")) {
                        ports << "  " << e.value("type", "?") << " :" << e["config"]["port"].get<int>();
                    } else if (e.value("type", "") == "flowgen") {
                        ports << "  flowgen(" << e.value("profile", "calm") << ")";
                    }
                }
            }
            if (c.contains("feed"))
                ports << "  feed " << c["feed"].value("group", "") << ":" << c["feed"].value("port", 0);
            if (c.contains("symbols"))
                std::cout << "  symbols: " << c["symbols"].size()
                          << "  shards: " << c.value("shards", 1) << "\n";
            std::cout << "  ports: " << ports.str() << "\n";
        } catch (...) {
        }
    }
    std::cout << "  log:    " << log_file() << "\n";
    std::string last = read_last_line(log_file());
    if (!last.empty())
        std::cout << "  last:   " << last << "\n";
    return 0;
}

/* ---------------- logs ---------------- */

int cmd_logs(int argc, char **argv) {
    bool follow = false;
    std::string n = "50";
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-f") follow = true;
        else if (a == "-n" && i + 1 < argc) n = argv[++i];
        else {
            std::cerr << "usage: mktsim logs [-f] [-n N]\n";
            return 2;
        }
    }
    fs::path logf = log_file();
    std::error_code ec;
    if (!fs::exists(logf, ec)) {
        std::cout << "no log yet at " << logf << " (run `mktsim up`)\n";
        return 1;
    }
    std::string path = logf.string();
    std::vector<std::string> args{"tail", "-n", n};
    if (follow)
        args.push_back("-f");
    args.push_back(path);
    std::vector<char *> cargv;
    for (auto &s : args)
        cargv.push_back(s.data());
    cargv.push_back(nullptr);
    execvp("tail", cargv.data());
    std::cerr << "mktsim logs: cannot run tail: " << std::strerror(errno) << "\n";
    return 1;
}

} // namespace service
