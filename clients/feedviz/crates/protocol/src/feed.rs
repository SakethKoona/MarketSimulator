//! Feed messages (feed-v1.md §3). All little-endian, packed.

use crate::DecodeError;
use zerocopy::little_endian::{U16, U32, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

pub const VERSION: u16 = 1;
pub const SIDE_BUY: u8 = b'B';
pub const SIDE_SELL: u8 = b'S';

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum MsgType {
    SystemEvent = b'S',
    StockDirectory = b'R',
    AddOrder = b'A',
    OrderExecuted = b'E',
    OrderCancel = b'X',
    OrderDelete = b'D',
    OrderReplace = b'U',
    SnapshotStart = b'Q',
    SnapshotEnd = b'Z',
}

impl MsgType {
    pub fn from_u8(b: u8) -> Option<Self> {
        Some(match b {
            b'S' => Self::SystemEvent,
            b'R' => Self::StockDirectory,
            b'A' => Self::AddOrder,
            b'E' => Self::OrderExecuted,
            b'X' => Self::OrderCancel,
            b'D' => Self::OrderDelete,
            b'U' => Self::OrderReplace,
            b'Q' => Self::SnapshotStart,
            b'Z' => Self::SnapshotEnd,
            _ => return None,
        })
    }
}

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct SystemEvent {
    pub ty: u8,
    pub event_code: u8, // b'O' start of session, b'C' end of session
    pub version: U16,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<SystemEvent>() == 12);

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct StockDirectory {
    pub ty: u8,
    pub price_scale: u8,
    pub reserved: U16,
    pub symbol_id: U32,
    pub ticker: [u8; 8],
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<StockDirectory>() == 24);

impl StockDirectory {
    pub fn ticker_str(&self) -> &str {
        core::str::from_utf8(&self.ticker).unwrap_or("????????").trim_end()
    }
}

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct AddOrder {
    pub ty: u8,
    pub side: u8,
    pub symbol_id: U32,
    pub order_id: U64,
    pub price: U64,
    pub qty: U32,
    pub book_seq: U64,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<AddOrder>() == 42);

/// A resting order was matched. `remaining_qty == 0` means it left the
/// book and no Delete follows. The aggressor is the opposite of `side`.
#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct OrderExecuted {
    pub ty: u8,
    pub side: u8,
    pub symbol_id: U32,
    pub order_id: U64,
    pub price: U64,
    pub exec_qty: U32,
    pub remaining_qty: U32,
    pub match_id: U64,
    pub book_seq: U64,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<OrderExecuted>() == 54);

/// Owner reduced quantity; identity and priority unchanged.
#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct OrderCancel {
    pub ty: u8,
    pub symbol_id: U32,
    pub order_id: U64,
    pub remaining_qty: U32,
    pub book_seq: U64,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<OrderCancel>() == 33);

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct OrderDelete {
    pub ty: u8,
    pub symbol_id: U32,
    pub order_id: U64,
    pub book_seq: U64,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<OrderDelete>() == 29);

/// Same id, new price and/or qty, back of the queue at the new level.
#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct OrderReplace {
    pub ty: u8,
    pub side: u8,
    pub symbol_id: U32,
    pub order_id: U64,
    pub price: U64,
    pub qty: U32,
    pub book_seq: U64,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<OrderReplace>() == 42);

/// Snapshot framing (feed-v1.md §5): the book for `symbol_id` as of
/// `book_seq`, followed by `order_count` Add Orders and a Snapshot End.
#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct SnapshotStart {
    pub ty: u8,
    pub symbol_id: U32,
    pub book_seq: U64,
    pub order_count: U32,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<SnapshotStart>() == 25);

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct SnapshotEnd {
    pub ty: u8,
    pub symbol_id: U32,
    pub order_count: U32,
    pub ts_ns: U64,
}
const _: () = assert!(core::mem::size_of::<SnapshotEnd>() == 17);

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct SnapshotRequest {
    pub session: [u8; 10],
    pub symbol_id: U32,
}
const _: () = assert!(core::mem::size_of::<SnapshotRequest>() == 14);
pub const ALL_SYMBOLS: u32 = 0xFFFF_FFFF;

