

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


