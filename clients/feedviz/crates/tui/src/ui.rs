//! Main screen: header, ladder, candles + volume, event log, tape, feed
//! health, compact inspector, footer. Everything is drawn from `app.snap`.

use crate::app::App;
use crate::decode;
use crate::fmt;
use crate::theme::{self, *};
use feed_client::{EventRecord, SymbolSnapshot};
use ratatui::buffer::Buffer;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Paragraph, Sparkline};
use ratatui::Frame;

pub fn draw(f: &mut Frame, app: &App) {
    match app.screen {
        crate::app::Screen::Market => crate::market::draw(f, app),
        crate::app::Screen::Events => crate::events::draw(f, app),
    }
}

/// The original single-screen layout, kept for the render test and as a
/// fallback for very small terminals.
#[allow(dead_code)]
pub fn draw_classic(f: &mut Frame, app: &App) {
    let area = f.area();
    f.render_widget(Block::default().style(theme::base()), area);
    let [hdr, body, foot] =
        Layout::vertical([Constraint::Length(1), Constraint::Min(0), Constraint::Length(1)]).areas(area);
    draw_header(f, app, hdr);
    draw_footer(f, foot);

    let left_w = 52u16.min(body.width / 3);
    let right_w = 58u16.min(body.width / 3);
    let [left, centre, right] = Layout::horizontal([
        Constraint::Length(left_w),
        Constraint::Min(20),
        Constraint::Length(right_w),
    ])
    .areas(body);

    draw_ladder(f, app, left);

    let chart_h = (centre.height / 3).clamp(8, 22);
    let [chart, vol, log] =
        Layout::vertical([Constraint::Length(chart_h), Constraint::Length(4), Constraint::Min(5)]).areas(centre);
    draw_candles(f, app, chart);
    draw_volume(f, app, vol);
    draw_event_log(f, app, log);

    let tape_h = (right.height / 4).clamp(6, 14);
    let health_h = 13u16;
    let [tape, health, insp] =
        Layout::vertical([Constraint::Length(tape_h), Constraint::Length(health_h), Constraint::Min(6)]).areas(right);
    draw_tape(f, app, tape);
    draw_health(f, app, health);
    draw_inspector_panel(f, app, insp);
}

// ------------------------------------------------------------ helpers

pub fn panel(f: &mut Frame, area: Rect, title: &str, sub: &str) -> Rect {
    let block = Block::default().borders(Borders::ALL).border_style(fg(BORDER));
    let inner = block.inner(area);
    f.render_widget(block, area);
    // Title strip inside the border.
    let strip = Rect { x: inner.x, y: inner.y, width: inner.width, height: 1 };
    let mut spans = vec![Span::styled(format!(" {title}"), panel_title())];
    let used = title.chars().count() + 1;
    let room = strip.width as usize;
    if used + sub.chars().count() + 2 <= room {
        let pad = room - used - sub.chars().count() - 1;
        spans.push(Span::styled(" ".repeat(pad), panel_sub()));
        spans.push(Span::styled(format!("{sub} "), panel_sub()));
    } else {
        let pad = room.saturating_sub(used);
        spans.push(Span::styled(" ".repeat(pad), panel_sub()));
    }
    f.render_widget(Paragraph::new(Line::from(spans)), strip);
    Rect { x: inner.x, y: inner.y + 1, width: inner.width, height: inner.height.saturating_sub(1) }
}

pub fn pad_left(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w { s.chars().take(w).collect() } else { format!("{}{}", " ".repeat(w - n), s) }
}
pub fn pad_right(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w { s.chars().take(w).collect() } else { format!("{}{}", s, " ".repeat(w - n)) }
}
pub fn center(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w {
        return s.chars().take(w).collect();
    }
    let l = (w - n) / 2;
    format!("{}{}{}", " ".repeat(l), s, " ".repeat(w - n - l))
}

pub fn sym_name(s: &SymbolSnapshot) -> String {
    if s.ticker.is_empty() { format!("#{}", s.symbol_id) } else { s.ticker.clone() }
}

// ------------------------------------------------------------ header / footer

