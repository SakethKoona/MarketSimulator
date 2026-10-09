#include "feed_publisher.hpp"
#include "common.hpp"
#include "feed_encoder.hpp"
#include "feed_framer.hpp"
#include <chrono>
#include <cerrno>
#include <cstring>

// ---------------- RetransmitStore ----------------

RetransmitStore::RetransmitStore(std::size_t capacity) : slots_(capacity) {}

void RetransmitStore::put(std::uint64_t seq, const char *data, std::uint16_t len) {
    std::lock_guard<std::mutex> g(mu_);
    Slot &s = slots_[seq % slots_.size()];
    s.seq = seq;
    s.len = len;
    std::memcpy(s.data, data, len);
    newest_ = seq;
}

bool RetransmitStore::get(std::uint64_t seq, char *out, std::uint16_t &len) const {
    std::lock_guard<std::mutex> g(mu_);
    const Slot &s = slots_[seq % slots_.size()];
    if (s.len == 0 || s.seq != seq)
        return false;
    len = s.len;
    std::memcpy(out, s.data, len);
    return true;
}

std::uint64_t RetransmitStore::oldest() const {
    std::lock_guard<std::mutex> g(mu_);
    if (newest_ == 0)
        return 0;
    std::uint64_t span = slots_.size();
    return newest_ >= span ? newest_ - span + 1 : 1;
}

std::uint64_t RetransmitStore::newest() const {
    std::lock_guard<std::mutex> g(mu_);
    return newest_;
}

// ---------------- FeedPublisher ----------------

static void pad_session(const std::string &in, char out[mold::kSessionLen]) {
    std::memset(out, ' ', mold::kSessionLen);
    std::memcpy(out, in.data(),
                in.size() < mold::kSessionLen ? in.size() : mold::kSessionLen);
}

static mold::PacketBuilder make_builder(const std::string &session) {
    char s[mold::kSessionLen];
    pad_session(session, s);
    return mold::PacketBuilder(s);
}

FeedPublisher::FeedPublisher(std::vector<EventSink *> sinks, SymbolTable symbols,
                             FeedConfig cfg)
    : sinks_(std::move(sinks)), symbols_(std::move(symbols)), cfg_(std::move(cfg)),
      sock_(cfg_.group, cfg_.port, cfg_.interface, cfg_.ttl, cfg_.loop),
      packet_(make_builder(cfg_.session)), store_(cfg_.retransmit_capacity) {
    if (sinks_.empty())
        throw std::runtime_error("FeedPublisher: need at least one sink");
    framers_.reserve(sinks_.size());
    for (EventSink *s : sinks_)
        framers_.emplace_back(*s);
    seen_drops_.assign(sinks_.size(), 0);
    if (!cfg_.capture_path.empty()) {
        capture_ = std::fopen(cfg_.capture_path.c_str(), "wb");
        if (!capture_)
            throw std::runtime_error("cannot open capture file: " + cfg_.capture_path);
        static char buf[1 << 20];
        std::setvbuf(capture_, buf, _IOFBF, sizeof(buf));
    }
}

FeedPublisher::~FeedPublisher() {
    stop();
    if (capture_)
        std::fclose(capture_);
}

// Capture file format: repeated records of u32 little-endian packet length
// followed by the raw UDP payload (a MoldUDP64 packet), heartbeats included.
void FeedPublisher::send_packet() {
    if (sock_.send(packet_.data(), packet_.size()) < 0) {
        stats_.send_errors.fetch_add(1, std::memory_order_relaxed);
        stats_.last_errno.store(errno, std::memory_order_relaxed);
    }
    if (capture_) {
        std::uint32_t len = static_cast<std::uint32_t>(packet_.size());
        std::fwrite(&len, sizeof(len), 1, capture_);
        std::fwrite(packet_.data(), 1, packet_.size(), capture_);
    }
    last_send_ns_ = now_ns();
}

std::uint64_t FeedPublisher::now_ns() const { return get_monotonic_ns(); }

void FeedPublisher::start() {
    if (running_.exchange(true))
        return;
    thread_ = std::thread([this] { run(); });
}

void FeedPublisher::stop() {
    if (!running_.exchange(false))
        return;
    if (thread_.joinable())
        thread_.join();
}

