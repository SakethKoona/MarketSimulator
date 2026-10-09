//! Generic fuzzy picker overlay (telescope-style): a prompt, a list filtered
//! by subsequence match with the matched characters highlighted, ↑↓ to
//! move, Enter to pick, Esc to cancel. Used for themes and symbols.

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PickerKind {
    Theme,
    Symbol,
}

#[derive(Clone, Debug)]
pub struct Item {
    /// What the match runs over and what is shown first.
    pub label: String,
    /// Shown dimmed after the label.
    pub detail: String,
    /// Caller's index (theme index, symbol index).
    pub key: usize,
}

#[derive(Clone, Debug)]
pub struct Match {
    pub item: usize,
    pub score: i32,
    pub positions: Vec<usize>, // char indices in label
}

#[derive(Clone, Debug)]
pub struct Picker {
    pub kind: PickerKind,
    pub query: String,
    pub items: Vec<Item>,
    pub matches: Vec<Match>,
    pub selected: usize,
    /// Theme picker: index to restore on Esc.
    pub restore: usize,
}

impl Picker {
    pub fn new(kind: PickerKind, items: Vec<Item>, initial: usize, restore: usize) -> Self {
        let mut p = Picker { kind, query: String::new(), items, matches: Vec::new(), selected: 0, restore };
        p.refilter();
        if let Some(i) = p.matches.iter().position(|m| p.items[m.item].key == initial) {
            p.selected = i;
        }
        p
    }

    pub fn refilter(&mut self) {
        let q = self.query.clone();
        self.matches = self
            .items
            .iter()
            .enumerate()
            .filter_map(|(i, it)| fuzzy(&q, &it.label).map(|(score, positions)| Match { item: i, score, positions }))
            .collect();
        if !q.is_empty() {
            self.matches.sort_by(|a, b| b.score.cmp(&a.score).then(a.item.cmp(&b.item)));
        }
        if self.selected >= self.matches.len() {
            self.selected = self.matches.len().saturating_sub(1);
        }
    }

    pub fn current(&self) -> Option<&Item> {
        self.matches.get(self.selected).map(|m| &self.items[m.item])
    }

    pub fn push(&mut self, c: char) {
        self.query.push(c);
        self.selected = 0;
        self.refilter();
    }
    pub fn pop(&mut self) {
        self.query.pop();
        self.selected = 0;
        self.refilter();
    }
    pub fn up(&mut self) {
        if !self.matches.is_empty() {
            self.selected = (self.selected + self.matches.len() - 1) % self.matches.len();
        }
    }
    pub fn down(&mut self) {
        if !self.matches.is_empty() {
            self.selected = (self.selected + 1) % self.matches.len();
        }
    }
}

/// Case-insensitive subsequence match with fzf-like scoring: bonuses for
/// consecutive matches and for matching at a word start, a small penalty
/// for gaps. Returns the score and matched char positions, or None.
pub fn fuzzy(query: &str, text: &str) -> Option<(i32, Vec<usize>)> {
    if query.is_empty() {
        return Some((0, Vec::new()));
    }
    let t: Vec<char> = text.chars().map(|c| c.to_ascii_lowercase()).collect();
    let q: Vec<char> = query.chars().map(|c| c.to_ascii_lowercase()).collect();
    // Greedy left-to-right match, then try to improve by preferring
    // word-start positions for the first character.
    let mut best: Option<(i32, Vec<usize>)> = None;
    let starts: Vec<usize> = (0..t.len()).filter(|&i| t[i] == q[0]).collect();
    for &s in starts.iter().take(8) {
        let mut pos = Vec::with_capacity(q.len());
        let mut ti = s;
        let mut ok = true;
        for &qc in &q {
            match (ti..t.len()).find(|&i| t[i] == qc) {
                Some(i) => {
                    pos.push(i);
                    ti = i + 1;
                }
                None => {
                    ok = false;
                    break;
                }
            }
        }
        if !ok {
            continue;
        }
        let mut score = 0i32;
        for (k, &p) in pos.iter().enumerate() {
            let word_start = p == 0 || !t[p - 1].is_alphanumeric();
            if word_start {
                score += 8;
            }
            if k > 0 {
                let gap = p - pos[k - 1] - 1;
                score += if gap == 0 { 6 } else { -(gap.min(10) as i32) };
            }
        }
        score -= pos[0] as i32; // earlier first match wins ties
        if pos.len() == t.len() {
            score += 30; // the whole label matched: "mono" beats "monokai"
        }
        score -= ((t.len() - q.len()) as i32).min(8) / 2; // shorter labels edge ahead
        if best.as_ref().map_or(true, |(b, _)| score > *b) {
            best = Some((score, pos));
        }
    }
    best
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fuzzy_matches_subsequences_and_prefers_word_starts() {
        assert!(fuzzy("kan", "kanagawa").is_some());
        assert!(fuzzy("cpl", "catppuccin-latte").is_some());
        assert!(fuzzy("xyz", "kanagawa").is_none());
        let (s1, _) = fuzzy("gl", "gruvbox-light").unwrap();
        let (s2, _) = fuzzy("gl", "google").unwrap();
        assert!(s1 > s2, "word-start match should outscore an in-word one");
        let (_, pos) = fuzzy("nv", "NVDA").unwrap();
        assert_eq!(pos, vec![0, 1]);
        let (exact, _) = fuzzy("mono", "mono").unwrap();
        let (longer, _) = fuzzy("mono", "monokai").unwrap();
        assert!(exact > longer, "exact match must rank first");
    }
}
