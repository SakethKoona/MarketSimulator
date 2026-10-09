//! MARKET screen: ladder, candles with interval and VWAP, volume,
//! indicator tiles, flow charts, symbol monitor, tape, health strip.
//! Everything here moves at human speed; the firehose is on EVENTS.

use crate::app::App;
use crate::fmt;
use crate::theme::*;
use crate::ui::{
    draw_candles, draw_footer_keys, draw_header, draw_ladder, draw_tape, draw_volume, pad_left, pad_right, panel,
    sym_name,
};
use feed_client::{FlowSec, SymbolSnapshot};
use ratatui::layout::{Constraint, Layout, Rect};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Paragraph};
use ratatui::Frame;

pub fn draw(f: &mut Frame, app: &mut App) {
    let area = f.area();
    f.render_widget(Block::default().style(crate::theme::base()), area);
    let [hdr, body, foot] =
        Layout::vertical([Constraint::Length(1), Constraint::Min(0), Constraint::Length(1)]).areas(area);
    draw_header(f, app, hdr);
    draw_footer_keys(
        f,
        foot,
        &[
            ("q", "quit"),
            ("e", "events"),
            ("space", "pause"),
            ("[ ]", "symbol"),
            ("i", "interval"),
            ("d", "depth"),
            ("/", "symbol"),
            ("T", "themes"),
        ],
    );

    let left_w = 52u16.min(body.width / 3);
    let right_w = 66u16.min(body.width * 2 / 5);
    let [left, centre, right] =
        Layout::horizontal([Constraint::Length(left_w), Constraint::Min(24), Constraint::Length(right_w)]).areas(body);
    draw_ladder(f, app, left);

    let per_row = tiles_per_row(centre.width.saturating_sub(2));
    let tile_rows = 10usize.div_ceil(per_row);
    let tiles_h = (tile_rows * 3 + 3) as u16; // 3 lines per tile + border + title
    let chart_h = centre.height.saturating_sub(4 + tiles_h + 8).max(8);
    let [chart, vol, tiles, flow] = Layout::vertical([
        Constraint::Length(chart_h),
        Constraint::Length(4),
        Constraint::Length(tiles_h as u16),
        Constraint::Min(6),
    ])
    .areas(centre);
    draw_candles(f, app, chart);
    draw_volume(f, app, vol);
    draw_tiles(f, app, tiles);
    draw_flow(f, app, flow);

    let mon_h = (app.snap.symbols.len() as u16 + 4).clamp(6, 14);
    let [mon, tape, health] =
        Layout::vertical([Constraint::Length(mon_h), Constraint::Min(6), Constraint::Length(6)]).areas(right);
    draw_monitor(f, app, mon);
    draw_tape(f, app, tape);
    draw_health_strip(f, app, health);
}

// ------------------------------------------------------------ indicators

struct Tile {
    name: &'static str,
    value: String,
    chg: String,
    color: Color,
    chg_color: Color,
    spark: Vec<u64>,
    spark_color: Color,
}

fn last_n<'a>(flow: &'a [FlowSec], n: usize) -> &'a [FlowSec] {
    &flow[flow.len().saturating_sub(n)..]
}

