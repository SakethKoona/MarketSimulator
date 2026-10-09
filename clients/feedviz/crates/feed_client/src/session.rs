//! Mold sequencing: gap and duplicate detection, directory and session
//! events, per-type counts. Owns the books for every symbol.

use crate::book::{Applied, Book};
use crate::ingest::{KIND_LIVE, KIND_RETRANSMIT, KIND_RETRANSMIT_DONE, KIND_SNAPSHOT, KIND_SNAPSHOT_DONE};
use crate::recovery::RecoveryRequest;
use protocol::feed::{Message, MsgType};
use protocol::mold::PacketIter;
use protocol::DecodeError;
use std::collections::{BTreeMap, HashMap, VecDeque};
use std::sync::mpsc::Sender;
use std::time::{Duration, Instant};

fn debug() -> bool {
    static ON: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ON.get_or_init(|| std::env::var_os("FEEDVIZ_DEBUG").is_some())
}

/// Live packets buffered while recovering, at most this many.
const MAX_PENDING: usize = 65536;
const RECOVERY_TIMEOUT: Duration = Duration::from_secs(4);

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Recovery {
    None,
    /// Waiting for a retransmission to bring next_seq up to `upto`.
    Retransmit {
        upto: u64,
    },
    /// Loading a snapshot; live packets are buffered.
    Snapshot,
}

pub const MAX_EVENTS: usize = 16384;
pub const MAX_RAW: usize = 54;

/// One decoded message as the event log and inspector see it. Fixed size,
/// copyable, keeps its raw bytes.
#[derive(Clone, Copy, Debug)]
pub struct EventRecord {
    pub seq: u64,
    pub ts_ns: u64,
    pub recv_ns: u64,
    pub ty: u8,
    pub side: u8, // b'B' / b'S' / 0
    pub symbol_id: u32,
    pub order_id: u64,
    pub price: u64,
    pub qty: u32,       // added / executed / cancelled / replaced qty
    pub remaining: u32, // after the message
    pub match_id: u64,
    pub book_seq: u64,
    pub packet_seq: u64,
    pub block: u16,
    pub packet_count: u16,
    pub applied: Applied,
    pub raw_len: u8,
    pub raw: [u8; MAX_RAW],
}

impl Default for EventRecord {
    fn default() -> Self {
        EventRecord {
            seq: 0,
            ts_ns: 0,
            recv_ns: 0,
            ty: 0,
            side: 0,
            symbol_id: 0,
            order_id: 0,
            price: 0,
            qty: 0,
            remaining: 0,
            match_id: 0,
            book_seq: 0,
            packet_seq: 0,
            block: 0,
            packet_count: 0,
            applied: Applied::default(),
            raw_len: 0,
            raw: [0; MAX_RAW],
        }
    }
}

impl EventRecord {
    pub fn raw_bytes(&self) -> &[u8] {
        &self.raw[..usize::from(self.raw_len)]
    }
    pub fn is_gap(&self) -> bool {
        self.ty == b'!'
    }
}

#[derive(Clone, Debug, Default)]
pub struct SessionStats {
    pub packets: u64,
    pub messages: u64,
    pub heartbeats: u64,
    pub duplicates: u64,
    pub gaps: u64,
    pub lost_messages: u64,
    pub decode_errors: u64,
    pub next_seq: u64,
    pub by_type: [u64; 7],
    pub session: [u8; 10],
    pub ended: bool,
    pub version: u16,
    /// Latency samples (receive time minus message ts), nanoseconds.
    pub latency_ns: Vec<u64>,
    pub recovering: bool,
    pub recovered_gaps: u64,
    pub snapshots_loaded: u64,
    pub recovery_failed: u64,
    pub gated: u64, // messages skipped because a snapshot already covered them
}

impl SessionStats {
    pub fn type_index(t: MsgType) -> usize {
        match t {
            MsgType::SystemEvent => 0,
            MsgType::StockDirectory => 1,
            MsgType::AddOrder => 2,
            MsgType::OrderExecuted => 3,
            MsgType::OrderCancel => 4,
            MsgType::OrderDelete => 5,
            MsgType::OrderReplace => 6,
            MsgType::SnapshotStart | MsgType::SnapshotEnd => 0,
        }
    }
}

#[derive(Default)]
pub struct Session {
    pub stats: SessionStats,
    pub books: HashMap<u32, Book>,
    /// Most recent messages (and gap markers), oldest first.
    pub events: VecDeque<EventRecord>,
    /// True once the next expected sequence is known.
    synced: bool,
    /// Set when a gap was seen and books may be missing orders.
    pub stale: bool,
    pub recovery: Recovery,
    recovery_since: Option<Instant>,
    /// Live packets held back while recovering, by first sequence.
    pending: BTreeMap<u64, Vec<u8>>,
    /// Per symbol: apply only messages with book_seq above this.
    gate: HashMap<u32, u64>,
    requester: Option<Sender<RecoveryRequest>>,
    /// Id of the outstanding recovery request; answers to others are ignored.
    recovery_id: u64,
}

