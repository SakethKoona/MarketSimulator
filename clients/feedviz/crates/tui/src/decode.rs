//! Field layouts per message type, for the inspector's decoded table and
//! the colour-keyed hex dump. Offsets follow docs/protocol/feed-v1.md.

use crate::fmt;
use crate::theme;
use feed_client::EventRecord;
use ratatui::style::Color;

pub struct Field {
    pub off: usize,
    pub len: usize,
    pub name: &'static str,
    pub ty: &'static str,
    pub value: String,
    pub note: String,
    pub color: Color,
}

fn u16_at(b: &[u8], o: usize) -> u64 {
    u16::from_le_bytes([b[o], b[o + 1]]) as u64
}
fn u32_at(b: &[u8], o: usize) -> u64 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]]) as u64
}
fn u64_at(b: &[u8], o: usize) -> u64 {
    let mut a = [0u8; 8];
    a.copy_from_slice(&b[o..o + 8]);
    u64::from_le_bytes(a)
}

struct Palette {
    ty: Color,
    side: Color,
    sym: Color,
    oid: Color,
    px: Color,
    qty: Color,
    rem: Color,
    mat: Color,
    bseq: Color,
    ts: Color,
}
fn palette() -> Palette {
    let th = theme::th();
    Palette {
        ty: th.ask,
        side: th.amber_text,
        sym: th.text,
        oid: th.muted,
        px: th.bid,
        qty: th.green,
        rem: th.green,
        mat: th.purple,
        bseq: th.dim,
        ts: th.dimmer,
    }
}

