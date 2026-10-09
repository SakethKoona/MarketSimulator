//! Wires the threads together: ingest → ring → book thread → snapshot.

use crate::ingest::{self, Packet};
use crate::session::Session;
use crate::snapshot::Snapshot;
use anyhow::{Context, Result};
use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};
use triple_buffer::{triple_buffer, Output};

#[derive(Clone, Debug)]
pub enum Source {
    Multicast { group: Ipv4Addr, port: u16, iface: Ipv4Addr },
    Capture { path: PathBuf },
}

#[derive(Clone, Debug)]
pub struct Config {
    pub source: Source,
    /// Packets the ingest ring can hold (power of two recommended).
    pub ring_capacity: usize,
    /// Levels per side in each snapshot.
    pub depth: usize,
    /// How often the book thread publishes a snapshot.
    pub snapshot_interval: Duration,
}

impl Default for Config {
    fn default() -> Self {
        Config {
            source: Source::Multicast {
                group: Ipv4Addr::new(239, 1, 1, 1),
                port: 30001,
                iface: Ipv4Addr::UNSPECIFIED,
            },
            ring_capacity: 4096,
            depth: 20,
            snapshot_interval: Duration::from_millis(16),
        }
    }
}

pub struct FeedClient {
    out: Output<Snapshot>,
    stop: Arc<AtomicBool>,
    threads: Vec<JoinHandle<()>>,
    pub ingest_drops: Arc<AtomicU64>,
    pub finished: Arc<AtomicBool>,
}

impl FeedClient {
    pub fn start(cfg: Config) -> Result<Self> {
        let (tx, mut rx) = rtrb::RingBuffer::<Packet>::new(cfg.ring_capacity);
        let (mut snap_in, snap_out) = triple_buffer(&Snapshot::default());
        let stop = Arc::new(AtomicBool::new(false));
        let drops = Arc::new(AtomicU64::new(0));
        let finished = Arc::new(AtomicBool::new(false));
        let source_done = Arc::new(AtomicBool::new(false));
        let mut threads = Vec::new();

        // Ingest thread.
        {
            let stop = stop.clone();
            let drops = drops.clone();
            let source_done = source_done.clone();
            let src = cfg.source.clone();
            let sock = match &src {
                Source::Multicast { group, port, iface } => Some(
                    ingest::join_multicast(*group, *port, *iface)
                        .with_context(|| format!("joining {group}:{port} on {iface}"))?,
                ),
                Source::Capture { .. } => None,
            };
            threads.push(
                thread::Builder::new()
                    .name("feed-ingest".into())
                    .spawn(move || {
                        match src {
                            Source::Multicast { .. } => ingest::run_socket(sock.unwrap(), tx, &stop, &drops),
                            Source::Capture { path } => {
                                if let Err(e) = ingest::run_capture(&path, tx, &stop) {
                                    eprintln!("feed-ingest: capture error: {e}");
                                }
                            }
                        }
                        source_done.store(true, Ordering::Release);
                    })
                    .context("spawning ingest thread")?,
            );
        }

        // Book thread.
        {
            let stop = stop.clone();
            let drops = drops.clone();
            let finished = finished.clone();
            let depth = cfg.depth;
            let interval = cfg.snapshot_interval;
            let capacity = cfg.ring_capacity as f32;
            threads.push(
                thread::Builder::new()
                    .name("feed-book".into())
                    .spawn(move || {
                        let mut session = Session::new();
                        let mut revision = 0u64;
                        let mut last_snap = Instant::now();
                        let mut last_rate = Instant::now();
                        let mut msgs_at_rate = 0u64;
                        let mut rate = 0u64;
                        let mut rate_hist: std::collections::VecDeque<u64> = std::collections::VecDeque::new();
                        let mut p99_hist: std::collections::VecDeque<u64> = std::collections::VecDeque::new();
                        let mut lat_window: Vec<u64> = Vec::new(); // this second's samples
                        let mut lat_pct = (0u64, 0u64);             // last full second
                        loop {
                            let mut worked = false;
                            for _ in 0..256 {
                                match rx.pop() {
                                    Ok(p) => {
                                        session.on_packet(p.bytes(), p.recv_ns);
                                        worked = true;
                                    }
                                    Err(_) => break,
                                }
                            }
                            let now = Instant::now();
                            // Move this tick's latency samples into the one-second window.
                            if !session.stats.latency_ns.is_empty() {
                                lat_window.append(&mut session.stats.latency_ns);
                                if lat_window.len() > 200_000 {
                                    lat_window.drain(..lat_window.len() - 200_000);
                                }
                            }
                            if now - last_rate >= Duration::from_secs(1) {
                                rate = session.stats.messages - msgs_at_rate;
                                msgs_at_rate = session.stats.messages;
                                last_rate = now;
                                if rate_hist.len() == 120 { rate_hist.pop_front(); }
                                rate_hist.push_back(rate);
                                lat_pct = crate::snapshot::percentiles(&mut lat_window);
                                lat_window.clear();
                                if p99_hist.len() == 120 { p99_hist.pop_front(); }
                                p99_hist.push_back(lat_pct.1);
                            }
                            let src_done = source_done.load(Ordering::Acquire) && rx.is_empty();
                            if now - last_snap >= interval || src_done {
                                revision += 1;
                                let mut s = Snapshot::build(&mut session, depth, revision, lat_pct);
                                s.rate_hist = rate_hist.iter().copied().collect();
                                s.p99_hist = p99_hist.iter().copied().collect();
                                s.msgs_per_sec = rate;
                                s.ring_occupancy = rx.slots() as f32 / capacity;
                                s.ring_drops = drops.load(Ordering::Relaxed);
                                s.finished = src_done;
                                snap_in.write(s);
                                last_snap = now;
                            }
                            if src_done {
                                finished.store(true, Ordering::Release);
                                break;
                            }
                            if stop.load(Ordering::Relaxed) {
                                break;
                            }
                            if !worked {
                                thread::sleep(Duration::from_micros(200));
                            }
                        }
                    })
                    .context("spawning book thread")?,
            );
        }

        Ok(FeedClient { out: snap_out, stop, threads, ingest_drops: drops, finished })
    }

    /// The latest snapshot. Cheap; call it once per frame.
    pub fn latest(&mut self) -> &Snapshot {
        self.out.read()
    }

    pub fn is_finished(&self) -> bool {
        self.finished.load(Ordering::Acquire)
    }

    pub fn stop(mut self) {
        self.stop.store(true, Ordering::Release);
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
    }
}

impl Drop for FeedClient {
    fn drop(&mut self) {
        self.stop.store(true, Ordering::Release);
        for t in self.threads.drain(..) {
            let _ = t.join();
        }
    }
}
