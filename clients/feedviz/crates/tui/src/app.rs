//! UI state and key handling. The app never touches messages: it reads the
//! latest Snapshot once per frame and keeps its own selection state.

use crate::picker::{Item, Picker, PickerKind};
use feed_client::{EventRecord, FeedClient, Snapshot};
use std::collections::{HashMap, VecDeque};
use std::time::{Duration, Instant};

/// How long a changed cell stays highlighted.
pub const FLASH: Duration = Duration::from_millis(600);
pub const GROUPS: [u64; 4] = [1, 2, 5, 10];

pub const DEPTHS: [usize; 4] = [10, 14, 20, 40];

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Screen {
    Market,
    Events,
}

/// Bit per message type in the EVENTS filter: A E X D U.
pub const TYPE_KEYS: [u8; 5] = [b'A', b'E', b'X', b'D', b'U'];

pub struct App {
    pub client: FeedClient,
    /// Latest snapshot (EVENTS screen, inspector): refreshed every frame.
    pub snap: Snapshot,
    /// Conflated copy for the ladder and tape: 8 Hz.
    pub fast: Snapshot,
    /// Conflated copy for indicators, flow and monitor: 1 Hz.
    pub slow: Snapshot,
    last_fast: Instant,
    last_slow: Instant,
    /// (symbol, side, price) -> (qty when last seen, when it changed).
    pub flash: HashMap<(u32, u8, u64), (u64, Instant)>,
    /// Last-trade price per symbol, for monitor flashes.
    pub last_px: HashMap<u32, (u64, Instant, bool)>,
    /// Ladder anchor price per symbol (centre row); None = follow mid.
    pub anchor: HashMap<u32, u64>,
    pub group_idx: usize,
    pub screen: Screen,
    pub interval_idx: usize,
    pub type_mask: u8,
    pub follow: Option<u64>,
    /// Fuzzy picker overlay (themes on T, symbols on /).
    pub picker: Option<Picker>,
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
            fast: Snapshot::default(),
            slow: Snapshot::default(),
            last_fast: Instant::now() - Duration::from_secs(1),
            last_slow: Instant::now() - Duration::from_secs(1),
            flash: HashMap::new(),
            last_px: HashMap::new(),
            anchor: HashMap::new(),
            group_idx: 0,
            screen: Screen::Market,
            interval_idx: 1, // 5s candles by default; 1s is noise live
            type_mask: 0b11111,
            follow: None,
            picker: None,
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

    pub fn group(&self) -> u64 {
        GROUPS[self.group_idx]
    }

