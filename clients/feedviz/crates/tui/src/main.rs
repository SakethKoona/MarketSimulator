//! feedviz: terminal visualizer for the MarketSimulator feed.
//!
//!   feedviz                              join 239.1.1.1:30001 on the default interface
//!   feedviz --iface 127.0.0.1            same-host testing on macOS
//!   feedviz --replay testdata/capture.bin
//!   feedviz --headless [--seconds S]     print top of book instead of drawing

mod app;
mod decode;
mod events;
mod fmt;
mod inspect;
mod market;
mod picker;
mod theme;
mod ui;

use anyhow::{bail, Result};
use app::App;
use crossterm::event::{self, Event, KeyEventKind};
use crossterm::execute;
use crossterm::terminal::{BeginSynchronizedUpdate, EndSynchronizedUpdate};
use feed_client::{Config, FeedClient, Snapshot, Source};
use std::io;
use std::net::Ipv4Addr;
use std::path::PathBuf;
use std::time::{Duration, Instant};

struct Args {
    source: Source,
    depth: usize,
    seconds: f64,
    interval_ms: u64,
    headless: bool,
    fps: u64,
}

fn parse_args() -> Result<Args> {
    let mut group = Ipv4Addr::new(239, 1, 1, 1);
    let mut port = 30001u16;
    let mut iface = Ipv4Addr::UNSPECIFIED;
    let mut replay: Option<PathBuf> = None;
    let mut depth = 40usize;
    let mut seconds = 0.0f64;
    let mut interval_ms = 1000u64;
    let mut headless = false;
    let mut fps = 60u64;
    let mut theme_name: Option<String> = None;
    let mut exchange_host = Ipv4Addr::new(127, 0, 0, 1);
    let mut retransmit_port = 30002u16;
    let mut snapshot_port = 30003u16;
    let mut session = *b"MKTSIM0001";
    let mut no_snapshot = false;
    let mut drop_every = 0u32;
    let mut it = std::env::args().skip(1);
    while let Some(a) = it.next() {
        let mut val = || it.next().ok_or_else(|| anyhow::anyhow!("missing value for {a}"));
        match a.as_str() {
            "--group" => group = val()?.parse()?,
            "--port" => port = val()?.parse()?,
            "--iface" => iface = val()?.parse()?,
            "--replay" => replay = Some(PathBuf::from(val()?)),
            "--depth" => depth = val()?.parse()?,
            "--seconds" => seconds = val()?.parse()?,
            "--interval-ms" => interval_ms = val()?.parse()?,
            "--fps" => fps = val()?.parse()?,
            "--headless" => headless = true,
            "--theme" => theme_name = Some(val()?),
            "--exchange-host" => exchange_host = val()?.parse()?,
            "--retransmit-port" => retransmit_port = val()?.parse()?,
            "--snapshot-port" => snapshot_port = val()?.parse()?,
            "--session" => {
                let v = val()?;
                let b = v.as_bytes();
                session = *b"          ";
                session[..b.len().min(10)].copy_from_slice(&b[..b.len().min(10)]);
            }
            "--no-snapshot" => no_snapshot = true,
            "--test-drop" => drop_every = val()?.parse()?,
            "--list-themes" => {
                for t in theme::THEMES.iter() {
                    println!("{:<18} {}", t.name, theme::blurb(t.name));
                }
                std::process::exit(0);
            }
            "-h" | "--help" => {
                println!("feedviz [--group G] [--port P] [--iface IP] [--exchange-host IP] [--retransmit-port P] [--snapshot-port P] [--no-snapshot] [--replay FILE] [--headless] [--depth N] [--seconds S] [--fps N] [--interval-ms MS] [--theme NAME] [--list-themes]");
                std::process::exit(0);
            }
            other => bail!("unknown argument {other}"),
        }
    }
    if let Some(name) = theme_name {
        match theme::by_name(&name) {
            Some(i) => theme::set(i),
            None => bail!(
                "unknown theme {name}; one of {}",
                theme::THEMES.iter().map(|t| t.name).collect::<Vec<_>>().join(", ")
            ),
        }
    }
    let source = match replay {
        Some(path) => Source::Capture { path },
        None => {
            let mut src = Source::multicast(group, port, iface).with_recovery(
                exchange_host,
                retransmit_port,
                snapshot_port,
                session,
            );
            if let Source::Multicast { snapshot_on_start, drop_every: d, .. } = &mut src {
                *snapshot_on_start = !no_snapshot;
                *d = drop_every;
            }
            src
        }
    };
    Ok(Args { source, depth, seconds, interval_ms, headless, fps: fps.clamp(5, 240) })
}

