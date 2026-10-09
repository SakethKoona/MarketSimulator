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

pub fn draw(f: &mut Frame, app: &mut App) {
    match app.screen {
        crate::app::Screen::Market => crate::market::draw(f, app),
        crate::app::Screen::Events => crate::events::draw(f, app),
    }
    if let Some(p) = &app.picker {
        draw_picker(f, p);
    }
}

/// Centred fuzzy picker: prompt line, matches with highlighted characters.
pub fn draw_picker(f: &mut Frame, p: &crate::picker::Picker) {
    use crate::picker::PickerKind;
    let area = f.area();
    let w = 70u16.min(area.width.saturating_sub(4));
    let h = (p.items.len() as u16 + 5).clamp(7, area.height.saturating_sub(2).max(7));
    let rect =
        Rect { x: (area.width.saturating_sub(w)) / 2, y: (area.height.saturating_sub(h)) / 2, width: w, height: h };
    f.render_widget(ratatui::widgets::Clear, rect);
    let title = match p.kind {
        PickerKind::Theme => " THEMES  type to filter · ↑↓ preview · Enter keep · Esc revert ",
        PickerKind::Symbol => " SYMBOLS  type to filter · ↑↓ · Enter select · Esc ",
    };
    let block = Block::default()
        .borders(Borders::ALL)
        .border_style(fg(th().amber))
        .style(Style::default().bg(th().panel_hdr_bg))
        .title(Span::styled(title, panel_title()));
    let inner = block.inner(rect);
    f.render_widget(block, rect);
    if inner.height < 3 {
        return;
    }
    // Prompt.
    let prompt = Line::from(vec![
        Span::styled(" > ", bold(th().amber)),
        Span::styled(p.query.clone(), fg(th().text_bright)),
        Span::styled("▏", fg(th().amber)),
        Span::styled(format!("   {}/{}", p.matches.len(), p.items.len()), dim()),
    ]);
    f.render_widget(Paragraph::new(prompt), Rect { x: inner.x, y: inner.y, width: inner.width, height: 1 });
    let sep = Line::from(Span::styled("─".repeat(inner.width as usize), fg(th().border)));
    f.render_widget(Paragraph::new(sep), Rect { x: inner.x, y: inner.y + 1, width: inner.width, height: 1 });

    let rows = inner.height as usize - 2;
    let start = p.selected.saturating_sub(rows / 2).min(p.matches.len().saturating_sub(rows));
    let label_w = p.items.iter().map(|i| i.label.chars().count()).max().unwrap_or(8).clamp(6, 20);
    let mut lines = Vec::with_capacity(rows);
    for (row, m) in p.matches.iter().enumerate().skip(start).take(rows) {
        let it = &p.items[m.item];
        let sel = row == p.selected;
        let bg = if sel { th().select_bg } else { th().panel_hdr_bg };
        let mut spans = vec![Span::styled(if sel { " ▶ " } else { "   " }, Style::default().fg(th().amber).bg(bg))];
        // Label with matched characters highlighted.
        let chars: Vec<char> = it.label.chars().collect();
        for (ci, c) in chars.iter().enumerate() {
            let hit = m.positions.contains(&ci);
            let st = if hit {
                bold(th().amber_bright).bg(bg)
            } else if sel {
                bold(th().text_bright).bg(bg)
            } else {
                Style::default().fg(th().text).bg(bg)
            };
            spans.push(Span::styled(c.to_string(), st));
        }
        spans.push(Span::styled(" ".repeat(label_w.saturating_sub(chars.len()) + 1), Style::default().bg(bg)));
        if p.kind == PickerKind::Theme {
            let t = &crate::theme::THEMES[it.key];
            spans.push(Span::styled("■", Style::default().fg(t.amber).bg(bg)));
            spans.push(Span::styled("■", Style::default().fg(t.bid).bg(bg)));
            spans.push(Span::styled("■", Style::default().fg(t.ask).bg(bg)));
            spans.push(Span::styled("■ ", Style::default().fg(t.text).bg(t.bg)));
        }
        let used: usize = spans.iter().map(|x| x.content.chars().count()).sum();
        let detail_w = (inner.width as usize).saturating_sub(used);
        spans.push(Span::styled(pad_right(&it.detail, detail_w), Style::default().fg(th().dim).bg(bg)));
        lines.push(Line::from(spans));
    }
    if p.matches.is_empty() {
        lines.push(Line::from(Span::styled("   no match", dim())));
    }
    f.render_widget(
        Paragraph::new(lines),
        Rect { x: inner.x, y: inner.y + 2, width: inner.width, height: inner.height - 2 },
    );
}

