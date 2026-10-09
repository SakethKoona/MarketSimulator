//! Full-screen event inspector: decoded fields and hex, order lifecycle,
//! book impact, match sides, packet context, timing, spec.

use crate::app::App;
use crate::decode;
use crate::fmt;
use crate::theme::*;
use crate::ui::{event_cols, event_line, hex_lines, level_before_after, panel};
use feed_client::EventRecord;
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Paragraph, Wrap};
use ratatui::Frame;

fn pad_left(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w {
        s.chars().take(w).collect()
    } else {
        format!("{}{}", " ".repeat(w - n), s)
    }
}
fn pad_right(s: &str, w: usize) -> String {
    let n = s.chars().count();
    if n >= w {
        s.chars().take(w).collect()
    } else {
        format!("{}{}", s, " ".repeat(w - n))
    }
}

pub fn draw(f: &mut Frame, app: &App) {
    let area = f.area();
    f.render_widget(ratatui::widgets::Block::default().style(crate::theme::base()), area);
    let Some(e) = app.selected_event() else { return };
    let ticker =
        app.snap.symbols.iter().find(|s| s.symbol_id == e.symbol_id).map(|s| s.ticker.clone()).unwrap_or_default();
    let [hdr, body, foot] =
        Layout::vertical([Constraint::Length(1), Constraint::Min(0), Constraint::Length(1)]).areas(area);

    // Header.
    let kv = |k: &str, v: String| {
        vec![
            Span::styled(format!("  {k} "), Style::default().fg(th().amber_dim).bg(th().hdr_bg)),
            Span::styled(v, Style::default().fg(th().amber_text).bg(th().hdr_bg)),
        ]
    };
    let mut spans = vec![Span::styled(" FEEDVIZ ", bold(th().amber_bright).bg(th().hdr_bg))];
    spans.extend(kv("INSPECT", format!("seq {} · ", fmt::commas(e.seq))));
    spans.push(Span::styled(
        format!("{} {}", e.ty as char, fmt::type_name(e.ty)),
        bold(type_color(e.ty)).bg(th().hdr_bg),
    ));
    spans.push(Span::styled(
        format!(" · {}", if ticker.is_empty() { format!("#{}", e.symbol_id) } else { ticker.clone() }),
        Style::default().fg(th().amber_text).bg(th().hdr_bg),
    ));
    spans.extend(kv(
        "PACKET",
        format!("seq {} · {} msgs · block {}", fmt::commas(e.packet_seq), e.packet_count, e.block),
    ));
    let right = vec![
        Span::styled(format!("{}  ", fmt::time_ms(e.ts_ns)), Style::default().fg(th().amber_text).bg(th().hdr_bg)),
        Span::styled(" ❚❚ PAUSED ", bold(th().pause_fg).bg(th().pause_bg)),
        Span::styled(" ", Style::default().bg(th().hdr_bg)),
    ];
    let l: usize = spans.iter().map(|x| x.content.chars().count()).sum();
    let r: usize = right.iter().map(|x| x.content.chars().count()).sum();
    spans.push(Span::styled(" ".repeat((hdr.width as usize).saturating_sub(l + r)), Style::default().bg(th().hdr_bg)));
    spans.extend(right);
    f.render_widget(Paragraph::new(Line::from(spans)).style(Style::default().bg(th().hdr_bg)), hdr);

    // Footer.
    let keys =
        [("Esc", "back"), ("↑↓", "prev/next event"), ("n p", "next/prev for this order"), ("K", "first in packet")];
    let mut fs = vec![Span::styled(" ", Style::default().bg(th().panel_hdr_bg))];
    for (k, v) in keys {
        fs.push(Span::styled(k, bold(th().amber).bg(th().panel_hdr_bg)));
        fs.push(Span::styled(format!(" {v}   "), Style::default().fg(th().dim).bg(th().panel_hdr_bg)));
    }
    f.render_widget(Paragraph::new(Line::from(fs)).style(Style::default().bg(th().panel_hdr_bg)), foot);

    let left_w = 64u16.min(body.width * 2 / 5);
    let right_w = 54u16.min(body.width / 4);
    let [left, centre, right] =
        Layout::horizontal([Constraint::Length(left_w), Constraint::Min(20), Constraint::Length(right_w)]).areas(body);

    // ---- Left: decoded + raw
    let fields = decode::fields(&e, &ticker);
    let dec_h = (fields.len() as u16 * 2 + 4).min(left.height.saturating_sub(6));
    let [dec, raw] = Layout::vertical([Constraint::Length(dec_h), Constraint::Min(4)]).areas(left);
    let inner = panel(f, dec, &format!("DECODED · {} bytes", e.raw_len), "little-endian");
    let mut lines = vec![Line::from(Span::styled(
        format!(
            "{} {} {} {} VALUE",
            pad_right("OFF", 3),
            pad_right("LEN", 3),
            pad_right("FIELD", 14),
            pad_right("TYPE", 5)
        ),
        dim(),
    ))];
    for fl in &fields {
        lines.push(Line::from(vec![
            Span::styled(pad_right(&fl.off.to_string(), 3), dimmer()),
            Span::raw(" "),
            Span::styled(pad_right(&fl.len.to_string(), 3), dimmer()),
            Span::raw(" "),
            Span::styled(pad_right(fl.name, 14), bold(fl.color)),
            Span::raw(" "),
            Span::styled(pad_right(fl.ty, 5), dimmer()),
            Span::raw(" "),
            Span::styled(fl.value.clone(), fg(th().text_bright)),
        ]));
        if !fl.note.is_empty() {
            lines.push(Line::from(vec![Span::raw("      "), Span::styled(fl.note.clone(), dim())]));
        }
    }
    f.render_widget(Paragraph::new(lines), inner);
    let inner = panel(f, raw, "RAW BYTES · colour = field", "");
    let per = ((inner.width as usize).saturating_sub(4) / 3).clamp(8, 16);
    f.render_widget(Paragraph::new(hex_lines(&e, &fields, per)), inner);

    // ---- Centre: lifecycle, book impact, match
    let [life, impact, matchp] =
        Layout::vertical([Constraint::Min(8), Constraint::Length(9), Constraint::Length(7)]).areas(centre);
    draw_lifecycle(f, app, &e, &ticker, life);
    draw_impact(f, &e, &ticker, impact);
    draw_match(f, app, &e, matchp);

    // ---- Right: packet, timing, spec
    let [pkt, timing, spec] =
        Layout::vertical([Constraint::Min(6), Constraint::Length(9), Constraint::Min(5)]).areas(right);
    draw_packet(f, app, &e, pkt);
    draw_timing(f, &e, timing);
    let inner = panel(f, spec, "SPEC", "feed-v1.md");
    f.render_widget(Paragraph::new(decode::spec_text(e.ty)).style(fg(th().muted)).wrap(Wrap { trim: true }), inner);
}