pub fn draw_header(f: &mut Frame, app: &App, area: Rect) {
    let s = &app.snap;
    let mut spans = vec![Span::styled(" FEEDVIZ ", bold(AMBER_BRIGHT).bg(HDR_BG)), Span::styled(" ", Style::default().bg(HDR_BG))];
    for (name, sc) in [("MARKET", crate::app::Screen::Market), ("EVENTS", crate::app::Screen::Events)] {
        if app.screen == sc {
            spans.push(Span::styled(format!(" {name} "), Style::default().fg(HDR_BG).bg(AMBER).add_modifier(ratatui::style::Modifier::BOLD)));
        } else {
            spans.push(Span::styled(format!(" {name} "), Style::default().fg(AMBER).bg(HDR_BG)));
        }
    }
    spans.push(Span::styled("  ", Style::default().bg(HDR_BG)));
    for (i, sym) in s.symbols.iter().enumerate() {
        let name = format!(" {} ", sym_name(sym));
        if i == app.sym_idx {
            spans.push(Span::styled(name, Style::default().fg(PAUSE_FG).bg(PAUSE_BG).add_modifier(ratatui::style::Modifier::BOLD)));
        } else {
            spans.push(Span::styled(name, Style::default().fg(AMBER).bg(HDR_BG)));
        }
        spans.push(Span::styled(" ", Style::default().bg(HDR_BG)));
    }
    let kv = |k: &str, v: String, c: Color| {
        vec![Span::styled(format!("  {k} "), Style::default().fg(AMBER_DIM).bg(HDR_BG)), Span::styled(v, Style::default().fg(c).bg(HDR_BG))]
    };
    let session = String::from_utf8_lossy(&s.stats.session).trim().to_string();
    let wide = area.width >= 170;
    if wide {
        spans.extend(kv("SESSION", if session.is_empty() { "-".into() } else { session }, AMBER_TEXT));
    }
    spans.extend(kv("SEQ", fmt::commas(s.stats.next_seq), AMBER_TEXT));
    spans.extend(kv("MSGS/S", fmt::commas(s.msgs_per_sec), AMBER_TEXT));
    spans.extend(kv("P99", fmt::micros(s.latency_p99_ns), AMBER_TEXT));
    spans.extend(kv("GAPS", s.stats.gaps.to_string(), if s.stats.gaps > 0 { RED } else { AMBER_TEXT }));
    if wide {
        let shards = s.symbols.iter().map(|x| fmt::shard_of(x.book_seq)).max().map(|m| m as usize + 1).unwrap_or(1);
        spans.extend(kv("SHARDS", shards.to_string(), AMBER_TEXT));
    }
    let clock = s.events.last().map(|e| fmt::time_ms(e.ts_ns)).unwrap_or_else(|| "--:--:--.---".into());
    let state = if app.paused {
        Span::styled(" ❚❚ PAUSED ", bold(PAUSE_FG).bg(PAUSE_BG))
    } else if app.replay && s.finished {
        Span::styled(" ■ ENDED ", bold(PAUSE_FG).bg(PAUSE_BG))
    } else if app.replay {
        Span::styled(" ▶ REPLAY ", bold(LIVE_FG).bg(LIVE_BG))
    } else {
        Span::styled(" ● LIVE ", bold(LIVE_FG).bg(LIVE_BG))
    };
    let right = vec![Span::styled(format!("{clock}  "), Style::default().fg(AMBER_TEXT).bg(HDR_BG)), state, Span::styled(" ", Style::default().bg(HDR_BG))];
    let left_len: usize = spans.iter().map(|x| x.content.chars().count()).sum();
    let right_len: usize = right.iter().map(|x| x.content.chars().count()).sum();
    let pad = (area.width as usize).saturating_sub(left_len + right_len).max(2);
    spans.push(Span::styled(" ".repeat(pad), Style::default().bg(HDR_BG)));
    spans.extend(right);
    f.render_widget(Paragraph::new(Line::from(spans)).style(Style::default().bg(HDR_BG)), area);
}

pub fn draw_footer_keys(f: &mut Frame, area: Rect, keys: &[(&str, &str)]) {
    let mut spans = vec![Span::styled(" ", Style::default().bg(PANEL_HDR_BG))];
    for (k, v) in keys {
        spans.push(Span::styled(k.to_string(), bold(AMBER).bg(PANEL_HDR_BG)));
        spans.push(Span::styled(format!(" {v}   "), Style::default().fg(DIM).bg(PANEL_HDR_BG)));
    }
    let tail = "feedviz 0.2 · feed v1 · ratatui ";
    let used: usize = spans.iter().map(|x| x.content.chars().count()).sum();
    let pad = (area.width as usize).saturating_sub(used + tail.len());
    spans.push(Span::styled(" ".repeat(pad), Style::default().bg(PANEL_HDR_BG)));
    spans.push(Span::styled(tail, Style::default().fg(DIMMER).bg(PANEL_HDR_BG)));
    f.render_widget(Paragraph::new(Line::from(spans)).style(Style::default().bg(PANEL_HDR_BG)), area);
}

