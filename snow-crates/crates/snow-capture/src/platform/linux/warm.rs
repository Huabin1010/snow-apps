//! Idle ScreenCast streams. Creating a Mutter session, connecting PipeWire and
//! waiting for the first buffer takes most of a second, which is what made the
//! screenshot hotkey feel slow. A finished capture parks its stream here, and
//! the next capture of the same rectangle reuses it. A background reaper drops
//! streams that stay unused, so the "screen is being recorded" indicator does
//! not stay on for long.

use std::sync::{Mutex, Once};
use std::thread;
use std::time::{Duration, Instant};

use super::pipewire::Pump;

/// How long an unused stream stays open after the last capture.
const WARM_TTL: Duration = Duration::from_secs(60);
/// Upper bound on parked streams, one per distinct capture rectangle.
const MAX_PARKED: usize = 4;
const REAP_INTERVAL: Duration = Duration::from_secs(1);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub(crate) struct Key {
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
    pub window: Option<u64>,
}

struct Parked {
    key: Key,
    pump: Pump,
    since: Instant,
}

static PARKED: Mutex<Vec<Parked>> = Mutex::new(Vec::new());
static REAPER: Once = Once::new();

fn lock() -> std::sync::MutexGuard<'static, Vec<Parked>> {
    PARKED.lock().unwrap_or_else(|error| error.into_inner())
}

/// Keep `pump` open for the next capture of `key`. Unhealthy streams are dropped.
pub(crate) fn park(key: Key, pump: Pump) {
    if !pump.is_healthy() {
        return;
    }
    // Dropping a Pump joins its thread, so do it after the lock is released.
    let mut evicted = Vec::new();
    {
        let mut parked = lock();
        while let Some(index) = parked.iter().position(|entry| entry.key == key) {
            evicted.push(parked.remove(index));
        }
        parked.push(Parked {
            key,
            pump,
            since: Instant::now(),
        });
        while parked.len() > MAX_PARKED {
            evicted.push(parked.remove(0));
        }
    }
    drop(evicted);
    REAPER.call_once(|| {
        let _ = thread::Builder::new()
            .name("snow-capture-reaper".into())
            .spawn(reap_loop);
    });
}

/// Take the parked stream for `key`, positioned on its newest buffer.
pub(crate) fn take(key: Key) -> Option<Pump> {
    let mut stale = Vec::new();
    let mut found = None;
    {
        let mut parked = lock();
        if let Some(index) = parked.iter().position(|entry| entry.key == key) {
            let entry = parked.remove(index);
            if entry.pump.is_healthy() && entry.since.elapsed() < WARM_TTL {
                found = Some(entry.pump);
            } else {
                stale.push(entry);
            }
        }
    }
    drop(stale);
    found.map(|mut pump| {
        pump.rewind();
        pump
    })
}

fn reap_loop() {
    loop {
        thread::sleep(REAP_INTERVAL);
        let mut expired = Vec::new();
        {
            let mut parked = lock();
            let mut index = 0;
            while index < parked.len() {
                if parked[index].since.elapsed() >= WARM_TTL || !parked[index].pump.is_healthy() {
                    expired.push(parked.remove(index));
                } else {
                    index += 1;
                }
            }
        }
        drop(expired);
    }
}
