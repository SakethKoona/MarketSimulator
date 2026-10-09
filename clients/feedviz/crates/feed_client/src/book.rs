//! L3 order book reconstructed from feed messages. Integer prices only.

use protocol::feed::{self, Message};
use std::collections::{BTreeMap, HashMap};

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

    fn add(&mut self, id: u64, side: Side, price: u64, qty: u32) {
        let lv = self.side_mut(side).entry(price).or_default();
        lv.qty += u64::from(qty);
        lv.count += 1;
        self.orders.insert(id, Order { side, price, qty });
    }

    fn remove(&mut self, id: u64) -> Option<Order> {
        let o = self.orders.remove(&id)?;
        let levels = self.side_mut(o.side);
        if let Some(lv) = levels.get_mut(&o.price) {
            lv.qty = lv.qty.saturating_sub(u64::from(o.qty));
            lv.count = lv.count.saturating_sub(1);
            if lv.count == 0 {
                levels.remove(&o.price);
            }
        }
        Some(o)
    }

    fn shrink(&mut self, id: u64, remaining: u32) -> bool {
        let Some(o) = self.orders.get_mut(&id) else { return false };
        let delta = u64::from(o.qty.saturating_sub(remaining));
        o.qty = remaining;
        let (side, price) = (o.side, o.price);
        if let Some(lv) = self.side_mut(side).get_mut(&price) {
            lv.qty = lv.qty.saturating_sub(delta);
        }
        true
    }

    fn note_seq(&mut self, seq: u64) {
        if seq < self.last_book_seq {
            self.out_of_order += 1;
        }
        self.last_book_seq = seq;
    }

    /// Applies one message for this symbol.
    pub fn apply(&mut self, msg: &Message<'_>) {
        match msg {
            Message::AddOrder(m) => {
                self.note_seq(m.book_seq.get());
                if let Some(side) = Side::from_wire(m.side) {
                    self.add(m.order_id.get(), side, m.price.get(), m.qty.get());
                }
            }
            Message::OrderExecuted(m) => {
                self.note_seq(m.book_seq.get());
                let id = m.order_id.get();
                let known = if m.remaining_qty.get() == 0 {
                    self.remove(id).is_some()
                } else {
                    self.shrink(id, m.remaining_qty.get())
                };
                if !known {
                    self.unknown_orders += 1;
                }
                let exec = m.exec_qty.get();
                self.volume += u64::from(exec);
                self.trades += 1;
                self.last_trade = Some(Trade {
                    price: m.price.get(),
                    qty: exec,
                    aggressor_buy: m.side == feed::SIDE_SELL,
                    ts_ns: m.ts_ns.get(),
                    match_id: m.match_id.get(),
                });
            }
            Message::OrderCancel(m) => {
                self.note_seq(m.book_seq.get());
                if !self.shrink(m.order_id.get(), m.remaining_qty.get()) {
                    self.unknown_orders += 1;
                }
            }
            Message::OrderDelete(m) => {
                self.note_seq(m.book_seq.get());
                if self.remove(m.order_id.get()).is_none() {
                    self.unknown_orders += 1;
                }
            }
            Message::OrderReplace(m) => {
                self.note_seq(m.book_seq.get());
                let id = m.order_id.get();
                self.remove(id);
                if let Some(side) = Side::from_wire(m.side) {
                    self.add(id, side, m.price.get(), m.qty.get());
                }
            }
            Message::StockDirectory(m) => {
                if self.ticker.is_empty() {
                    self.ticker = m.ticker_str().to_string();
                }
            }
            Message::SystemEvent(_) => {}
        }
    }
}
