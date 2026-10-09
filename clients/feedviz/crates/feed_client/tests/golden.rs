//! Replays a capture recorded by `exchange_server --capture` and requires
//! the reconstructed books to equal the engine's final books, level by
//! level, as written to `<capture>.books.txt`.

use feed_client::{Config, FeedClient, Session, Source};
use protocol::capture::CaptureReader;
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant};

fn testdata(name: &str) -> PathBuf {
    Path::new(env!("CARGO_MANIFEST_DIR")).join("../../testdata").join(name)
}

type Levels = BTreeMap<(u32, char, u64), (u64, u32)>; // (sym, side, px) -> (qty, count)

fn expected_levels(path: &Path) -> Levels {
    let text = std::fs::read_to_string(path).expect("books file");
    let mut m = Levels::new();
    for line in text.lines() {
        let f: Vec<&str> = line.split_whitespace().collect();
        if f.len() != 5 {
            continue;
        }
        m.insert(
            (f[0].parse().unwrap(), f[1].chars().next().unwrap(), f[2].parse().unwrap()),
            (f[3].parse().unwrap(), f[4].parse().unwrap()),
        );
    }
    m
}

fn actual_levels(session: &Session) -> Levels {
    let mut m = Levels::new();
    for (sym, book) in &session.books {
        for (px, l) in &book.bids {
            m.insert((*sym, 'B', *px), (l.qty, l.count));
        }
        for (px, l) in &book.asks {
            m.insert((*sym, 'S', *px), (l.qty, l.count));
        }
    }
    m
}

fn check_capture(name: &str) {
    let cap = testdata(name);
    if !cap.exists() {
        eprintln!("skipping {name}: fixture missing");
        return;
    }
    let mut rd = CaptureReader::open(&cap).unwrap();
    let mut session = Session::new();
    while let Some(pkt) = rd.next_packet().unwrap() {
        session.on_packet(pkt, 0);
    }
    assert!(session.stats.ended, "capture should end with End of Session");
    assert_eq!(session.stats.gaps, 0, "a capture has every packet");
    assert_eq!(session.stats.decode_errors, 0);
    for b in session.books.values() {
        assert_eq!(b.unknown_orders, 0, "symbol {}: message for unknown order", b.symbol_id);
        assert_eq!(b.out_of_order, 0, "symbol {}: book_seq went backwards", b.symbol_id);
        assert!(!b.ticker.is_empty(), "symbol {} never got a directory entry", b.symbol_id);
    }

    let want = expected_levels(&testdata(&format!("{name}.books.txt")));
    let got = actual_levels(&session);
    for (k, v) in &want {
        assert_eq!(got.get(k), Some(v), "level {k:?}: engine has {v:?}, client has {:?}", got.get(k));
    }
    for (k, v) in &got {
        assert!(want.contains_key(k), "level {k:?} = {v:?} exists on the client but not in the engine");
    }
    eprintln!(
        "{name}: {} packets, {} messages, {} levels match",
        session.stats.packets, session.stats.messages, want.len()
    );
}

#[test]
fn reconstruction_matches_engine_three_shards() {
    check_capture("capture.bin");
}

#[test]
fn reconstruction_matches_engine_one_shard() {
    check_capture("capture_1shard.bin");
}

#[test]
fn pipeline_replays_capture_to_completion() {
    let cap = testdata("capture.bin");
    let mut client = FeedClient::start(Config {
        source: Source::Capture { path: cap.clone() },
        ring_capacity: 256,
        depth: 5,
        snapshot_interval: Duration::from_millis(5),
    })
    .unwrap();
    let deadline = Instant::now() + Duration::from_secs(10);
    while !client.is_finished() {
        assert!(Instant::now() < deadline, "replay did not finish");
        std::thread::sleep(Duration::from_millis(5));
    }
    let snap = client.latest().clone();
    assert!(snap.finished);
    assert!(snap.stats.ended);
    assert_eq!(snap.stats.gaps, 0);
    assert_eq!(snap.ring_drops, 0, "replay must apply backpressure, never drop");
    assert_eq!(snap.symbols.len(), 3);
    for s in &snap.symbols {
        assert!(!s.ticker.is_empty());
        assert!(s.bids.len() <= 5 && s.asks.len() <= 5);
        if let (Some(b), Some(a)) = (s.best_bid(), s.best_ask()) {
            assert!(b.price < a.price, "{}: crossed book", s.ticker);
        }
    }
    // Direct replay and the threaded pipeline must agree on message count.
    let mut rd = CaptureReader::open(&cap).unwrap();
    let mut direct = Session::new();
    while let Some(p) = rd.next_packet().unwrap() {
        direct.on_packet(p, 0);
    }
    assert_eq!(snap.stats.messages, direct.stats.messages);
    client.stop();
}