/// The original single-screen layout, kept for the render test and as a
/// fallback for very small terminals.
#[allow(dead_code)]
pub fn draw_classic(f: &mut Frame, app: &mut App) {
    let area = f.area();
    f.render_widget(Block::default().style(theme::base()), area);
    let [hdr, body, foot] =
        Layout::vertical([Constraint::Length(1), Constraint::Min(0), Constraint::Length(1)]).areas(area);
    draw_header(f, app, hdr);
    draw_footer(f, foot);

    let left_w = 52u16.min(body.width / 3);
    let right_w = 58u16.min(body.width / 3);
    let [left, centre, right] =
        Layout::horizontal([Constraint::Length(left_w), Constraint::Min(20), Constraint::Length(right_w)]).areas(body);

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
    let block = Block::default().borders(Borders::ALL).border_style(fg(th().border));
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
    if n >= w {
        s.chars().take(w).collect()
    } else {
        format!("{}{}", " ".repeat(w - n), s)
    }
}
pub fn pad_right(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w {
        s.chars().take(w).collect()
    } else {
        format!("{}{}", s, " ".repeat(w - n))
    }
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
    if s.ticker.is_empty() {
        format!("#{}", s.symbol_id)
    } else {
        s.ticker.clone()
    }
}

// ------------------------------------------------------------ header / footer

pub fn draw_header(f: &mut Frame, app: &App, area: Rect) {
    let s = &app.snap;
    let mut spans = vec![
        Span::styled(" FEEDVIZ ", bold(th().amber_bright).bg(th().hdr_bg)),
        Span::styled(" ", Style::default().bg(th().hdr_bg)),
    ];
    for (name, sc) in [("MARKET", crate::app::Screen::Market), ("EVENTS", crate::app::Screen::Events)] {
        if app.screen == sc {
            spans.push(Span::styled(
                format!(" {name} "),
                Style::default().fg(th().hdr_bg).bg(th().amber).add_modifier(ratatui::style::Modifier::BOLD),
            ));
        } else {
            spans.push(Span::styled(format!(" {name} "), Style::default().fg(th().amber).bg(th().hdr_bg)));
        }
    }
    spans.push(Span::styled("  ", Style::default().bg(th().hdr_bg)));
    for (i, sym) in s.symbols.iter().enumerate() {
        let name = format!(" {} ", sym_name(sym));
        if i == app.sym_idx {
            spans.push(Span::styled(
                name,
                Style::default().fg(th().pause_fg).bg(th().pause_bg).add_modifier(ratatui::style::Modifier::BOLD),
            ));
        } else {
            spans.push(Span::styled(name, Style::default().fg(th().amber).bg(th().hdr_bg)));
        }
        spans.push(Span::styled(" ", Style::default().bg(th().hdr_bg)));
    }
    let kv = |k: &str, v: String, c: Color| {
        vec![
            Span::styled(format!("  {k} "), Style::default().fg(th().amber_dim).bg(th().hdr_bg)),
            Span::styled(v, Style::default().fg(c).bg(th().hdr_bg)),
        ]
    };
    let session = String::from_utf8_lossy(&s.stats.session).trim().to_string();
    let wide = area.width >= 170;
    if wide {
        spans.extend(kv("SESSION", if session.is_empty() { "-".into() } else { session }, th().amber_text));
    }
    spans.extend(kv("SEQ", fmt::commas(s.stats.next_seq), th().amber_text));
    spans.extend(kv("MSGS/S", fmt::commas(s.msgs_per_sec), th().amber_text));
    spans.extend(kv("P99", fmt::micros(s.latency_p99_ns), th().amber_text));
    spans.extend(kv("GAPS", s.stats.gaps.to_string(), if s.stats.gaps > 0 { th().red } else { th().amber_text }));
    if wide {
        let shards = s.symbols.iter().map(|x| fmt::shard_of(x.book_seq)).max().map(|m| m as usize + 1).unwrap_or(1);
        spans.extend(kv("SHARDS", shards.to_string(), th().amber_text));
    }
    let clock = s.events.last().map(|e| fmt::time_ms(e.ts_ns)).unwrap_or_else(|| "--:--:--.---".into());
    let state = if app.paused {
        Span::styled(" ❚❚ PAUSED ", bold(th().pause_fg).bg(th().pause_bg))
    } else if s.stats.recovering {
        Span::styled(" ◌ SYNCING ", bold(th().pause_fg).bg(th().pause_bg))
    } else if app.replay && s.finished {
        Span::styled(" ■ ENDED ", bold(th().pause_fg).bg(th().pause_bg))
    } else if app.replay {
        Span::styled(" ▶ REPLAY ", bold(th().live_fg).bg(th().live_bg))
    } else {
        Span::styled(" ● LIVE ", bold(th().live_fg).bg(th().live_bg))
    };
    let right = vec![
        Span::styled(format!("{clock}  "), Style::default().fg(th().amber_text).bg(th().hdr_bg)),
        state,
        Span::styled(" ", Style::default().bg(th().hdr_bg)),
    ];
    let left_len: usize = spans.iter().map(|x| x.content.chars().count()).sum();
    let right_len: usize = right.iter().map(|x| x.content.chars().count()).sum();
    let pad = (area.width as usize).saturating_sub(left_len + right_len).max(2);
    spans.push(Span::styled(" ".repeat(pad), Style::default().bg(th().hdr_bg)));
    spans.extend(right);
    f.render_widget(Paragraph::new(Line::from(spans)).style(Style::default().bg(th().hdr_bg)), area);
}

