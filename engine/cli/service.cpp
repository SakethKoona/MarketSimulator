#include "service.hpp"
#include "scaffold.hpp"
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
#include <termios.h>
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
    // A daemon that fails during startup (a plugin that will not load, a
    // port in use) dies a moment after writing its pid; catch that here.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    if (!pid_alive(pid)) {
        std::cerr << "mktsim up: exchange_server exited right after starting\n";
        std::ifstream in(logf);
        std::vector<std::string> tail;
        for (std::string line; std::getline(in, line);) {
            tail.push_back(line);
            if (tail.size() > 6) tail.erase(tail.begin());
        }
        for (auto &l : tail)
            std::cerr << "  log: " << l << "\n";
        std::error_code ec2;
        fs::remove(pidf, ec2);
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

// Terminal styling for the setup prompts: colours and cursor moves only when
// both ends are a TTY and NO_COLOR is unset, so piped runs stay plain.
struct Ui {
    bool tty;
    explicit Ui() : tty(isatty(fileno(stdin)) && isatty(fileno(stdout)) &&
                        std::getenv("NO_COLOR") == nullptr) {}
    std::string c(const char *code) const { return tty ? std::string("\033[") + code + "m" : ""; }
    std::string reset() const { return c("0"); }
    std::string bold() const { return c("1"); }
    std::string dim() const { return c("2"); }
    std::string cyan() const { return c("36"); }
    std::string green() const { return c("32"); }
    std::string yellow() const { return c("33"); }
    std::string red() const { return c("31"); }

    void banner(const std::string &title) const {
        std::cout << "\n" << cyan() << "┌" << reset() << "  " << bold() << title << reset()
                  << "\n" << cyan() << "│" << reset() << "\n";
    }
    void section(const std::string &name) const {
        std::cout << cyan() << "│" << reset() << "\n"
                  << cyan() << "◇" << reset() << "  " << bold() << name << reset() << "\n";
    }
    void rail() const { std::cout << cyan() << "│" << reset() << "\n"; }
    void note(const std::string &msg) const {
        std::cout << cyan() << "│" << reset() << "  " << green() << "✓ " << reset() << msg << "\n";
    }
    void warn(const std::string &msg) const {
        std::cout << cyan() << "│" << reset() << "  " << yellow() << "▲ " << msg << reset() << "\n";
    }
    void done(const std::string &msg) const {
        std::cout << cyan() << "└" << reset() << "  " << msg << "\n\n";
    }

    // Asks one question. Shows title, a dimmed hint, and a prompt with the
    // default; on a TTY the three lines collapse into one confirmed line.
    bool ask(const std::string &title, const std::string &hint, const std::string &def,
             std::string &out) const {
        std::cout << cyan() << "◆" << reset() << "  " << bold() << title << reset() << "\n";
        int lines = 1;
        if (!hint.empty()) {
            std::cout << cyan() << "│" << reset() << "  " << dim() << hint << reset() << "\n";
            lines++;
        }
        std::cout << cyan() << "│" << reset() << "  " << cyan() << "›" << reset() << " ";
        if (!def.empty())
            std::cout << dim() << def << reset() << " ";
        std::cout << std::flush;
        lines++;

        std::string line;
        if (!std::getline(std::cin, line)) {
            std::cout << "\n";
            return false;
        }
        auto b = line.find_first_not_of(" \t");
        auto e = line.find_last_not_of(" \t\r");
        line = (b == std::string::npos) ? "" : line.substr(b, e - b + 1);
        out = line.empty() ? def : line;

        if (tty) {
            // Up over the question block, clear it, print the answer line
            std::cout << "\033[" << lines << "A\033[J";
            std::cout << dim() << "◇" << reset() << "  " << title << dim() << " · " << reset()
                      << (out.empty() ? dim() + "skipped" : green() + out) << reset() << "\n";
        } else {
            std::cout << "\n"; // piped input is not echoed
        }
        return true;
    }

    struct Option {
        std::string label;
        std::string desc;
    };