fn draw_footer(f: &mut Frame, area: Rect) {
    draw_footer_keys(f, area, &[("q", "quit"), ("space", "pause"), ("[ ]", "symbol"), ("d", "depth"), ("f", "filter"), ("↑↓", "select"), ("Enter", "inspect"), ("Esc", "follow")]);
}

// ------------------------------------------------------------ ladder

pub fn draw_ladder(f: &mut Frame, app: &App, area: Rect) {
    let Some(sym) = app.current_symbol() else {
        let inner = panel(f, area, "L2 BOOK", "");
        f.render_widget(Paragraph::new("waiting for feed…").style(dim()), inner);
        return;
    };
    let depth = app.depth();
    let inner = panel(f, area, &format!("L2 BOOK · {}", sym_name(sym)), &format!("depth {depth} · cum ▮"));
    let w = inner.width as usize;
    // columns: ord(3) sp qty(8) sp bar(b) sp price(9) sp bar(b) sp qty(8) sp ord(3)
    let fixed = 3 + 1 + 8 + 1 + 1 + 9 + 1 + 1 + 8 + 1 + 3;
    let barw = w.saturating_sub(fixed) / 2;
    let mut lines: Vec<Line> = Vec::new();
    lines.push(Line::from(vec![Span::styled(
        format!("{} {} {} {} {} {} {}", pad_left("ORD", 3), pad_left("BID", 8), " ".repeat(barw), center("PRICE", 9), " ".repeat(barw), pad_right("ASK", 8), pad_right("ORD", 3)),
        dim(),
    )]));
    let rows_avail = inner.height.saturating_sub(2) as usize; // header + mid line
    let per_side = (rows_avail / 2).min(depth);
    let asks: Vec<_> = sym.asks.iter().take(per_side).collect();
    let bids: Vec<_> = sym.bids.iter().take(per_side).collect();
    let max_cum = asks.last().map(|l| l.cum).unwrap_or(0).max(bids.last().map(|l| l.cum).unwrap_or(0)).max(1);
    // Asks: worst at top, best just above the mid line.
    for l in asks.iter().rev() {
        let bw = ((l.cum as f64 / max_cum as f64) * barw as f64).round() as usize;
        lines.push(Line::from(vec![
            Span::raw(format!("{} {} {} ", " ".repeat(3), " ".repeat(8), " ".repeat(barw))),
            Span::styled(center(&fmt::commas(l.price), 9), fg(ASK)),
            Span::raw(" "),
            Span::styled(" ".repeat(bw.min(barw)), Style::default().bg(ASK_BAR)),
            Span::raw(" ".repeat(barw.saturating_sub(bw))),
            Span::raw(" "),
            Span::styled(pad_right(&fmt::commas(l.qty), 8), fg(ASK_TEXT)),
            Span::raw(" "),
            Span::styled(pad_right(&l.count.to_string(), 3), dim()),
        ]));
    }
    // Mid line.
    let mid = match (sym.best_bid(), sym.best_ask()) {
        (Some(b), Some(a)) => {
            let bid_cum = bids.last().map(|l| l.cum).unwrap_or(0) as f64;
            let ask_cum = asks.last().map(|l| l.cum).unwrap_or(0) as f64;
            let imb = if bid_cum + ask_cum > 0.0 { (bid_cum - ask_cum) / (bid_cum + ask_cum) * 100.0 } else { 0.0 };
            let midpx = (a.price + b.price) as f64 / 2.0;
            Line::from(vec![
                Span::styled(" MID ", dim()),
                Span::styled(format!("{midpx:.1}"), bold(AMBER)),
                Span::styled("  SPREAD ", dim()),
                Span::styled(fmt::commas(a.price - b.price), amber()),
                Span::styled("  IMB ", dim()),
                Span::styled(format!("{imb:+.0}%"), fg(if imb >= 0.0 { BID } else { ASK })),
            ])
        }
        _ => Line::from(Span::styled(" MID  —", dim())),
    };
    lines.push(mid);
    for l in bids.iter() {
        let bw = ((l.cum as f64 / max_cum as f64) * barw as f64).round() as usize;
        lines.push(Line::from(vec![
            Span::styled(pad_left(&l.count.to_string(), 3), dim()),
            Span::raw(" "),
            Span::styled(pad_left(&fmt::commas(l.qty), 8), fg(BID_TEXT)),
            Span::raw(" "),
            Span::raw(" ".repeat(barw.saturating_sub(bw))),
            Span::styled(" ".repeat(bw.min(barw)), Style::default().bg(BID_BAR)),
            Span::raw(" "),
            Span::styled(center(&fmt::commas(l.price), 9), fg(BID)),
        ]));
    }
    // Footer line inside the panel if room remains.
    let footer = Line::from(vec![
        Span::styled(" LAST ", dim()),
        Span::styled(
            sym.last_trade.map(|t| format!("{} × {}", fmt::commas(t.price), fmt::commas(t.qty as u64))).unwrap_or_else(|| "—".into()),
            fg(sym.last_trade.map(|t| if t.aggressor_buy { BID } else { ASK }).unwrap_or(DIM)),
        ),
        Span::styled("  VOL ", dim()),
        Span::styled(fmt::commas(sym.volume), fg(TEXT)),
        Span::styled("  ORD ", dim()),
        Span::styled(fmt::commas(sym.orders as u64), fg(TEXT)),
        Span::styled("  LVL ", dim()),
        Span::styled(format!("{}/{}", sym.bid_levels, sym.ask_levels), fg(TEXT)),
    ]);
    while lines.len() + 1 < inner.height as usize {
        lines.push(Line::raw(""));
    }
    lines.truncate(inner.height.saturating_sub(1) as usize);
    lines.push(footer);
    f.render_widget(Paragraph::new(lines), inner);
}

