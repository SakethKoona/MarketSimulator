//! Number and time formatting without extra dependencies.

pub fn commas(n: u64) -> String {
    let s = n.to_string();
    let mut out = String::with_capacity(s.len() + s.len() / 3);
    for (i, c) in s.chars().enumerate() {
        if i > 0 && (s.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(c);
    }
    out
}

/// HH:MM:SS.nnnnnnnnn in UTC from nanoseconds since the Unix epoch.
pub fn time_ns(ts_ns: u64) -> String {
    let secs = ts_ns / 1_000_000_000;
    let ns = ts_ns % 1_000_000_000;
    let sod = secs % 86_400;
    format!("{:02}:{:02}:{:02}.{:09}", sod / 3600, (sod / 60) % 60, sod % 60, ns)
}

/// HH:MM:SS.mmm
pub fn time_ms(ts_ns: u64) -> String {
    let secs = ts_ns / 1_000_000_000;
    let ms = (ts_ns % 1_000_000_000) / 1_000_000;
    let sod = secs % 86_400;
    format!("{:02}:{:02}:{:02}.{:03}", sod / 3600, (sod / 60) % 60, sod % 60, ms)
}

pub fn micros(ns: u64) -> String {
    if ns >= 10_000_000 {
        format!("{:.1} ms", ns as f64 / 1e6)
    } else {
        format!("{} µs", ns / 1000)
    }
}

/// Shard-tagged ids: top 8 bits are the shard.
pub fn shard_of(v: u64) -> u8 {
    (v >> 56) as u8
}
pub fn seq_of(v: u64) -> u64 {
    v & ((1u64 << 56) - 1)
}
pub fn hex_id(v: u64) -> String {
    format!("0x{v:016x}")
}
pub fn short_id(v: u64) -> String {
    format!("{}:{}", shard_of(v), seq_of(v))
}

pub fn type_name(ty: u8) -> &'static str {
    match ty {
        b'A' => "ADD ORDER",
        b'E' => "ORDER EXECUTED",
        b'X' => "ORDER CANCEL",
        b'D' => "ORDER DELETE",
        b'U' => "ORDER REPLACE",
        b'R' => "STOCK DIRECTORY",
        b'S' => "SYSTEM EVENT",
        b'!' => "GAP",
        _ => "UNKNOWN",
    }
}

pub fn side_name(side: u8) -> &'static str {
    match side {
        b'B' => "BUY",
        b'S' => "SELL",
        _ => "",
    }
}

/// Unicode block sparkline from values, one char per value.
pub fn spark(values: &[u64], width: usize) -> String {
    const BARS: [char; 8] = ['▁', '▂', '▃', '▄', '▅', '▆', '▇', '█'];
    let take = values.len().min(width);
    let slice = &values[values.len() - take..];
    let max = slice.iter().copied().max().unwrap_or(0).max(1);
    let mut s = String::with_capacity(width);
    for _ in 0..width.saturating_sub(take) {
        s.push(' ');
    }
    for v in slice {
        let i = ((*v as f64 / max as f64) * 7.0).round() as usize;
        s.push(BARS[i.min(7)]);
    }
    s
}