#[allow(non_snake_case)]
pub fn fields(e: &EventRecord, ticker: &str) -> Vec<Field> {
    let b = e.raw_bytes();
    let p = palette();
    let (C_TYPE, C_SIDE, C_SYM, C_OID, C_PX, C_QTY, C_REM, C_MATCH, C_BSEQ, C_TS) =
        (p.ty, p.side, p.sym, p.oid, p.px, p.qty, p.rem, p.mat, p.bseq, p.ts);
    let mut v = Vec::new();
    let mut f =
        |off: usize, len: usize, name: &'static str, ty: &'static str, value: String, note: String, color: Color| {
            if off + len <= b.len() {
                v.push(Field { off, len, name, ty, value, note, color });
            }
        };
    let type_val = |t: u8| format!("0x{:02x}  '{}'", t, t as char);
    let side_note = |s: u8| match s {
        b'B' => "buy".to_string(),
        b'S' => "sell".to_string(),
        _ => "?".to_string(),
    };
    let ts_note = |ts: u64| {
        if e.recv_ns > ts && e.recv_ns != 0 {
            format!("{} UTC · received +{}", fmt::time_ns(ts), fmt::micros(e.recv_ns - ts))
        } else {
            format!("{} UTC", fmt::time_ns(ts))
        }
    };
    let id_note = |id: u64, what: &str| format!("shard {} · seq {} · {}", fmt::shard_of(id), fmt::seq_of(id), what);
    let sym_note = format!("{} · shard {}", if ticker.is_empty() { "ticker unknown yet" } else { ticker }, e.symbol_id);
    match e.ty {
        b'A' => {
            f(0, 1, "type", "u8", type_val(b[0]), fmt::type_name(b'A').to_lowercase(), C_TYPE);
            f(1, 1, "side", "u8", type_val(b[1]), side_note(b[1]), C_SIDE);
            f(2, 4, "symbol_id", "u32", u32_at(b, 2).to_string(), sym_note.clone(), C_SYM);
            f(6, 8, "order_id", "u64", fmt::hex_id(u64_at(b, 6)), id_note(u64_at(b, 6), "new resting order"), C_OID);
            f(14, 8, "price", "u64", fmt::commas(u64_at(b, 14)), "price_scale 0".into(), C_PX);
            f(22, 4, "qty", "u32", fmt::commas(u32_at(b, 22)), "resting quantity".into(), C_QTY);
            f(
                26,
                8,
                "book_seq",
                "u64",
                fmt::hex_id(u64_at(b, 26)),
                format!("{} · increasing within symbol", fmt::short_id(u64_at(b, 26))),
                C_BSEQ,
            );
            f(34, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 34)), ts_note(u64_at(b, 34)), C_TS);
        }
        b'E' => {
            f(0, 1, "type", "u8", type_val(b[0]), "order executed".into(), C_TYPE);
            f(
                1,
                1,
                "side",
                "u8",
                type_val(b[1]),
                format!("resting {} → aggressor {}", side_note(b[1]), if b[1] == b'S' { "BUY" } else { "SELL" }),
                C_SIDE,
            );
            f(2, 4, "symbol_id", "u32", u32_at(b, 2).to_string(), sym_note.clone(), C_SYM);
            f(6, 8, "order_id", "u64", fmt::hex_id(u64_at(b, 6)), id_note(u64_at(b, 6), "the resting order"), C_OID);
            f(14, 8, "price", "u64", fmt::commas(u64_at(b, 14)), "execution px = resting px".into(), C_PX);
            f(22, 4, "exec_qty", "u32", fmt::commas(u32_at(b, 22)), "shares filled in this match".into(), C_QTY);
            let rem = u32_at(b, 26);
            f(
                26,
                4,
                "remaining_qty",
                "u32",
                fmt::commas(rem),
                if rem == 0 {
                    "order left the book · no Delete follows".into()
                } else {
                    "still resting, priority kept".into()
                },
                C_REM,
            );
            f(
                30,
                8,
                "match_id",
                "u64",
                fmt::hex_id(u64_at(b, 30)),
                id_note(u64_at(b, 30), "same id in the BOE report"),
                C_MATCH,
            );
            f(
                38,
                8,
                "book_seq",
                "u64",
                fmt::hex_id(u64_at(b, 38)),
                format!("{} · increasing within symbol", fmt::short_id(u64_at(b, 38))),
                C_BSEQ,
            );
            f(46, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 46)), ts_note(u64_at(b, 46)), C_TS);
        }
        b'X' => {
            f(0, 1, "type", "u8", type_val(b[0]), "order cancel (owner reduced qty)".into(), C_TYPE);
            f(1, 4, "symbol_id", "u32", u32_at(b, 1).to_string(), sym_note.clone(), C_SYM);
            f(5, 8, "order_id", "u64", fmt::hex_id(u64_at(b, 5)), id_note(u64_at(b, 5), "priority kept"), C_OID);
            f(
                13,
                4,
                "remaining_qty",
                "u32",
                fmt::commas(u32_at(b, 13)),
                format!("cancelled {}", fmt::commas(e.qty as u64)),
                C_REM,
            );
            f(17, 8, "book_seq", "u64", fmt::hex_id(u64_at(b, 17)), fmt::short_id(u64_at(b, 17)), C_BSEQ);
            f(25, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 25)), ts_note(u64_at(b, 25)), C_TS);
        }
        b'D' => {
            f(0, 1, "type", "u8", type_val(b[0]), "order delete".into(), C_TYPE);
            f(1, 4, "symbol_id", "u32", u32_at(b, 1).to_string(), sym_note.clone(), C_SYM);
            f(
                5,
                8,
                "order_id",
                "u64",
                fmt::hex_id(u64_at(b, 5)),
                id_note(u64_at(b, 5), &format!("removed {} resting", fmt::commas(e.qty as u64))),
                C_OID,
            );
            f(13, 8, "book_seq", "u64", fmt::hex_id(u64_at(b, 13)), fmt::short_id(u64_at(b, 13)), C_BSEQ);
            f(21, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 21)), ts_note(u64_at(b, 21)), C_TS);
        }
        b'U' => {
            f(0, 1, "type", "u8", type_val(b[0]), "order replace (same id, back of queue)".into(), C_TYPE);
            f(1, 1, "side", "u8", type_val(b[1]), side_note(b[1]), C_SIDE);
            f(2, 4, "symbol_id", "u32", u32_at(b, 2).to_string(), sym_note.clone(), C_SYM);
            f(
                6,
                8,
                "order_id",
                "u64",
                fmt::hex_id(u64_at(b, 6)),
                id_note(u64_at(b, 6), "keeps its id, loses priority"),
                C_OID,
            );
            f(14, 8, "price", "u64", fmt::commas(u64_at(b, 14)), "new price".into(), C_PX);
            f(22, 4, "qty", "u32", fmt::commas(u32_at(b, 22)), "new resting quantity".into(), C_QTY);
            f(26, 8, "book_seq", "u64", fmt::hex_id(u64_at(b, 26)), fmt::short_id(u64_at(b, 26)), C_BSEQ);
            f(34, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 34)), ts_note(u64_at(b, 34)), C_TS);
        }
        b'R' => {
            f(0, 1, "type", "u8", type_val(b[0]), "stock directory".into(), C_TYPE);
            f(1, 1, "price_scale", "u8", b[1].to_string(), "implied decimals".into(), C_SIDE);
            f(2, 2, "reserved", "u16", u16_at(b, 2).to_string(), String::new(), C_BSEQ);
            f(4, 4, "symbol_id", "u32", u32_at(b, 4).to_string(), String::new(), C_SYM);
            f(
                8,
                8,
                "ticker",
                "char[8]",
                String::from_utf8_lossy(&b[8..16]).trim_end().to_string(),
                String::new(),
                C_OID,
            );
            f(16, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 16)), ts_note(u64_at(b, 16)), C_TS);
        }
        b'S' => {
            f(0, 1, "type", "u8", type_val(b[0]), "system event".into(), C_TYPE);
            f(
                1,
                1,
                "event_code",
                "u8",
                type_val(b[1]),
                if b[1] == b'O' { "start of session".into() } else { "end of session".into() },
                C_SIDE,
            );
            f(2, 2, "version", "u16", u16_at(b, 2).to_string(), "protocol version".into(), C_SYM);
            f(4, 8, "ts_ns", "u64", fmt::commas(u64_at(b, 4)), ts_note(u64_at(b, 4)), C_TS);
        }
        _ => {}
    }
    v
}