#[cfg(test)]
mod render_tests {
    use super::*;
    use ratatui::backend::TestBackend;
    use ratatui::Terminal;

    fn dump(term: &Terminal<TestBackend>, name: &str) {
        let buf = term.backend().buffer();
        let mut out = String::new();
        for y in 0..buf.area.height {
            for x in 0..buf.area.width {
                out.push_str(buf[(x, y)].symbol());
            }
            out.push('\n');
        }
        let dir = std::env::var("FEEDVIZ_DUMP_DIR").unwrap_or_else(|_| std::env::temp_dir().display().to_string());
        std::fs::write(format!("{dir}/{name}.txt"), out).unwrap();
    }

    #[test]
    fn both_screens_render_at_several_sizes() {
        let cap = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../../testdata/capture.bin");
        let client =
            FeedClient::start(Config { source: Source::Capture { path: cap }, depth: 40, ..Config::default() })
                .unwrap();
        while !client.is_finished() {
            std::thread::sleep(Duration::from_millis(5));
        }
        let mut app = App::new(client, true);
        app.tick();
        assert_eq!(app.snap.symbols.len(), 3);
        for (w, h) in [(200u16, 55u16), (140, 40), (100, 30), (60, 20)] {
            let mut term = Terminal::new(TestBackend::new(w, h)).unwrap();
            app.inspect = false;
            app.selected = None;
            app.screen = app::Screen::Market;
            for iv in 0..feed_client::INTERVALS.len() {
                app.interval_idx = iv;
                term.draw(|f| ui::draw(f, &app)).unwrap();
            }
            app.interval_idx = 0;
            term.draw(|f| ui::draw(f, &app)).unwrap();
            if w == 200 {
                dump(&term, "market_200x55");
            }
            if w == 140 {
                dump(&term, "market_140x40");
            }
            app.screen = app::Screen::Events;
            term.draw(|f| ui::draw(f, &app)).unwrap();
            if w == 200 {
                dump(&term, "events_200x55");
            }
            if w == 140 {
                dump(&term, "events_140x40");
            }
            term.draw(|f| ui::draw_classic(f, &app)).unwrap();
            // Select an execution and open the inspector.
            let e = app.snap.events.iter().rev().find(|e| e.ty == b'E' && e.applied.known).copied().unwrap();
            app.selected = Some(e.seq);
            term.draw(|f| ui::draw(f, &app)).unwrap();
            if w == 200 {
                dump(&term, "events_selected_200x55");
            }
            app.follow = Some(e.order_id);
            term.draw(|f| ui::draw(f, &app)).unwrap();
            if w == 200 {
                dump(&term, "events_follow_200x55");
            }
            app.follow = None;
            app.inspect = true;
            term.draw(|f| inspect::draw(f, &app)).unwrap();
            if w == 200 {
                dump(&term, "inspect_200x55");
            }
            if w == 140 {
                dump(&term, "inspect_140x40");
            }
            // Every other type too.
            for ty in [b'A', b'X', b'D', b'U', b'R', b'S'] {
                if let Some(e) = app.snap.events.iter().rev().find(|e| e.ty == ty).copied() {
                    app.selected = Some(e.seq);
                    term.draw(|f| inspect::draw(f, &app)).unwrap();
                }
            }
        }
        // Every theme renders both screens, and the picker overlay.
        for i in 0..theme::THEMES.len() {
            theme::set(i);
            let mut term = Terminal::new(TestBackend::new(200, 55)).unwrap();
            app.screen = app::Screen::Market;
            term.draw(|f| ui::draw(f, &app)).unwrap();
            app.screen = app::Screen::Events;
            term.draw(|f| ui::draw(f, &app)).unwrap();
            app.open_theme_picker();
            term.draw(|f| ui::draw(f, &app)).unwrap();
            if i == 0 {
                dump(&term, "picker_200x55");
            }
            app.picker = None;
        }
        theme::set(0);
        assert!(theme::THEMES.iter().all(|t| !theme::blurb(t.name).is_empty()), "every theme needs a blurb");
        // Fuzzy pickers: type into them and select.
        {
            use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
            let key = |c: KeyCode| KeyEvent::new(c, KeyModifiers::NONE);
            app.on_key(key(KeyCode::Char('T')));
            for c in "kan".chars() {
                app.on_key(key(KeyCode::Char(c)));
            }
            assert_eq!(app.picker.as_ref().unwrap().current().unwrap().label, "kanagawa");
            let mut term = Terminal::new(TestBackend::new(200, 55)).unwrap();
            term.draw(|f| ui::draw(f, &app)).unwrap();
            dump(&term, "picker_typed_200x55");
            app.on_key(key(KeyCode::Enter));
            assert_eq!(theme::th().name, "kanagawa");
            app.on_key(key(KeyCode::Char('T')));
            for c in "mono".chars() {
                app.on_key(key(KeyCode::Char(c)));
            }
            assert_eq!(theme::th().name, "mono", "theme previews live while typing");
            app.on_key(key(KeyCode::Esc));
            assert_eq!(theme::th().name, "kanagawa", "Esc reverts the preview");
            theme::set(0);
            app.on_key(key(KeyCode::Char('/')));
            for c in "nv".chars() {
                app.on_key(key(KeyCode::Char(c)));
            }
            term.draw(|f| ui::draw(f, &app)).unwrap();
            dump(&term, "symbol_picker_200x55");
            app.on_key(key(KeyCode::Enter));
            assert_eq!(app.current_symbol().unwrap().ticker, "NVDA");
        }
        // Key handling smoke: walk the log and toggles without panicking.
        use crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
        let key = |c: KeyCode| KeyEvent::new(c, KeyModifiers::NONE);
        app.inspect = false;
        for c in [
            KeyCode::Up,
            KeyCode::Up,
            KeyCode::Down,
            KeyCode::Enter,
            KeyCode::Char('n'),
            KeyCode::Char('p'),
            KeyCode::Char('K'),
            KeyCode::Esc,
            KeyCode::Char(']'),
            KeyCode::Char('['),
            KeyCode::Char('d'),
            KeyCode::Char('f'),
            KeyCode::Char(' '),
            KeyCode::PageUp,
            KeyCode::PageDown,
            KeyCode::Char('m'),
            KeyCode::Char('e'),
            KeyCode::Tab,
            KeyCode::Char('i'),
            KeyCode::Char('1'),
            KeyCode::Char('2'),
            KeyCode::Char('1'),
            KeyCode::Up,
            KeyCode::Char('o'),
            KeyCode::Char('o'),
            KeyCode::Esc,
        ] {
            app.on_key(key(c));
        }
        let mut term = Terminal::new(TestBackend::new(160, 45)).unwrap();
        term.draw(|f| ui::draw(f, &app)).unwrap();
    }
}

