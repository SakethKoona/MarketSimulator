//! Mold sequencing: gap and duplicate detection, directory and session
//! events, per-type counts. Owns the books for every symbol.

use crate::book::Book;
use protocol::feed::{Message, MsgType};
use protocol::mold::PacketIter;
use protocol::DecodeError;
use std::collections::HashMap;

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
        for (i, item) in iter.enumerate() {
            match item {
                Ok((_, payload)) => {
                    if (i as u64) < skip {
                        continue; // overlapping retransmission
                    }
                    self.on_message(payload, recv_ns);
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
        self.stats.gaps += 1;
        self.stats.lost_messages += seq - self.stats.next_seq;
        self.stale = true;
    }

    fn on_message(&mut self, payload: &[u8], recv_ns: u64) {
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
        match &msg {
            Message::SystemEvent(e) => {
                self.stats.version = e.version.get();
                if e.event_code == b'C' {
                    self.stats.ended = true;
                }
            }
            _ => {
                if let Some(sym) = msg.symbol_id() {
                    self.books.entry(sym).or_insert_with(|| Book::new(sym)).apply(&msg);
                }
            }
        }
    }
}
