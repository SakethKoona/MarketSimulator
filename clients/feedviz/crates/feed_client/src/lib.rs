//! Three-stage pipeline: an ingest thread receives raw packets into a
//! wait-free ring, a book thread applies them to L3 books and publishes
//! snapshots, and the UI reads only the latest snapshot.

pub mod book;
pub mod ingest;
pub mod pipeline;
pub mod session;
pub mod snapshot;

pub use book::{Book, Level, Side, Trade};
pub use pipeline::{Config, FeedClient, Source};
pub use session::{Session, SessionStats};
pub use snapshot::{LevelSnap, Snapshot, SymbolSnapshot};
