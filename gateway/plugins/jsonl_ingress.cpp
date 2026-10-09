// jsonl_ingress: an ingress adapter built only against ingress/api.h, as a
// shared library. One TCP connection = one session. Newline-delimited JSON
// both ways, so a client is a few lines of any language.
//
// Client → exchange, one object per line:
//   {"new":{"id":"c1","sym":"AAPL","side":"B","qty":100,"px":10000,"tif":"GTC"}}
//   {"new":{"id":"c2","sym":"AAPL","side":"S","qty":50,"type":"MARKET","tif":"IOC"}}
//   {"cancel":{"id":"c1"}}            cancel by client id
//   {"modify":{"id":"c1","qty":60,"px":10010}}   px optional
//   {"ping":1}
// Exchange → client:
//   {"ev":"accepted","id":"c1","order":123,"qty":100,"px":10000,"leaves":100}
//   {"ev":"rejected","id":"c1","reason":"symbol_not_found"}
//   {"ev":"exec","id":"c1","order":123,"qty":50,"px":10000,"match":77}
//   {"ev":"cancelled","id":"c1","order":123}
//   {"ev":"modified","id":"c1","order":123,"qty":60,"px":10010,"leaves":60}
//   {"ev":"pong"}  {"ev":"error","msg":"..."}
//
// Config: {"port": 30020, "bind": "0.0.0.0"}
#include "ingress/api.h"
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace {

// Minimal JSON helpers: enough for flat objects of strings and integers.
std::string json_str(const std::string &obj, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    std::size_t p = obj.find(k);
    if (p == std::string::npos)
        return "";
    p = obj.find(':', p + k.size());
    if (p == std::string::npos)
        return "";
    ++p;
    while (p < obj.size() && (obj[p] == ' ' || obj[p] == '\t'))
        ++p;
    if (p >= obj.size())
        return "";
    if (obj[p] == '"') {
        std::size_t e = obj.find('"', p + 1);
        return e == std::string::npos ? "" : obj.substr(p + 1, e - p - 1);
    }
    std::size_t e = p;
    while (e < obj.size() && (std::isalnum(static_cast<unsigned char>(obj[e])) || obj[e] == '-' || obj[e] == '.'))
        ++e;
    return obj.substr(p, e - p);
}
std::uint64_t json_u64(const std::string &obj, const char *key) {
    std::string v = json_str(obj, key);
    return v.empty() ? 0 : std::strtoull(v.c_str(), nullptr, 10);
}
std::string json_obj(const std::string &line, const char *key) {
    std::string k = std::string("\"") + key + "\"";
    std::size_t p = line.find(k);
    if (p == std::string::npos)
        return "";
    p = line.find('{', p);
    if (p == std::string::npos)
        return "";
    int depth = 0;
    for (std::size_t i = p; i < line.size(); ++i) {
        if (line[i] == '{')
            ++depth;
        else if (line[i] == '}' && --depth == 0)
            return line.substr(p, i - p + 1);
    }
    return "";
}

const char *status_name(uint8_t st) {
    switch (st) {
    case MKTSIM_ST_SYMBOL_NOT_FOUND: return "symbol_not_found";
    case MKTSIM_ST_ORDER_NOT_FOUND: return "order_not_found";
    case MKTSIM_ST_NOT_ENOUGH_LIQUIDITY: return "no_liquidity";
    case MKTSIM_ST_FOK_FAILED: return "fok_unfillable";
    case MKTSIM_ST_INVALID_PRICE: return "invalid_price";
    case MKTSIM_ST_INVALID_QTY: return "invalid_qty";
    case MKTSIM_ST_DUPLICATE: return "duplicate";
    case MKTSIM_ST_OK: return "no_liquidity"; // rejected new order with nothing done
    default: return "failed";
    }
}

struct Conn {
    int fd;
    mktsim_session *session;
    std::string rx, tx;
    std::uint64_t next_req = 1;
    std::unordered_map<std::uint64_t, std::string> pending; // request_id -> client id
    std::unordered_map<std::uint64_t, std::string> by_order; // order_id -> client id
    std::unordered_map<std::string, std::uint64_t> by_client; // client id -> order_id
};

struct Adapter {
    const mktsim_exchange_api *api;
    mktsim_exchange *ex;
    std::uint16_t port = 30020;
    std::string bind = "0.0.0.0";
    int listen_fd = -1;
    std::thread thread;
    std::atomic<bool> running{false};
    std::vector<Conn *> conns;

    void log(const std::string &m) { api->log(ex, "jsonl", m.c_str()); }

    void send(Conn &c, const std::string &line) {
        c.tx += line;
        c.tx += '\n';
    }

    void handle_line(Conn &c, const std::string &line) {
        if (line.find("\"ping\"") != std::string::npos) {
            send(c, "{\"ev\":\"pong\"}");
            return;
        }
        std::string o;
        if (!(o = json_obj(line, "new")).empty()) {
            std::string id = json_str(o, "id"), sym = json_str(o, "sym"), side = json_str(o, "side");
            std::string type = json_str(o, "type"), tif = json_str(o, "tif");
            if (id.empty() || sym.empty() || side.empty()) {
                send(c, "{\"ev\":\"error\",\"msg\":\"new needs id, sym, side\"}");
                return;
            }
            if (c.by_client.count(id)) {
                send(c, "{\"ev\":\"rejected\",\"id\":\"" + id + "\",\"reason\":\"duplicate\"}");
                return;
            }
            uint32_t symbol_id;
            if (api->resolve_symbol(ex, sym.data(), sym.size(), &symbol_id) != MKTSIM_OK) {
                send(c, "{\"ev\":\"rejected\",\"id\":\"" + id + "\",\"reason\":\"symbol_not_found\"}");
                return;
            }
            mktsim_order_req r{};
            r.request_id = c.next_req++;
            r.kind = MKTSIM_REQ_NEW;
            r.side = side[0] == 'S' || side[0] == 's' ? MKTSIM_SELL : MKTSIM_BUY;
            r.ord_type = (type == "MARKET" || type == "market") ? MKTSIM_MARKET : MKTSIM_LIMIT;
            r.tif = tif == "IOC" ? MKTSIM_IOC : tif == "FOK" ? MKTSIM_FOK : MKTSIM_GTC;
            if (r.ord_type == MKTSIM_MARKET && r.tif == MKTSIM_GTC)
                r.tif = MKTSIM_IOC;
            r.symbol_id = symbol_id;
            r.qty = static_cast<uint32_t>(json_u64(o, "qty"));
            r.price = json_u64(o, "px");
            int rc = api->submit(c.session, &r);
            if (rc != MKTSIM_OK) {
                send(c, "{\"ev\":\"rejected\",\"id\":\"" + id + "\",\"reason\":\"" +
                            (rc == MKTSIM_EBUSY ? "busy" : rc == MKTSIM_EBADSYMBOL ? "symbol_not_found" : "bad_request") + "\"}");
                return;
            }
            c.pending[r.request_id] = id;
            c.by_client[id] = 0; // reserved until accepted
            return;
        }
        if (!(o = json_obj(line, "cancel")).empty() || !(o = json_obj(line, "modify")).empty()) {
            bool is_cancel = line.find("\"cancel\"") != std::string::npos;
            std::string id = json_str(o, "id");
            auto it = c.by_client.find(id);
            if (it == c.by_client.end() || it->second == 0) {
                send(c, "{\"ev\":\"rejected\",\"id\":\"" + id + "\",\"reason\":\"order_not_found\"}");
                return;
            }
            mktsim_order_req r{};
            r.request_id = c.next_req++;
            r.kind = is_cancel ? MKTSIM_REQ_CANCEL : MKTSIM_REQ_MODIFY;
            r.order_id = it->second;
            if (!is_cancel) {
                r.qty = static_cast<uint32_t>(json_u64(o, "qty"));
                r.price = json_u64(o, "px");
            }
            int rc = api->submit(c.session, &r);
            if (rc != MKTSIM_OK) {
                send(c, "{\"ev\":\"rejected\",\"id\":\"" + id + "\",\"reason\":\"busy\"}");
                return;
            }
            c.pending[r.request_id] = id;
            return;
        }
        send(c, "{\"ev\":\"error\",\"msg\":\"expected new, cancel, modify or ping\"}");
    }

    static void on_report(void *user, const mktsim_report *r) {
        auto &c = *static_cast<Conn *>(user);
        // Resolve the client id: by request for solicited reports, by order id otherwise.
        std::string id;
        if (r->request_id) {
            auto p = c.pending.find(r->request_id);
            if (p != c.pending.end()) {
                id = p->second;
                if (r->kind != MKTSIM_RPT_EXECUTION)
                    c.pending.erase(p);
            }
        }
        if (id.empty()) {
            auto b = c.by_order.find(r->order_id);
            if (b != c.by_order.end())
                id = b->second;
        }
        if (id.empty())
            return;
        char buf[320];
        switch (r->kind) {
        case MKTSIM_RPT_ACCEPTED:
            c.by_order[r->order_id] = id;
            c.by_client[id] = r->order_id;
            std::snprintf(buf, sizeof(buf), "{\"ev\":\"accepted\",\"id\":\"%s\",\"order\":%llu,\"qty\":%u,\"px\":%llu,\"leaves\":%u}", id.c_str(),
                          (unsigned long long)r->order_id, r->qty, (unsigned long long)r->price, r->leaves_qty);
            break;
        case MKTSIM_RPT_REJECTED:
            c.by_client.erase(id);
            std::snprintf(buf, sizeof(buf), "{\"ev\":\"rejected\",\"id\":\"%s\",\"reason\":\"%s\"}", id.c_str(), status_name(r->status));
            break;
        case MKTSIM_RPT_CANCELLED:
            c.by_client.erase(id);
            c.by_order.erase(r->order_id);
            std::snprintf(buf, sizeof(buf), "{\"ev\":\"cancelled\",\"id\":\"%s\",\"order\":%llu}", id.c_str(), (unsigned long long)r->order_id);
            break;
        case MKTSIM_RPT_MODIFIED:
            std::snprintf(buf, sizeof(buf), "{\"ev\":\"modified\",\"id\":\"%s\",\"order\":%llu,\"qty\":%u,\"px\":%llu,\"leaves\":%u}", id.c_str(),
                          (unsigned long long)r->order_id, r->qty, (unsigned long long)r->price, r->leaves_qty);
            break;
        case MKTSIM_RPT_EXECUTION:
            std::snprintf(buf, sizeof(buf), "{\"ev\":\"exec\",\"id\":\"%s\",\"order\":%llu,\"qty\":%u,\"px\":%llu,\"match\":%llu}", id.c_str(),
                          (unsigned long long)r->order_id, r->last_qty, (unsigned long long)r->price, (unsigned long long)r->match_id);
            break;
        default:
            return;
        }
        c.tx += buf;
        c.tx += '\n';
    }

    void run() {
        std::vector<pollfd> pfds;
        char buf[16384];
        while (running.load(std::memory_order_relaxed)) {
            pfds.clear();
            pfds.push_back({listen_fd, POLLIN, 0});
            for (auto *c : conns)
                pfds.push_back({c->fd, static_cast<short>(POLLIN | (c->tx.empty() ? 0 : POLLOUT)), 0});
            int n = ::poll(pfds.data(), pfds.size(), 1);
            if (n > 0 && (pfds[0].revents & POLLIN)) {
                for (;;) {
                    int fd = ::accept(listen_fd, nullptr, nullptr);
                    if (fd < 0)
                        break;
                    int one = 1;
                    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                    auto *c = new Conn{};
                    c->fd = fd;
                    c->session = api->open_session(ex, "jsonl");
                    conns.push_back(c);
                }
            }
            for (std::size_t i = 0; i < conns.size();) {
                Conn &c = *conns[i];
                bool dead = false;
                short rev = (i + 1 < pfds.size()) ? pfds[i + 1].revents : 0;
                if (rev & (POLLERR | POLLHUP | POLLNVAL))
                    dead = true;
                if (!dead && (rev & POLLIN)) {
                    ssize_t r = ::recv(c.fd, buf, sizeof(buf), 0);
                    if (r <= 0 && !(r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)))
                        dead = true;
                    else if (r > 0) {
                        c.rx.append(buf, r);
                        std::size_t nl;
                        while ((nl = c.rx.find('\n')) != std::string::npos) {
                            std::string line = c.rx.substr(0, nl);
                            c.rx.erase(0, nl + 1);
                            if (!line.empty())
                                handle_line(c, line);
                        }
                        if (c.rx.size() > (1u << 20))
                            dead = true;
                    }
                }
                if (!dead) {
                    api->poll(c.session, &Adapter::on_report, &c, 1024);
                    while (!c.tx.empty()) {
                        ssize_t w = ::send(c.fd, c.tx.data(), c.tx.size(), 0);
                        if (w < 0) {
                            if (errno != EAGAIN && errno != EWOULDBLOCK)
                                dead = true;
                            break;
                        }
                        c.tx.erase(0, w);
                    }
                }
                if (dead) {
                    api->close_session(c.session);
                    ::close(c.fd);
                    delete conns[i];
                    conns.erase(conns.begin() + i);
                    pfds.erase(pfds.begin() + i + 1);
                } else {
                    ++i;
                }
            }
        }
        for (auto *c : conns) {
            api->close_session(c->session);
            ::close(c->fd);
            delete c;
        }
        conns.clear();
    }
};

} // namespace

