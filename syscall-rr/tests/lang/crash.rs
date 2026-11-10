// Rust: a panic that unwinds, or a NULL write in unsafe code.
use std::{env, fs::File, io::Read, thread, time::{Duration, SystemTime}};
fn main() {
    let mode = env::args().nth(1).unwrap_or_default();
    let mut rnd = File::open("/dev/urandom").unwrap();
    let t0 = SystemTime::now();
    let mut sum = 0u64;
    let mut i = 0u64;
    loop {
        let mut b = [0u8; 8];
        rnd.read_exact(&mut b).unwrap();
        sum ^= u64::from_le_bytes(b);
        println!("iter {i} {sum:016x}");
        thread::sleep(Duration::from_millis(20));
        i += 1;
        if t0.elapsed().unwrap() > Duration::from_millis(3200) {
            if mode == "segv" {
                unsafe { std::ptr::null_mut::<u64>().write_volatile(sum) };
            }
            let v: Vec<u64> = vec![sum];
            println!("{}", v[(i as usize) + 5]);
        }
    }
}