impl Default for Recovery {
    fn default() -> Self {
        Recovery::None
    }
}

impl Session {
    pub fn new() -> Self {
        Self::default()
    }

    /// A session that can ask a recovery thread for retransmits/snapshots.
    pub fn with_recovery(tx: Sender<RecoveryRequest>) -> Self {
        Session { requester: Some(tx), ..Default::default() }
    }

    pub fn can_recover(&self) -> bool {
        self.requester.is_some()
    }

    /// Start loading a full snapshot now (late join, or `r` in the UI).
    pub fn begin_snapshot(&mut self) {
        if self.recovery == Recovery::Snapshot {
            return;
        }
        if let Some(tx) = &self.requester {
            self.recovery_id += 1;
            if tx.send(RecoveryRequest::Snapshot { id: self.recovery_id, symbol: protocol::feed::ALL_SYMBOLS }).is_ok()
            {
                self.recovery = Recovery::Snapshot;
                self.recovery_since = Some(Instant::now());
                self.stats.recovering = true;
            }
        }
    }

    /// Call periodically from the book thread: gives up on a recovery that
    /// has taken too long so the display is not frozen forever.
    pub fn tick(&mut self) {
        if self.recovery == Recovery::None {
            return;
        }
        if let Some(t0) = self.recovery_since {
            if t0.elapsed() > RECOVERY_TIMEOUT {
                self.stats.recovery_failed += 1;
                self.finish_recovery(false);
            }
        }
    }

    fn finish_recovery(&mut self, success: bool) {
        if debug() {
            eprintln!("[rec] finish success {} next {} pending {}", success, self.stats.next_seq, self.pending.len());
        }
        self.recovery = Recovery::None;
        self.recovery_since = None;
        self.stats.recovering = false;
        if success {
            self.stale = false;
        }
        // Apply everything held back, in order. After a snapshot the gate
        // makes replaying already-covered messages harmless. If a packet in
        // the buffer reveals another gap, recovery restarts and the rest of
        // the buffer is kept for after that one.
        let pending = std::mem::take(&mut self.pending);
        for (seq, bytes) in pending {
            if self.recovery != Recovery::None {
                self.pending.entry(seq).or_insert(bytes);
                continue;
            }
            self.apply_packet(&bytes, 0);
        }
    }

    /// Entry point for every packet from the rings. For recovery kinds,
    /// `recv_ns` carries the request id the packet answers.
    pub fn on_packet_kind(&mut self, kind: u8, packet: &[u8], ok: bool, recv_ns: u64) {
        if kind != KIND_LIVE && recv_ns != 0 && recv_ns != self.recovery_id {
            return; // answer to a superseded request
        }
        match kind {
            KIND_LIVE => self.on_packet(packet, recv_ns),
            KIND_RETRANSMIT => {
                if debug() {
                    if let Ok(it) = PacketIter::new(packet) {
                        eprintln!(
                            "[rec] retransmit pkt id {} seq {} count {} (next {})",
                            recv_ns,
                            it.header().seq(),
                            it.header().count(),
                            self.stats.next_seq
                        );
                    }
                }
                self.apply_packet(packet, 0);
                self.drain_pending();
                if let Recovery::Retransmit { upto } = self.recovery {
                    if self.stats.next_seq >= upto {
                        self.stats.recovered_gaps += 1;
                        self.finish_recovery(true);
                    }
                }
            }
            KIND_RETRANSMIT_DONE => {
                if debug() {
                    eprintln!(
                        "[rec] retransmit done id {} ok {} next {} state {:?}",
                        recv_ns, ok, self.stats.next_seq, self.recovery
                    );
                }
                if let Recovery::Retransmit { upto } = self.recovery {
                    if self.stats.next_seq < upto {
                        // The server could not supply the range (too old, or
                        // the fetch failed): fall back to a snapshot.
                        let _ = ok;
                        self.recovery = Recovery::None;
                        self.begin_snapshot();
                        if self.recovery != Recovery::Snapshot {
                            self.stats.recovery_failed += 1;
                            self.finish_recovery(false);
                        }
                    }
                }
            }
            KIND_SNAPSHOT => self.apply_snapshot_packet(packet),
            KIND_SNAPSHOT_DONE => {
                if self.recovery == Recovery::Snapshot {
                    if ok {
                        self.stats.snapshots_loaded += 1;
                        // Resume from whatever live packet comes first.
                        if let Some((&first, _)) = self.pending.iter().next() {
                            self.stats.next_seq = first;
                        }
                        self.finish_recovery(true);
                    } else {
                        self.stats.recovery_failed += 1;
                        self.finish_recovery(false);
                    }
                }
            }
            _ => {}
        }
    }

