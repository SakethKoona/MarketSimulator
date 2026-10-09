//! Ingest sources. Each pushes raw packets into a wait-free ring; the
//! book thread does all decoding. Nothing here allocates per packet.

use rtrb::Producer;
use socket2::{Domain, Protocol, Socket, Type};
use std::fs::File;
use std::io::{self, BufReader, Read};
use std::net::{Ipv4Addr, SocketAddrV4, UdpSocket};
use std::path::Path;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

pub const MAX_PACKET: usize = 1500;

/// Where a packet came from. Recovery packets bypass the live-buffering
/// rule in the session; the *_DONE markers carry no data.
pub const KIND_LIVE: u8 = 0;
pub const KIND_RETRANSMIT: u8 = 1;
pub const KIND_SNAPSHOT: u8 = 2;
pub const KIND_RETRANSMIT_DONE: u8 = 3;
pub const KIND_SNAPSHOT_DONE: u8 = 4;

/// One received datagram. Fixed size so the ring never allocates.
#[derive(Clone, Copy)]
pub struct Packet {
    pub len: u16,
    pub kind: u8,
    /// For *_DONE: 1 if the fetch succeeded.
    pub ok: u8,
    /// Wall-clock receive time in ns, or 0 when replaying a capture.
    pub recv_ns: u64,
    pub data: [u8; MAX_PACKET],
}

impl Default for Packet {
    fn default() -> Self {
        Packet { len: 0, kind: KIND_LIVE, ok: 0, recv_ns: 0, data: [0; MAX_PACKET] }
    }
}

/// Splits a byte stream of concatenated MoldUDP64 packets (as the TCP
/// retransmit and snapshot servers send them) into packet slices.
pub fn split_mold_stream(buf: &[u8]) -> Vec<&[u8]> {
    let mut out = Vec::new();
    let mut i = 0usize;
    while i + 20 <= buf.len() {
        let count = u16::from_le_bytes([buf[i + 18], buf[i + 19]]);
        let mut j = i + 20;
        let n = if count == 0xFFFF { 0 } else { count as usize };
        let mut ok = true;
        for _ in 0..n {
            if j + 2 > buf.len() {
                ok = false;
                break;
            }
            let l = u16::from_le_bytes([buf[j], buf[j + 1]]) as usize;
            j += 2 + l;
            if j > buf.len() {
                ok = false;
                break;
            }
        }
        if !ok {
            break;
        }
        out.push(&buf[i..j]);
        i = j;
    }
    out
}

impl Packet {
    pub fn bytes(&self) -> &[u8] {
        &self.data[..usize::from(self.len)]
    }
}

pub fn wall_ns() -> u64 {
    SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_nanos() as u64).unwrap_or(0)
}

/// Joins `group:port` on `iface` (0.0.0.0 = kernel default) with address
/// and port reuse so several clients can run on one host.
pub fn join_multicast(group: Ipv4Addr, port: u16, iface: Ipv4Addr) -> io::Result<UdpSocket> {
    let sock = Socket::new(Domain::IPV4, Type::DGRAM, Some(Protocol::UDP))?;
    sock.set_reuse_address(true)?;
    #[cfg(not(target_os = "windows"))]
    sock.set_reuse_port(true)?;
    sock.set_recv_buffer_size(8 << 20)?;
    sock.bind(&SocketAddrV4::new(Ipv4Addr::UNSPECIFIED, port).into())?;
    sock.join_multicast_v4(&group, &iface)?;
    sock.set_read_timeout(Some(Duration::from_millis(100)))?;
    Ok(sock.into())
}

/// Receives until `stop` is set. A full ring drops the packet and counts
/// it; the socket is never blocked by a slow consumer.
pub fn run_socket(sock: UdpSocket, mut tx: Producer<Packet>, stop: &AtomicBool, dropped: &AtomicU64, drop_every: u32) {
    let mut pkt = Packet::default();
    let mut n_rx: u32 = 0;
    while !stop.load(Ordering::Relaxed) {
        match sock.recv(&mut pkt.data) {
            Ok(n) => {
                n_rx = n_rx.wrapping_add(1);
                if drop_every > 0 && n_rx % drop_every == 0 {
                    continue; // test hook: simulate packet loss
                }
                pkt.len = n as u16;
                pkt.recv_ns = wall_ns();
                if tx.push(pkt).is_err() {
                    dropped.fetch_add(1, Ordering::Relaxed);
                }
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock || e.kind() == io::ErrorKind::TimedOut => {}
            Err(_) => break,
        }
    }
}

/// Replays an `exchange_server --capture` file as fast as the book thread
/// can take it (backpressure instead of drops, since nothing is lost by
/// waiting). Returns the number of packets replayed.
pub fn run_capture(path: &Path, mut tx: Producer<Packet>, stop: &AtomicBool) -> io::Result<u64> {
    let mut rd = BufReader::with_capacity(1 << 20, File::open(path)?);
    let mut pkt = Packet::default();
    let mut n = 0u64;
    loop {
        if stop.load(Ordering::Relaxed) {
            break;
        }
        let mut len = [0u8; 4];
        match rd.read_exact(&mut len) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => break,
            Err(e) => return Err(e),
        }
        let len = u32::from_le_bytes(len) as usize;
        if len > MAX_PACKET {
            return Err(io::Error::new(io::ErrorKind::InvalidData, "capture packet too large"));
        }
        rd.read_exact(&mut pkt.data[..len])?;
        pkt.len = len as u16;
        pkt.recv_ns = 0;
        let mut p = pkt;
        loop {
            match tx.push(p) {
                Ok(()) => break,
                Err(rtrb::PushError::Full(back)) => {
                    p = back;
                    if stop.load(Ordering::Relaxed) {
                        return Ok(n);
                    }
                    std::thread::yield_now();
                }
            }
        }
        n += 1;
    }
    Ok(n)
}
