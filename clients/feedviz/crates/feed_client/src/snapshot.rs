//! What the UI thread sees: an immutable copy of everything it draws,
//! rebuilt by the book thread a few dozen times a second.

use crate::book::{Bar, Book, FlowSec, SessionStatsSym, Trade};
use crate::session::{EventRecord, Session, SessionStats};

/// Events carried in each snapshot (the newest ones).
pub const SNAPSHOT_EVENTS: usize = 2048;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct LevelSnap {
    pub price: u64,
    pub qty: u64,
    pub count: u32,
    /// Cumulative quantity from the top of book through this level.
    pub cum: u64,
}

#[derive(Clone, Debug, Default)]
pub struct SymbolSnapshot {
    pub symbol_id: u32,
    pub ticker: String,
    /// Best first.
    pub bids: Vec<LevelSnap>,
    pub asks: Vec<LevelSnap>,
    pub bid_levels: usize,
    pub ask_levels: usize,
    pub orders: usize,
    pub last_trade: Option<Trade>,
    pub volume: u64,
    pub trades: u64,
    pub book_seq: u64,
    pub out_of_order: u64,
    pub unknown_orders: u64,
    /// One Vec per entry of book::INTERVALS.
    pub bars: [Vec<Bar>; 4],
    /// Newest last.
    pub tape: Vec<Trade>,
    pub flow: Vec<FlowSec>,
    pub session: SessionStatsSym,
    /// Messages per second for this symbol over the last full second.
    pub msgs_per_sec: u32,
}

impl SymbolSnapshot {
    pub fn best_bid(&self) -> Option<&LevelSnap> {
        self.bids.first()
    }
    pub fn best_ask(&self) -> Option<&LevelSnap> {
        self.asks.first()
    }
    pub fn spread(&self) -> Option<u64> {
        Some(self.best_ask()?.price.saturating_sub(self.best_bid()?.price))
    }
    pub fn mid_x2(&self) -> Option<u64> {
        Some(self.best_ask()?.price + self.best_bid()?.price)
    }
}

#[derive(Clone, Debug, Default)]
pub struct Snapshot {
    /// Sorted by symbol id.
    pub symbols: Vec<SymbolSnapshot>,
    pub stats: SessionStats,
    pub stale: bool,
    /// Messages applied in the last second, as measured by the book thread.
    pub msgs_per_sec: u64,
    /// Receive latency percentiles over the last snapshot interval, ns.
    pub latency_p50_ns: u64,
    pub latency_p99_ns: u64,
    /// Ingest ring occupancy 0..1 and packets the ingest thread dropped.
    pub ring_occupancy: f32,
    pub ring_drops: u64,
    /// True once a capture source has been fully replayed.
    pub finished: bool,
    /// Monotonic counter; the UI can skip a redraw when it hasn't changed.
    pub revision: u64,
    /// Newest events last, including gap markers.
    pub events: Vec<EventRecord>,
    /// Per-second histories, oldest first.
    pub rate_hist: Vec<u64>,
    pub p99_hist: Vec<u64>,
}

impl Snapshot {
    /// Builds a snapshot of `session` with `depth` levels per side. The
    /// latency percentiles are supplied by the caller, which keeps a
    /// one-second window of samples.
    pub fn build(session: &mut Session, depth: usize, revision: u64, latency: (u64, u64)) -> Snapshot {
        let mut symbols: Vec<SymbolSnapshot> =
            session.books.values().map(|b| book_snapshot(b, depth)).collect();
        symbols.sort_by_key(|s| s.symbol_id);

        let (p50, p99) = latency;
        let mut stats = session.stats.clone();
        stats.latency_ns = Vec::new();

        Snapshot {
            symbols,
            stats,
            stale: session.stale,
            msgs_per_sec: 0,
            latency_p50_ns: p50,
            latency_p99_ns: p99,
            ring_occupancy: 0.0,
            ring_drops: 0,
            finished: false,
            revision,
            events: session.events.iter().rev().take(SNAPSHOT_EVENTS).rev().copied().collect(),
            rate_hist: Vec::new(),
            p99_hist: Vec::new(),
        }
    }
}

fn book_snapshot(b: &Book, depth: usize) -> SymbolSnapshot {
    let mut cum = 0u64;
    let bids = b
        .bids
        .iter()
        .rev()
        .take(depth)
        .map(|(p, l)| {
            cum += l.qty;
            LevelSnap { price: *p, qty: l.qty, count: l.count, cum }
        })
        .collect();
    cum = 0;
    let asks = b
        .asks
        .iter()
        .take(depth)
        .map(|(p, l)| {
            cum += l.qty;
            LevelSnap { price: *p, qty: l.qty, count: l.count, cum }
        })
        .collect();
    SymbolSnapshot {
        symbol_id: b.symbol_id,
        ticker: b.ticker.clone(),
        bids,
        asks,
        bid_levels: b.bids.len(),
        ask_levels: b.asks.len(),
        orders: b.order_count(),
        last_trade: b.last_trade,
        volume: b.volume,
        trades: b.trades,
        book_seq: b.last_book_seq,
        out_of_order: b.out_of_order,
        unknown_orders: b.unknown_orders,
        bars: [
            b.bars[0].iter().copied().collect(),
            b.bars[1].iter().copied().collect(),
            b.bars[2].iter().copied().collect(),
            b.bars[3].iter().copied().collect(),
        ],
        tape: b.tape.iter().copied().collect(),
        flow: b.flow.iter().copied().collect(),
        session: b.session,
        msgs_per_sec: b.flow.iter().rev().nth(1).map(|f| f.messages).unwrap_or(0),
    }
}

pub fn percentiles(samples: &mut [u64]) -> (u64, u64) {
    if samples.is_empty() {
        return (0, 0);
    }
    samples.sort_unstable();
    let at = |q: f64| samples[((samples.len() - 1) as f64 * q) as usize];
    (at(0.50), at(0.99))
}
