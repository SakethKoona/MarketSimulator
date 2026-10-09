//! One theme, amber on black, bids blue and asks orange so sides differ in
//! lightness as well as hue.
use ratatui::style::{Color, Modifier, Style};

pub const BG: Color = Color::Rgb(7, 7, 7);
pub const PANEL_HDR_BG: Color = Color::Rgb(18, 18, 18);
pub const HDR_BG: Color = Color::Rgb(27, 19, 0);
pub const BORDER: Color = Color::Rgb(38, 38, 38);
pub const TEXT: Color = Color::Rgb(217, 217, 217);
pub const TEXT_BRIGHT: Color = Color::Rgb(232, 232, 232);
pub const DIM: Color = Color::Rgb(140, 140, 140);
pub const DIMMER: Color = Color::Rgb(92, 92, 92);
pub const AMBER: Color = Color::Rgb(242, 163, 58);
pub const AMBER_BRIGHT: Color = Color::Rgb(255, 196, 107);
pub const AMBER_DIM: Color = Color::Rgb(154, 122, 58);
pub const AMBER_TEXT: Color = Color::Rgb(230, 184, 106);
pub const SELECT_BG: Color = Color::Rgb(42, 29, 0);
pub const BID: Color = Color::Rgb(111, 177, 255);
pub const BID_TEXT: Color = Color::Rgb(156, 200, 255);
pub const BID_BAR: Color = Color::Rgb(18, 42, 69);
pub const ASK: Color = Color::Rgb(255, 138, 92);
pub const ASK_TEXT: Color = Color::Rgb(255, 178, 143);
pub const ASK_BAR: Color = Color::Rgb(74, 34, 19);
pub const GREEN: Color = Color::Rgb(154, 210, 154);
pub const PURPLE: Color = Color::Rgb(199, 155, 255);
pub const RED: Color = Color::Rgb(255, 107, 107);
pub const RED_BG: Color = Color::Rgb(42, 15, 15);
pub const LIVE_BG: Color = Color::Rgb(31, 107, 58);
pub const LIVE_FG: Color = Color::Rgb(200, 255, 216);
pub const PAUSE_BG: Color = Color::Rgb(90, 61, 0);
pub const PAUSE_FG: Color = Color::Rgb(255, 226, 176);

pub fn base() -> Style {
    Style::default().fg(TEXT).bg(BG)
}
pub fn dim() -> Style {
    Style::default().fg(DIM)
}
pub fn dimmer() -> Style {
    Style::default().fg(DIMMER)
}
pub fn amber() -> Style {
    Style::default().fg(AMBER)
}
pub fn panel_title() -> Style {
    Style::default().fg(AMBER).bg(PANEL_HDR_BG).add_modifier(Modifier::BOLD)
}
pub fn panel_sub() -> Style {
    Style::default().fg(DIM).bg(PANEL_HDR_BG)
}
pub fn fg(c: Color) -> Style {
    Style::default().fg(c)
}
pub fn bold(c: Color) -> Style {
    Style::default().fg(c).add_modifier(Modifier::BOLD)
}

/// Colour for a message type letter.
pub fn type_color(ty: u8) -> Color {
    match ty {
        b'A' => BID,
        b'E' => ASK,
        b'X' => AMBER_TEXT,
        b'D' => Color::Rgb(189, 189, 189),
        b'U' => PURPLE,
        b'R' | b'S' => DIM,
        b'!' => RED,
        _ => TEXT,
    }
}

pub fn side_color(side: u8) -> Color {
    match side {
        b'B' => BID,
        b'S' => ASK,
        _ => DIM,
    }
}