pub fn draw_footer_keys(f: &mut Frame, area: Rect, keys: &[(&str, &str)]) {
    let mut spans = vec![Span::styled(" ", Style::default().bg(th().panel_hdr_bg))];
    for (k, v) in keys {
        spans.push(Span::styled(k.to_string(), bold(th().amber).bg(th().panel_hdr_bg)));
        spans.push(Span::styled(format!(" {v}   "), Style::default().fg(th().dim).bg(th().panel_hdr_bg)));
    }
    let tail = format!("theme {} (T) · feedviz 0.2 ", th().name);
    let used: usize = spans.iter().map(|x| x.content.chars().count()).sum();
    let pad = (area.width as usize).saturating_sub(used + tail.chars().count());
    spans.push(Span::styled(" ".repeat(pad), Style::default().bg(th().panel_hdr_bg)));
    spans.push(Span::styled(tail, Style::default().fg(th().dimmer).bg(th().panel_hdr_bg)));
    f.render_widget(Paragraph::new(Line::from(spans)).style(Style::default().bg(th().panel_hdr_bg)), area);
}

fn draw_footer(f: &mut Frame, area: Rect) {
    draw_footer_keys(
        f,
        area,
        &[
            ("q", "quit"),
            ("space", "pause"),
            ("[ ]", "symbol"),
            ("d", "depth"),
            ("f", "filter"),
            ("↑↓", "select"),
            ("Enter", "inspect"),
            ("Esc", "follow"),
            ("/", "symbol"),
            ("T", "themes"),
        ],
    );
}

// ------------------------------------------------------------ ladder

/// Blend a theme colour toward a flash colour by `age` (0 fresh .. 1 faded).
fn blend(base: Color, flash: Color, age: f32) -> Color {
    let (Color::Rgb(r1, g1, b1), Color::Rgb(r2, g2, b2)) = (base, flash) else { return base };
    let k = 1.0 - age.clamp(0.0, 1.0);
    let mix = |a: u8, b: u8| (a as f32 + (b as f32 - a as f32) * k * 0.55) as u8;
    Color::Rgb(mix(r1, r2), mix(g1, g2), mix(b1, b2))
}

