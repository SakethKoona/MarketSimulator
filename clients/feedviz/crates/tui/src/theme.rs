//! Themes. Each keeps the same rules: bids and asks differ in lightness as
//! well as hue, chrome (the "amber" slot) is never used for data, and
//! dim text stays above 4.5:1 on the ground. The active theme is a global
//! index so every widget reads `th().field`.
use ratatui::style::{Color, Modifier, Style};
use std::sync::atomic::{AtomicUsize, Ordering};

#[derive(Clone, Copy, Debug)]
pub struct Theme {
    pub name: &'static str,
    pub bg: Color,
    pub panel_hdr_bg: Color,
    pub panel_alt: Color, // chips, chain boxes
    pub hdr_bg: Color,
    pub border: Color,
    pub text: Color,
    pub text_bright: Color,
    pub muted: Color, // order ids, Delete rows
    pub dim: Color,
    pub dimmer: Color,
    pub amber: Color, // chrome: titles, keys, selection accents
    pub amber_bright: Color,
    pub amber_dim: Color,
    pub amber_text: Color,
    pub select_bg: Color,
    pub bid: Color,
    pub bid_text: Color,
    pub bid_bar: Color,
    pub bid_bar_solid: Color,
    pub ask: Color,
    pub ask_text: Color,
    pub ask_bar: Color,
    pub ask_bar_solid: Color,
    pub green: Color,
    pub purple: Color,
    pub red: Color,
    pub red_bg: Color,
    pub live_bg: Color,
    pub live_fg: Color,
    pub pause_bg: Color,
    pub pause_fg: Color,
    pub mix_d: Color,
    pub mix_x: Color,
    pub mix_u: Color,
}

const fn rgb(r: u8, g: u8, b: u8) -> Color {
    Color::Rgb(r, g, b)
}

