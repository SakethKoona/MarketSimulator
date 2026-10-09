//! L3 order book reconstructed from feed messages. Integer prices only.

use protocol::feed::{self, Message};
use std::collections::{BTreeMap, HashMap, VecDeque};

pub const MAX_BARS: usize = 180;
pub const MAX_TAPE: usize = 128;

/// One-second OHLCV bar from execution prices.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Bar {
    pub sec: u64, // unix seconds
    pub open: u64,
    pub high: u64,
    pub low: u64,
    pub close: u64,
    pub volume: u64,
    pub trades: u32,
}

/// What a message did to its price level, for the inspector.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Applied {
    pub level_qty_after: u64,
    pub level_count_after: u32,
    /// Quantity the message moved: added, executed, cancelled, or the
    /// order's resting qty for a Delete.
    pub qty_moved: u32,
    pub known: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Side {
    Bid,
    Ask,
}

impl Side {
    pub fn from_wire(b: u8) -> Option<Side> {
        match b {
            feed::SIDE_BUY => Some(Side::Bid),
            feed::SIDE_SELL => Some(Side::Ask),
            _ => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Level {
    pub qty: u64,
    pub count: u32,
}

#[derive(Clone, Copy, Debug)]
pub struct Order {
    pub side: Side,
    pub price: u64,
    pub qty: u32,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Trade {
    pub price: u64,
    pub qty: u32,
    pub aggressor_buy: bool,
    pub ts_ns: u64,
    pub match_id: u64,
}

/// One symbol's book. Bids and asks are keyed by price; iterate bids in
/// reverse for best-first.
#[derive(Default, Debug)]
pub struct Book {
    pub symbol_id: u32,
    pub ticker: String,
    pub bids: BTreeMap<u64, Level>,
    pub asks: BTreeMap<u64, Level>,
    orders: HashMap<u64, Order>,
    pub last_book_seq: u64,
    pub last_trade: Option<Trade>,
    pub volume: u64,
    pub trades: u64,
    pub bars: VecDeque<Bar>,
    pub tape: VecDeque<Trade>,
    /// Messages whose order id was unknown, e.g. after a late join.
    pub unknown_orders: u64,
    pub out_of_order: u64,
}

impl Book {
    pub fn new(symbol_id: u32) -> Self {
        Self { symbol_id, ..Default::default() }
    }

    pub fn order_count(&self) -> usize {
        self.orders.len()
    }
    pub fn best_bid(&self) -> Option<(u64, Level)> {
        self.bids.iter().next_back().map(|(p, l)| (*p, *l))
    }
    pub fn best_ask(&self) -> Option<(u64, Level)> {
        self.asks.iter().next().map(|(p, l)| (*p, *l))
    }

    fn side_mut(&mut self, side: Side) -> &mut BTreeMap<u64, Level> {
        match side {
            Side::Bid => &mut self.bids,
            Side::Ask => &mut self.asks,
        }
    }

    fn level_state(&self, side: Side, price: u64) -> (u64, u32) {
        let levels = match side {
            Side::Bid => &self.bids,
            Side::Ask => &self.asks,
        };
        levels.get(&price).map(|l| (l.qty, l.count)).unwrap_or((0, 0))
    }

    fn add(&mut self, id: u64, side: Side, price: u64, qty: u32) -> Applied {
        let lv = self.side_mut(side).entry(price).or_default();
        lv.qty += u64::from(qty);
        lv.count += 1;
        let (q, c) = (lv.qty, lv.count);
        self.orders.insert(id, Order { side, price, qty });
        Applied { level_qty_after: q, level_count_after: c, qty_moved: qty, known: true }
    }

    fn remove(&mut self, id: u64) -> Option<(Order, Applied)> {
        let o = self.orders.remove(&id)?;
        let levels = self.side_mut(o.side);
        let mut after = (0u64, 0u32);
        if let Some(lv) = levels.get_mut(&o.price) {
            lv.qty = lv.qty.saturating_sub(u64::from(o.qty));
            lv.count = lv.count.saturating_sub(1);
            after = (lv.qty, lv.count);
            if lv.count == 0 {
                levels.remove(&o.price);
            }
        }
        Some((o, Applied { level_qty_after: after.0, level_count_after: after.1, qty_moved: o.qty, known: true }))
    }

    fn shrink(&mut self, id: u64, remaining: u32) -> Option<Applied> {
        let o = self.orders.get_mut(&id)?;
        let moved = o.qty.saturating_sub(remaining);
        o.qty = remaining;
        let (side, price) = (o.side, o.price);
        let mut after = (0u64, 0u32);
        if let Some(lv) = self.side_mut(side).get_mut(&price) {
            lv.qty = lv.qty.saturating_sub(u64::from(moved));
            after = (lv.qty, lv.count);
        }
        Some(Applied { level_qty_after: after.0, level_count_after: after.1, qty_moved: moved, known: true })
    }

    fn record_trade(&mut self, t: Trade) {
        self.volume += u64::from(t.qty);
        self.trades += 1;
        self.last_trade = Some(t);
        if self.tape.len() == MAX_TAPE {
            self.tape.pop_front();
        }
        self.tape.push_back(t);
        let sec = t.ts_ns / 1_000_000_000;
        match self.bars.back_mut() {
            Some(b) if b.sec == sec => {
                b.high = b.high.max(t.price);
                b.low = b.low.min(t.price);
                b.close = t.price;
                b.volume += u64::from(t.qty);
                b.trades += 1;
            }
            _ => {
                if self.bars.len() == MAX_BARS {
                    self.bars.pop_front();
                }
                self.bars.push_back(Bar { sec, open: t.price, high: t.price, low: t.price, close: t.price, volume: u64::from(t.qty), trades: 1 });
            }
        }
    }

    fn note_seq(&mut self, seq: u64) {
        if seq < self.last_book_seq {
            self.out_of_order += 1;
        }
        self.last_book_seq = seq;
    }

    /// Applies one message for this symbol and reports what it did to the
    /// affected level.
    pub fn apply(&mut self, msg: &Message<'_>) -> Applied {
        let unknown = |this: &mut Self, side: Option<Side>, px: u64| {
            this.unknown_orders += 1;
            let (q, c) = side.map(|s| this.level_state(s, px)).unwrap_or((0, 0));
            Applied { level_qty_after: q, level_count_after: c, qty_moved: 0, known: false }
        };
        match msg {
            Message::AddOrder(m) => {
                self.note_seq(m.book_seq.get());
                match Side::from_wire(m.side) {
                    Some(side) => self.add(m.order_id.get(), side, m.price.get(), m.qty.get()),
                    None => unknown(self, None, 0),
                }
            }
            Message::OrderExecuted(m) => {
                self.note_seq(m.book_seq.get());
                let id = m.order_id.get();
                let exec = m.exec_qty.get();
                let applied = if m.remaining_qty.get() == 0 {
                    self.remove(id).map(|(_, a)| Applied { qty_moved: exec, ..a })
                } else {
                    self.shrink(id, m.remaining_qty.get())
                };
                let applied = match applied {
                    Some(a) => a,
                    None => unknown(self, Side::from_wire(m.side), m.price.get()),
                };
                self.record_trade(Trade {
                    price: m.price.get(),
                    qty: exec,
                    aggressor_buy: m.side == feed::SIDE_SELL,
                    ts_ns: m.ts_ns.get(),
                    match_id: m.match_id.get(),
                });
                applied
            }
            Message::OrderCancel(m) => {
                self.note_seq(m.book_seq.get());
                match self.shrink(m.order_id.get(), m.remaining_qty.get()) {
                    Some(a) => a,
                    None => unknown(self, None, 0),
                }
            }
            Message::OrderDelete(m) => {
                self.note_seq(m.book_seq.get());
                match self.remove(m.order_id.get()) {
                    Some((_, a)) => a,
                    None => unknown(self, None, 0),
                }
            }
            Message::OrderReplace(m) => {
                self.note_seq(m.book_seq.get());
                let id = m.order_id.get();
                self.remove(id);
                match Side::from_wire(m.side) {
                    Some(side) => self.add(id, side, m.price.get(), m.qty.get()),
                    None => unknown(self, None, 0),
                }
            }
            Message::StockDirectory(m) => {
                if self.ticker.is_empty() {
                    self.ticker = m.ticker_str().to_string();
                }
                Applied::default()
            }
            Message::SystemEvent(_) => Applied::default(),
        }
    }

    /// Side and resting price of a live order, if known.
    pub fn order(&self, id: u64) -> Option<Order> {
        self.orders.get(&id).copied()
    }
}