fn tiles_for(sym: &SymbolSnapshot, depth_levels: usize) -> Vec<Tile> {
    let st = &sym.session;
    let flow = &sym.flow;
    let last60 = last_n(flow, 60);
    let closes: Vec<u64> = sym.bars[0].iter().rev().take(60).rev().map(|b| b.close).collect();
    let spark_rel = |v: &[u64]| -> Vec<u64> {
        let min = v.iter().copied().min().unwrap_or(0);
        v.iter().map(|x| x - min + 1).collect()
    };
    let chg = st.last as i64 - st.open as i64;
    let chg_pct = if st.open > 0 { chg as f64 / st.open as f64 * 100.0 } else { 0.0 };
    let vol = st.buy_vol + st.sell_vol;
    let vol60: u64 = last60.iter().map(|f| f.buy_vol + f.sell_vol).sum();
    let trades_s = last60.iter().rev().nth(1).map(|f| f.trades).unwrap_or(0);
    let avg_size = if sym.trades > 0 { sym.volume as f64 / sym.trades as f64 } else { 0.0 };
    let avg_size_prev: f64 = {
        let t: u32 = last60.iter().map(|f| f.trades).sum();
        let v: u64 = last60.iter().map(|f| f.buy_vol + f.sell_vol).sum();
        if t > 0 {
            v as f64 / t as f64
        } else {
            0.0
        }
    };
    let buy_share = if vol > 0 { st.buy_vol as f64 / vol as f64 * 100.0 } else { 0.0 };
    let spread_now = sym.spread().unwrap_or(0);
    let bid_depth: u64 = sym.bids.iter().take(depth_levels).map(|l| l.qty).sum();
    let ask_depth: u64 = sym.asks.iter().take(depth_levels).map(|l| l.qty).sum();
    let pressure = if bid_depth + ask_depth > 0 {
        (bid_depth as f64 - ask_depth as f64) / (bid_depth + ask_depth) as f64 * 100.0
    } else {
        0.0
    };
    let f1 = last60.iter().rev().nth(1).copied().unwrap_or_default();
    let vwap = st.vwap();
    let dir = |x: f64| if x >= 0.0 { th().bid } else { th().ask };
    vec![
        Tile {
            name: "LAST",
            value: fmt::commas(st.last),
            chg: format!("{} {}  {:+.2}%", if chg >= 0 { "▲" } else { "▼" }, chg.abs(), chg_pct),
            color: th().amber_bright,
            chg_color: dir(chg as f64),
            spark: spark_rel(&closes),
            spark_color: dir(chg as f64),
        },
        Tile {
            name: "VWAP",
            value: format!("{vwap:.1}"),
            chg: if st.last > 0 {
                format!(
                    "last {} by {:.1}",
                    if st.last as f64 >= vwap { "above" } else { "below" },
                    (st.last as f64 - vwap).abs()
                )
            } else {
                String::new()
            },
            color: th().text,
            chg_color: th().dim,
            spark: spark_rel(&closes),
            spark_color: th().amber_dim,
        },
        Tile {
            name: "HIGH / LOW",
            value: format!("{} / {}", fmt::commas(st.high), fmt::commas(st.low)),
            chg: format!("range {}", st.high.saturating_sub(st.low)),
            color: th().text,
            chg_color: th().dim,
            spark: last60.iter().map(|f| f.trades as u64).collect(),
            spark_color: th().dimmer,
        },
        Tile {
            name: "VOLUME",
            value: fmt::commas(vol),
            chg: format!("+{} last 60s", fmt::commas(vol60)),
            color: th().text,
            chg_color: th().dim,
            spark: last60.iter().map(|f| f.buy_vol + f.sell_vol).collect(),
            spark_color: th().mix_x,
        },
        Tile {
            name: "TRADES",
            value: fmt::commas(sym.trades),
            chg: format!("{} /s", fmt::commas(trades_s as u64)),
            color: th().text,
            chg_color: th().dim,
            spark: last60.iter().map(|f| f.trades as u64).collect(),
            spark_color: th().ask_bar_solid,
        },
        Tile {
            name: "AVG TRADE SIZE",
            value: format!("{avg_size:.1}"),
            chg: format!("{} {:.1} last 60s", if avg_size_prev >= avg_size { "▲" } else { "▼" }, avg_size_prev),
            color: th().text,
            chg_color: dir(avg_size_prev - avg_size),
            spark: last60
                .iter()
                .map(|f| if f.trades > 0 { (f.buy_vol + f.sell_vol) / f.trades as u64 } else { 0 })
                .collect(),
            spark_color: th().dimmer,
        },
        Tile {
            name: "BUY VOL SHARE",
            value: format!("{buy_share:.0}%"),
            chg: "aggressor side".into(),
            color: dir(buy_share - 50.0),
            chg_color: th().dim,
            spark: last60
                .iter()
                .map(|f| if f.buy_vol + f.sell_vol > 0 { f.buy_vol * 100 / (f.buy_vol + f.sell_vol) } else { 50 })
                .collect(),
            spark_color: th().bid_bar_solid,
        },
        Tile {
            name: "AVG SPREAD",
            value: format!("{:.1}", st.avg_spread()),
            chg: format!("now {spread_now}"),
            color: th().text,
            chg_color: th().dim,
            spark: last60.iter().map(|f| if f.spread_n > 0 { f.spread_sum / f.spread_n as u64 } else { 0 }).collect(),
            spark_color: th().mix_x,
        },
        Tile {
            name: "BOOK PRESSURE",
            value: format!("{pressure:+.0}%"),
            chg: format!("{} heavy · top {}", if pressure >= 0.0 { "bid" } else { "ask" }, depth_levels),
            color: dir(pressure),
            chg_color: th().dim,
            spark: Vec::new(),
            spark_color: th().dimmer,
        },
        Tile {
            name: "ORDER FLOW /s",
            value: format!("{} A", fmt::commas(f1.adds as u64)),
            chg: format!(
                "{} X/D {} E {} U",
                fmt::commas(f1.cancels as u64),
                fmt::commas(f1.execs as u64),
                fmt::commas(f1.replaces as u64)
            ),
            color: th().text,
            chg_color: th().dim,
            spark: last60.iter().map(|f| f.messages as u64).collect(),
            spark_color: th().mix_d,
        },
    ]
}

