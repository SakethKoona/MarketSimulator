//! Zero-copy wire structs for the MarketSimulator market data feed.
//!
//! Contract: `docs/protocol/feed-v1.md`. Everything is little-endian and
//! packed; every struct size is asserted at compile time against the spec.

pub mod capture;
pub mod feed;
pub mod mold;

pub use feed::{decode, Message, MsgType};
pub use mold::{PacketHeader, PacketIter, END_OF_SESSION, SESSION_LEN};

#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum DecodeError {
    #[error("packet shorter than the MoldUDP64 header")]
    ShortPacket,
    #[error("packet truncated inside message block {block}")]
    Truncated { block: u16 },
    #[error("unknown message type {0:#04x}")]
    UnknownType(u8),
    #[error("message of type {ty:?} has length {got}, expected {expected}")]
    BadLength { ty: u8, got: usize, expected: usize },
}