/// Colour of each raw byte, from the field table.
pub fn byte_colors(fields: &[Field], len: usize) -> Vec<Color> {
    let mut v = vec![theme::th().dimmer; len];
    for f in fields {
        for i in f.off..(f.off + f.len).min(len) {
            v[i] = f.color;
        }
    }
    v
}

/// The spec paragraph for a message type.
pub fn spec_text(ty: u8) -> &'static str {
    match ty {
        b'A' => "§3.3 Add Order. A new order is resting on the book at price for qty. Append it at the back of its level.",
        b'E' => "§3.4 Order Executed. A resting order was matched, in part or in full. The aggressor is the opposite side of `side`. When remaining_qty is 0 the order has left the book; no separate Delete follows. Trade volume is the sum of exec_qty over Order Executed messages; there is no separate Trade message.",
        b'X' => "§3.5 Order Cancel. A resting order's quantity was reduced by its owner. Identity and priority are unchanged. remaining_qty is the new resting quantity, always > 0.",
        b'D' => "§3.6 Order Delete. A resting order was removed by its owner, or a replace left nothing resting.",
        b'U' => "§3.7 Order Replace. A resting order changed price and/or increased quantity. It keeps its order_id but loses time priority: remove it from its current level and append it at the back of the new one. Fills caused by the replacement are published as Order Executed before this message.",
        b'R' => "§3.2 Stock Directory. One per symbol at session start and every few seconds so late joiners can decode symbol ids.",
        b'S' => "§3.1 System Event. 'O' start of session, 'C' end of session. A client that receives an unknown version must stop.",
        _ => "",
    }
}
