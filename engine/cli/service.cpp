#include "service.hpp"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
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
    std::cerr << "usage: mktsim up [config.json] [--flow calm|busy|load|off] [-i] [-y]\n"
                 "  no config + a terminal: interactive setup; -y uses configs/default.json\n";
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

/* ---------------- launch ---------------- */

// Starts exchange_server --daemon on `use_cfg`. `cfg` is what the user
// asked for (recorded in status), `flow` a label for status ("" = as in
// the config).
static int launch(const fs::path &cfg, const fs::path &use_cfg,
                  const std::string &flow) {
    std::error_code ec;
    fs::create_directories(state_dir(), ec);

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
        for (auto &a : args)
            cargv.push_back(a.data());
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

    pid_t pid = 0;
    for (int i = 0; i < 60; i++) { // the daemon writes its pid once it is up
        pid = read_pid();
        if (pid_alive(pid))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!pid_alive(pid)) {
        std::cerr << "mktsim up: no live pid in " << pidf.string() << " after 3s\n";
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

// Applies --flow to a config: the flowgen ingress entry's enabled/profile.
static json apply_flow(json c, const std::string &flow) {
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
    return c;
}

/* ---------------- wizard ---------------- */

namespace {

// One prompt; Enter keeps the default. Returns false on EOF.
bool ask(const std::string &label, const std::string &def, std::string &out) {
    std::cout << "  " << label;
    if (!def.empty())
        std::cout << " [" << def << "]";
    std::cout << ": " << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
        return false;
    // trim
    auto b = line.find_first_not_of(" \t");
    auto e = line.find_last_not_of(" \t\r");
    line = (b == std::string::npos) ? "" : line.substr(b, e - b + 1);
    out = line.empty() ? def : line;
    return true;
}

bool ask_int(const std::string &label, long def, long lo, long hi, long &out) {
    while (true) {
        std::string s;
        if (!ask(label, std::to_string(def), s))
            return false;
        char *end = nullptr;
        long v = std::strtol(s.c_str(), &end, 10);
        if (end && *end == '\0' && v >= lo && v <= hi) {
            out = v;
            return true;
        }
        std::cout << "    please enter a number between " << lo << " and " << hi << "\n";
    }
}

bool ask_yes(const std::string &label, bool def, bool &out) {
    std::string s;
    if (!ask(label, def ? "Y/n" : "y/N", s))
        return false;
    if (s == "Y/n" || s == "y/N") { out = def; return true; }
    char c = static_cast<char>(std::tolower(static_cast<unsigned char>(s[0])));
    out = (c == 'y');
    return true;
}

std::string expand_home(std::string p) {
    if (!p.empty() && p[0] == '~') {
        const char *home = std::getenv("HOME");
        p = std::string(home ? home : "") + p.substr(1);
    }
    return p;
}

} // namespace

// Interactive config builder. Starts from the repo's default config, asks
// for the parameters that matter, returns the config and where it was
// saved. Returns false if the user bailed (EOF).
static bool wizard(json &out_cfg, fs::path &out_path, std::string &out_flow) {
    fs::path root = repo_root();
    fs::path base = root.empty() ? fs::path() : root / "configs" / "default.json";
    json c;
    std::error_code ec;
    if (!base.empty() && fs::exists(base, ec)) {
        std::ifstream in(base);
        c = json::parse(in);
    } else {
        c = {{"symbols", json::object()}, {"sink_size", 65536}, {"shards", 1},
             {"feed", {{"group", "239.1.1.1"}, {"port", 30001}, {"interface", "127.0.0.1"},
                       {"session", "MKTSIM0001"}, {"flush_us", 100}, {"heartbeat_ms", 1000},
                       {"directory_s", 2}, {"ttl", 1}, {"loop", true},
                       {"retransmit_capacity", 65536}}},
             {"ingress", json::array()}};
    }
    std::vector<std::string> all_syms;
    for (auto &[k, v] : c["symbols"].items())
        all_syms.push_back(k);

    std::cout << "\nmktsim: set up an exchange (Enter keeps the default)\n\n";

    // Symbols
    std::string s;
    if (!ask("Symbols: 'all' (" + std::to_string(all_syms.size()) +
             " from configs/default.json), a count, or a comma list",
             "all", s))
        return false;
    json syms = json::object();
    if (s == "all") {
        syms = c["symbols"];
    } else if (std::all_of(s.begin(), s.end(), ::isdigit)) {
        long n = std::strtol(s.c_str(), nullptr, 10);
        for (long i = 0; i < n && i < (long)all_syms.size(); i++)
            syms[all_syms[i]] = json::object();
        if (n > (long)all_syms.size())
            for (long i = all_syms.size(); i < n; i++)
                syms["SYM" + std::to_string(i + 1)] = json::object();
    } else {
        std::stringstream ss(s);
        std::string t;
        while (std::getline(ss, t, ',')) {
            auto b = t.find_first_not_of(" \t"), e = t.find_last_not_of(" \t");
            if (b == std::string::npos) continue;
            t = t.substr(b, e - b + 1);
            for (auto &ch : t) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (t.size() > 8) t.resize(8);
            syms[t] = json::object();
        }
    }
    if (syms.empty()) {
        std::cout << "    no symbols; using AAPL\n";
        syms["AAPL"] = json::object();
    }
    c["symbols"] = syms;

    // Shards
    long nsym = syms.size();
    long shards = std::min<long>(c.value("shards", 1), nsym);
    if (!ask_int("Shards (engine threads; at most one per symbol)", std::max(1L, shards), 1, std::min(256L, nsym), shards))
        return false;
    c["shards"] = shards;

    // Flow
    std::string flow;
    while (true) {
        if (!ask("Synthetic order flow: calm, busy, load, or off", "calm", flow))
            return false;
        if (flow == "calm" || flow == "busy" || flow == "load" || flow == "off")
            break;
        std::cout << "    one of calm, busy, load, off\n";
    }
    c = apply_flow(c, flow);
    if (flow != "off") {
        long sessions = 2;
        for (auto &e : c["ingress"])
            if (e.value("type", "") == "flowgen")
                sessions = e.value("sessions", 2);
        if (!ask_int("Flow sessions (simulated participants)", sessions, 1, 64, sessions))
            return false;
        for (auto &e : c["ingress"])
            if (e.value("type", "") == "flowgen")
                e["sessions"] = sessions;
    }

    // Order entry
    json ingress = json::array();
    for (auto &e : c["ingress"])
        if (e.value("type", "") == "flowgen")
            ingress.push_back(e);
    json boe;
    for (auto &e : c["ingress"])
        if (e.value("type", "") == "boe")
            boe = e;
    if (boe.is_null())
        boe = {{"type", "boe"}, {"port", 30000}, {"bind", "0.0.0.0"}, {"heartbeat_ms", 1000},
               {"timeout_ms", 5000}, {"poll_ms", 1}, {"max_sessions", 64}};
    {
        std::string port;
        while (true) {
            if (!ask("Order entry (BOE over TCP) port, or 'off'", std::to_string(boe.value("port", 30000)), port))
                return false;
            if (port == "off") { boe = nullptr; break; }
            char *end = nullptr;
            long v = std::strtol(port.c_str(), &end, 10);
            if (end && *end == '\0' && v > 0 && v < 65536) { boe["port"] = v; break; }
            std::cout << "    a port number or off\n";
        }
        if (!boe.is_null())
            ingress.push_back(boe);
    }

    // Example plugin
    for (auto &e : c["ingress"]) {
        if (e.value("type", "") == "plugin") {
            std::string path = e.value("path", "");
            long port = e.contains("config") ? e["config"].value("port", 0) : 0;
            bool keep = false;
            if (!ask_yes("Keep the bundled plugin " + path +
                         (port ? " on :" + std::to_string(port) : ""), false, keep))
                return false;
            if (keep)
                ingress.push_back(e);
        }
    }

    // Own plugins
    long next_port = 30030;
    while (true) {
        std::string path;
        if (!ask("Add your own ingress plugin? Path to its shared library, or Enter to continue", "", path))
            return false;
        if (path.empty())
            break;
        path = expand_home(path);
        fs::path pp = path;
        if (!fs::exists(pp, ec) && !root.empty() && fs::exists(root / pp, ec))
            pp = root / pp;
        if (!fs::exists(pp, ec)) {
            bool keep = false;
            if (!ask_yes("    " + path + " does not exist yet; add it anyway", false, keep))
                return false;
            if (!keep)
                continue;
        } else {
            pp = fs::weakly_canonical(pp, ec);
        }
        long port = 0;
        if (!ask_int("    Port for it (0 if it needs none)", next_port, 0, 65535, port))
            return false;
        json pc = json::object();
        if (port) { pc["port"] = port; pc["bind"] = "0.0.0.0"; next_port = port + 1; }
        ingress.push_back({{"type", "plugin"}, {"path", pp.string()}, {"config", pc}});
        std::cout << "    added " << pp.string() << "\n";
    }
    c["ingress"] = ingress;

    // Feed
    {
        std::string gp;
        std::string def = c["feed"].value("group", "239.1.1.1") + ":" + std::to_string(c["feed"].value("port", 30001));
        while (true) {
            if (!ask("Market data multicast group:port", def, gp))
                return false;
            auto k = gp.find(':');
            if (k != std::string::npos) {
                char *end = nullptr;
                long v = std::strtol(gp.c_str() + k + 1, &end, 10);
                if (end && *end == '\0' && v > 0 && v < 65536) {
                    c["feed"]["group"] = gp.substr(0, k);
                    c["feed"]["port"] = v;
                    break;
                }
            }
            std::cout << "    like 239.1.1.1:30001\n";
        }
    }

    // Save
    std::string save;
    fs::path def_path = state_dir() / "exchange.json";
    if (!ask("Save config as", def_path.string(), save))
        return false;
    fs::path sp = expand_home(save);
    fs::create_directories(sp.parent_path().empty() ? fs::path(".") : sp.parent_path(), ec);
    {
        std::ofstream out(sp);
        if (!out) {
            std::cerr << "mktsim: cannot write " << sp.string() << "\n";
            return false;
        }
        out << c.dump(4) << "\n";
    }

    // Summary
    std::cout << "\n  symbols " << syms.size() << " · shards " << shards << " · flow " << flow;
    std::cout << " · ingress:";
    for (auto &e : ingress) {
        std::string t = e.value("type", "?");
        if (t == "flowgen") continue;
        std::cout << " " << t;
        if (e.contains("port")) std::cout << ":" << e["port"].get<long>();
        else if (e.contains("config") && e["config"].contains("port")) std::cout << ":" << e["config"]["port"].get<long>();
    }
    std::cout << "\n  feed " << c["feed"]["group"].get<std::string>() << ":" << c["feed"]["port"].get<long>()
              << "\n  saved " << sp.string() << "\n\n";

    out_cfg = c;
    out_path = fs::weakly_canonical(sp, ec);
    if (ec) out_path = sp;
    out_flow = flow;
    return true;
}

int cmd_init(int argc, char **argv) {
    (void)argc; (void)argv;
    json c; fs::path p; std::string flow;
    if (!wizard(c, p, flow))
        return 1;
    std::cout << "start it with: mktsim up " << p.string() << "\n";
    return 0;
}

/* ---------------- up ---------------- */

int cmd_up(int argc, char **argv) {
    const char *cfg_arg = nullptr;
    std::string flow;
    bool force_wizard = false, defaults = false;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--flow") {
            if (i + 1 >= argc)
                return usage_up();
            flow = argv[++i];
            if (flow != "calm" && flow != "busy" && flow != "load" && flow != "off")
                return usage_up();
        } else if (a == "-i" || a == "--interactive") {
            force_wizard = true;
        } else if (a == "--defaults" || a == "-y") {
            defaults = true;
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

    // No config named and a terminal: walk through the parameters.
    bool interactive = force_wizard || (!cfg_arg && !defaults && isatty(fileno(stdin)));
    if (interactive) {
        json c; fs::path p; std::string wflow;
        if (!wizard(c, p, wflow))
            return 1;
        if (!flow.empty() && flow != wflow) { // --flow on the command line wins
            std::ofstream out(p);
            out << apply_flow(c, flow).dump(4) << "\n";
            wflow = flow;
        }
        bool go = true;
        if (!ask_yes("Start the exchange now", true, go))
            return 1;
        if (!go) {
            std::cout << "start it later with: mktsim up " << p.string() << "\n";
            return 0;
        }
        return launch(p, p, wflow);
    }

    fs::path cfg = resolve_config(cfg_arg);
    if (cfg.empty()) {
        std::cerr << "mktsim up: config not found"
                  << (cfg_arg ? std::string(": ") + cfg_arg : "") << "\n";
        return 1;
    }

    fs::path use_cfg = cfg;
    if (!flow.empty()) {
        std::error_code ec;
        fs::create_directories(state_dir(), ec);
        std::ifstream in(cfg);
        json c = apply_flow(json::parse(in), flow);
        std::ofstream out(derived_cfg());
        out << c.dump(4) << "\n";
        use_cfg = derived_cfg();
    }
    return launch(cfg, use_cfg, flow);
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
            std::cout << "  stale pid file " << pid_file().string() << " (pid " << pid << ")\n";
        std::string last = read_last_line(log_file());
        if (!last.empty())
            std::cout << "  last log: " << last << "\n";
        return 1;
    }

    std::cout << "exchange: running (pid " << pid << ")\n";

    // uptime from the meta file, else the pid file's mtime
    std::chrono::seconds up{0};
    std::string cfg, used_s, flow;
    {
        std::ifstream meta(meta_file());
        std::string line;
        long started = 0;
        while (std::getline(meta, line)) {
            if (line.rfind("config=", 0) == 0) cfg = line.substr(7);
            else if (line.rfind("used=", 0) == 0) used_s = line.substr(5);
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

    // Ports from the config actually passed to the server
    std::error_code ec;
    fs::path used = used_s.empty() ? fs::path(cfg) : fs::path(used_s);
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
    std::cout << "  log:    " << log_file().string() << "\n";
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
        std::cout << "no log yet at " << logf.string() << " (run `mktsim up`)\n";
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
