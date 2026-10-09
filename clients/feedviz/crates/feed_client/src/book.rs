//! L3 order book reconstructed from feed messages. Integer prices only.

use protocol::feed::{self, Message};
use std::collections::{BTreeMap, HashMap, VecDeque};

pub const MAX_BARS: usize = 240;
pub const MAX_TAPE: usize = 128;
pub const MAX_FLOW: usize = 180;
/// Candle intervals in seconds, in UI order.
pub const INTERVALS: [u64; 4] = [1, 5, 30, 60];

/// One second of activity, for the flow charts and indicators.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct FlowSec {
    pub sec: u64,
    pub trades: u32,
    pub buy_vol: u64,
    pub sell_vol: u64,
    pub adds: u32,
    pub cancels: u32, // X + D
    pub execs: u32,
    pub replaces: u32,
    pub messages: u32,
    pub spread_sum: u64,
    pub spread_n: u32,
}

/// Running session statistics for one symbol.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SessionStatsSym {
    pub open: u64,
    pub high: u64,
    pub low: u64,
    pub last: u64,
    pub vwap_num: u128, // sum(price * qty)
    pub buy_vol: u64,
    pub sell_vol: u64,
    pub spread_sum: u64,
    pub spread_n: u64,
}

impl SessionStatsSym {
    pub fn vwap(&self) -> f64 {
        let v = self.buy_vol + self.sell_vol;
        if v == 0 {
            0.0
        } else {
            self.vwap_num as f64 / v as f64
        }
    }
    pub fn avg_spread(&self) -> f64 {
        if self.spread_n == 0 {
            0.0
        } else {
            self.spread_sum as f64 / self.spread_n as f64
        }
    }
}

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
    /// One deque per entry of INTERVALS.
    pub bars: [VecDeque<Bar>; 4],
    pub tape: VecDeque<Trade>,
    pub flow: VecDeque<FlowSec>,
    pub session: SessionStatsSym,
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

    /// The flow bucket for `ts_ns`, creating it if the second rolled over.
    fn flow_mut(&mut self, ts_ns: u64) -> &mut FlowSec {
        let sec = ts_ns / 1_000_000_000;
        let fresh = match self.flow.back() {
            Some(f) => f.sec != sec,
            None => true,
        };
        if fresh {
            if self.flow.len() == MAX_FLOW {
                self.flow.pop_front();
            }
            self.flow.push_back(FlowSec { sec, ..Default::default() });
        }
        self.flow.back_mut().unwrap()
    }

    /// Samples the current spread into the flow bucket and session stats.
    fn sample_spread(&mut self, ts_ns: u64) {
        if let (Some((b, _)), Some((a, _))) = (self.best_bid(), self.best_ask()) {
            let sp = a.saturating_sub(b);
            self.session.spread_sum += sp;
            self.session.spread_n += 1;
            let f = self.flow_mut(ts_ns);
            f.spread_sum += sp;
            f.spread_n += 1;
        }
    }

    fn record_trade(&mut self, t: Trade) {
        let q = u64::from(t.qty);
        self.volume += q;
        self.trades += 1;
        self.last_trade = Some(t);
        if self.tape.len() == MAX_TAPE {
            self.tape.pop_front();
        }
        self.tape.push_back(t);

        let st = &mut self.session;
        if st.open == 0 {
            st.open = t.price;
            st.high = t.price;
            st.low = t.price;
        }
        st.high = st.high.max(t.price);
        st.low = st.low.min(t.price);
        st.last = t.price;
        st.vwap_num += (t.price as u128) * (q as u128);
        if t.aggressor_buy {
            st.buy_vol += q
        } else {
            st.sell_vol += q
        }

        let sec = t.ts_ns / 1_000_000_000;
        for (i, iv) in INTERVALS.iter().enumerate() {
            let bsec = sec - sec % iv;
            let bars = &mut self.bars[i];
            match bars.back_mut() {
                Some(b) if b.sec == bsec => {
                    b.high = b.high.max(t.price);
                    b.low = b.low.min(t.price);
                    b.close = t.price;
                    b.volume += q;
                    b.trades += 1;
                }
                _ => {
                    if bars.len() == MAX_BARS {
                        bars.pop_front();
                    }
                    bars.push_back(Bar {
                        sec: bsec,
                        open: t.price,
                        high: t.price,
                        low: t.price,
                        close: t.price,
                        volume: q,
                        trades: 1,
                    });
                }
            }
        }
        let f = self.flow_mut(t.ts_ns);
        f.trades += 1;
        if t.aggressor_buy {
            f.buy_vol += q
        } else {
            f.sell_vol += q
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
        let applied = self.apply_inner(msg);
        if !matches!(msg, Message::StockDirectory(_) | Message::SystemEvent(_)) {
            let ts = msg.ts_ns();
            {
                let f = self.flow_mut(ts);
                f.messages += 1;
                match msg {
                    Message::AddOrder(_) => f.adds += 1,
                    Message::OrderExecuted(_) => f.execs += 1,
                    Message::OrderCancel(_) | Message::OrderDelete(_) => f.cancels += 1,
                    Message::OrderReplace(_) => f.replaces += 1,
                    _ => {}
                }
            }
            self.sample_spread(ts);
        }
        applied
    }

    fn apply_inner(&mut self, msg: &Message<'_>) -> Applied {
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