/// A borrowed view of one decoded message.
#[derive(Clone, Copy, Debug)]
pub enum Message<'a> {
    SystemEvent(&'a SystemEvent),
    StockDirectory(&'a StockDirectory),
    AddOrder(&'a AddOrder),
    OrderExecuted(&'a OrderExecuted),
    OrderCancel(&'a OrderCancel),
    OrderDelete(&'a OrderDelete),
    OrderReplace(&'a OrderReplace),
    SnapshotStart(&'a SnapshotStart),
    SnapshotEnd(&'a SnapshotEnd),
}

impl<'a> Message<'a> {
    pub fn msg_type(&self) -> MsgType {
        match self {
            Message::SystemEvent(_) => MsgType::SystemEvent,
            Message::StockDirectory(_) => MsgType::StockDirectory,
            Message::AddOrder(_) => MsgType::AddOrder,
            Message::OrderExecuted(_) => MsgType::OrderExecuted,
            Message::OrderCancel(_) => MsgType::OrderCancel,
            Message::OrderDelete(_) => MsgType::OrderDelete,
            Message::OrderReplace(_) => MsgType::OrderReplace,
            Message::SnapshotStart(_) => MsgType::SnapshotStart,
            Message::SnapshotEnd(_) => MsgType::SnapshotEnd,
        }
    }
    /// Symbol this message is about, if any.
    pub fn symbol_id(&self) -> Option<u32> {
        Some(match self {
            Message::SystemEvent(_) => return None,
            Message::StockDirectory(m) => m.symbol_id.get(),
            Message::AddOrder(m) => m.symbol_id.get(),
            Message::OrderExecuted(m) => m.symbol_id.get(),
            Message::OrderCancel(m) => m.symbol_id.get(),
            Message::OrderDelete(m) => m.symbol_id.get(),
            Message::OrderReplace(m) => m.symbol_id.get(),
            Message::SnapshotStart(m) => m.symbol_id.get(),
            Message::SnapshotEnd(m) => m.symbol_id.get(),
        })
    }
    /// Engine sequence carried by book messages; 0 for the others.
    pub fn book_seq(&self) -> u64 {
        match self {
            Message::AddOrder(m) => m.book_seq.get(),
            Message::OrderExecuted(m) => m.book_seq.get(),
            Message::OrderCancel(m) => m.book_seq.get(),
            Message::OrderDelete(m) => m.book_seq.get(),
            Message::OrderReplace(m) => m.book_seq.get(),
            Message::SnapshotStart(m) => m.book_seq.get(),
            _ => 0,
        }
    }
    pub fn ts_ns(&self) -> u64 {
        match self {
            Message::SystemEvent(m) => m.ts_ns.get(),
            Message::StockDirectory(m) => m.ts_ns.get(),
            Message::AddOrder(m) => m.ts_ns.get(),
            Message::OrderExecuted(m) => m.ts_ns.get(),
            Message::OrderCancel(m) => m.ts_ns.get(),
            Message::OrderDelete(m) => m.ts_ns.get(),
            Message::OrderReplace(m) => m.ts_ns.get(),
            Message::SnapshotStart(m) => m.ts_ns.get(),
            Message::SnapshotEnd(m) => m.ts_ns.get(),
        }
    }
}

fn cast<'a, T: FromBytes + KnownLayout + Immutable + Unaligned>(
    ty: u8,
    payload: &'a [u8],
) -> Result<&'a T, DecodeError> {
    T::ref_from_bytes(payload).map_err(|_| DecodeError::BadLength {
        ty,
        got: payload.len(),
        expected: core::mem::size_of::<T>(),
    })
}

/// Decodes one message block payload in place.
pub fn decode(payload: &[u8]) -> Result<Message<'_>, DecodeError> {
    let ty = *payload.first().ok_or(DecodeError::BadLength { ty: 0, got: 0, expected: 1 })?;
    Ok(match MsgType::from_u8(ty).ok_or(DecodeError::UnknownType(ty))? {
        MsgType::SystemEvent => Message::SystemEvent(cast(ty, payload)?),
        MsgType::StockDirectory => Message::StockDirectory(cast(ty, payload)?),
        MsgType::AddOrder => Message::AddOrder(cast(ty, payload)?),
        MsgType::OrderExecuted => Message::OrderExecuted(cast(ty, payload)?),
        MsgType::OrderCancel => Message::OrderCancel(cast(ty, payload)?),
        MsgType::OrderDelete => Message::OrderDelete(cast(ty, payload)?),
        MsgType::OrderReplace => Message::OrderReplace(cast(ty, payload)?),
        MsgType::SnapshotStart => Message::SnapshotStart(cast(ty, payload)?),
        MsgType::SnapshotEnd => Message::SnapshotEnd(cast(ty, payload)?),
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The reference encoding from feed-v1.md §6.
    #[test]
    fn reference_add_order() {
        let bytes: [u8; 42] = [
            0x41, 0x42, 0, 0, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0x69, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0,
        ];
        match decode(&bytes).unwrap() {
            Message::AddOrder(a) => {
                assert_eq!(a.side, SIDE_BUY);
                assert_eq!(a.symbol_id.get(), 0);
                assert_eq!(a.order_id.get(), 7);
                assert_eq!(a.price.get(), 105);
                assert_eq!(a.qty.get(), 5);
                assert_eq!(a.book_seq.get(), 2);
            }
            other => panic!("decoded {other:?}"),
        }
        assert_eq!(decode(&bytes[..41]).unwrap_err(), DecodeError::BadLength { ty: b'A', got: 41, expected: 42 });
        assert_eq!(decode(&[0x59]).unwrap_err(), DecodeError::UnknownType(0x59));
    }
}
