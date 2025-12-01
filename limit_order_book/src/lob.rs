
use std::{cmp::{self, Ordering}, collections::VecDeque, error::Error};
use crate::types::{LimitOrder};

pub struct LimitOrderBook {
   pub bids: Vec<LimitOrder>,
   pub asks: Vec<LimitOrder>,
}


impl LimitOrderBook {
    pub fn add_order(order: LimitOrder) -> Result<(), String> {
        todo!("Implement adding an order");
    }

    pub fn cancel_order(order_id: i32) -> Result<(), String> {
        todo!("Implement canceling an order");
    }

}