fn tiles_per_row(width: u16) -> usize {
    if width >= 120 {
        5
    } else if width >= 72 {
        3
    } else {
        2
    }
}

fn draw_tiles(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "INDICATORS · session", "1 Hz · value · change · last 60s");
    let Some(sym) = app.slow.symbols.get(app.sym_idx) else { return };
    let tiles = tiles_for(sym, app.depth().min(sym.bids.len().max(sym.asks.len()).max(1)));
    let per_row = tiles_per_row(inner.width);
    let tile_w = (inner.width as usize / per_row).max(12);
    const TILE_H: usize = 3; // name / value + change / sparkline
    for (i, t) in tiles.iter().enumerate() {
        let r = i / per_row;
        let c = i % per_row;
        let x = inner.x + (c * tile_w) as u16;
        let y = inner.y + (r * TILE_H) as u16;
        if y + TILE_H as u16 > inner.y + inner.height || x + tile_w as u16 > inner.x + inner.width + 1 {
            continue;
        }
        let w = tile_w.saturating_sub(2);
        let value = Line::from(vec![
            Span::styled(t.value.clone(), bold(t.color)),
            Span::raw("  "),
            Span::styled(t.chg.clone(), fg(t.chg_color)),
        ]);
        let lines = vec![
            Line::from(Span::styled(pad_right(t.name, w), dim())),
            value,
            Line::from(Span::styled(fmt::spark(&t.spark, w.min(28)), fg(t.spark_color))),
        ];
        let rect = Rect { x, y, width: w as u16, height: TILE_H as u16 };
        f.render_widget(Paragraph::new(lines), rect);
    }
}

// ------------------------------------------------------------ flow

fn draw_flow(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "FLOW · per second", "last 90s");
    let Some(sym) = app.slow.symbols.get(app.sym_idx) else { return };
    let flow = last_n(&sym.flow, 90);
    let col_w = inner.width / 3;
    let cols = [
        Rect { x: inner.x, y: inner.y, width: col_w, height: inner.height },
        Rect { x: inner.x + col_w, y: inner.y, width: col_w, height: inner.height },
        Rect { x: inner.x + 2 * col_w, y: inner.y, width: inner.width - 2 * col_w, height: inner.height },
    ];
    let trades: Vec<u64> = flow.iter().map(|x| x.trades as u64).collect();
    let buys: Vec<u64> = flow.iter().map(|x| x.buy_vol).collect();
    let sells: Vec<u64> = flow.iter().map(|x| x.sell_vol).collect();
    let spreads: Vec<u64> =
        flow.iter().map(|x| if x.spread_n > 0 { x.spread_sum / x.spread_n as u64 } else { 0 }).collect();
    let now_trades = flow.iter().rev().nth(1).map(|x| x.trades).unwrap_or(0);
    let tot_b: u64 = buys.iter().sum();
    let tot_s: u64 = sells.iter().sum();
    let (pb, ps) =
        if tot_b + tot_s > 0 { (tot_b * 100 / (tot_b + tot_s), tot_s * 100 / (tot_b + tot_s)) } else { (50, 50) };

    bar_chart(f, cols[0], "TRADES/s", &fmt::commas(now_trades as u64), &[(&trades, th().ask_bar_solid)], false);
    bar_chart(
        f,
        cols[1],
        "BUY·SELL VOL/s",
        &format!("{pb}%/{ps}%"),
        &[(&buys, th().bid_bar_solid), (&sells, th().ask_bar_solid)],
        true,
    );
    bar_chart(
        f,
        cols[2],
        "SPREAD",
        &format!("{} avg {:.1}", sym.spread().unwrap_or(0), sym.session.avg_spread()),
        &[(&spreads, th().mix_x)],
        false,
    );
}

