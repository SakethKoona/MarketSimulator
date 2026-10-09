//! MoldUDP64-style framing, little-endian variant (feed-v1.md §2).

use crate::DecodeError;
use zerocopy::little_endian::{U16, U64};
use zerocopy::{FromBytes, Immutable, IntoBytes, KnownLayout, Unaligned};

pub const SESSION_LEN: usize = 10;
pub const MAX_PACKET: usize = 1400;
/// `message_count` of an end-of-session packet.
pub const END_OF_SESSION: u16 = 0xFFFF;

#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct PacketHeader {
    pub session: [u8; SESSION_LEN],
    pub sequence_number: U64,
    pub message_count: U16,
}
const _: () = assert!(core::mem::size_of::<PacketHeader>() == 20);

/// Retransmission request sent over TCP (feed-v1.md §2.1).
#[derive(FromBytes, IntoBytes, Immutable, KnownLayout, Unaligned, Clone, Copy, Debug)]
#[repr(C, packed)]
pub struct RetransmitRequest {
    pub session: [u8; SESSION_LEN],
    pub sequence_number: U64,
    pub message_count: U16,
}
const _: () = assert!(core::mem::size_of::<RetransmitRequest>() == 20);

impl PacketHeader {
    pub fn seq(&self) -> u64 {
        self.sequence_number.get()
    }
    pub fn count(&self) -> u16 {
        self.message_count.get()
    }
    pub fn is_heartbeat(&self) -> bool {
        self.count() == 0
    }
    pub fn is_end_of_session(&self) -> bool {
        self.count() == END_OF_SESSION
    }
    /// Sequence number of the message after this packet's last one.
    pub fn next_seq(&self) -> u64 {
        if self.is_end_of_session() {
            self.seq()
        } else {
            self.seq() + u64::from(self.count())
        }
    }
}

/// Iterates the message blocks of one packet, yielding `(seq, payload)`.
pub struct PacketIter<'a> {
    header: &'a PacketHeader,
    rest: &'a [u8],
    remaining: u16,
    seq: u64,
    block: u16,
}

impl<'a> PacketIter<'a> {
    pub fn new(packet: &'a [u8]) -> Result<Self, DecodeError> {
        let (header, rest) = PacketHeader::ref_from_prefix(packet).map_err(|_| DecodeError::ShortPacket)?;
        let remaining = if header.is_end_of_session() { 0 } else { header.count() };
        Ok(Self { header, rest, remaining, seq: header.seq(), block: 0 })
    }
    pub fn header(&self) -> &'a PacketHeader {
        self.header
    }
}

impl<'a> Iterator for PacketIter<'a> {
    type Item = Result<(u64, &'a [u8]), DecodeError>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.remaining == 0 {
            return None;
        }
        let (len, rest) = match U16::ref_from_prefix(self.rest) {
            Ok(x) => x,
            Err(_) => {
                self.remaining = 0;
                return Some(Err(DecodeError::Truncated { block: self.block }));
            }
        };
        let len = usize::from(len.get());
        if rest.len() < len {
            self.remaining = 0;
            return Some(Err(DecodeError::Truncated { block: self.block }));
        }
        let (payload, rest) = rest.split_at(len);
        self.rest = rest;
        self.remaining -= 1;
        self.block += 1;
        let seq = self.seq;
        self.seq += 1;
        Some(Ok((seq, payload)))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn walks_blocks_and_detects_truncation() {
        let mut pkt = Vec::new();
        pkt.extend_from_slice(b"20261008A ");
        pkt.extend_from_slice(&100u64.to_le_bytes());
        pkt.extend_from_slice(&2u16.to_le_bytes());
        pkt.extend_from_slice(&3u16.to_le_bytes());
        pkt.extend_from_slice(&[1, 2, 3]);
        pkt.extend_from_slice(&1u16.to_le_bytes());
        pkt.extend_from_slice(&[9]);

        let it = PacketIter::new(&pkt).unwrap();
        assert_eq!(it.header().seq(), 100);
        let got: Vec<_> = it.map(|r| r.unwrap()).collect();
        assert_eq!(got, vec![(100, &[1u8, 2, 3][..]), (101, &[9u8][..])]);

        let it = PacketIter::new(&pkt[..pkt.len() - 1]).unwrap();
        let last = it.last().unwrap();
        assert_eq!(last, Err(DecodeError::Truncated { block: 1 }));
    }
}