    // Arrow-key choice. Up/Down or j/k move, Enter confirms, a digit jumps.
    // On a TTY the list redraws in place and folds into one line; otherwise
    // it is a numbered text prompt.
    bool select(const std::string &title, const std::string &hint,
                const std::vector<Option> &opts, std::size_t def, std::size_t &out) const {
        if (opts.empty())
            return false;
        if (def >= opts.size())
            def = 0;

        if (!tty) {
            std::string h = hint;
            for (std::size_t i = 0; i < opts.size(); i++)
                h += (i ? "  " : (h.empty() ? "" : "  ")) + std::to_string(i + 1) + ") " + opts[i].label;
            std::string v;
            if (!ask(title, h, opts[def].label, v))
                return false;
            for (std::size_t i = 0; i < opts.size(); i++)
                if (v == opts[i].label || v == std::to_string(i + 1)) {
                    out = i;
                    return true;
                }
            out = def;
            return true;
        }

        // Raw mode: no echo, byte-at-a-time, signals still work.
        termios saved{};
        tcgetattr(STDIN_FILENO, &saved);
        termios raw = saved;
        raw.c_lflag &= ~(ECHO | ICANON);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        std::cout << "\033[?25l"; // hide cursor

        std::size_t cur = def;
        int lines = 1 + (hint.empty() ? 0 : 1) + static_cast<int>(opts.size());
        auto draw = [&](bool first) {
            if (!first)
                std::cout << "\033[" << lines << "A";
            std::cout << "\r\033[J" << cyan() << "◆" << reset() << "  " << bold() << title << reset() << "\n";
            if (!hint.empty())
                std::cout << cyan() << "│" << reset() << "  " << dim() << hint << reset() << "\n";
            for (std::size_t i = 0; i < opts.size(); i++) {
                bool on = (i == cur);
                std::cout << cyan() << "│" << reset() << "  "
                          << (on ? cyan() + "●" : dim() + "○") << reset() << " "
                          << (on ? bold() : dim()) << opts[i].label << reset();
                if (!opts[i].desc.empty())
                    std::cout << "  " << dim() << opts[i].desc << reset();
                std::cout << "\n";
            }
            std::cout << std::flush;
        };
        draw(true);

        bool ok = true;
        while (true) {
            char ch;
            ssize_t n = ::read(STDIN_FILENO, &ch, 1);
            if (n <= 0) { ok = false; break; }
            if (ch == '\n' || ch == '\r') break;
            if (ch == 'j' || ch == 'J') { cur = (cur + 1) % opts.size(); draw(false); continue; }
            if (ch == 'k' || ch == 'K') { cur = (cur + opts.size() - 1) % opts.size(); draw(false); continue; }
            if (ch >= '1' && ch <= '9' && static_cast<std::size_t>(ch - '1') < opts.size()) {
                cur = ch - '1'; draw(false); continue;
            }
            if (ch == 'q' || ch == 3) { ok = false; break; } // q / Ctrl-C
            if (ch == '\033') {
                char seq[2];
                if (::read(STDIN_FILENO, &seq[0], 1) <= 0 || seq[0] != '[') continue;
                if (::read(STDIN_FILENO, &seq[1], 1) <= 0) continue;
                if (seq[1] == 'A') cur = (cur + opts.size() - 1) % opts.size();
                else if (seq[1] == 'B') cur = (cur + 1) % opts.size();
                else continue;
                draw(false);
            }
        }

        std::cout << "\033[?25h"; // show cursor
        tcsetattr(STDIN_FILENO, TCSANOW, &saved);
        if (!ok) {
            std::cout << "\n";
            return false;
        }
        std::cout << "\033[" << lines << "A\r\033[J";
        std::cout << dim() << "◇" << reset() << "  " << title << dim() << " · " << reset()
                  << green() << opts[cur].label << reset() << "\n";
        out = cur;
        return true;
    }

    bool ask_int(const std::string &title, const std::string &hint, long def, long lo,
                 long hi, long &out) const {
        while (true) {
            std::string v;
            if (!ask(title, hint, std::to_string(def), v))
                return false;
            char *endp = nullptr;
            long n = std::strtol(v.c_str(), &endp, 10);
            if (endp && *endp == '\0' && n >= lo && n <= hi) {
                out = n;
                return true;
            }
            warn("enter a number between " + std::to_string(lo) + " and " + std::to_string(hi));
        }
    }

    bool ask_yes(const std::string &title, const std::string &hint, bool def, bool &out) const {
        std::size_t idx = 0;
        if (!select(title, hint, {{"yes", ""}, {"no", ""}}, def ? 0 : 1, idx))
            return false;
        out = (idx == 0);
        return true;
    }
};

std::string expand_home(std::string p) {
    if (!p.empty() && p[0] == '~') {
        const char *home = std::getenv("HOME");
        p = std::string(home ? home : "") + p.substr(1);
    }
    return p;
}