use ratatui::style::Color;

fn draw_lifecycle(f: &mut Frame, app: &App, e: &EventRecord, ticker: &str, area: Rect) {
    let title = if e.order_id == 0 {
        "ORDER LIFECYCLE".to_string()
    } else {
        format!("ORDER LIFECYCLE · {}", fmt::hex_id(e.order_id))
    };
    let sub = if e.order_id == 0 {
        String::new()
    } else {
        format!(
            "shard {} · seq {} · {} {}",
            fmt::shard_of(e.order_id),
            fmt::seq_of(e.order_id),
            fmt::side_name(e.side),
            fmt::commas(e.price)
        )
    };
    let inner = panel(f, area, &title, &sub);
    if e.order_id == 0 {
        f.render_widget(Paragraph::new("not an order message").style(dim()), inner);
        return;
    }
    let same: Vec<&EventRecord> = app.snap.events.iter().filter(|x| !x.is_gap() && x.order_id == e.order_id).collect();
    // Chain line.
    let mut chain: Vec<Span> = Vec::new();
    for (i, x) in same.iter().enumerate() {
        let label = match x.ty {
            b'A' => format!(" A add {} ", fmt::commas(x.qty as u64)),
            b'X' => format!(" X left {} ", fmt::commas(x.remaining as u64)),
            b'E' => format!(" E {} left {} ", fmt::commas(x.qty as u64), fmt::commas(x.remaining as u64)),
            b'U' => format!(" U {}@{} ", fmt::commas(x.qty as u64), fmt::commas(x.price)),
            b'D' => " D gone ".to_string(),
            _ => format!(" {} ", x.ty as char),
        };
        let here = x.seq == e.seq;
        let st =
            if here { bold(th().amber_bright).bg(th().select_bg) } else { fg(type_color(x.ty)).bg(th().panel_alt) };
        chain.push(Span::styled(label, st));
        if i + 1 < same.len() {
            chain.push(Span::styled(" → ", dimmer()));
        }
    }
    let mut lines = vec![Line::from(chain), Line::raw("")];
    let cols = event_cols(inner.width);
    let header: String = cols
        .iter()
        .map(|(n, w, r)| if *r { pad_left(n, *w as usize) } else { pad_right(n, *w as usize) })
        .collect::<Vec<_>>()
        .join(" ");
    lines.push(Line::from(Span::styled(pad_right(&header, inner.width as usize), dim())));
    let rows = inner.height.saturating_sub(3) as usize;
    let pos = same.iter().position(|x| x.seq == e.seq).unwrap_or(0);
    let start = pos.saturating_sub(rows.saturating_sub(1) / 2).min(same.len().saturating_sub(rows));
    for x in same.iter().skip(start).take(rows) {
        lines.push(event_line(x, &cols, ticker, x.seq == e.seq, inner.width));
    }
    if same.len() == 1 && e.ty != b'A' {
        lines.push(Line::from(Span::styled("earlier history is outside this client's event window", dim())));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

fn draw_impact(f: &mut Frame, e: &EventRecord, ticker: &str, area: Rect) {
    let inner = panel(
        f,
        area,
        &format!("BOOK IMPACT · {} {} {}", ticker, fmt::side_name(e.side), fmt::commas(e.price)),
        "before → after this message",
    );
    let Some((b, a)) = level_before_after(e) else {
        let msg = match e.ty {
            b'U' => "replace: order removed from its old level and appended at the back of the new one",
            b'E' | b'X' | b'D' if !e.applied.known => {
                "order unknown to this client (joined after its Add), level unchanged"
            }
            _ => "no level change",
        };
        f.render_widget(Paragraph::new(msg).style(dim()).wrap(Wrap { trim: true }), inner);
        return;
    };
    let c = side_color(e.side);
    let maxq = b.0.max(a.0).max(1);
    let barw = (inner.width as usize / 2).saturating_sub(24).max(4);
    let bar = |q: u64| " ".repeat(((q as f64 / maxq as f64) * barw as f64).round() as usize);
    let lines = vec![
        Line::from(vec![
            Span::styled(pad_right("PX", 9), dim()),
            Span::styled(pad_right("QTY", 10), dim()),
            Span::styled("ORD", dim()),
        ]),
        Line::from(vec![
            Span::styled(pad_right("before", 9), dim()),
            Span::styled(pad_right(&fmt::commas(b.0), 10), fg(c)),
            Span::styled(pad_right(&b.1.to_string(), 4), dim()),
            Span::styled(bar(b.0), Style::default().bg(if e.side == b'B' { th().bid_bar } else { th().ask_bar })),
        ]),
        Line::from(vec![
            Span::styled(pad_right("after", 9), dim()),
            Span::styled(pad_right(&fmt::commas(a.0), 10), fg(c)),
            Span::styled(pad_right(&a.1.to_string(), 4), dim()),
            Span::styled(bar(a.0), Style::default().bg(if e.side == b'B' { th().bid_bar } else { th().ask_bar })),
        ]),
        Line::raw(""),
        Line::from(Span::styled(
            match e.ty {
                b'A' => "new order appended at the back of this level".to_string(),
                b'E' if e.remaining == 0 => "resting order fully consumed and removed from the level".to_string(),
                b'E' => format!("resting order shrunk in place to {}, priority kept", fmt::commas(e.remaining as u64)),
                b'X' => format!("owner reduced the order to {}, priority kept", fmt::commas(e.remaining as u64)),
                b'D' => "order removed; level deleted if it was the last".to_string(),
                _ => String::new(),
            },
            dim(),
        )),
    ];
    f.render_widget(Paragraph::new(lines), inner);
}

fn draw_match(f: &mut Frame, app: &App, e: &EventRecord, area: Rect) {
    let inner = panel(
        f,
        area,
        &format!("MATCH {}", if e.match_id == 0 { String::new() } else { fmt::hex_id(e.match_id) }),
        "both sides",
    );
    if e.ty != b'E' {
        f.render_widget(Paragraph::new("not an execution").style(dim()), inner);
        return;
    }
    let aggr = if e.side == b'S' { "BUY" } else { "SELL" };
    let other: Vec<&EventRecord> =
        app.snap.events.iter().filter(|x| !x.is_gap() && x.match_id == e.match_id && x.seq != e.seq).collect();
    let mut lines = vec![
        Line::from(vec![
            Span::styled(format!("RESTING · {} · this order  ", fmt::side_name(e.side)), bold(side_color(e.side))),
            Span::styled(fmt::hex_id(e.order_id), fg(th().muted)),
        ]),
        Line::from(vec![
            Span::styled("filled ", dim()),
            Span::styled(format!("{} @ {}", fmt::commas(e.qty as u64), fmt::commas(e.price)), fg(th().text)),
            Span::styled("   leaves ", dim()),
            Span::styled(fmt::commas(e.remaining as u64), fg(th().text)),
        ]),
        Line::raw(""),
        Line::from(vec![
            Span::styled(
                format!("AGGRESSOR · {} · inferred  ", aggr),
                bold(side_color(if e.side == b'S' { b'B' } else { b'S' })),
            ),
            Span::styled("not on the public feed; the BOE session reports it to its owner by this match id", dim()),
        ]),
    ];
    if !other.is_empty() {
        lines.push(Line::from(Span::styled(format!("{} other execution(s) share this match id", other.len()), dim())));
    }
    f.render_widget(Paragraph::new(lines).wrap(Wrap { trim: true }), inner);
}

fn draw_packet(f: &mut Frame, app: &App, e: &EventRecord, area: Rect) {
    let session = String::from_utf8_lossy(&app.snap.stats.session).trim().to_string();
    let inner =
        panel(f, area, &format!("MOLD PACKET · seq {}", fmt::commas(e.packet_seq)), &format!("session {session}"));
    let blocks: Vec<&EventRecord> =
        app.snap.events.iter().filter(|x| !x.is_gap() && x.packet_seq == e.packet_seq).collect();
    let tickers: std::collections::HashMap<u32, &str> =
        app.snap.symbols.iter().map(|s| (s.symbol_id, s.ticker.as_str())).collect();
    let mut lines = vec![Line::from(Span::styled(
        format!("{} {} {}  SUMMARY", pad_right("BLK", 3), pad_right("SEQ", 8), "TY"),
        dim(),
    ))];
    let rows = inner.height.saturating_sub(2) as usize;
    let pos = blocks.iter().position(|x| x.seq == e.seq).unwrap_or(0);
    let start = pos.saturating_sub(rows / 2).min(blocks.len().saturating_sub(rows));
    for x in blocks.iter().skip(start).take(rows) {
        let t = tickers.get(&x.symbol_id).copied().unwrap_or("");
        let sum = match x.ty {
            b'A' | b'U' => {
                format!("{} {} {} @ {}", t, fmt::side_name(x.side), fmt::commas(x.qty as u64), fmt::commas(x.price))
            }
            b'E' => format!(
                "{} {} {} @ {} left {}",
                t,
                fmt::side_name(x.side),
                fmt::commas(x.qty as u64),
                fmt::commas(x.price),
                fmt::commas(x.remaining as u64)
            ),
            b'X' => format!("{} {} left {}", t, fmt::short_id(x.order_id), fmt::commas(x.remaining as u64)),
            b'D' => format!("{} {} deleted", t, fmt::short_id(x.order_id)),
            b'R' => format!("directory {}", x.symbol_id),
            b'S' => "system event".to_string(),
            _ => String::new(),
        };
        let here = x.seq == e.seq;
        let bg = if here { th().select_bg } else { th().bg };
        lines.push(Line::from(vec![
            Span::styled(pad_right(&x.block.to_string(), 3), dimmer().bg(bg)),
            Span::styled(" ", Style::default().bg(bg)),
            Span::styled(pad_right(&fmt::commas(x.seq), 8), dim().bg(bg)),
            Span::styled(" ", Style::default().bg(bg)),
            Span::styled(format!("{} ", x.ty as char), bold(type_color(x.ty)).bg(bg)),
            Span::styled(pad_right(&sum, (inner.width as usize).saturating_sub(16)), fg(th().muted).bg(bg)),
        ]));
    }
    lines.push(Line::from(vec![
        Span::styled("count ", dim()),
        Span::styled(e.packet_count.to_string(), fg(th().text)),
        Span::styled("  next ", dim()),
        Span::styled(fmt::commas(e.packet_seq + e.packet_count as u64), fg(th().text)),
        Span::styled("  in window ", dim()),
        Span::styled(blocks.len().to_string(), fg(th().text)),
    ]));
    f.render_widget(Paragraph::new(lines), inner);
}

fn draw_timing(f: &mut Frame, e: &EventRecord, area: Rect) {
    let inner = panel(f, area, "TIMING", "wall clock");
    let recv = if e.recv_ns > e.ts_ns { e.recv_ns - e.ts_ns } else { 0 };
    let barw = (inner.width as usize).saturating_sub(26).max(4);
    let total = recv.max(1) as f64;
    let seg = |from: u64, to: u64| {
        let a = ((from as f64 / total) * barw as f64).round() as usize;
        let b = ((to as f64 / total) * barw as f64).round() as usize;
        (a.min(barw), b.clamp(a.min(barw), barw))
    };
    let row = |name: &str, from: u64, to: u64, c: Color, val: String| {
        let (a, b) = seg(from, to);
        Line::from(vec![
            Span::styled(pad_right(name, 14), dim()),
            Span::raw(" ".repeat(a)),
            Span::styled(" ".repeat((b - a).max(1)), Style::default().bg(c)),
            Span::raw(" ".repeat(barw.saturating_sub(b.max(a + 1)))),
            Span::styled(pad_left(&val, 11), fg(c)),
        ])
    };
    let lines = if recv == 0 {
        vec![
            Line::from(Span::styled("engine ts_ns  ", dim())).patch_style(fg(th().text)),
            Line::from(Span::styled(fmt::time_ns(e.ts_ns), fg(th().text))),
            Line::from(Span::styled("no receive timestamp (replay)", dim())),
        ]
    } else {
        vec![
            row("engine ts_ns", 0, 0, th().dim, "0".into()),
            row("socket recv", 0, recv, th().amber, format!("+{}", fmt::micros(recv))),
            Line::raw(""),
            Line::from(vec![Span::styled("engine ", dim()), Span::styled(fmt::time_ns(e.ts_ns), fg(th().text))]),
            Line::from(vec![Span::styled("recv   ", dim()), Span::styled(fmt::time_ns(e.recv_ns), fg(th().text))]),
            Line::from(Span::styled("book apply and frame times are in FEED HEALTH", dimmer())),
        ]
    };
    f.render_widget(Paragraph::new(lines), inner);
}