/// Vertical bars at one column per second, right-aligned. With `stacked`
/// the second series sits under the first.
fn bar_chart(f: &mut Frame, area: Rect, title: &str, value: &str, series: &[(&[u64], Color)], stacked: bool) {
    if area.height < 3 || area.width < 8 {
        return;
    }
    let w = area.width as usize - 2;
    let title_w = w.saturating_sub(value.chars().count() + 1);
    // Title yields to the value when the column is too narrow for both.
    let title_shown = if title.chars().count() <= title_w {
        title
    } else if title_w >= 6 {
        &title[..title.char_indices().nth(title_w - 1).map(|(i, _)| i).unwrap_or(0)]
    } else {
        ""
    };
    let head = Line::from(vec![
        Span::styled(pad_right(title_shown, title_w), dim()),
        Span::raw(" "),
        Span::styled(value.to_string(), fg(th().text)),
    ]);
    f.render_widget(Paragraph::new(head), Rect { x: area.x + 1, y: area.y, width: w as u16, height: 1 });
    let plot = Rect { x: area.x + 1, y: area.y + 1, width: w as u16, height: area.height - 1 };
    let n = series[0].0.len().min(w);
    let start = series[0].0.len() - n;
    let totals: Vec<u64> = (start..series[0].0.len())
        .map(|i| series.iter().map(|(s, _)| s.get(i).copied().unwrap_or(0)).sum::<u64>())
        .collect();
    let max = if stacked {
        totals.iter().copied().max().unwrap_or(0)
    } else {
        series.iter().flat_map(|(s, _)| s[start..].iter().copied()).max().unwrap_or(0)
    }
    .max(1);
    let rows = plot.height as usize * 2; // half-block resolution
    let buf = f.buffer_mut();
    for (k, i) in (start..series[0].0.len()).enumerate() {
        let x = plot.x + (w - n + k) as u16;
        let mut base = 0usize;
        for (s, color) in series {
            let v = s.get(i).copied().unwrap_or(0);
            let h = ((v as f64 / max as f64) * rows as f64).round() as usize;
            if h == 0 {
                continue;
            }
            // fill sub-rows [base, base+h) from the bottom
            for sub in base..(base + h).min(rows) {
                let row = plot.y + plot.height - 1 - (sub / 2) as u16;
                let top_half = sub % 2 == 1;
                if let Some(c) = buf.cell_mut((x, row)) {
                    let cur = c.symbol().to_string();
                    let prev_fg = c.fg;
                    match (cur.as_str(), top_half) {
                        // Lower half already drawn by the series below: keep
                        // its colour as background and draw the top half.
                        ("▄", true) => {
                            c.set_symbol("▀").set_fg(*color).set_bg(prev_fg);
                        }
                        ("▀", false) => {
                            c.set_symbol("▄").set_fg(*color).set_bg(prev_fg);
                        }
                        (_, true) => {
                            c.set_symbol("▀").set_fg(*color).set_bg(th().bg);
                        }
                        (_, false) => {
                            c.set_symbol("▄").set_fg(*color).set_bg(th().bg);
                        }
                    }
                }
            }
            if !stacked {
                break;
            }
            base += h;
        }
    }
}

// ------------------------------------------------------------ monitor