/// Price-anchored ladder: one row per tick group, empty ticks shown blank,
/// centred on an anchor that only moves when the mid drifts near an edge
/// (or on `c`). Changed quantities flash and fade.
pub fn draw_ladder(f: &mut Frame, app: &mut App, area: Rect) {
    let Some(sym) = app.fast.symbols.get(app.sym_idx).cloned() else {
        let inner = panel(f, area, "L2 BOOK", "");
        f.render_widget(Paragraph::new("waiting for feed…").style(dim()), inner);
        return;
    };
    let g = app.group();
    let depth = app.depth();
    let inner = panel(
        f,
        area,
        &format!("L2 BOOK · {}", sym_name(&sym)),
        &format!("{} rows · tick ×{} · c centre", depth * 2, g),
    );
    let w = inner.width as usize;
    let fixed = 3 + 1 + 8 + 1 + 1 + 9 + 1 + 1 + 8 + 1 + 3;
    let barw = w.saturating_sub(fixed) / 2;
    let rows_avail = inner.height.saturating_sub(3) as usize; // header, mid, footer
    let per_side = (rows_avail / 2).min(depth).max(1);

    // Bin levels by group.
    let bin = |p: u64| (p / g) * g;
    let mut bids: std::collections::BTreeMap<u64, (u64, u32)> = Default::default();
    let mut asks: std::collections::BTreeMap<u64, (u64, u32)> = Default::default();
    for l in &sym.bids {
        let e = bids.entry(bin(l.price)).or_default();
        e.0 += l.qty;
        e.1 += l.count;
    }
    for l in &sym.asks {
        let e = asks.entry(bin(l.price)).or_default();
        e.0 += l.qty;
        e.1 += l.count;
    }
    // Anchor: the mid, re-centred only when it drifts past half the window.
    let mid2 = sym.mid_x2().or_else(|| sym.last_trade.map(|t| t.price * 2)).unwrap_or(0);
    let mid_bin = bin(mid2 / 2);
    let anchor = {
        let cur = app.anchor.get(&sym.symbol_id).copied();
        let keep = cur.filter(|a| a.abs_diff(mid_bin) < (per_side as u64 / 2).max(1) * g);
        let a = keep.unwrap_or(mid_bin);
        app.anchor.insert(sym.symbol_id, a);
        a
    };
    // Rows: asks above the anchor (anchor+g*per_side .. anchor+g), bids below (anchor .. anchor-g*(per_side-1)).
    let max_cum = {
        let mut c = 0u64;
        let mut m = 0u64;
        for (p, (q, _)) in asks.iter() {
            if *p > anchor && *p <= anchor + g * per_side as u64 {
                c += q;
                m = m.max(c);
            }
        }
        c = 0;
        for (p, (q, _)) in bids.iter().rev() {
            if *p <= anchor && *p + g * per_side as u64 > anchor {
                c += q;
                m = m.max(c);
            }
        }
        m.max(1)
    };
    let mut lines: Vec<Line> = Vec::with_capacity(rows_avail + 3);
    lines.push(Line::from(Span::styled(
        format!(
            "{} {} {} {} {} {} {}",
            pad_left("ORD", 3),
            pad_left("BID", 8),
            " ".repeat(barw),
            center("PRICE", 9),
            " ".repeat(barw),
            pad_right("ASK", 8),
            pad_right("ORD", 3)
        ),
        dim(),
    )));
    let sid = sym.symbol_id;
    // Asks: top row is the farthest.
    let mut cum = 0u64;
    let mut ask_rows: Vec<Line> = Vec::new();
    for i in 1..=per_side as u64 {
        let p = anchor + g * i;
        let lv = asks.get(&p).copied();
        if let Some((q, _)) = lv {
            cum += q;
        }
        let bw = ((cum as f64 / max_cum as f64) * barw as f64).round() as usize;
        let flash = lv.and_then(|_| app.flash_age(sid, b'S', p));
        let qty_style = match flash {
            Some(age) => Style::default().fg(th().ask_text).bg(blend(th().bg, th().ask_bar_solid, age)),
            None => fg(th().ask_text),
        };
        let px_style = if lv.is_some() { fg(th().ask) } else { dimmer() };
        ask_rows.push(Line::from(vec![
            Span::raw(format!("{} {} {} ", " ".repeat(3), " ".repeat(8), " ".repeat(barw))),
            Span::styled(center(&fmt::commas(p), 9), px_style),
            Span::raw(" "),
            Span::styled(" ".repeat(if lv.is_some() { bw.min(barw) } else { 0 }), Style::default().bg(th().ask_bar)),
            Span::raw(" ".repeat(barw.saturating_sub(if lv.is_some() { bw } else { 0 }))),
            Span::raw(" "),
            Span::styled(pad_right(&lv.map(|(q, _)| fmt::commas(q)).unwrap_or_default(), 8), qty_style),
            Span::raw(" "),
            Span::styled(pad_right(&lv.map(|(_, n)| n.to_string()).unwrap_or_default(), 3), dim()),
        ]));
    }
    ask_rows.reverse();
    lines.extend(ask_rows);
    // Mid line.
    let mid = match (sym.best_bid(), sym.best_ask()) {
        (Some(b), Some(a)) => {
            let bid_depth: u64 = sym.bids.iter().take(depth).map(|l| l.qty).sum();
            let ask_depth: u64 = sym.asks.iter().take(depth).map(|l| l.qty).sum();
            let imb = if bid_depth + ask_depth > 0 {
                (bid_depth as f64 - ask_depth as f64) / (bid_depth + ask_depth) as f64 * 100.0
            } else {
                0.0
            };
            Line::from(vec![
                Span::styled(" MID ", dim()),
                Span::styled(format!("{:.1}", (a.price + b.price) as f64 / 2.0), bold(th().amber)),
                Span::styled("  SPREAD ", dim()),
                Span::styled(fmt::commas(a.price - b.price), amber()),
                Span::styled("  IMB ", dim()),
                Span::styled(format!("{imb:+.0}%"), fg(if imb >= 0.0 { th().bid } else { th().ask })),
                Span::styled(if anchor != mid_bin { "  ⌖ off-centre" } else { "" }, dimmer()),
            ])
        }
        _ => Line::from(Span::styled(" MID  —", dim())),
    };
    lines.push(mid);
    cum = 0;
    for i in 0..per_side as u64 {
        let Some(p) = anchor.checked_sub(g * i) else { break };
        let lv = bids.get(&p).copied();
        if let Some((q, _)) = lv {
            cum += q;
        }
        let bw = ((cum as f64 / max_cum as f64) * barw as f64).round() as usize;
        let flash = lv.and_then(|_| app.flash_age(sid, b'B', p));
        let qty_style = match flash {
            Some(age) => Style::default().fg(th().bid_text).bg(blend(th().bg, th().bid_bar_solid, age)),
            None => fg(th().bid_text),
        };
        let px_style = if lv.is_some() { fg(th().bid) } else { dimmer() };
        lines.push(Line::from(vec![
            Span::styled(pad_left(&lv.map(|(_, n)| n.to_string()).unwrap_or_default(), 3), dim()),
            Span::raw(" "),
            Span::styled(pad_left(&lv.map(|(q, _)| fmt::commas(q)).unwrap_or_default(), 8), qty_style),
            Span::raw(" "),
            Span::raw(" ".repeat(barw.saturating_sub(if lv.is_some() { bw } else { 0 }))),
            Span::styled(" ".repeat(if lv.is_some() { bw.min(barw) } else { 0 }), Style::default().bg(th().bid_bar)),
            Span::raw(" "),
            Span::styled(center(&fmt::commas(p), 9), px_style),
        ]));
    }
    let last_style = match app.last_px_flash(sid) {
        Some((buy, age)) => Style::default().fg(if buy { th().bid } else { th().ask }).bg(blend(
            th().bg,
            if buy { th().bid_bar_solid } else { th().ask_bar_solid },
            age,
        )),
        None => fg(sym.last_trade.map(|t| if t.aggressor_buy { th().bid } else { th().ask }).unwrap_or(th().dim)),
    };
    let footer = Line::from(vec![
        Span::styled(" LAST ", dim()),
        Span::styled(
            sym.last_trade
                .map(|t| format!("{} × {}", fmt::commas(t.price), fmt::commas(t.qty as u64)))
                .unwrap_or_else(|| "—".into()),
            last_style,
        ),
        Span::styled("  VOL ", dim()),
        Span::styled(fmt::commas(sym.volume), fg(th().text)),
        Span::styled("  ORD ", dim()),
        Span::styled(fmt::commas(sym.orders as u64), fg(th().text)),
        Span::styled("  LVL ", dim()),
        Span::styled(format!("{}/{}", sym.bid_levels, sym.ask_levels), fg(th().text)),
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
    if s >= 60 {
        format!("{}m", s / 60)
    } else {
        format!("{s}s")
    }
}

pub fn draw_candles(f: &mut Frame, app: &App, area: Rect) {
    let Some(sym) = app.fast.symbols.get(app.sym_idx) else { return };
    let bars_all = &sym.bars[app.interval_idx];
    let last = bars_all.last();
    let vwap = sym.session.vwap();
    let sub = last
        .map(|b| {
            format!(
                "O {} H {} L {} C {} · {}{} · VWAP {:.1}",
                b.open,
                b.high,
                b.low,
                b.close,
                if b.close >= b.open { "▲ +" } else { "▼ -" },
                b.close.abs_diff(b.open),
                vwap
            )
        })
        .unwrap_or_default();
    let ivs: String = (0..feed_client::INTERVALS.len())
        .map(|i| {
            if i == app.interval_idx {
                format!("[{}]", interval_label(i))
            } else {
                format!(" {} ", interval_label(i))
            }
        })
        .collect();
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
                c.set_symbol("╌").set_fg(th().amber_dim).set_bg(th().bg);
            }
        }
    }
    let lead = plot.width.saturating_sub(bars.len() as u16 * 2);
    for (i, b) in bars.iter().enumerate() {
        let x = plot.x + lead + (i as u16) * 2;
        let color = if b.close >= b.open { th().bid } else { th().ask };
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
                c.set_symbol(sym_ch).set_fg(color).set_bg(th().bg);
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
    let Some(sym) = app.fast.symbols.get(app.sym_idx) else { return };
    let block =
        Block::default().borders(Borders::LEFT | Borders::RIGHT | Borders::BOTTOM).border_style(fg(th().border));
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
    f.render_widget(Sparkline::default().data(&data).style(fg(th().amber_dim)), plot);
    let label = Rect { x: plot.x + plot.width, y: inner.y, width: 8, height: 1 };
    f.render_widget(Paragraph::new(Span::styled(pad_left("VOL", 8), dimmer())), label);
}

