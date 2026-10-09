//! feedviz. Checkpoint 1: a headless monitor that runs the full pipeline
//! and prints the top of book and feed health once a second. The ratatui
//! screens from the design replace `print_frame` without touching the
//! pipeline.
//!
//!   feedviz                              join 239.1.1.1:30001 on the default interface
//!   feedviz --iface 127.0.0.1            same-host testing on macOS
//!   feedviz --replay testdata/capture.bin
//!   feedviz --depth 5 --seconds 10

use anyhow::{bail, Result};
use feed_client::{Config, FeedClient, Snapshot, Source};
use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::time::{Duration, Instant};

struct Args {
    source: Source,
    depth: usize,
    seconds: f64,
    interval_ms: u64,
}

fn parse_args() -> Result<Args> {
    let mut group = Ipv4Addr::new(239, 1, 1, 1);
    let mut port = 30001u16;
    let mut iface = Ipv4Addr::UNSPECIFIED;
    let mut replay: Option<PathBuf> = None;
    let mut depth = 5usize;
    let mut seconds = 0.0f64;
    let mut interval_ms = 1000u64;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut val = || it.next().ok_or_else(|| anyhow::anyhow!("missing value for {a}"));
        match a.as_str() {
            "--group" => group = val()?.parse()?,
            "--port" => port = val()?.parse()?,
            "--iface" => iface = val()?.parse()?,
            "--replay" => replay = Some(PathBuf::from(val()?)),
            "--depth" => depth = val()?.parse()?,
            "--seconds" => seconds = val()?.parse()?,
            "--interval-ms" => interval_ms = val()?.parse()?,
            "-h" | "--help" => {
                println!("feedviz [--group G] [--port P] [--iface IP] [--replay FILE] [--depth N] [--seconds S] [--interval-ms MS]");
                std::process::exit(0);
            }
            other => bail!("unknown argument {other}"),
        }
    }
    let source = match replay {
        Some(path) => Source::Capture { path },
        None => Source::Multicast { group, port, iface },
    };
    Ok(Args { source, depth, seconds, interval_ms })
}

fn fmt_px(p: u64) -> String {
    p.to_string()
}

/// Ids and book_seq carry the engine shard in their top 8 bits.
fn fmt_shard_seq(v: u64) -> String {
    format!("{}:{}", v >> 56, v & ((1u64 << 56) - 1))
}

fn print_frame(s: &Snapshot, elapsed: f64) {
    println!(
        "[{:7.1}s] seq={} msgs={} ({}/s) pkts={} hb={} gaps={} lost={} dups={} ring={:.0}% drops={} lat p50={}µs p99={}µs{}{}",
        elapsed,
        s.stats.next_seq,
        s.stats.messages,
        s.msgs_per_sec,
        s.stats.packets,
        s.stats.heartbeats,
        s.stats.gaps,
        s.stats.lost_messages,
        s.stats.duplicates,
        s.ring_occupancy * 100.0,
        s.ring_drops,
        s.latency_p50_ns / 1000,
        s.latency_p99_ns / 1000,
        if s.stale { " STALE" } else { "" },
        if s.stats.ended { " ENDED" } else { "" },
    );
    for sym in &s.symbols {
        let bb = sym.best_bid().map(|l| format!("{}x{}", l.qty, fmt_px(l.price))).unwrap_or_else(|| "-".into());
        let ba = sym.best_ask().map(|l| format!("{}x{}", fmt_px(l.price), l.qty)).unwrap_or_else(|| "-".into());
        let last = sym
            .last_trade
            .map(|t| format!("{}@{} {}", t.qty, fmt_px(t.price), if t.aggressor_buy { "▲" } else { "▼" }))
            .unwrap_or_else(|| "-".into());
        println!(
            "   {:<8} bid {:>14}  |  ask {:<14} spread={:<4} last={:<16} vol={:<9} trades={:<7} orders={:<6} levels={}/{} seq={}{}",
            if sym.ticker.is_empty() { format!("#{}", sym.symbol_id) } else { sym.ticker.clone() },
            bb,
            ba,
            sym.spread().map(|x| x.to_string()).unwrap_or_else(|| "-".into()),
            last,
            sym.volume,
            sym.trades,
            sym.orders,
            sym.bid_levels,
            sym.ask_levels,
            fmt_shard_seq(sym.book_seq),
            if sym.unknown_orders > 0 { format!(" unknown={}", sym.unknown_orders) } else { String::new() },
        );
        let depth = sym.bids.len().max(sym.asks.len());
        for i in 0..depth {
            let b = sym.bids.get(i).map(|l| format!("{:>4} {:>8} {:>8}", l.count, l.qty, fmt_px(l.price))).unwrap_or_else(|| " ".repeat(22));
            let a = sym.asks.get(i).map(|l| format!("{:<8} {:<8} {:<4}", fmt_px(l.price), l.qty, l.count)).unwrap_or_default();
            println!("            {b}  |  {a}");
        }
    }
}

fn main() -> Result<()> {
    let args = parse_args()?;
    let is_capture = matches!(args.source, Source::Capture { .. });
    eprintln!("feedviz: source {:?}", args.source);
    let mut client = FeedClient::start(Config {
        source: args.source,
        depth: args.depth,
        ..Config::default()
    })?;
    let start = Instant::now();
    let interval = Duration::from_millis(args.interval_ms);
    let mut last_rev = 0;
    loop {
        std::thread::sleep(interval);
        let elapsed = start.elapsed().as_secs_f64();
        let snap = client.latest();
        if snap.revision != last_rev || is_capture {
            print_frame(snap, elapsed);
            last_rev = snap.revision;
        } else {
            println!("[{elapsed:7.1}s] no packets yet (is exchange_server running? on macOS try --iface 127.0.0.1 with feed.interface 127.0.0.1)");
        }
        if snap.finished || (args.seconds > 0.0 && elapsed >= args.seconds) {
            break;
        }
    }
    client.stop();
    Ok(())
}