// ------------------------------------------------------------ candles

pub fn interval_label(i: usize) -> String {
    let s = feed_client::INTERVALS[i];
    if s >= 60 { format!("{}m", s / 60) } else { format!("{s}s") }
}

pub fn draw_candles(f: &mut Frame, app: &App, area: Rect) {
    let Some(sym) = app.current_symbol() else { return };
    let bars_all = &sym.bars[app.interval_idx];
    let last = bars_all.last();
    let vwap = sym.session.vwap();
    let sub = last
        .map(|b| format!("O {} H {} L {} C {} · {}{} · VWAP {:.1}", b.open, b.high, b.low, b.close, if b.close >= b.open { "▲ +" } else { "▼ -" }, b.close.abs_diff(b.open), vwap))
        .unwrap_or_default();
    let ivs: String = (0..feed_client::INTERVALS.len()).map(|i| if i == app.interval_idx { format!("[{}]", interval_label(i)) } else { format!(" {} ", interval_label(i)) }).collect();
    let inner = panel(f, area, &format!("{} · CANDLES {}", sym_name(sym), ivs), &sub);
    if inner.height < 3 || inner.width < 12 {
        return;
    }
    let axis_w = 8u16;
    let plot = Rect { x: inner.x, y: inner.y, width: inner.width - axis_w, height: inner.height };
    let n = (plot.width as usize / 2).max(1);
    let bars: Vec<_> = bars_all.iter().rev().take(n).rev().collect();
    if bars.is_empty() {
        f.render_widget(Paragraph::new("no trades yet").style(dim()), plot);
        return;
    }
    let lo = bars.iter().map(|b| b.low).min().unwrap();
    let hi = bars.iter().map(|b| b.high).max().unwrap();
    let (lo, hi) = if lo == hi { (lo.saturating_sub(1), hi + 1) } else { (lo, hi) };
    let sub_rows = plot.height as f64 * 2.0;
    let y_of = |p: u64| -> i32 { (((hi - p) as f64 / (hi - lo) as f64) * (sub_rows - 1.0)).round() as i32 };
    let buf = f.buffer_mut();
    // VWAP as a dashed line behind the candles.
    if vwap > 0.0 && vwap >= lo as f64 && vwap <= hi as f64 {
        let row = (((hi as f64 - vwap) / (hi - lo) as f64) * (plot.height as f64 - 1.0)).round() as u16;
        for x in (plot.x..plot.x + plot.width).step_by(2) {
            if let Some(c) = buf.cell_mut((x, plot.y + row)) {
                c.set_symbol("╌").set_fg(AMBER_DIM).set_bg(BG);
            }
        }
    }
    let lead = plot.width.saturating_sub(bars.len() as u16 * 2);
    for (i, b) in bars.iter().enumerate() {
        let x = plot.x + lead + (i as u16) * 2;
        let color = if b.close >= b.open { BID } else { ASK };
        let (top, bot) = (y_of(b.open.max(b.close)), y_of(b.open.min(b.close)));
        let (wt, wb) = (y_of(b.high), y_of(b.low));
        for row in 0..plot.height {
            let s0 = (row as i32) * 2;
            let s1 = s0 + 1;
            let body0 = s0 >= top && s0 <= bot;
            let body1 = s1 >= top && s1 <= bot;
            let wick0 = s0 >= wt && s0 <= wb;
            let wick1 = s1 >= wt && s1 <= wb;
            let sym_ch = match (body0, body1, wick0, wick1) {
                (true, true, _, _) => "█",
                (true, false, _, false) => "▀",
                (false, true, false, _) => "▄",
                (true, false, _, true) => "█",
                (false, true, true, _) => "█",
                (false, false, true, true) => "│",
                (false, false, true, false) => "╵",
                (false, false, false, true) => "╷",
                _ => continue,
            };
            if let Some(c) = buf.cell_mut((x, plot.y + row)) {
                c.set_symbol(sym_ch).set_fg(color).set_bg(BG);
            }
        }
    }
    // Price axis on the right.
    let axis = Rect { x: plot.x + plot.width, y: plot.y, width: axis_w, height: plot.height };
    let labels = plot.height.min(5).max(2);
    let mut lines = vec![Line::raw(""); plot.height as usize];
    for k in 0..labels {
        let row = if labels == 1 { 0 } else { (k as usize * (plot.height as usize - 1)) / (labels as usize - 1) };
        let p = hi - ((hi - lo) as f64 * (row as f64 / (plot.height as f64 - 1.0).max(1.0))).round() as u64;
        lines[row] = Line::from(Span::styled(pad_left(&p.to_string(), axis_w as usize), dimmer()));
    }
    f.render_widget(Paragraph::new(lines), axis);
}

