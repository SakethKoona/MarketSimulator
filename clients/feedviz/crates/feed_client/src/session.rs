//! Mold sequencing: gap and duplicate detection, directory and session
//! events, per-type counts. Owns the books for every symbol.

use crate::book::{Applied, Book};
use protocol::feed::{Message, MsgType};
use protocol::mold::PacketIter;
use protocol::DecodeError;
use std::collections::{HashMap, VecDeque};

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
            seq: 0, ts_ns: 0, recv_ns: 0, ty: 0, side: 0, symbol_id: 0, order_id: 0, price: 0, qty: 0,
            remaining: 0, match_id: 0, book_seq: 0, packet_seq: 0, block: 0, packet_count: 0,
            applied: Applied::default(), raw_len: 0, raw: [0; MAX_RAW],
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
}

impl Session {
    pub fn new() -> Self {
        Self::default()
    }

    /// Applies one raw packet. `recv_ns` is the wall-clock receive time for
    /// latency measurement, or 0 to skip it (replay).
    pub fn on_packet(&mut self, packet: &[u8], recv_ns: u64) {
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
                        rec.side = m.side; rec.order_id = m.order_id.get(); rec.price = m.price.get();
                        rec.qty = m.qty.get(); rec.remaining = m.qty.get(); rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderExecuted(m) => {
                        rec.side = m.side; rec.order_id = m.order_id.get(); rec.price = m.price.get();
                        rec.qty = m.exec_qty.get(); rec.remaining = m.remaining_qty.get();
                        rec.match_id = m.match_id.get(); rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderCancel(m) => {
                        rec.order_id = m.order_id.get(); rec.qty = rec.applied.qty_moved;
                        rec.remaining = m.remaining_qty.get(); rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderDelete(m) => {
                        rec.order_id = m.order_id.get(); rec.qty = rec.applied.qty_moved;
                        rec.remaining = 0; rec.book_seq = m.book_seq.get();
                    }
                    Message::OrderReplace(m) => {
                        rec.side = m.side; rec.order_id = m.order_id.get(); rec.price = m.price.get();
                        rec.qty = m.qty.get(); rec.remaining = m.qty.get(); rec.book_seq = m.book_seq.get();
                    }
                    _ => {}
                }
            }
        }
        self.push_event(rec);
    }
}