pub static THEMES: [Theme; 5] = [
    // Bloomberg-style amber on black.
    Theme {
        name: "amber",
        bg: rgb(7, 7, 7), panel_hdr_bg: rgb(18, 18, 18), panel_alt: rgb(22, 22, 22), hdr_bg: rgb(27, 19, 0),
        border: rgb(38, 38, 38), text: rgb(217, 217, 217), text_bright: rgb(232, 232, 232), muted: rgb(189, 189, 189),
        dim: rgb(140, 140, 140), dimmer: rgb(92, 92, 92),
        amber: rgb(242, 163, 58), amber_bright: rgb(255, 196, 107), amber_dim: rgb(154, 122, 58), amber_text: rgb(230, 184, 106),
        select_bg: rgb(42, 29, 0),
        bid: rgb(111, 177, 255), bid_text: rgb(156, 200, 255), bid_bar: rgb(18, 42, 69), bid_bar_solid: rgb(43, 79, 122),
        ask: rgb(255, 138, 92), ask_text: rgb(255, 178, 143), ask_bar: rgb(74, 34, 19), ask_bar_solid: rgb(138, 58, 30),
        green: rgb(154, 210, 154), purple: rgb(199, 155, 255), red: rgb(255, 107, 107), red_bg: rgb(42, 15, 15),
        live_bg: rgb(31, 107, 58), live_fg: rgb(200, 255, 216), pause_bg: rgb(90, 61, 0), pause_fg: rgb(255, 226, 176),
        mix_d: rgb(74, 74, 74), mix_x: rgb(107, 90, 42), mix_u: rgb(90, 58, 122),
    },
    // Deep navy, ice-blue chrome, cyan bids vs magenta asks.
    Theme {
        name: "midnight",
        bg: rgb(9, 13, 26), panel_hdr_bg: rgb(16, 22, 40), panel_alt: rgb(20, 27, 48), hdr_bg: rgb(13, 24, 52),
        border: rgb(36, 46, 74), text: rgb(208, 216, 234), text_bright: rgb(232, 238, 250), muted: rgb(168, 178, 204),
        dim: rgb(132, 144, 176), dimmer: rgb(88, 98, 128),
        amber: rgb(140, 184, 255), amber_bright: rgb(190, 215, 255), amber_dim: rgb(92, 122, 176), amber_text: rgb(166, 200, 255),
        select_bg: rgb(24, 40, 82),
        bid: rgb(76, 201, 240), bid_text: rgb(150, 224, 248), bid_bar: rgb(14, 56, 76), bid_bar_solid: rgb(27, 110, 140),
        ask: rgb(247, 90, 150), ask_text: rgb(255, 160, 196), ask_bar: rgb(72, 20, 44), ask_bar_solid: rgb(140, 40, 86),
        green: rgb(126, 224, 160), purple: rgb(196, 160, 255), red: rgb(255, 110, 120), red_bg: rgb(60, 18, 30),
        live_bg: rgb(22, 98, 80), live_fg: rgb(196, 255, 236), pause_bg: rgb(60, 70, 120), pause_fg: rgb(220, 230, 255),
        mix_d: rgb(70, 80, 108), mix_x: rgb(96, 108, 160), mix_u: rgb(110, 80, 160),
    },
    // Green CRT. Monochrome base; asks go amber so sides still differ.
    Theme {
        name: "phosphor",
        bg: rgb(2, 10, 4), panel_hdr_bg: rgb(6, 20, 9), panel_alt: rgb(8, 26, 12), hdr_bg: rgb(5, 30, 10),
        border: rgb(20, 60, 28), text: rgb(170, 255, 170), text_bright: rgb(210, 255, 210), muted: rgb(130, 210, 130),
        dim: rgb(90, 170, 90), dimmer: rgb(56, 120, 56),
        amber: rgb(57, 255, 20), amber_bright: rgb(160, 255, 140), amber_dim: rgb(40, 140, 30), amber_text: rgb(120, 255, 100),
        select_bg: rgb(15, 61, 26),
        bid: rgb(140, 255, 140), bid_text: rgb(190, 255, 190), bid_bar: rgb(14, 70, 24), bid_bar_solid: rgb(30, 130, 46),
        ask: rgb(255, 209, 102), ask_text: rgb(255, 226, 150), ask_bar: rgb(74, 58, 14), ask_bar_solid: rgb(140, 110, 30),
        green: rgb(100, 255, 160), purple: rgb(200, 255, 120), red: rgb(255, 120, 90), red_bg: rgb(50, 20, 10),
        live_bg: rgb(20, 110, 40), live_fg: rgb(200, 255, 200), pause_bg: rgb(80, 70, 10), pause_fg: rgb(255, 240, 180),
        mix_d: rgb(40, 90, 50), mix_x: rgb(110, 100, 30), mix_u: rgb(90, 130, 40),
    },
    // Solarized dark.
    Theme {
        name: "solarized",
        bg: rgb(0, 43, 54), panel_hdr_bg: rgb(7, 54, 66), panel_alt: rgb(7, 54, 66), hdr_bg: rgb(7, 54, 66),
        border: rgb(40, 80, 92), text: rgb(147, 161, 161), text_bright: rgb(238, 232, 213), muted: rgb(131, 148, 150),
        dim: rgb(121, 140, 142), dimmer: rgb(88, 110, 117),
        amber: rgb(181, 137, 0), amber_bright: rgb(222, 178, 40), amber_dim: rgb(130, 100, 10), amber_text: rgb(203, 160, 30),
        select_bg: rgb(18, 70, 84),
        bid: rgb(38, 139, 210), bid_text: rgb(110, 176, 230), bid_bar: rgb(10, 60, 100), bid_bar_solid: rgb(26, 100, 160),
        ask: rgb(203, 75, 22), ask_text: rgb(236, 130, 80), ask_bar: rgb(80, 36, 16), ask_bar_solid: rgb(150, 60, 20),
        green: rgb(133, 153, 0), purple: rgb(108, 113, 196), red: rgb(220, 50, 47), red_bg: rgb(70, 25, 30),
        live_bg: rgb(60, 90, 0), live_fg: rgb(230, 240, 180), pause_bg: rgb(110, 85, 0), pause_fg: rgb(250, 230, 170),
        mix_d: rgb(60, 85, 95), mix_x: rgb(120, 95, 10), mix_u: rgb(90, 95, 160),
    },
    // Light: warm paper, ochre chrome, navy bids, burnt-orange asks.
    Theme {
        name: "paper",
        bg: rgb(246, 241, 231), panel_hdr_bg: rgb(236, 230, 218), panel_alt: rgb(232, 225, 210), hdr_bg: rgb(234, 223, 200),
        border: rgb(207, 198, 182), text: rgb(43, 43, 43), text_bright: rgb(20, 20, 20), muted: rgb(80, 80, 80),
        dim: rgb(110, 102, 92), dimmer: rgb(150, 142, 130),
        amber: rgb(138, 90, 0), amber_bright: rgb(90, 58, 0), amber_dim: rgb(170, 130, 60), amber_text: rgb(120, 78, 0),
        select_bg: rgb(255, 233, 184),
        bid: rgb(31, 95, 191), bid_text: rgb(20, 70, 150), bid_bar: rgb(207, 224, 247), bid_bar_solid: rgb(120, 160, 220),
        ask: rgb(194, 65, 12), ask_text: rgb(150, 50, 10), ask_bar: rgb(248, 215, 196), ask_bar_solid: rgb(230, 150, 110),
        green: rgb(47, 125, 50), purple: rgb(109, 40, 217), red: rgb(185, 28, 28), red_bg: rgb(250, 220, 220),
        live_bg: rgb(47, 125, 50), live_fg: rgb(240, 255, 240), pause_bg: rgb(138, 90, 0), pause_fg: rgb(255, 245, 225),
        mix_d: rgb(180, 172, 160), mix_x: rgb(210, 180, 120), mix_u: rgb(190, 170, 230),
    },
];