pub fn draw_volume(f: &mut Frame, app: &App, area: Rect) {
    let Some(sym) = app.current_symbol() else { return };
    let block = Block::default().borders(Borders::LEFT | Borders::RIGHT | Borders::BOTTOM).border_style(fg(BORDER));
    let inner = block.inner(area);
    f.render_widget(block, area);
    if inner.height == 0 || inner.width < 12 {
        return;
    }
    let plot = Rect { x: inner.x, y: inner.y, width: inner.width - 8, height: inner.height };
    let n = (plot.width as usize / 2).max(1);
    let bars: Vec<_> = sym.bars[app.interval_idx].iter().rev().take(n).rev().collect();
    let mut data: Vec<u64> = Vec::with_capacity(plot.width as usize);
    let lead = (plot.width as usize).saturating_sub(bars.len() * 2);
    for _ in 0..lead {
        data.push(0);
    }
    for b in &bars {
        data.push(b.volume);
        data.push(0);
    }
    f.render_widget(Sparkline::default().data(&data).style(fg(AMBER_DIM)), plot);
    let label = Rect { x: plot.x + plot.width, y: inner.y, width: 8, height: 1 };
    f.render_widget(Paragraph::new(Span::styled(pad_left("VOL", 8), dimmer())), label);
}

// ------------------------------------------------------------ event log

pub fn event_cols(width: u16) -> Vec<(&'static str, u16, bool)> {
    // (name, width, right-aligned)
    let compact = width < 90;
    let mut cols = vec![
        ("SEQ", 8, true), ("TIME", 18, false), ("TY", 2, false), ("SYM", 5, false),
        (if compact { "OID" } else { "ORDER ID" }, if compact { 9 } else { 18 }, false),
        ("SD", 2, false), ("PX", 7, true), ("QTY", 6, true), ("LEFT", 6, true), ("MATCH", 10, false),
        ("BSEQ", 10, false), ("LAT", 7, true),
    ];
    let mut total: u16 = cols.iter().map(|c| c.1 + 1).sum();
    // Drop optional columns until it fits, least useful first.
    let optional = ["BSEQ", "MATCH", "LAT", "SYM", "TIME", "LEFT", "SEQ"];
    for name in optional {
        if total <= width {
            break;
        }
        if let Some(i) = cols.iter().position(|c| c.0 == name) {
            total -= cols[i].1 + 1;
            cols.remove(i);
        }
    }
    cols
}