fn main() -> Result<()> {
    let args = parse_args()?;
    let is_capture = matches!(args.source, Source::Capture { .. });
    let client = FeedClient::start(Config { source: args.source.clone(), depth: args.depth, ..Config::default() })?;
    if args.headless {
        return headless(client, args, is_capture);
    }

    let mut terminal = ratatui::init();
    let result = run_tui(&mut terminal, client, &args, is_capture);
    ratatui::restore();
    result
}

fn run_tui(terminal: &mut ratatui::DefaultTerminal, client: FeedClient, args: &Args, replay: bool) -> Result<()> {
    let mut app = App::new(client, replay);
    let frame = Duration::from_micros(1_000_000 / args.fps);
    let start = Instant::now();
    loop {
        let t0 = Instant::now();
        app.tick();
        execute!(io::stdout(), BeginSynchronizedUpdate)?;
        terminal.draw(|f| {
            if app.inspect {
                inspect::draw(f, &app);
            } else {
                ui::draw(f, &app);
            }
        })?;
        execute!(io::stdout(), EndSynchronizedUpdate)?;
        app.record_frame(t0.elapsed().as_micros() as u64);

        // Drain input until the next frame is due.
        loop {
            let left = frame.saturating_sub(t0.elapsed());
            if left.is_zero() {
                break;
            }
            if event::poll(left)? {
                match event::read()? {
                    Event::Key(k) if k.kind == KeyEventKind::Press || k.kind == KeyEventKind::Repeat => app.on_key(k),
                    Event::Resize(_, _) => break,
                    _ => {}
                }
            }
            if app.quit {
                break;
            }
        }
        if app.quit || (args.seconds > 0.0 && start.elapsed().as_secs_f64() >= args.seconds) {
            break;
        }
    }
    app.client.stop();
    Ok(())
}

