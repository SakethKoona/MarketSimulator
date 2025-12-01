
use std::collections::VecDeque;
use std::cmp::Ordering;

pub(crate) enum Side {
    Buy,
    Sell,
}


pub(crate) struct MarketOrder {
    side: crate::types::Side,
    order_id: u32,
    timestamp: std::time::SystemTime,
    symbol: String,
    quantity: u32,
}

pub(crate) struct LimitOrder {
    order_id: u32,
    price: f32,
    quantity: u32,
    symbol: String,
    timestamp: std::time::SystemTime,
    side: crate::types::Side,
}

pub struct PriceLevel {
    price: u32,
    orders: VecDeque<LimitOrder>,
}

impl PartialEq for PriceLevel {
    fn eq(&self, other: &Self) -> bool {
        self.price == other.price
    }
}

impl Eq for PriceLevel {}

impl PartialOrd for PriceLevel {
    fn partial_cmp(&self, other: &Self) -> Option<Ordering> {
        return Some(self.cmp(&other));
    }
}


impl Ord for PriceLevel {
    fn cmp(&self, other: &Self) -> Ordering {
        return self.price.cmp(&other.price);
    }
}