    fn drain_pending(&mut self) {
        loop {
            let Some((&first, _)) = self.pending.iter().next() else { break };
            if first > self.stats.next_seq {
                break;
            }
            let bytes = self.pending.remove(&first).unwrap();
            self.apply_packet(&bytes, 0);
        }
    }

    fn apply_snapshot_packet(&mut self, packet: &[u8]) {
        let Ok(iter) = PacketIter::new(packet) else { return };
        for item in iter {
            let Ok((_, payload)) = item else { break };
            let Ok(msg) = protocol::decode(payload) else { continue };
            match msg {
                Message::SnapshotStart(st) => {
                    let sym = st.symbol_id.get();
                    let ticker = self.books.get(&sym).map(|b| b.ticker.clone()).unwrap_or_default();
                    let mut b = Book::new(sym);
                    b.ticker = ticker;
                    self.books.insert(sym, b);
                    self.gate.insert(sym, st.book_seq.get());
                }
                Message::AddOrder(_) => {
                    if let Some(sym) = msg.symbol_id() {
                        let b = self.books.entry(sym).or_insert_with(|| Book::new(sym));
                        b.apply(&msg);
                        b.last_book_seq = self.gate.get(&sym).copied().unwrap_or(0);
                    }
                }
                Message::SnapshotEnd(_) => {}
                _ => {}
            }
        }
    }

    /// Applies one live packet. `recv_ns` is the wall-clock receive time for
    /// latency measurement, or 0 to skip it (replay). While recovering, live
    /// packets are held back and applied once the gap is filled.
    pub fn on_packet(&mut self, packet: &[u8], recv_ns: u64) {
        if self.recovery != Recovery::None {
            if let Ok(it) = PacketIter::new(packet) {
                let h = it.header();
                if !h.is_heartbeat() && !h.is_end_of_session() {
                    if self.pending.len() >= MAX_PENDING {
                        self.stats.recovery_failed += 1;
                        self.finish_recovery(false);
                    } else {
                        self.pending.entry(h.seq()).or_insert_with(|| packet.to_vec());
                        return;
                    }
                } else {
                    return;
                }
            }
        }
        self.apply_packet(packet, recv_ns);
    }

    fn apply_packet(&mut self, packet: &[u8], recv_ns: u64) {
        let iter = match PacketIter::new(packet) {
            Ok(it) => it,
            Err(_) => {
                self.stats.decode_errors += 1;
                return;
            }
        };
        let h = iter.header();
        self.stats.packets += 1;
        if self.stats.packets == 1 {
            self.stats.session = h.session;
        }
        if h.is_end_of_session() {
            self.stats.ended = true;
            return;
        }
        let seq = h.seq();
        if !self.synced {
            self.synced = true;
            self.stats.next_seq = seq;
        }
        if h.is_heartbeat() {
            self.stats.heartbeats += 1;
            if seq > self.stats.next_seq {
                self.gap(seq);
            }
            return;
        }
        // Duplicates (same-host multicast quirk) and already-seen data.
        if seq < self.stats.next_seq {
            if h.next_seq() <= self.stats.next_seq {
                self.stats.duplicates += 1;
                return;
            }
        } else if seq > self.stats.next_seq {
            self.gap(seq);
            if self.recovery != Recovery::None {
                // Recovery started: hold this packet with the rest.
                self.pending.entry(seq).or_insert_with(|| packet.to_vec());
                return;
            }
        }
        let skip = self.stats.next_seq.saturating_sub(seq);
        let count = h.count();
        for (i, item) in iter.enumerate() {
            match item {
                Ok((mseq, payload)) => {
                    if (i as u64) < skip {
                        continue; // overlapping retransmission
                    }
                    self.on_message(payload, recv_ns, mseq, seq, i as u16, count);
                }
                Err(_) => {
                    self.stats.decode_errors += 1;
                    break;
                }
            }
        }
        self.stats.next_seq = h.next_seq();
    }

    fn gap(&mut self, seq: u64) {
        let lost = seq - self.stats.next_seq;
        self.stats.gaps += 1;
        self.stats.lost_messages += lost;
        self.stale = true;
        // Ask for the missing range; the live packet that revealed the gap
        // is applied by the caller and later ones are buffered.
        if self.recovery == Recovery::None {
            if let Some(tx) = &self.requester {
                let count = lost.min(1000) as u16;
                self.recovery_id += 1;
                if tx
                    .send(RecoveryRequest::Retransmit { id: self.recovery_id, seq: self.stats.next_seq, count })
                    .is_ok()
                {
                    self.recovery = Recovery::Retransmit { upto: seq };
                    self.recovery_since = Some(Instant::now());
                    self.stats.recovering = true;
                }
            }
        }
        // A marker row in the event log: ty '!' with the range in seq/qty.
        self.push_event(EventRecord {
            seq: self.stats.next_seq,
            ty: b'!',
            qty: lost.min(u32::MAX as u64) as u32,
            packet_seq: seq,
            ..EventRecord::default()
        });
    }