    /// Pulls the newest snapshot unless paused, and refreshes the
    /// conflated copies on their own cadences. Rendering cost follows
    /// frame rate; what the eye sees follows these cadences.
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
        let now = Instant::now();
        if now - self.last_fast >= Duration::from_millis(125) {
            self.last_fast = now;
            self.note_changes(now);
            self.fast = self.snap.clone();
        }
        if now - self.last_slow >= Duration::from_secs(1) {
            self.last_slow = now;
            self.slow = self.snap.clone();
        }
    }

    /// Records which ladder levels and last prices changed since the
    /// previous fast refresh, so the renderer can flash them.
    fn note_changes(&mut self, now: Instant) {
        let Some(sym) = self.snap.symbols.get(self.sym_idx) else { return };
        let sid = sym.symbol_id;
        for (side, levels) in [(b'B', &sym.bids), (b'S', &sym.asks)] {
            for l in levels {
                let key = (sid, side, l.price);
                match self.flash.get_mut(&key) {
                    Some((q, t)) => {
                        if *q != l.qty {
                            *q = l.qty;
                            *t = now;
                        }
                    }
                    None => {
                        self.flash.insert(key, (l.qty, now));
                    }
                }
            }
        }
        if self.flash.len() > 4096 {
            self.flash.retain(|_, (_, t)| now - *t < FLASH);
        }
        for s in &self.snap.symbols {
            if let Some(t) = s.last_trade {
                let e = self.last_px.entry(s.symbol_id).or_insert((t.price, now - FLASH, t.aggressor_buy));
                if e.0 != t.price {
                    *e = (t.price, now, t.aggressor_buy);
                }
            }
        }
    }

    /// 0.0 (fresh) .. 1.0 (faded) for a ladder cell, or None if unchanged.
    pub fn flash_age(&self, sym: u32, side: u8, price: u64) -> Option<f32> {
        let (_, t) = self.flash.get(&(sym, side, price))?;
        let age = t.elapsed();
        if age >= FLASH {
            None
        } else {
            Some(age.as_secs_f32() / FLASH.as_secs_f32())
        }
    }

    pub fn last_px_flash(&self, sym: u32) -> Option<(bool, f32)> {
        let (_, t, buy) = self.last_px.get(&sym)?;
        let age = t.elapsed();
        if age >= FLASH {
            None
        } else {
            Some((*buy, age.as_secs_f32() / FLASH.as_secs_f32()))
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

    pub fn type_on(&self, ty: u8) -> bool {
        match TYPE_KEYS.iter().position(|t| *t == ty) {
            Some(i) => self.type_mask & (1 << i) != 0,
            None => true,
        }
    }

    /// Events shown in the log after the symbol, type and follow filters.
    /// Gap rows always show.
    pub fn visible_events(&self) -> Vec<&EventRecord> {
        let sym = self.current_symbol().map(|s| s.symbol_id);
        self.snap
            .events
            .iter()
            .filter(|e| e.ty != b'R')
            .filter(|e| {
                e.is_gap()
                    || match self.follow {
                        Some(id) => e.order_id == id,
                        None => (!self.filter_sym || Some(e.symbol_id) == sym) && self.type_on(e.ty),
                    }
            })
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

    pub fn open_theme_picker(&mut self) {
        let items = crate::theme::THEMES
            .iter()
            .enumerate()
            .map(|(i, t)| Item { label: t.name.to_string(), detail: crate::theme::blurb(t.name).to_string(), key: i })
            .collect();
        let cur = crate::theme::index();
        self.picker = Some(Picker::new(PickerKind::Theme, items, cur, cur));
    }

    pub fn open_symbol_picker(&mut self) {
        let items = self
            .snap
            .symbols
            .iter()
            .enumerate()
            .map(|(i, s)| {
                let st = &s.session;
                let chg = st.last as i64 - st.open as i64;
                Item {
                    label: if s.ticker.is_empty() { format!("#{}", s.symbol_id) } else { s.ticker.clone() },
                    detail: format!("last {}  chg {:+}  vol {}  {} msgs/s", st.last, chg, s.volume, s.msgs_per_sec),
                    key: i,
                }
            })
            .collect();
        self.picker = Some(Picker::new(PickerKind::Symbol, items, self.sym_idx, self.sym_idx));
    }

    fn on_picker_key(&mut self, key: crossterm::event::KeyEvent) {
        use crossterm::event::{KeyCode, KeyModifiers};
        let Some(p) = self.picker.as_mut() else { return };
        let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
        match key.code {
            KeyCode::Esc => {
                if p.kind == PickerKind::Theme {
                    crate::theme::set(p.restore);
                }
                self.picker = None;
            }
            KeyCode::Enter => {
                if let Some(it) = p.current() {
                    match p.kind {
                        PickerKind::Theme => crate::theme::set(it.key),
                        PickerKind::Symbol => {
                            self.sym_idx = it.key;
                            self.selected = None;
                        }
                    }
                }
                self.picker = None;
            }
            KeyCode::Up => p.up(),
            KeyCode::Down | KeyCode::Tab => p.down(),
            KeyCode::Char('p') if ctrl => p.up(),
            KeyCode::Char('n') | KeyCode::Char('j') if ctrl => p.down(),
            KeyCode::Char('k') if ctrl => p.up(),
            KeyCode::Char('u') if ctrl => {
                p.query.clear();
                p.selected = 0;
                p.refilter();
            }
            KeyCode::Backspace => p.pop(),
            KeyCode::Char(c) if !ctrl => p.push(c),
            _ => {}
        }
        // Live preview for themes.
        if let Some(p) = &self.picker {
            if p.kind == PickerKind::Theme {
                if let Some(it) = p.current() {
                    crate::theme::set(it.key);
                }
            }
        }
    }

    pub fn on_key(&mut self, key: crossterm::event::KeyEvent) {
        use crossterm::event::{KeyCode, KeyModifiers};
        if key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c') {
            self.quit = true;
            return;
        }
        if self.picker.is_some() {
            self.on_picker_key(key);
            return;
        }
        if key.code == KeyCode::Char('T') {
            self.open_theme_picker();
            return;
        }
        if key.code == KeyCode::Char('/') {
            self.open_symbol_picker();
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
            KeyCode::Char('m') => self.screen = Screen::Market,
            KeyCode::Char('t') => crate::theme::next(),
            KeyCode::Char('r') => self.client.request_snapshot(),
            KeyCode::Char('e') => self.screen = Screen::Events,
            KeyCode::Char('i') => self.interval_idx = (self.interval_idx + 1) % feed_client::INTERVALS.len(),
            KeyCode::Char(c @ '1'..='5') => {
                let i = c as u8 - b'1';
                self.type_mask ^= 1 << i;
                if self.type_mask == 0 {
                    self.type_mask = 0b11111;
                }
            }
            KeyCode::Char('o') => {
                self.follow = match (self.follow, self.selected_event()) {
                    (Some(_), _) => None,
                    (None, Some(e)) if e.order_id != 0 => Some(e.order_id),
                    _ => None,
                };
            }
            KeyCode::Tab => {
                self.screen = match self.screen {
                    Screen::Market => Screen::Events,
                    Screen::Events => Screen::Market,
                };
            }
            KeyCode::Char(']') => {
                if !self.snap.symbols.is_empty() {
                    self.sym_idx = (self.sym_idx + 1) % self.snap.symbols.len();
                    self.selected = None;
                }
            }
            KeyCode::Char('[') => {
                if !self.snap.symbols.is_empty() {
                    self.sym_idx = (self.sym_idx + self.snap.symbols.len() - 1) % self.snap.symbols.len();
                    self.selected = None;
                }
            }
            KeyCode::Char('d') => self.depth_idx = (self.depth_idx + 1) % DEPTHS.len(),
            KeyCode::Char('g') => self.group_idx = (self.group_idx + 1) % GROUPS.len(),
            KeyCode::Char('c') => {
                // Re-centre the ladder on the current mid.
                if let Some(id) = self.current_symbol().map(|s| s.symbol_id) {
                    self.anchor.remove(&id);
                }
            }
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
                self.follow = None;
                self.paused = false;
            }
            _ => {}
        }
    }
}
