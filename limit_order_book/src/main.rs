
mod types;


/*
 * This file would be responsible for instantiating the order book that we created, and also
 * connecting to the simulator that we build on Python using TCP. We would then pass those messages
 * through the matching engine and then eventually over to the LOB, where they would then be sent
 * out on a multicast broadcast using UDP.
 * */
fn main() {
    println!("Hello, world!");
}