pub fn event_line(e: &EventRecord, cols: &[(&str, u16, bool)], ticker: &str, selected: bool, width: u16) -> Line<'static> {
    if e.is_gap() {
        let text = format!(" — GAP — expected {} got {} ({} lost) ", fmt::commas(e.seq), fmt::commas(e.packet_seq), fmt::commas(e.qty as u64));
        return Line::from(Span::styled(pad_right(&text, width as usize), fg(RED).bg(RED_BG)));
    }
    let bg = if selected { SELECT_BG } else { BG };
    let st = |c: Color| Style::default().fg(c).bg(bg);
    let lat = if e.recv_ns > e.ts_ns && e.recv_ns != 0 { (e.recv_ns - e.ts_ns) / 1000 } else { 0 };
    let mut spans = Vec::with_capacity(cols.len() * 2);
    for (name, w, right) in cols {
        let (text, color) = match *name {
            "SEQ" => (fmt::commas(e.seq), DIM),
            "TIME" => (fmt::time_ns(e.ts_ns), TEXT),
            "TY" => ((e.ty as char).to_string(), type_color(e.ty)),
            "SYM" => (if ticker.is_empty() { format!("#{}", e.symbol_id) } else { ticker.to_string() }, TEXT),
            "ORDER ID" => (if e.order_id == 0 { String::new() } else { fmt::hex_id(e.order_id) }, Color::Rgb(189, 189, 189)),
            "OID" => (if e.order_id == 0 { String::new() } else { fmt::short_id(e.order_id) }, Color::Rgb(189, 189, 189)),
            "SD" => ((if e.side == b'B' || e.side == b'S' { e.side as char } else { ' ' }).to_string(), side_color(e.side)),
            "PX" => (if e.price == 0 { String::new() } else { fmt::commas(e.price) }, TEXT),
            "QTY" => (if e.qty == 0 { String::new() } else { fmt::commas(e.qty as u64) }, TEXT),
            "LEFT" => (if matches!(e.ty, b'E' | b'X' | b'D') { fmt::commas(e.remaining as u64) } else { String::new() }, DIM),
            "MATCH" => (if e.match_id == 0 { String::new() } else { fmt::short_id(e.match_id) }, DIM),
            "BSEQ" => (if e.book_seq == 0 { String::new() } else { fmt::short_id(e.book_seq) }, DIM),
            "LAT" => (if lat == 0 { String::new() } else { fmt::commas(lat) }, if lat > 500 { RED } else { DIM }),
            _ => (String::new(), DIM),
        };
        let cell = if *right { pad_left(&text, *w as usize) } else { pad_right(&text, *w as usize) };
        let style = if *name == "TY" { st(color).add_modifier(ratatui::style::Modifier::BOLD) } else { st(color) };
        spans.push(Span::styled(cell, style));
        spans.push(Span::styled(" ", st(TEXT)));
    }
    let used: u16 = cols.iter().map(|c| c.1 + 1).sum();
    if width > used {
        spans.push(Span::styled(" ".repeat((width - used) as usize), st(TEXT)));
    }
    Line::from(spans)
}