extern "C" {

int mktsim_ingress_init(const mktsim_exchange_api *api, mktsim_exchange *ex, const char *config_json, void **state) {
    if (api->version != MKTSIM_INGRESS_API_VERSION)
        return 1;
    auto *a = new Adapter{};
    a->api = api;
    a->ex = ex;
    std::string cfg = config_json ? config_json : "{}";
    if (std::uint64_t p = json_u64(cfg, "port"))
        a->port = static_cast<std::uint16_t>(p);
    std::string b = json_str(cfg, "bind");
    if (!b.empty())
        a->bind = b;
    *state = a;
    return 0;
}

int mktsim_ingress_start(void *state) {
    auto *a = static_cast<Adapter *>(state);
    a->listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (a->listen_fd < 0)
        return 1;
    int one = 1;
    ::setsockopt(a->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(a->port);
    if (inet_pton(AF_INET, a->bind.c_str(), &addr.sin_addr) != 1)
        return 1;
    if (::bind(a->listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0 || ::listen(a->listen_fd, 64) < 0) {
        a->log(std::string("bind/listen failed: ") + std::strerror(errno));
        return 1;
    }
    ::fcntl(a->listen_fd, F_SETFL, ::fcntl(a->listen_fd, F_GETFL, 0) | O_NONBLOCK);
    a->running.store(true);
    a->thread = std::thread([a] { a->run(); });
    a->log("listening on " + a->bind + ":" + std::to_string(a->port));
    return 0;
}

void mktsim_ingress_stop(void *state) {
    auto *a = static_cast<Adapter *>(state);
    if (!a->running.exchange(false))
        return;
    if (a->thread.joinable())
        a->thread.join();
    if (a->listen_fd >= 0)
        ::close(a->listen_fd);
    a->listen_fd = -1;
}

void mktsim_ingress_destroy(void *state) { delete static_cast<Adapter *>(state); }

} // extern "C"
