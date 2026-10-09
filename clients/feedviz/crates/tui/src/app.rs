//! UI state and key handling. The app never touches messages: it reads the
//! latest Snapshot once per frame and keeps its own selection state.

use feed_client::{EventRecord, FeedClient, Snapshot};
use std::collections::VecDeque;
use std::time::Instant;

pub const DEPTHS: [usize; 4] = [10, 14, 20, 40];

pub struct App {
    pub client: FeedClient,
    pub snap: Snapshot,
    pub paused: bool,
    pub sym_idx: usize,
    pub depth_idx: usize,
    pub filter_sym: bool,
    /// Mold seq of the selected event-log row, if any.
    pub selected: Option<u64>,
    pub inspect: bool,
    pub frame_us: VecDeque<u64>,
    pub frame_hist: VecDeque<u64>,
    pub last_frame_tick: Instant,
    pub quit: bool,
    pub replay: bool,
    pub started: Instant,
}

impl App {
    pub fn new(client: FeedClient, replay: bool) -> Self {
        App {
            client,
            snap: Snapshot::default(),
            paused: false,
            sym_idx: 0,
            depth_idx: 1,
            filter_sym: true,
            selected: None,
            inspect: false,
            frame_us: VecDeque::with_capacity(256),
            frame_hist: VecDeque::with_capacity(64),
            last_frame_tick: Instant::now(),
            quit: false,
            replay,
            started: Instant::now(),
        }
    }

    pub fn depth(&self) -> usize {
        DEPTHS[self.depth_idx]
    }

    /// Pulls the newest snapshot unless paused.
    pub fn tick(&mut self) {
        if self.paused {
            return;
        }
        let latest = self.client.latest();
        if latest.revision != self.snap.revision {
            self.snap = latest.clone();
        }
        if self.sym_idx >= self.snap.symbols.len() {
            self.sym_idx = 0;
        }
    }

    pub fn record_frame(&mut self, us: u64) {
        if self.frame_us.len() == 256 {
            self.frame_us.pop_front();
        }
        self.frame_us.push_back(us);
        if self.last_frame_tick.elapsed().as_millis() >= 250 {
            self.last_frame_tick = Instant::now();
            let worst = self.frame_us.iter().rev().take(8).copied().max().unwrap_or(0);
            if self.frame_hist.len() == 64 {
                self.frame_hist.pop_front();
            }
            self.frame_hist.push_back(worst);
        }
    }

    pub fn frame_percentiles(&self) -> (u64, u64) {
        if self.frame_us.is_empty() {
            return (0, 0);
        }
        let mut v: Vec<u64> = self.frame_us.iter().copied().collect();
        v.sort_unstable();
        let at = |q: f64| v[((v.len() - 1) as f64 * q) as usize];
        (at(0.5), at(0.99))
    }

    pub fn current_symbol(&self) -> Option<&feed_client::SymbolSnapshot> {
        self.snap.symbols.get(self.sym_idx)
    }

    /// Events shown in the log: all, or the current symbol's plus gaps.
    pub fn visible_events(&self) -> Vec<&EventRecord> {
        let sym = self.current_symbol().map(|s| s.symbol_id);
        self.snap
            .events
            .iter()
            .filter(|e| !self.filter_sym || e.is_gap() || Some(e.symbol_id) == sym)
            .filter(|e| e.ty != b'R')
            .collect()
    }

    pub fn selected_event(&self) -> Option<EventRecord> {
        let seq = self.selected?;
        self.snap.events.iter().find(|e| e.seq == seq && !e.is_gap()).copied()
    }

    fn move_selection(&mut self, delta: i64) {
        let vis = self.visible_events();
        if vis.is_empty() {
            return;
        }
        let cur = match self.selected {
            Some(seq) => vis.iter().position(|e| e.seq == seq).unwrap_or(vis.len() - 1) as i64,
            None => vis.len() as i64 - 1,
        };
        let next = (cur + delta).clamp(0, vis.len() as i64 - 1) as usize;
        self.selected = Some(vis[next].seq);
        if self.selected.is_some() && !self.paused && delta != 0 {
            // Moving through history implies you want it to hold still.
            self.paused = true;
        }
    }

    /// Steps to the previous/next event for the same order as the selection.
    fn move_same_order(&mut self, delta: i64) {
        let Some(cur) = self.selected_event() else { return };
        let same: Vec<&EventRecord> =
            self.snap.events.iter().filter(|e| !e.is_gap() && e.order_id == cur.order_id && e.order_id != 0).collect();
        let Some(i) = same.iter().position(|e| e.seq == cur.seq) else { return };
        let next = (i as i64 + delta).clamp(0, same.len() as i64 - 1) as usize;
        self.selected = Some(same[next].seq);
    }

    /// Jumps to the first event of the packet the selection arrived in.
    fn jump_packet(&mut self) {
        let Some(cur) = self.selected_event() else { return };
        if let Some(e) = self.snap.events.iter().find(|e| !e.is_gap() && e.packet_seq == cur.packet_seq) {
            self.selected = Some(e.seq);
        }
    }

    pub fn on_key(&mut self, key: crossterm::event::KeyEvent) {
        use crossterm::event::{KeyCode, KeyModifiers};
        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            self.quit = true;
            return;
        }
        if self.inspect {
            match key.code {
                KeyCode::Esc | KeyCode::Char('q') => self.inspect = false,
                KeyCode::Up | KeyCode::Char('k') => self.move_selection(-1),
                KeyCode::Down | KeyCode::Char('j') => self.move_selection(1),
                KeyCode::Char('p') => self.move_same_order(-1),
                KeyCode::Char('n') => self.move_same_order(1),
                KeyCode::Char('K') => self.jump_packet(),
                _ => {}
            }
            return;
        }
        match key.code {
            KeyCode::Char('q') => self.quit = true,
            KeyCode::Char(' ') => self.paused = !self.paused,
            KeyCode::Char(']') | KeyCode::Tab => {
                if !self.snap.symbols.is_empty() {
                    self.sym_idx = (self.sym_idx + 1) % self.snap.symbols.len();
                    self.selected = None;
                }
            }
            KeyCode::Char('[') | KeyCode::BackTab => {
                if !self.snap.symbols.is_empty() {
                    self.sym_idx = (self.sym_idx + self.snap.symbols.len() - 1) % self.snap.symbols.len();
                    self.selected = None;
                }
            }
            KeyCode::Char('d') => self.depth_idx = (self.depth_idx + 1) % DEPTHS.len(),
            KeyCode::Char('f') => self.filter_sym = !self.filter_sym,
            KeyCode::Up | KeyCode::Char('k') => self.move_selection(-1),
            KeyCode::Down | KeyCode::Char('j') => self.move_selection(1),
            KeyCode::PageUp => self.move_selection(-20),
            KeyCode::PageDown => self.move_selection(20),
            KeyCode::Enter => {
                if self.selected.is_none() {
                    self.move_selection(0);
                    self.paused = true;
                }
                self.inspect = self.selected_event().is_some();
            }
            KeyCode::Esc => {
                self.selected = None;
                self.paused = false;
            }
            _ => {}
        }
    }
}