static ACTIVE: AtomicUsize = AtomicUsize::new(0);

pub fn th() -> &'static Theme {
    &THEMES[ACTIVE.load(Ordering::Relaxed) % THEMES.len()]
}
pub fn set(i: usize) {
    ACTIVE.store(i % THEMES.len(), Ordering::Relaxed);
}
pub fn next() {
    set(ACTIVE.load(Ordering::Relaxed) + 1);
}
pub fn by_name(name: &str) -> Option<usize> {
    THEMES.iter().position(|th| th.name.eq_ignore_ascii_case(name))
}

pub fn base() -> Style {
    Style::default().fg(th().text).bg(th().bg)
}
pub fn dim() -> Style {
    Style::default().fg(th().dim)
}
pub fn dimmer() -> Style {
    Style::default().fg(th().dimmer)
}
pub fn amber() -> Style {
    Style::default().fg(th().amber)
}
pub fn panel_title() -> Style {
    Style::default().fg(th().amber).bg(th().panel_hdr_bg).add_modifier(Modifier::BOLD)
}
pub fn panel_sub() -> Style {
    Style::default().fg(th().dim).bg(th().panel_hdr_bg)
}
pub fn fg(c: Color) -> Style {
    Style::default().fg(c)
}
pub fn bold(c: Color) -> Style {
    Style::default().fg(c).add_modifier(Modifier::BOLD)
}

/// Colour for a message type letter.
pub fn type_color(ty: u8) -> Color {
    let th = th();
    match ty {
        b'A' => th.bid,
        b'E' => th.ask,
        b'X' => th.amber_text,
        b'D' => th.muted,
        b'U' => th.purple,
        b'R' | b'S' => th.dim,
        b'!' => th.red,
        _ => th.text,
    }
}

pub fn side_color(side: u8) -> Color {
    match side {
        b'B' => th().bid,
        b'S' => th().ask,
        _ => th().dim,
    }
}