void FeedPublisher::publish(const char *msg, std::uint16_t len) {
    if (packet_.empty()) {
        packet_.begin(next_seq_);
        packet_open_ns_ = now_ns();
    }
    if (!packet_.append(msg, len)) {
        flush();
        packet_.begin(next_seq_);
        packet_open_ns_ = now_ns();
        packet_.append(msg, len);
    }
    store_.put(next_seq_, msg, len);
    ++next_seq_;
    stats_.next_seq.store(next_seq_, std::memory_order_relaxed);
    stats_.messages.fetch_add(1, std::memory_order_relaxed);
}

void FeedPublisher::flush() {
    if (packet_.empty())
        return;
    send_packet();
    stats_.packets.fetch_add(1, std::memory_order_relaxed);
    packet_.begin(next_seq_); // leaves it empty with the right seq
}

void FeedPublisher::send_control(std::uint16_t count) {
    flush();
    packet_.control(next_seq_, count);
    send_packet();
    packet_.begin(next_seq_);
    if (count == 0)
        stats_.heartbeats.fetch_add(1, std::memory_order_relaxed);
}

void FeedPublisher::send_directory() {
    const std::uint64_t ts = get_current_timestamp();
    for (const auto &[id, ticker] : symbols_) {
        std::uint16_t n = feed::encode_directory(static_cast<std::uint32_t>(id),
                                                 ticker, ts, scratch_);
        publish(scratch_, n);
    }
    last_dir_ns_ = now_ns();
}

void FeedPublisher::run() {
    // Session bootstrap.
    {
        std::uint16_t n = feed::encode_system_event(
            feed::SystemEventCode::StartOfSession, get_current_timestamp(), scratch_);
        publish(scratch_, n);
        send_directory();
        flush();
    }

    const std::uint64_t flush_ns = std::uint64_t(cfg_.flush_us) * 1000;
    const std::uint64_t hb_ns = std::uint64_t(cfg_.heartbeat_ms) * 1'000'000;
    const std::uint64_t dir_ns = std::uint64_t(cfg_.directory_s) * 1'000'000'000;

    while (running_.load(std::memory_order_relaxed)) {
        bool did_work = false;

        // Round-robin over shard sinks, a bounded batch per sink per pass
        // so one busy shard cannot starve the others. Pairing is per sink,
        // since each Execute/TradeFill pair is adjacent in its own ring.
        std::uint64_t unpaired = 0;
        for (std::size_t i = 0; i < sinks_.size(); ++i) {
            // Engine overflow: events were lost. Skip the sequence by that
            // many so clients see a gap instead of a silently wrong book.
            std::uint64_t drops = sinks_[i]->dropped();
            if (drops != seen_drops_[i]) {
                std::uint64_t lost = drops - seen_drops_[i];
                seen_drops_[i] = drops;
                flush();
                next_seq_ += lost;
                stats_.engine_drops.fetch_add(lost, std::memory_order_relaxed);
            }
            for (int k = 0; k < 64; ++k) {
                std::uint16_t n = framers_[i].next(scratch_, 50'000'000); // 50ms
                if (!n)
                    break;
                did_work = true;
                publish(scratch_, n);
            }
            unpaired += framers_[i].unpaired();
        }
        stats_.unpaired_executes.store(unpaired, std::memory_order_relaxed);

        // Re-read the clock after each send: flush() updates last_send_ns_,
        // and comparing it against an older `now` underflows (unsigned).
        if (!packet_.empty() && now_ns() - packet_open_ns_ >= flush_ns)
            flush();
        if (now_ns() - last_dir_ns_ >= dir_ns) {
            send_directory();
            flush();
        }
        if (now_ns() - last_send_ns_ >= hb_ns)
            send_control(0);

        if (!did_work && cfg_.idle_sleep_us)
            std::this_thread::sleep_for(std::chrono::microseconds(cfg_.idle_sleep_us));
    }

    // Drain whatever is left, then end the session.
    for (auto &f : framers_)
        while (std::uint16_t n = f.next(scratch_, 0))
            publish(scratch_, n);
    std::uint16_t n = feed::encode_system_event(feed::SystemEventCode::EndOfSession,
                                                get_current_timestamp(), scratch_);
    publish(scratch_, n);
    flush();
    send_control(mold::kEndOfSessionCount);
    if (capture_)
        std::fflush(capture_);
}
