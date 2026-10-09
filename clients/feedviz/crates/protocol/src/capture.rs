//! Reader for `exchange_server --capture` files: repeated records of a
//! little-endian u32 length followed by one raw MoldUDP64 packet.

use std::fs::File;
use std::io::{self, BufReader, Read};
use std::path::Path;

pub struct CaptureReader {
    inner: BufReader<File>,
    buf: Vec<u8>,
}

impl CaptureReader {
    pub fn open(path: impl AsRef<Path>) -> io::Result<Self> {
        Ok(Self { inner: BufReader::with_capacity(1 << 20, File::open(path)?), buf: Vec::new() })
    }

    /// Reads the next packet into an internal buffer. `Ok(None)` at EOF.
    pub fn next_packet(&mut self) -> io::Result<Option<&[u8]>> {
        let mut len = [0u8; 4];
        match self.inner.read_exact(&mut len) {
            Ok(()) => {}
            Err(e) if e.kind() == io::ErrorKind::UnexpectedEof => return Ok(None),
            Err(e) => return Err(e),
        }
        let len = u32::from_le_bytes(len) as usize;
        self.buf.resize(len, 0);
        self.inner.read_exact(&mut self.buf)?;
        Ok(Some(&self.buf))
    }
}
