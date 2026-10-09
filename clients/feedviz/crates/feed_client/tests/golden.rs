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
        session.stats.packets,
        session.stats.messages,
        want.len()
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

/// Drop a run of packets from the capture, keep applying the rest (the
/// session must hold them back), then deliver the missing ones as a
/// retransmission. The books must equal a straight replay and the gap
/// must be reported as recovered.
#[test]
fn gap_is_healed_by_retransmission() {
    use feed_client::ingest::{split_mold_stream, KIND_LIVE, KIND_RETRANSMIT, KIND_RETRANSMIT_DONE};
    use feed_client::RecoveryRequest;
    let cap = testdata("capture.bin");
    let mut rd = CaptureReader::open(&cap).unwrap();
    let mut packets: Vec<Vec<u8>> = Vec::new();
    while let Some(p) = rd.next_packet().unwrap() {
        packets.push(p.to_vec());
    }
    // Straight replay for the expected books.
    let mut truth = Session::new();
    for p in &packets {
        truth.on_packet(p, 0);
    }

    let (tx, rx) = std::sync::mpsc::channel::<RecoveryRequest>();
    let mut s = Session::with_recovery(tx);
    let drop_from = 400usize;
    let drop_n = 7usize;
    for (i, p) in packets.iter().enumerate() {
        if i >= drop_from && i < drop_from + drop_n {
            continue;
        }
        s.on_packet_kind(KIND_LIVE, p, false, 0);
    }
    assert_eq!(s.stats.gaps, 1);
    assert!(s.stats.recovering, "a gap must start recovery");
    let req = rx.try_recv().expect("a retransmit request");
    let (id, seq, count) = match req {
        RecoveryRequest::Retransmit { id, seq, count } => (id, seq, count),
        other => panic!("unexpected request {other:?}"),
    };
    // Serve it from the dropped packets, concatenated as the TCP server would.
    let mut stream = Vec::new();
    for p in &packets[drop_from..drop_from + drop_n] {
        stream.extend_from_slice(p);
    }
    let served = split_mold_stream(&stream);
    assert_eq!(served.len(), drop_n);
    let first_seq = u64::from_le_bytes(served[0][10..18].try_into().unwrap());
    assert_eq!(first_seq, seq, "request starts at the first missing sequence");
    assert!(count as usize >= 1);
    for p in served {
        s.on_packet_kind(KIND_RETRANSMIT, p, true, id);
    }
    s.on_packet_kind(KIND_RETRANSMIT_DONE, &[], true, id);

    assert!(!s.stats.recovering);
    assert!(!s.stale, "healed gap must clear STALE");
    assert_eq!(s.stats.recovered_gaps, 1);
    assert_eq!(s.stats.messages, truth.stats.messages, "every message applied exactly once");
    assert_eq!(s.stats.next_seq, truth.stats.next_seq);
    assert_eq!(actual_levels(&s), actual_levels(&truth));
    for b in s.books.values() {
        assert_eq!(b.unknown_orders, 0);
        assert_eq!(b.out_of_order, 0);
    }
}

/// A snapshot replaces the books and gates older messages: apply the first
/// half live, load a "snapshot" made from the truth session's books at that
/// point, then the second half; the result must equal the straight replay.
#[test]
fn snapshot_gates_already_covered_messages() {
    use feed_client::ingest::{KIND_LIVE, KIND_SNAPSHOT, KIND_SNAPSHOT_DONE};
    use feed_client::RecoveryRequest;
    let cap = testdata("capture.bin");
    let mut rd = CaptureReader::open(&cap).unwrap();
    let mut packets: Vec<Vec<u8>> = Vec::new();
    while let Some(p) = rd.next_packet().unwrap() {
        packets.push(p.to_vec());
    }
    let mut truth = Session::new();
    for p in &packets {
        truth.on_packet(p, 0);
    }
    // Build the snapshot bytes at the midpoint from a reference session.
    let mid = packets.len() / 2;
    let mut at_mid = Session::new();
    for p in &packets[..mid] {
        at_mid.on_packet(p, 0);
    }
    let mut snap = Vec::new();
    for (sym, book) in &at_mid.books {
        // One Mold packet per symbol: Q, Adds (bids best first, asks best first), Z.
        let mut body = Vec::new();
        let mut count = 0u32;
        let mut adds = Vec::new();
        // Real orders with their real ids, as the exchange's snapshot sends.
        for (oid, o) in book.orders() {
            let side = if o.side == feed_client::Side::Bid { b'B' } else { b'S' };
            let mut a = vec![b'A', side];
            a.extend_from_slice(&(*sym).to_le_bytes());
            a.extend_from_slice(&oid.to_le_bytes());
            a.extend_from_slice(&o.price.to_le_bytes());
            a.extend_from_slice(&o.qty.to_le_bytes());
            a.extend_from_slice(&book.last_book_seq.to_le_bytes());
            a.extend_from_slice(&0u64.to_le_bytes());
            adds.push(a);
            count += 1;
        }
        let mut q = vec![b'Q'];
        q.extend_from_slice(&sym.to_le_bytes());
        q.extend_from_slice(&book.last_book_seq.to_le_bytes());
        q.extend_from_slice(&count.to_le_bytes());
        q.extend_from_slice(&0u64.to_le_bytes());
        let mut z = vec![b'Z'];
        z.extend_from_slice(&sym.to_le_bytes());
        z.extend_from_slice(&count.to_le_bytes());
        z.extend_from_slice(&0u64.to_le_bytes());
        let mut msgs = vec![q];
        msgs.extend(adds);
        msgs.push(z);
        body.extend_from_slice(b"MKTSIM0001");
        body.extend_from_slice(&0u64.to_le_bytes());
        body.extend_from_slice(&(msgs.len() as u16).to_le_bytes());
        for m in msgs {
            body.extend_from_slice(&(m.len() as u16).to_le_bytes());
            body.extend_from_slice(&m);
        }
        snap.push(body);
    }

    let (tx, rx) = std::sync::mpsc::channel::<RecoveryRequest>();
    let mut s = Session::with_recovery(tx);
    s.begin_snapshot();
    let sid = match rx.try_recv() {
        Ok(RecoveryRequest::Snapshot { id, .. }) => id,
        other => panic!("expected a snapshot request, got {other:?}"),
    };
    // Live packets arrive while the snapshot is "in flight": the first half
    // gets buffered, then the snapshot lands, then the rest streams in.
    for p in &packets[..mid] {
        s.on_packet_kind(KIND_LIVE, p, false, 0);
    }
    for p in &snap {
        s.on_packet_kind(KIND_SNAPSHOT, p, true, sid);
    }
    s.on_packet_kind(KIND_SNAPSHOT_DONE, &[], true, sid);
    assert!(!s.stats.recovering);
    assert_eq!(s.stats.snapshots_loaded, 1);
    for p in &packets[mid..] {
        s.on_packet_kind(KIND_LIVE, p, false, 0);
    }
    assert_eq!(actual_levels(&s), actual_levels(&truth), "books after snapshot + live must equal a straight replay");
    assert!(s.stats.gated > 0, "messages covered by the snapshot must be gated");
    for b in s.books.values() {
        assert_eq!(b.unknown_orders, 0, "symbol {}: a real snapshot leaves no unknown orders", b.symbol_id);
    }
}
