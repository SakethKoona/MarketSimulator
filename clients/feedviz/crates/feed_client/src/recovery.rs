//! Recovery thread: fetches retransmissions and snapshots over TCP on
//! request from the book thread and feeds the packets into a second ring.
//! Blocking I/O is fine here; it never touches the live socket.

use crate::ingest::{Packet, KIND_RETRANSMIT, KIND_RETRANSMIT_DONE, KIND_SNAPSHOT, KIND_SNAPSHOT_DONE, MAX_PACKET};
use rtrb::Producer;
use std::io::{Read, Write};
use std::net::{SocketAddr, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, RecvTimeoutError};
use std::time::Duration;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum RecoveryRequest {
    /// `id` is echoed in `Packet::recv_ns` on every packet and the done
    /// marker, so the session can ignore answers to superseded requests.
    Retransmit {
        id: u64,
        seq: u64,
        count: u16,
    },
    Snapshot {
        id: u64,
        symbol: u32,
    }, // protocol::feed::ALL_SYMBOLS for all
}

#[derive(Clone, Debug)]
pub struct RecoveryEndpoints {
    pub retransmit: SocketAddr,
    pub snapshot: SocketAddr,
    pub session: [u8; 10],
}

fn fetch(addr: SocketAddr, request: &[u8]) -> std::io::Result<Vec<u8>> {
    let mut s = TcpStream::connect_timeout(&addr, Duration::from_secs(2))?;
    s.set_read_timeout(Some(Duration::from_secs(3)))?;
    s.set_write_timeout(Some(Duration::from_secs(2)))?;
    s.write_all(request)?;
    let mut buf = Vec::with_capacity(64 * 1024);
    s.read_to_end(&mut buf)?;
    Ok(buf)
}

fn push_stream(tx: &mut Producer<Packet>, bytes: &[u8], kind: u8, id: u64, stop: &AtomicBool) -> usize {
    let mut n = 0;
    for pkt in crate::ingest::split_mold_stream(bytes) {
        if pkt.len() > MAX_PACKET {
            continue;
        }
        let mut p = Packet::default();
        p.kind = kind;
        p.recv_ns = id;
        p.len = pkt.len() as u16;
        p.data[..pkt.len()].copy_from_slice(pkt);
        loop {
            match tx.push(p) {
                Ok(()) => break,
                Err(rtrb::PushError::Full(back)) => {
                    p = back;
                    if stop.load(Ordering::Relaxed) {
                        return n;
                    }
                    std::thread::sleep(Duration::from_micros(200));
                }
            }
        }
        n += 1;
    }
    n
}

fn push_marker(tx: &mut Producer<Packet>, kind: u8, id: u64, ok: bool) {
    let mut p = Packet::default();
    p.kind = kind;
    p.ok = ok as u8;
    p.recv_ns = id;
    while tx.push(p).is_err() {
        std::thread::sleep(Duration::from_micros(200));
    }
}

pub fn run(ep: RecoveryEndpoints, rx: Receiver<RecoveryRequest>, mut tx: Producer<Packet>, stop: &AtomicBool) {
    while !stop.load(Ordering::Relaxed) {
        let req = match rx.recv_timeout(Duration::from_millis(100)) {
            Ok(r) => r,
            Err(RecvTimeoutError::Timeout) => continue,
            Err(RecvTimeoutError::Disconnected) => return,
        };
        match req {
            RecoveryRequest::Retransmit { id, seq, count } => {
                let mut r = Vec::with_capacity(20);
                r.extend_from_slice(&ep.session);
                r.extend_from_slice(&seq.to_le_bytes());
                r.extend_from_slice(&count.to_le_bytes());
                let ok = match fetch(ep.retransmit, &r) {
                    Ok(bytes) => {
                        push_stream(&mut tx, &bytes, KIND_RETRANSMIT, id, stop);
                        true
                    }
                    Err(e) => {
                        eprintln!("feed-recovery: retransmit {seq}+{count}: {e}");
                        false
                    }
                };
                push_marker(&mut tx, KIND_RETRANSMIT_DONE, id, ok);
            }
            RecoveryRequest::Snapshot { id, symbol } => {
                let mut r = Vec::with_capacity(14);
                r.extend_from_slice(&ep.session);
                r.extend_from_slice(&symbol.to_le_bytes());
                let ok = match fetch(ep.snapshot, &r) {
                    Ok(bytes) => {
                        push_stream(&mut tx, &bytes, KIND_SNAPSHOT, id, stop);
                        true
                    }
                    Err(e) => {
                        eprintln!("feed-recovery: snapshot: {e}");
                        false
                    }
                };
                push_marker(&mut tx, KIND_SNAPSHOT_DONE, id, ok);
            }
        }
    }
}
