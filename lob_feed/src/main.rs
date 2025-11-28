use std::io::Result;

use tokio::net::UdpSocket;
use tokio;

#[tokio::main]
async fn main() -> Result<()> {

    let sock = UdpSocket::bind("0.0.0.0:8000").await?;

    // Sample receive a message from the port we binded to before
    let mut buf = [0; 1024];
    let (len, addr) = sock.recv_from(&mut buf).await?;

    println!("{:?} bytes from address: {:?}", len, addr);

    Ok(())
}