// ------------------------------------------------------------ event log

pub fn event_cols(width: u16) -> Vec<(&'static str, u16, bool)> {
    // (name, width, right-aligned)
    let compact = width < 90;
    let mut cols = vec![
        ("SEQ", 8, true),
        ("TIME", 18, false),
        ("TY", 2, false),
        ("SYM", 5, false),
        (if compact { "OID" } else { "ORDER ID" }, if compact { 9 } else { 18 }, false),
        ("SD", 2, false),
        ("PX", 7, true),
        ("QTY", 6, true),
        ("LEFT", 6, true),
        ("MATCH", 10, false),
        ("BSEQ", 10, false),
        ("LAT", 7, true),
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

pub fn event_line(
    e: &EventRecord,
    cols: &[(&str, u16, bool)],
    ticker: &str,
    selected: bool,
    width: u16,
) -> Line<'static> {
    if e.is_gap() {
        let text = format!(
            " — GAP — expected {} got {} ({} lost) ",
            fmt::commas(e.seq),
            fmt::commas(e.packet_seq),
            fmt::commas(e.qty as u64)
        );
        return Line::from(Span::styled(pad_right(&text, width as usize), fg(th().red).bg(th().red_bg)));
    }
    let bg = if selected { th().select_bg } else { th().bg };
    let st = |c: Color| Style::default().fg(c).bg(bg);
    let lat = if e.recv_ns > e.ts_ns && e.recv_ns != 0 { (e.recv_ns - e.ts_ns) / 1000 } else { 0 };
    let mut spans = Vec::with_capacity(cols.len() * 2);
    for (name, w, right) in cols {
        let (text, color) = match *name {
            "SEQ" => (fmt::commas(e.seq), th().dim),
            "TIME" => (fmt::time_ns(e.ts_ns), th().text),
            "TY" => ((e.ty as char).to_string(), type_color(e.ty)),
            "SYM" => (if ticker.is_empty() { format!("#{}", e.symbol_id) } else { ticker.to_string() }, th().text),
            "ORDER ID" => (if e.order_id == 0 { String::new() } else { fmt::hex_id(e.order_id) }, th().muted),
            "OID" => (if e.order_id == 0 { String::new() } else { fmt::short_id(e.order_id) }, th().muted),
            "SD" => {
                ((if e.side == b'B' || e.side == b'S' { e.side as char } else { ' ' }).to_string(), side_color(e.side))
            }
            "PX" => (if e.price == 0 { String::new() } else { fmt::commas(e.price) }, th().text),
            "QTY" => (if e.qty == 0 { String::new() } else { fmt::commas(e.qty as u64) }, th().text),
            "LEFT" => (
                if matches!(e.ty, b'E' | b'X' | b'D') { fmt::commas(e.remaining as u64) } else { String::new() },
                th().dim,
            ),
            "MATCH" => (if e.match_id == 0 { String::new() } else { fmt::short_id(e.match_id) }, th().dim),
            "BSEQ" => (if e.book_seq == 0 { String::new() } else { fmt::short_id(e.book_seq) }, th().dim),
            "LAT" => {
                (if lat == 0 { String::new() } else { fmt::commas(lat) }, if lat > 500 { th().red } else { th().dim })
            }
            _ => (String::new(), th().dim),
        };
        let cell = if *right { pad_left(&text, *w as usize) } else { pad_right(&text, *w as usize) };
        let style = if *name == "TY" { st(color).add_modifier(ratatui::style::Modifier::BOLD) } else { st(color) };
        spans.push(Span::styled(cell, style));
        spans.push(Span::styled(" ", st(th().text)));
    }
    let used: u16 = cols.iter().map(|c| c.1 + 1).sum();
    if width > used {
        spans.push(Span::styled(" ".repeat((width - used) as usize), st(th().text)));
    }
    Line::from(spans)
}

pub fn draw_event_log(f: &mut Frame, app: &App, area: Rect) {
    let sym_name_s = app.current_symbol().map(sym_name).unwrap_or_default();
    let sub = format!(
        "filter: {} · ↑↓ select · Enter inspect · f filter",
        if app.filter_sym { sym_name_s.as_str() } else { "all" }
    );
    let inner = panel(f, area, "EVENT LOG · every message, in stream order", &sub);
    if inner.height < 2 {
        return;
    }
    let cols = event_cols(inner.width);
    let header: String = cols
        .iter()
        .map(|(n, w, r)| if *r { pad_left(n, *w as usize) } else { pad_right(n, *w as usize) })
        .collect::<Vec<_>>()
        .join(" ");
    let mut lines = vec![Line::from(Span::styled(pad_right(&header, inner.width as usize), dim()))];
    let rows = inner.height as usize - 1;
    let vis = app.visible_events();
    let end = match app.selected {
        Some(seq) => {
            vis.iter().position(|e| e.seq == seq).map(|i| (i + rows / 2 + 1).min(vis.len())).unwrap_or(vis.len())
        }
        None => vis.len(),
    };
    let start = end.saturating_sub(rows);
    let tickers: std::collections::HashMap<u32, &str> =
        app.snap.symbols.iter().map(|s| (s.symbol_id, s.ticker.as_str())).collect();
    for e in &vis[start..end] {
        let t = tickers.get(&e.symbol_id).copied().unwrap_or("");
        lines.push(event_line(e, &cols, t, app.selected == Some(e.seq), inner.width));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

// ------------------------------------------------------------ tape

/// Consecutive fills on the same side within 200 µs are one sweep: print
/// one line with the total size and the number of prints.
pub fn draw_tape(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "TIME & SALES", "sweeps grouped · px · qty · n");
    let Some(sym) = app.fast.symbols.get(app.sym_idx) else { return };
    let rows = inner.height as usize;
    struct Row {
        ts: u64,
        px_lo: u64,
        px_hi: u64,
        qty: u64,
        n: u32,
        buy: bool,
    }
    let mut groups: Vec<Row> = Vec::new();
    for t in sym.tape.iter() {
        match groups.last_mut() {
            Some(g) if g.buy == t.aggressor_buy && t.ts_ns.saturating_sub(g.ts) <= 200_000 => {
                g.qty += t.qty as u64;
                g.n += 1;
                g.px_lo = g.px_lo.min(t.price);
                g.px_hi = g.px_hi.max(t.price);
                g.ts = t.ts_ns;
            }
            _ => groups.push(Row {
                ts: t.ts_ns,
                px_lo: t.price,
                px_hi: t.price,
                qty: t.qty as u64,
                n: 1,
                buy: t.aggressor_buy,
            }),
        }
    }
    let mut lines = Vec::with_capacity(rows);
    for g in groups.iter().rev().take(rows) {
        let c = if g.buy { th().bid } else { th().ask };
        let px = if g.px_lo == g.px_hi {
            fmt::commas(g.px_lo)
        } else {
            format!("{}–{}", fmt::commas(g.px_lo), fmt::commas(g.px_hi))
        };
        let big = g.qty >= 2000 || g.n >= 4;
        lines.push(Line::from(vec![
            Span::styled(fmt::time_ms(g.ts), dim()),
            Span::raw(" "),
            Span::styled(pad_left(&px, 13), if big { bold(c) } else { fg(c) }),
            Span::raw(" "),
            Span::styled(pad_left(&fmt::commas(g.qty), 7), if big { bold(th().text_bright) } else { fg(th().text) }),
            Span::raw(" "),
            Span::styled(if g.n > 1 { format!("×{:<3}", g.n) } else { "    ".into() }, dim()),
            Span::styled(if g.buy { "BUY ▲" } else { "SELL ▼" }, fg(c)),
            Span::styled(if g.n >= 4 { " sweep" } else { "" }, bold(th().amber)),
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
            Span::styled(pad_left(&value, 12), fg(th().text)),
        ])
    };
    let total: u64 = s.stats.by_type.iter().sum::<u64>().max(1);
    let mix = |i: usize| (s.stats.by_type[i] as f64 / total as f64 * 100.0).round() as u64;
    let mixw = w.saturating_sub(8);
    let mut mixbar: Vec<Span> = vec![Span::styled(pad_right("mix", 8), dim())];
    for (i, c) in [(2, th().bid_bar), (3, th().ask_bar), (5, th().mix_d), (4, th().mix_x), (6, th().mix_u)] {
        let n = ((s.stats.by_type[i] as f64 / total as f64) * mixw as f64).round() as usize;
        mixbar.push(Span::styled(" ".repeat(n), Style::default().bg(c)));
    }
    let lines = vec![
        row("msgs/s", &s.rate_hist, th().bid, fmt::commas(s.msgs_per_sec)),
        row("lat p99", &s.p99_hist, th().amber, fmt::micros(s.latency_p99_ns)),
        row("frame", &frame_hist, th().green, format!("{} ms", fp99 as f64 / 1000.0)),
        Line::from(mixbar),
        Line::from(vec![
            Span::styled(pad_right("", 8), dim()),
            Span::styled(format!("A {}%", mix(2)), fg(th().bid)),
            Span::styled(" · ", dim()),
            Span::styled(format!("E {}%", mix(3)), fg(th().ask)),
            Span::styled(" · ", dim()),
            Span::styled(format!("D {}%", mix(5)), fg(th().muted)),
            Span::styled(" · ", dim()),
            Span::styled(format!("X {}%", mix(4)), fg(th().amber_text)),
            Span::styled(" · ", dim()),
            Span::styled(format!("U {}%", mix(6)), fg(th().purple)),
        ]),
        Line::from(vec![
            Span::styled("ring ", dim()),
            Span::styled(format!("{:.0}%", s.ring_occupancy * 100.0), fg(th().text)),
            Span::styled("   drops ", dim()),
            Span::styled(fmt::commas(s.ring_drops), fg(if s.ring_drops > 0 { th().red } else { th().green })),
            Span::styled("   gaps ", dim()),
            Span::styled(s.stats.gaps.to_string(), fg(if s.stats.gaps > 0 { th().red } else { th().green })),
            Span::styled(format!(" ({} lost)", fmt::commas(s.stats.lost_messages)), dimmer()),
            Span::styled("   dups ", dim()),
            Span::styled(fmt::commas(s.stats.duplicates), fg(th().text)),
        ]),
        Line::from(vec![
            Span::styled("pkts ", dim()),
            Span::styled(fmt::commas(s.stats.packets), fg(th().text)),
            Span::styled("   msgs ", dim()),
            Span::styled(fmt::commas(s.stats.messages), fg(th().text)),
            Span::styled("   hb ", dim()),
            Span::styled(fmt::commas(s.stats.heartbeats), fg(th().text)),
            Span::styled("   lat p50 ", dim()),
            Span::styled(fmt::micros(s.latency_p50_ns), fg(th().text)),
        ]),
        Line::from(vec![
            Span::styled("frame p50 ", dim()),
            Span::styled(format!("{:.1} ms", fp50 as f64 / 1000.0), fg(th().text)),
            Span::styled("   p99 ", dim()),
            Span::styled(format!("{:.1} ms", fp99 as f64 / 1000.0), fg(th().text)),
            Span::styled("   uptime ", dim()),
            Span::styled(format!("{}s", app.started.elapsed().as_secs()), fg(th().text)),
            Span::styled(if s.stale { "   STALE" } else { "" }, bold(th().red)),
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
    let mut lines = vec![Line::from(Span::styled(
        format!("{}  {} · {} bytes", e.ty as char, fmt::type_name(e.ty), e.raw_len),
        bold(type_color(e.ty)),
    ))];
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
        lines
            .push(Line::from(Span::styled("order unknown to this client (joined after its Add)", fg(th().amber_text))));
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