// ------------------------------------------------------------ headless

fn print_frame(s: &Snapshot, elapsed: f64) {
    println!(
        "[{:7.1}s] seq={} msgs={} ({}/s) pkts={} hb={} gaps={} lost={} recovered={} snapshots={} failed={} gated={} dups={} ring={:.0}% drops={} lat p50={}µs p99={}µs{}{}{}",
        elapsed, s.stats.next_seq, s.stats.messages, s.msgs_per_sec, s.stats.packets, s.stats.heartbeats, s.stats.gaps,
        s.stats.lost_messages, s.stats.recovered_gaps, s.stats.snapshots_loaded, s.stats.recovery_failed, s.stats.gated,
        s.stats.duplicates, s.ring_occupancy * 100.0, s.ring_drops, s.latency_p50_ns / 1000,
        s.latency_p99_ns / 1000, if s.stale { " STALE" } else { "" }, if s.stats.recovering { " RECOVERING" } else { "" }, if s.stats.ended { " ENDED" } else { "" },
    );
    for sym in &s.symbols {
        let bb = sym.best_bid().map(|l| format!("{}x{}", l.qty, l.price)).unwrap_or_else(|| "-".into());
        let ba = sym.best_ask().map(|l| format!("{}x{}", l.price, l.qty)).unwrap_or_else(|| "-".into());
        let last = sym
            .last_trade
            .map(|t| format!("{}@{} {}", t.qty, t.price, if t.aggressor_buy { "▲" } else { "▼" }))
            .unwrap_or_else(|| "-".into());
        println!(
            "   {:<8} bid {:>14}  |  ask {:<14} spread={:<4} last={:<16} vol={:<9} trades={:<7} orders={:<6} levels={}/{} seq={}{}",
            if sym.ticker.is_empty() { format!("#{}", sym.symbol_id) } else { sym.ticker.clone() },
            bb, ba,
            sym.spread().map(|x| x.to_string()).unwrap_or_else(|| "-".into()),
            last, sym.volume, sym.trades, sym.orders, sym.bid_levels, sym.ask_levels, fmt::short_id(sym.book_seq),
            if sym.unknown_orders > 0 { format!(" unknown={}", sym.unknown_orders) } else { String::new() },
        );
        for i in 0..sym.bids.len().max(sym.asks.len()).min(5) {
            let b = sym
                .bids
                .get(i)
                .map(|l| format!("{:>4} {:>8} {:>8}", l.count, l.qty, l.price))
                .unwrap_or_else(|| " ".repeat(22));
            let a = sym.asks.get(i).map(|l| format!("{:<8} {:<8} {:<4}", l.price, l.qty, l.count)).unwrap_or_default();
            println!("            {b}  |  {a}");
        }
    }
}

fn headless(mut client: FeedClient, args: Args, is_capture: bool) -> Result<()> {
    eprintln!("feedviz: headless, source {:?}", args.source);
    let start = Instant::now();
    let interval = Duration::from_millis(args.interval_ms);
    let mut last_rev = 0;
    loop {
        std::thread::sleep(interval);
        let elapsed = start.elapsed().as_secs_f64();
        let snap = client.latest();
        if snap.revision != last_rev || is_capture {
            print_frame(snap, elapsed);
            last_rev = snap.revision;
        } else {
            println!("[{elapsed:7.1}s] no packets yet (is exchange_server running? on macOS try --iface 127.0.0.1 with feed.interface 127.0.0.1)");
        }
        if snap.finished || (args.seconds > 0.0 && elapsed >= args.seconds) {
            break;
        }
    }
    client.stop();
    Ok(())
}