fn draw_monitor(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "SYMBOL MONITOR", "1 Hz · all symbols");
    let w = inner.width as usize;
    let wide = w >= 62;
    let head = if wide {
        format!(
            "{} {} {} {} {} {} {} {}",
            pad_right("SYM", 6),
            pad_left("LAST", 8),
            pad_left("CHG", 6),
            pad_left("th().bid", 8),
            pad_left("th().ask", 8),
            pad_left("SPR", 4),
            pad_left("VOL", 10),
            pad_left("MSG/s", 7)
        )
    } else {
        format!(
            "{} {} {} {} {}",
            pad_right("SYM", 6),
            pad_left("LAST", 8),
            pad_left("CHG", 6),
            pad_left("SPR", 4),
            pad_left("VOL", 10)
        )
    };
    let mut lines = vec![Line::from(Span::styled(pad_right(&head, w), dim()))];
    for (i, s) in app.slow.symbols.iter().enumerate() {
        let st = &s.session;
        let chg = st.last as i64 - st.open as i64;
        let c = if chg >= 0 { th().bid } else { th().ask };
        let bg = if i == app.sym_idx { th().select_bg } else { th().bg };
        let last_style = match app.last_px_flash(s.symbol_id) {
            Some((buy, age)) => Style::default().fg(th().text_bright).bg(blend_bg(
                bg,
                if buy { th().bid_bar_solid } else { th().ask_bar_solid },
                age,
            )),
            None => Style::default().fg(th().text).bg(bg),
        };
        let st_ = |col: Color| Style::default().fg(col).bg(bg);
        let closes: Vec<u64> = s.bars[0].iter().rev().take(8).rev().map(|b| b.close).collect();
        let min = closes.iter().copied().min().unwrap_or(0);
        let spark: Vec<u64> = closes.iter().map(|x| x - min + 1).collect();
        let mut spans = vec![
            Span::styled(
                pad_right(&sym_name(s), 6),
                if i == app.sym_idx { bold(th().amber_bright).bg(bg) } else { st_(th().text) },
            ),
            Span::styled(format!(" {}", pad_left(&fmt::commas(st.last), 8)), last_style),
            Span::styled(format!(" {}", pad_left(&format!("{chg:+}"), 6)), st_(c)),
        ];
        if wide {
            spans.push(Span::styled(
                format!(" {}", pad_left(&s.best_bid().map(|l| fmt::commas(l.price)).unwrap_or_default(), 8)),
                st_(th().bid_text),
            ));
            spans.push(Span::styled(
                format!(" {}", pad_left(&s.best_ask().map(|l| fmt::commas(l.price)).unwrap_or_default(), 8)),
                st_(th().ask_text),
            ));
        }
        spans.push(Span::styled(
            format!(" {}", pad_left(&s.spread().map(|x| x.to_string()).unwrap_or_default(), 4)),
            st_(th().dim),
        ));
        spans.push(Span::styled(format!(" {}", pad_left(&fmt::commas(s.volume), 10)), st_(th().text)));
        if wide {
            spans.push(Span::styled(format!(" {}", pad_left(&fmt::commas(s.msgs_per_sec as u64), 7)), st_(th().dim)));
        }
        spans.push(Span::styled(format!(" {}", fmt::spark(&spark, 8)), st_(c)));
        let used: usize = spans.iter().map(|x| x.content.chars().count()).sum();
        spans.push(Span::styled(" ".repeat(w.saturating_sub(used)), Style::default().bg(bg)));
        lines.push(Line::from(spans));
    }
    f.render_widget(Paragraph::new(lines), inner);
}

fn blend_bg(base: Color, flash: Color, age: f32) -> Color {
    let (Color::Rgb(r1, g1, b1), Color::Rgb(r2, g2, b2)) = (base, flash) else { return base };
    let k = (1.0 - age.clamp(0.0, 1.0)) * 0.55;
    Color::Rgb(
        (r1 as f32 + (r2 as f32 - r1 as f32) * k) as u8,
        (g1 as f32 + (g2 as f32 - g1 as f32) * k) as u8,
        (b1 as f32 + (b2 as f32 - b1 as f32) * k) as u8,
    )
}

// ------------------------------------------------------------ health strip

fn draw_health_strip(f: &mut Frame, app: &App, area: Rect) {
    let inner = panel(f, area, "FEED HEALTH", "e for the event screen");
    let s = &app.snap;
    let w = inner.width as usize;
    let sparkw = w.saturating_sub(8 + 12);
    let (_, fp99) = app.frame_percentiles();
    let lines = vec![
        Line::from(vec![
            Span::styled(pad_right("msgs/s", 8), dim()),
            Span::styled(fmt::spark(&s.rate_hist, sparkw), fg(th().bid)),
            Span::styled(pad_left(&fmt::commas(s.msgs_per_sec), 12), fg(th().text)),
        ]),
        Line::from(vec![
            Span::styled(pad_right("lat p99", 8), dim()),
            Span::styled(fmt::spark(&s.p99_hist, sparkw), fg(th().amber)),
            Span::styled(pad_left(&fmt::micros(s.latency_p99_ns), 12), fg(th().text)),
        ]),
        Line::from(vec![
            Span::styled("frame ", dim()),
            Span::styled(format!("{:.1} ms", fp99 as f64 / 1000.0), fg(th().text)),
            Span::styled("   ring ", dim()),
            Span::styled(format!("{:.0}%", s.ring_occupancy * 100.0), fg(th().text)),
            Span::styled("   drops ", dim()),
            Span::styled(fmt::commas(s.ring_drops), fg(if s.ring_drops > 0 { th().red } else { th().green })),
            Span::styled("   gaps ", dim()),
            Span::styled(s.stats.gaps.to_string(), fg(if s.stats.gaps > 0 { th().red } else { th().green })),
            Span::styled("   dups ", dim()),
            Span::styled(fmt::commas(s.stats.duplicates), fg(th().text)),
            Span::styled(if s.stale { "   STALE" } else { "" }, bold(th().red)),
            Span::styled(if s.stats.recovering { "   SYNCING" } else { "" }, bold(th().amber)),
        ]),
    ];
    f.render_widget(Paragraph::new(lines), inner);
}