    fn push_event(&mut self, e: EventRecord) {
        if self.events.len() == MAX_EVENTS {
            self.events.pop_front();
        }
        self.events.push_back(e);
    }

    fn on_message(&mut self, payload: &[u8], recv_ns: u64, seq: u64, packet_seq: u64, block: u16, packet_count: u16) {
        let msg = match protocol::decode(payload) {
            Ok(m) => m,
            Err(DecodeError::UnknownType(_)) => {
                self.stats.decode_errors += 1;
                return;
            }
            Err(_) => {
                self.stats.decode_errors += 1;
                return;
            }
        };
        if let Some(sym) = msg.symbol_id() {
            if let Some(&as_of) = self.gate.get(&sym) {
                let bs = msg.book_seq();
                if bs != 0 && bs <= as_of {
                    self.stats.gated += 1;
                    return;
                }
            }
        }
        self.stats.messages += 1;
        self.stats.by_type[SessionStats::type_index(msg.msg_type())] += 1;
        if recv_ns != 0 {
            let ts = msg.ts_ns();
            if recv_ns >= ts && self.stats.latency_ns.len() < 100_000 {
                self.stats.latency_ns.push(recv_ns - ts);
            }
        }
        let mut rec = EventRecord {
            seq,
            ts_ns: msg.ts_ns(),
            recv_ns,
            ty: msg.msg_type() as u8,
            symbol_id: msg.symbol_id().unwrap_or(0),
            packet_seq,
            block,
            packet_count,
            raw_len: payload.len().min(MAX_RAW) as u8,
            ..EventRecord::default()
        };
        rec.raw[..usize::from(rec.raw_len)].copy_from_slice(&payload[..usize::from(rec.raw_len)]);
        match &msg {
            Message::SystemEvent(e) => {
                self.stats.version = e.version.get();
                if e.event_code == b'C' {
                    self.stats.ended = true;
                }
                rec.side = e.event_code;
            }
            Message::StockDirectory(_) => {
                if let Some(sym) = msg.symbol_id() {
                    self.books.entry(sym).or_insert_with(|| Book::new(sym)).apply(&msg);
                }
            }
            _ => {
                let sym = msg.symbol_id().unwrap_or(0);
                let book = self.books.entry(sym).or_insert_with(|| Book::new(sym));
                // Side/price for messages that don't carry them come from
                // the live order before it is changed.
                match &msg {
                    Message::OrderCancel(m) => {
                        if let Some(o) = book.order(m.order_id.get()) {
                            rec.side = if o.side == crate::book::Side::Bid { b'B' } else { b'S' };
                            rec.price = o.price;
                        }
                    }
                    Message::OrderDelete(m) => {
                        if let Some(o) = book.order(m.order_id.get()) {
                            rec.side = if o.side == crate::book::Side::Bid { b'B' } else { b'S' };
                            rec.price = o.price;
                        }
                    }
                    _ => {}
                }
                rec.applied = book.apply(&msg);
                match &msg {
                    Message::AddOrder(m) => {
                        rec.side = m.side;
                        rec.order_id = m.order_id.get();
                        rec.price = m.price.get();
                        rec.qty = m.qty.get();
                        rec.remaining = m.qty.get();
                        rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderExecuted(m) => {
                        rec.side = m.side;
                        rec.order_id = m.order_id.get();
                        rec.price = m.price.get();
                        rec.qty = m.exec_qty.get();
                        rec.remaining = m.remaining_qty.get();
                        rec.match_id = m.match_id.get();
                        rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderCancel(m) => {
                        rec.order_id = m.order_id.get();
                        rec.qty = rec.applied.qty_moved;
                        rec.remaining = m.remaining_qty.get();
                        rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderDelete(m) => {
                        rec.order_id = m.order_id.get();
                        rec.qty = rec.applied.qty_moved;
                        rec.remaining = 0;
                        rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderReplace(m) => {
                        rec.side = m.side;
                        rec.order_id = m.order_id.get();
                        rec.price = m.price.get();
                        rec.qty = m.qty.get();
                        rec.remaining = m.qty.get();
                        rec.book_seq = m.book_seq.get();
                    }
                    _ => {}
                }
            }
        }
        self.push_event(rec);
    }
}
