//! EVENTS screen: the firehose. Full-width per-message log with a filter
//! bar, and a right column with the compact inspector, the selected
//! order's lifecycle strip, and the full health diagnostics.

use crate::app::{App, TYPE_KEYS};
use crate::fmt;
use crate::theme::*;
use crate::ui::{draw_footer_keys, draw_header, draw_health, draw_inspector_panel, event_cols, event_line, pad_left, pad_right, panel, sym_name};
use feed_client::EventRecord;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Paragraph};
use ratatui::Frame;

pub fn draw(f: &mut Frame, app: &App) {
    let area = f.area();
    f.render_widget(Block::default().style(crate::theme::base()), area);
    let [hdr, body, foot] = Layout::vertical([Constraint::Length(1), Constraint::Min(0), Constraint::Length(1)]).areas(area);
    draw_header(f, app, hdr);
    draw_footer_keys(f, foot, &[("m", "market"), ("space", "pause"), ("↑↓", "select"), ("Enter", "inspect"), ("n p", "same order"), ("K", "packet"), ("1-5", "types"), ("o", "follow"), ("f", "symbol"), ("Esc", "live")]);

    let right_w = 64u16.min(body.width * 2 / 5);
    let [left, right] = Layout::horizontal([Constraint::Min(30), Constraint::Length(right_w)]).areas(body);
    draw_log(f, app, left);

    let insp_h = (right.height / 2).clamp(12, 24);
    let [insp, life, health] = Layout::vertical([Constraint::Length(insp_h), Constraint::Length(5), Constraint::Min(8)]).areas(right);
    draw_inspector_panel(f, app, insp);
    draw_lifecycle_strip(f, app, life);
    draw_health(f, app, health);
}

fn draw_log(f: &mut Frame, app: &App, area: Rect) {
    let total = app.snap.stats.messages;
    let sub = format!("window {} of {} · {} /s", fmt::commas(app.snap.events.len() as u64), fmt::commas(total), fmt::commas(app.snap.msgs_per_sec));
    let inner = panel(f, area, "EVENT LOG · every message, in stream order", &sub);
    if inner.height < 3 {
        return;
    }
    // Filter bar.
    let mut bar: Vec<Span> = vec![Span::styled("TYPES ", dim())];
    for (i, t) in TYPE_KEYS.iter().enumerate() {
        let on = app.type_mask & (1 << i) != 0;
        let (fgc, bgc) = if on { (Color::Rgb(7, 7, 7), type_color(*t)) } else { (DIMMER, Color::Rgb(26, 26, 26)) };
        bar.push(Span::styled(format!(" {} ", *t as char), Style::default().fg(fgc).bg(bgc).add_modifier(ratatui::style::Modifier::BOLD)));
        bar.push(Span::raw(" "));
    }
    bar.push(Span::styled("  SYMBOL ", dim()));
    bar.push(Span::styled(if app.filter_sym { app.current_symbol().map(sym_name).unwrap_or_default() } else { "all".into() }, fg(AMBER_BRIGHT)));
    bar.push(Span::styled("  FOLLOW ", dim()));
    match app.follow {
        Some(id) => bar.push(Span::styled(fmt::hex_id(id), fg(AMBER_BRIGHT))),
        None => bar.push(Span::styled("none · o on a row pins its order", dimmer())),
    }
    f.render_widget(Paragraph::new(Line::from(bar)), Rect { x: inner.x, y: inner.y, width: inner.width, height: 1 });

    let body = Rect { x: inner.x, y: inner.y + 1, width: inner.width, height: inner.height - 1 };
    let cols = event_cols(body.width);
    let header: String = cols.iter().map(|(n, w, r)| if *r { pad_left(n, *w as usize) } else { pad_right(n, *w as usize) }).collect::<Vec<_>>().join(" ");
    let mut lines = vec![Line::from(Span::styled(pad_right(&header, body.width as usize), dim()))];
    let rows = body.height as usize - 1;
    let vis = app.visible_events();
    let end = match app.selected {
        Some(seq) => vis.iter().position(|e| e.seq == seq).map(|i| (i + rows / 2 + 1).min(vis.len())).unwrap_or(vis.len()),
        None => vis.len(),
    };
    let start = end.saturating_sub(rows);
    let tickers: std::collections::HashMap<u32, &str> = app.snap.symbols.iter().map(|s| (s.symbol_id, s.ticker.as_str())).collect();
    for e in &vis[start..end] {
        let t = tickers.get(&e.symbol_id).copied().unwrap_or("");
        lines.push(event_line(e, &cols, t, app.selected == Some(e.seq), body.width));
    }
    f.render_widget(Paragraph::new(lines), body);
}

fn draw_lifecycle_strip(f: &mut Frame, app: &App, area: Rect) {
    let Some(e) = app.selected_event() else {
        let inner = panel(f, area, "ORDER", "n p walk · o follow");
        f.render_widget(Paragraph::new("select a row to see its order's lifecycle").style(dim()), inner);
        return;
    };
    let inner = panel(f, area, &format!("ORDER {} · in window", if e.order_id == 0 { "—".into() } else { fmt::hex_id(e.order_id) }), "n p walk · o follow");
    if e.order_id == 0 {
        return;
    }
    let same: Vec<&EventRecord> = app.snap.events.iter().filter(|x| !x.is_gap() && x.order_id == e.order_id).collect();
    let mut chain: Vec<Span> = Vec::new();
    for (i, x) in same.iter().enumerate() {
        let label = match x.ty {
            b'A' => format!(" A {} ", fmt::commas(x.qty as u64)),
            b'X' => format!(" X left {} ", fmt::commas(x.remaining as u64)),
            b'E' => format!(" E {} left {} ", fmt::commas(x.qty as u64), fmt::commas(x.remaining as u64)),
            b'U' => format!(" U {}@{} ", fmt::commas(x.qty as u64), fmt::commas(x.price)),
            b'D' => " D ".to_string(),
            _ => format!(" {} ", x.ty as char),
        };
        let here = x.seq == e.seq;
        chain.push(Span::styled(label, if here { bold(AMBER_BRIGHT).bg(SELECT_BG) } else { fg(type_color(x.ty)).bg(Color::Rgb(16, 16, 16)) }));
        if i + 1 < same.len() {
            chain.push(Span::styled(" → ", dimmer()));
        }
    }
    let first = same.first().map(|x| x.ts_ns).unwrap_or(e.ts_ns);
    let last = same.last().map(|x| x.ts_ns).unwrap_or(e.ts_ns);
    let info = Line::from(vec![
        Span::styled(format!("{} message(s) in window · first {} · span {}", same.len(), fmt::time_ms(first), fmt::micros(last.saturating_sub(first))), dim()),
    ]);
    f.render_widget(Paragraph::new(vec![Line::from(chain), info]), inner);
}