pub fn draw_event_log(f: &mut Frame, app: &App, area: Rect) {
    let sym_name_s = app.current_symbol().map(sym_name).unwrap_or_default();
    let sub = format!("filter: {} · ↑↓ select · Enter inspect · f filter", if app.filter_sym { sym_name_s.as_str() } else { "all" });
    let inner = panel(f, area, "EVENT LOG · every message, in stream order", &sub);
    if inner.height < 2 {
        return;
    }
    let cols = event_cols(inner.width);
    let header: String = cols.iter().map(|(n, w, r)| if *r { pad_left(n, *w as usize) } else { pad_right(n, *w as usize) }).collect::<Vec<_>>().join(" ");
    let mut lines = vec![Line::from(Span::styled(pad_right(&header, inner.width as usize), dim()))];
    let rows = inner.height as usize - 1;
    let vis = app.visible_events();
    let end = match app.selected {
        Some(seq) => vis.iter().position(|e| e.seq == seq).map(|i| (i + rows / 2 + 1).min(vis.len())).unwrap_or(vis.len()),
        None => vis.len(),
    };
    let start = end.saturating_sub(rows);
    let tickers: std::collections::HashMap<u32, &str> = app.snap.symbols.iter().map(|s| (s.symbol_id, s.ticker.as_str())).collect();
    for e in &vis[start..end] {
        let t = tickers.get(&e.symbol_id).copied().unwrap_or("");
        lines.push(event_line(e, &cols, t, app.selected == Some(e.seq), inner.width));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

// ------------------------------------------------------------ tape

pub fn draw_tape(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "TIME & SALES", "match · px · qty · aggr");
    let Some(sym) = app.current_symbol() else { return };
    let rows = inner.height as usize;
    let mut lines = Vec::with_capacity(rows);
    for t in sym.tape.iter().rev().take(rows) {
        let c = if t.aggressor_buy { BID } else { ASK };
        lines.push(Line::from(vec![
            Span::styled(fmt::time_ms(t.ts_ns), dim()),
            Span::raw(" "),
            Span::styled(pad_right(&fmt::short_id(t.match_id), 9), dim()),
            Span::raw(" "),
            Span::styled(pad_left(&fmt::commas(t.price), 8), fg(c)),
            Span::raw(" "),
            Span::styled(pad_left(&fmt::commas(t.qty as u64), 7), fg(TEXT)),
            Span::raw("  "),
            Span::styled(if t.aggressor_buy { "BUY ▲" } else { "SELL ▼" }, fg(c)),
        ]));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

// ------------------------------------------------------------ health

pub fn draw_health(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "FEED HEALTH", if app.replay { "replay" } else { "multicast" });
    let s = &app.snap;
    let w = inner.width as usize;
    let sparkw = w.saturating_sub(8 + 12);
    let frame_hist: Vec<u64> = app.frame_hist.iter().copied().collect();
    let (fp50, fp99) = app.frame_percentiles();
    let row = |label: &str, data: &[u64], color: Color, value: String| {
        Line::from(vec![
            Span::styled(pad_right(label, 8), dim()),
            Span::styled(fmt::spark(data, sparkw), fg(color)),
            Span::styled(pad_left(&value, 12), fg(TEXT)),
        ])
    };
    let total: u64 = s.stats.by_type.iter().sum::<u64>().max(1);
    let mix = |i: usize| (s.stats.by_type[i] as f64 / total as f64 * 100.0).round() as u64;
    let mixw = w.saturating_sub(8);
    let mut mixbar: Vec<Span> = vec![Span::styled(pad_right("mix", 8), dim())];
    for (i, c) in [(2, BID_BAR), (3, ASK_BAR), (5, Color::Rgb(74, 74, 74)), (4, Color::Rgb(107, 90, 42)), (6, Color::Rgb(90, 58, 122))] {
        let n = ((s.stats.by_type[i] as f64 / total as f64) * mixw as f64).round() as usize;
        mixbar.push(Span::styled(" ".repeat(n), Style::default().bg(c)));
    }
    let lines = vec![
        row("msgs/s", &s.rate_hist, BID, fmt::commas(s.msgs_per_sec)),
        row("lat p99", &s.p99_hist, AMBER, fmt::micros(s.latency_p99_ns)),
        row("frame", &frame_hist, GREEN, format!("{} ms", fp99 as f64 / 1000.0)),
        Line::from(mixbar),
        Line::from(vec![
            Span::styled(pad_right("", 8), dim()),
            Span::styled(format!("A {}%", mix(2)), fg(BID)), Span::styled(" · ", dim()),
            Span::styled(format!("E {}%", mix(3)), fg(ASK)), Span::styled(" · ", dim()),
            Span::styled(format!("D {}%", mix(5)), fg(Color::Rgb(189, 189, 189))), Span::styled(" · ", dim()),
            Span::styled(format!("X {}%", mix(4)), fg(AMBER_TEXT)), Span::styled(" · ", dim()),
            Span::styled(format!("U {}%", mix(6)), fg(PURPLE)),
        ]),
        Line::from(vec![
            Span::styled("ring ", dim()), Span::styled(format!("{:.0}%", s.ring_occupancy * 100.0), fg(TEXT)),
            Span::styled("   drops ", dim()), Span::styled(fmt::commas(s.ring_drops), fg(if s.ring_drops > 0 { RED } else { GREEN })),
            Span::styled("   gaps ", dim()), Span::styled(s.stats.gaps.to_string(), fg(if s.stats.gaps > 0 { RED } else { GREEN })),
            Span::styled(format!(" ({} lost)", fmt::commas(s.stats.lost_messages)), dimmer()),
            Span::styled("   dups ", dim()), Span::styled(fmt::commas(s.stats.duplicates), fg(TEXT)),
        ]),
        Line::from(vec![
            Span::styled("pkts ", dim()), Span::styled(fmt::commas(s.stats.packets), fg(TEXT)),
            Span::styled("   msgs ", dim()), Span::styled(fmt::commas(s.stats.messages), fg(TEXT)),
            Span::styled("   hb ", dim()), Span::styled(fmt::commas(s.stats.heartbeats), fg(TEXT)),
            Span::styled("   lat p50 ", dim()), Span::styled(fmt::micros(s.latency_p50_ns), fg(TEXT)),
        ]),
        Line::from(vec![
            Span::styled("frame p50 ", dim()), Span::styled(format!("{:.1} ms", fp50 as f64 / 1000.0), fg(TEXT)),
            Span::styled("   p99 ", dim()), Span::styled(format!("{:.1} ms", fp99 as f64 / 1000.0), fg(TEXT)),
            Span::styled("   uptime ", dim()), Span::styled(format!("{}s", app.started.elapsed().as_secs()), fg(TEXT)),
            Span::styled(if s.stale { "   STALE" } else { "" }, bold(RED)),
        ]),
    ];
    f.render_widget(Paragraph::new(lines), inner);
}

// ------------------------------------------------------------ compact inspector

pub fn level_before_after(e: &EventRecord) -> Option<((u64, u32), (u64, u32))> {
    if !e.applied.known {
        return None;
    }
    let after = (e.applied.level_qty_after, e.applied.level_count_after);
    let before = match e.ty {
        b'A' => (after.0.saturating_sub(e.qty as u64), after.1.saturating_sub(1)),
        b'E' => (after.0 + e.applied.qty_moved as u64, after.1 + if e.remaining == 0 { 1 } else { 0 }),
        b'X' => (after.0 + e.applied.qty_moved as u64, after.1),
        b'D' => (after.0 + e.applied.qty_moved as u64, after.1 + 1),
        _ => return None,
    };
    Some((before, after))
}

pub fn hex_lines(e: &EventRecord, fields: &[decode::Field], per_line: usize) -> Vec<Line<'static>> {
    let raw = e.raw_bytes();
    let colors = decode::byte_colors(fields, raw.len());
    let mut lines = Vec::new();
    for (ri, chunk) in raw.chunks(per_line).enumerate() {
        let mut spans = vec![Span::styled(format!("{:02x}  ", ri * per_line), dimmer())];
        for (i, b) in chunk.iter().enumerate() {
            spans.push(Span::styled(format!("{b:02x} "), fg(colors[ri * per_line + i])));
        }
        lines.push(Line::from(spans));
    }
    lines
}

pub fn draw_inspector_panel(f: &mut Frame, app: &App, area: Rect) {
    let title = match app.selected {
        Some(seq) => format!("INSPECTOR · seq {}", fmt::commas(seq)),
        None => "INSPECTOR".to_string(),
    };
    let inner = panel(f, area, &title, "Enter for full screen");
    let Some(e) = app.selected_event() else {
        f.render_widget(Paragraph::new("↑↓ selects an event-log row; Enter opens it full screen.").style(dim()), inner);
        return;
    };
    let ticker = app.snap.symbols.iter().find(|s| s.symbol_id == e.symbol_id).map(|s| s.ticker.as_str()).unwrap_or("");
    let fields = decode::fields(&e, ticker);
    let mut lines = vec![Line::from(Span::styled(format!("{}  {} · {} bytes", e.ty as char, fmt::type_name(e.ty), e.raw_len), bold(type_color(e.ty))))];
    for fl in &fields {
        lines.push(Line::from(vec![
            Span::styled(pad_left(&fl.off.to_string(), 3), dimmer()),
            Span::raw(" "),
            Span::styled(pad_right(fl.name, 14), dim()),
            Span::styled(pad_right(fl.ty, 5), dimmer()),
            Span::styled(fl.value.clone(), fg(fl.color)),
        ]));
    }
    lines.push(Line::from(Span::styled("raw", dim())));
    let per = ((inner.width as usize).saturating_sub(4) / 3).clamp(8, 16);
    lines.extend(hex_lines(&e, &fields, per));
    if let Some((b, a)) = level_before_after(&e) {
        lines.push(Line::from(vec![
            Span::styled(format!("level {} {}: ", fmt::commas(e.price), fmt::side_name(e.side)), dim()),
            Span::styled(format!("{} × {}", fmt::commas(b.0), b.1), fg(side_color(e.side))),
            Span::styled(" → ", dim()),
            Span::styled(format!("{} × {}", fmt::commas(a.0), a.1), fg(side_color(e.side))),
            Span::styled(if e.ty == b'E' && e.remaining == 0 { " · order left the book" } else { "" }, dim()),
        ]));
    } else if !e.applied.known && matches!(e.ty, b'E' | b'X' | b'D') {
        lines.push(Line::from(Span::styled("order unknown to this client (joined after its Add)", fg(AMBER_TEXT))));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

#[allow(dead_code)]
pub fn fill(buf: &mut Buffer, area: Rect, color: Color) {
    for y in area.y..area.y + area.height {
        for x in area.x..area.x + area.width {
            if let Some(c) = buf.cell_mut((x, y)) {
                c.set_bg(color);
            }
        }
    }
}