std::string shorten_home(const std::string &p) {
    const char *home = std::getenv("HOME");
    if (home && p.rfind(home, 0) == 0)
        return "~" + p.substr(std::strlen(home));
    return p;
}

} // namespace

// Interactive config builder, in two stages:
//   1. Engine: symbols, shards, market data feed.
//   2. Ingress: demo flow (calm / normal / heavy) and order entry, or your
//      own adapter, scaffolded as a buildable project.
// Returns false if the user bailed (EOF / q).
static std::string g_start_warning; // set by wizard when starting now is unwise

static bool wizard(json &out_cfg, fs::path &out_path, std::string &out_flow) {
    g_start_warning.clear();
    Ui ui;
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

    ui.banner("mktsim · set up an exchange");
    std::cout << ui.cyan() << "│" << ui.reset() << "  " << ui.dim()
              << "Enter keeps the default · arrows or j/k move in lists" << ui.reset() << "\n";

    /* ================= 1. ENGINE ================= */
    ui.section("1 · Engine");

    std::string s;
    {
        std::size_t mode = 0;
        if (!ui.select("Symbols", "",
                       {{"all", std::to_string(all_syms.size()) + " tickers from configs/default.json"},
                        {"a count", "the first N of those"},
                        {"a list", "your own tickers, comma separated"}},
                       0, mode))
            return false;
        if (mode == 0) {
            s = "all";
        } else if (mode == 1) {
            long n = 10;
            if (!ui.ask_int("How many", "1 to " + std::to_string(all_syms.size()) + "; more than that adds SYM101, SYM102, ...", 10, 1, 10000, n))
                return false;
            s = std::to_string(n);
        } else {
            if (!ui.ask("Tickers", "comma separated, up to 8 characters each", "AAPL,MSFT,GOOG", s))
                return false;
        }
    }
    json syms = json::object();
    if (s == "all") {
        syms = c["symbols"];
    } else if (std::all_of(s.begin(), s.end(), ::isdigit)) {
        long n = std::strtol(s.c_str(), nullptr, 10);
        for (long i = 0; i < n && i < (long)all_syms.size(); i++)
            syms[all_syms[i]] = json::object();
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
        ui.warn("no symbols given; using AAPL");
        syms["AAPL"] = json::object();
    }
    c["symbols"] = syms;

    long nsym = syms.size();
    long shards = std::min<long>(c.value("shards", 1), nsym);
    if (!ui.ask_int("Shards", "matching threads; each symbol lives on exactly one, so at most " +
                    std::to_string(nsym), std::max(1L, shards), 1, std::min(256L, nsym), shards))
        return false;
    c["shards"] = shards;

    {
        std::string gp;
        std::string def = c["feed"].value("group", "239.1.1.1") + ":" + std::to_string(c["feed"].value("port", 30001));
        while (true) {
            if (!ui.ask("Market data feed", "UDP multicast group:port the TUI and feed clients subscribe to", def, gp))
                return false;
            auto k = gp.find(':');
            if (k != std::string::npos) {
                char *endp = nullptr;
                long v = std::strtol(gp.c_str() + k + 1, &endp, 10);
                if (endp && *endp == '\0' && v > 0 && v < 65536) {
                    c["feed"]["group"] = gp.substr(0, k);
                    c["feed"]["port"] = v;
                    break;
                }
            }
            ui.warn("like 239.1.1.1:30001");
        }
    }

    /* ================= 2. INGRESS ================= */
    ui.section("2 · Ingress");

    json flowgen_tpl, boe_tpl;
    for (auto &e : c["ingress"]) {
        if (e.value("type", "") == "flowgen") flowgen_tpl = e;
        if (e.value("type", "") == "boe") boe_tpl = e;
    }
    if (flowgen_tpl.is_null())
        flowgen_tpl = {{"type", "flowgen"}, {"enabled", true}, {"profile", "calm"}, {"sessions", 2}, {"seed", 1}};
    if (boe_tpl.is_null())
        boe_tpl = {{"type", "boe"}, {"port", 30000}, {"bind", "0.0.0.0"}, {"heartbeat_ms", 1000},
                   {"timeout_ms", 5000}, {"poll_ms", 1}, {"max_sessions", 64}};

    json ingress = json::array();
    std::string flow = "off";
    std::string own_summary;
    std::vector<std::string> next_steps;

    std::size_t src = 0;
    if (!ui.select("Where do orders come from",
                   "the demo keeps the books alive by itself; your own adapter speaks your protocol",
                   {{"demo flow", "built-in simulated participants, plus BOE order entry for mktsim connect"},
                    {"your own ingress", "scaffold an adapter project you code up; TCP, sessions and reports handled"},
                    {"both", "demo flow running alongside your adapter"}},
                   0, src))
        return false;
    bool want_demo = (src == 0 || src == 2);
    bool want_own = (src == 1 || src == 2);

    if (want_demo) {
        static const char *profiles[] = {"calm", "busy", "load"};
        std::size_t pi = 0;
        if (!ui.select("Demo flow", "how busy the simulated market is",
                       {{"calm", "a few orders a second per symbol"},
                        {"normal", "active two-sided flow, frequent trades"},
                        {"heavy", "as fast as the exchange will take"}},
                       0, pi))
            return false;
        flow = profiles[pi];
        long sessions = flowgen_tpl.value("sessions", 2);
        if (!ui.ask_int("Simulated participants", "each is a session placing orders", sessions, 1, 64, sessions))
            return false;
        json fg = flowgen_tpl;
        fg["enabled"] = true;
        fg["profile"] = flow;
        fg["sessions"] = sessions;
        ingress.push_back(fg);
    } else {
        json fg = flowgen_tpl;
        fg["enabled"] = false;
        ingress.push_back(fg);
    }

    // BOE order entry: on by default with the demo, optional otherwise
    {
        long cur_port = boe_tpl.value("port", 30000);
        bool keep = true;
        if (!ui.ask_yes("BOE order entry on :" + std::to_string(cur_port),
                        "binary order entry over TCP; what mktsim connect and flowgen speak", want_demo || !want_own, keep))
            return false;
        if (keep)
            ingress.push_back(boe_tpl);
    }

    if (want_own) {
        std::size_t how = 0;
        if (!ui.select("Your ingress", "",
                       {{"scaffold a new adapter", "a ready-to-build project; you fill in decode() and encode()"},
                        {"use an existing library", "a shared library you already built against ingress/api.h"}},
                       0, how))
            return false;

        if (how == 0) {
            std::string name;
            if (!ui.ask("Adapter name", "letters, digits and _; also the library name", "my_ingress", name))
                return false;
            std::string dir;
            if (!ui.ask("Project directory", "created if missing", "./" + name, dir))
                return false;
            long port = 30030;
            if (!ui.ask_int("Port it listens on", "one TCP connection = one exchange session", 30030, 1, 65535, port))
                return false;

            scaffold::Spec spec;
            spec.name = name;
            spec.dir = fs::weakly_canonical(fs::path(expand_home(dir)), ec);
            if (ec) spec.dir = fs::absolute(fs::path(expand_home(dir)));
            spec.repo = root;
            spec.port = static_cast<int>(port);

            std::string err = scaffold::generate(spec);
            if (!err.empty()) {
                ui.warn(err);
                return false;
            }
            ui.note("created " + shorten_home(spec.dir.string()) + "/");
            std::cout << ui.cyan() << "│" << ui.reset() << "    " << ui.dim()
                      << "CMakeLists.txt  README.md  src/protocol.hpp  src/adapter.cpp  include/ingress/api.h"
                      << ui.reset() << "\n";

            bool build_now = true;
            if (!ui.ask_yes("Build it now", "cmake configure + build into " + name + "/build", true, build_now))
                return false;
            bool built = false;
            if (build_now) {
                std::cout << ui.cyan() << "│" << ui.reset() << "  " << ui.dim() << "building…" << ui.reset() << "\n";
                built = scaffold::build(spec) == 0;
                if (built)
                    ui.note("built " + shorten_home(scaffold::library_path(spec)));
                else
                    ui.warn("build failed; fix it and run: cmake -S " + shorten_home(spec.dir.string()) +
                            " -B " + shorten_home((spec.dir / "build").string()) + " && cmake --build " +
                            shorten_home((spec.dir / "build").string()));
            }
            if (!built)
                g_start_warning = "the adapter library is not built yet, so the exchange would fail to load it";
            ingress.push_back({{"type", "plugin"}, {"path", scaffold::library_path(spec)},
                               {"config", {{"port", port}, {"bind", "0.0.0.0"}}}});
            own_summary = name + ":" + std::to_string(port);
            next_steps.push_back("edit  " + shorten_home((spec.dir / "src" / "protocol.hpp").string()));
            if (!built)
                next_steps.push_back("build  cmake -S " + shorten_home(spec.dir.string()) + " -B " +
                                     shorten_home((spec.dir / "build").string()) + " && cmake --build " +
                                     shorten_home((spec.dir / "build").string()));
            next_steps.push_back("try   nc localhost " + std::to_string(port) + "   then  NEW c1 AAPL B 100 10000");
        } else {
            std::string path;
            if (!ui.ask("Shared library", "path to the .dylib/.so, or without extension", "", path))
                return false;
            path = expand_home(path);
            fs::path pp = path;
            if (!fs::exists(pp, ec) && !root.empty() && fs::exists(root / pp, ec))
                pp = root / pp;
            if (fs::exists(pp, ec))
                pp = fs::weakly_canonical(pp, ec);
            else
                ui.warn(path + " does not exist yet; the exchange will fail to start until it does");
            long port = 30030;
            if (!ui.ask_int("Port for it", "0 if it takes none", 30030, 0, 65535, port))
                return false;
            json pc = json::object();
            if (port) { pc["port"] = port; pc["bind"] = "0.0.0.0"; }
            ingress.push_back({{"type", "plugin"}, {"path", pp.string()}, {"config", pc}});
            own_summary = pp.filename().string() + (port ? ":" + std::to_string(port) : "");
        }
    }
    c["ingress"] = ingress;

    /* ================= SAVE ================= */
    ui.section("Save");
    std::string save;
    fs::path def_path = state_dir() / "exchange.json";
    if (!ui.ask("Config file", "mktsim up <path> starts it; the default path is what plain mktsim up uses", shorten_home(def_path.string()), save))
        return false;
    fs::path sp = expand_home(save);
    fs::create_directories(sp.parent_path().empty() ? fs::path(".") : sp.parent_path(), ec);
    {
        std::ofstream out(sp);
        if (!out) {
            ui.warn("cannot write " + sp.string());
            return false;
        }
        out << c.dump(4) << "\n";
    }

    /* ================= SUMMARY ================= */
    std::ostringstream ing;
    for (auto &e : ingress) {
        std::string t = e.value("type", "?");
        if (t == "flowgen") continue;
        if (t == "plugin") { ing << (ing.tellp() > 0 ? " · " : "") << own_summary; continue; }
        ing << (ing.tellp() > 0 ? " · " : "") << t;
        if (e.contains("port")) ing << ":" << e["port"].get<long>();
    }
    auto row = [&](const std::string &k, const std::string &v) {
        std::cout << ui.cyan() << "│" << ui.reset() << "  " << ui.dim() << "│ " << ui.reset()
                  << ui.dim() << k << ui.reset() << std::string(k.size() < 10 ? 10 - k.size() : 1, ' ')
                  << v << "\n";
    };
    ui.rail();
    std::cout << ui.cyan() << "◇" << ui.reset() << "  " << ui.bold() << "Summary" << ui.reset() << "\n";
    std::cout << ui.cyan() << "│" << ui.reset() << "  " << ui.dim() << "┌" << ui.reset() << "\n";
    row("symbols", std::to_string(syms.size()));
    row("shards", std::to_string(shards));
    row("feed", c["feed"]["group"].get<std::string>() + ":" + std::to_string(c["feed"]["port"].get<long>()));
    row("demo flow", flow == "off" ? "off" : (flow == "calm" ? "calm" : flow == "busy" ? "normal" : "heavy"));
    row("ingress", ing.str().empty() ? "none" : ing.str());
    row("saved", shorten_home(sp.string()));
    std::cout << ui.cyan() << "│" << ui.reset() << "  " << ui.dim() << "└" << ui.reset() << "\n";
    if (!next_steps.empty()) {
        ui.rail();
        std::cout << ui.cyan() << "◇" << ui.reset() << "  " << ui.bold() << "Next" << ui.reset() << "\n";
        for (auto &n : next_steps)
            std::cout << ui.cyan() << "│" << ui.reset() << "  " << n << "\n";
    }

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
    Ui ui;
    ui.done("start it with  " + ui.bold() + "mktsim up " + shorten_home(p.string()) + ui.reset());
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
        Ui ui;
        bool go = g_start_warning.empty();
        if (!ui.ask_yes("Start the exchange now",
                        g_start_warning.empty() ? "runs in the background until mktsim down" : g_start_warning,
                        g_start_warning.empty(), go))
            return 1;
        if (!go) {
            ui.done("start it later with  " + ui.bold() + "mktsim up " + shorten_home(p.string()) + ui.reset());
            return 0;
        }
        ui.done("starting");
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
