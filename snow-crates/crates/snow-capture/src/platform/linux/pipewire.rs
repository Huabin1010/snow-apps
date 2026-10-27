//! PipeWire screen stream. The portal and Mutter each hand us a remote fd;
//! this thread turns the latest buffer into a tightly packed RGBA region.

use std::os::fd::OwnedFd;
use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, Instant};

use pipewire as pw;
use pw::properties::properties;
use pw::spa;

use crate::error::{CaptureError, CaptureResult};

use super::pixels::{bgra_to_rgba, bgrx_to_rgba, copy_rgba, crop_rgba};

#[derive(Clone, Copy)]
pub(crate) struct Region {
    pub stream_x: i32,
    pub stream_y: i32,
    pub x: i32,
    pub y: i32,
    pub width: u32,
    pub height: u32,
}

struct SlotInner {
    pixels: Vec<u8>,
    width: u32,
    height: u32,
    generation: u64,
    duplicate: bool,
    failed: Option<String>,
}

struct FrameSlot {
    inner: Mutex<SlotInner>,
    ready: Condvar,
}

pub(crate) struct Pump {
    stop: Arc<std::sync::atomic::AtomicBool>,
    slot: Arc<FrameSlot>,
    thread: Option<JoinHandle<()>>,
    seen: u64,
    _keepalive: Option<zbus::blocking::Connection>,
}

struct UserData {
    format: spa::param::video::VideoInfoRaw,
    previous: Vec<u8>,
    slot: Arc<FrameSlot>,
    region: Region,
}

impl Pump {
    pub(crate) fn start(
        fd: OwnedFd,
        node_id: Option<u32>,
        region: Region,
        keepalive: Option<zbus::blocking::Connection>,
    ) -> CaptureResult<Self> {
        let stop = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let slot = Arc::new(FrameSlot {
            inner: Mutex::new(SlotInner {
                pixels: Vec::new(),
                width: 0,
                height: 0,
                generation: 0,
                duplicate: false,
                failed: None,
            }),
            ready: Condvar::new(),
        });
        let thread_stop = Arc::clone(&stop);
        let thread_slot = Arc::clone(&slot);
        let thread = thread::Builder::new()
            .name("snow-capture-pipewire".into())
            .spawn(move || drive(fd, node_id, region, thread_stop, thread_slot))
            .map_err(|error| {
                CaptureError::platform(anyhow::anyhow!("pipewire thread failed to start: {error}"))
            })?;
        Ok(Self {
            stop,
            slot,
            thread: Some(thread),
            seen: 0,
            _keepalive: keepalive,
        })
    }

    /// Make the next `next` call return the newest buffer already received.
    /// The compositor only pushes frames on damage, so the latest buffer is
    /// the current screen. Without this, `next` would wait for a newer frame.
    pub(crate) fn rewind(&mut self) {
        let guard = self.slot.inner.lock().unwrap_or_else(|error| error.into_inner());
        self.seen = guard.generation.saturating_sub(1);
    }

    /// False once the stream thread has reported a failure.
    pub(crate) fn is_healthy(&self) -> bool {
        let guard = self.slot.inner.lock().unwrap_or_else(|error| error.into_inner());
        guard.failed.is_none()
    }

    pub(crate) fn wait_ready(&mut self, timeout: Duration) -> CaptureResult<()> {
        self.next(timeout).map(|_| ())
    }

    pub(crate) fn next(&mut self, timeout: Duration) -> CaptureResult<(Vec<u8>, u32, u32, bool)> {
        let deadline = Instant::now() + timeout;
        let mut guard = self.slot.inner.lock().unwrap_or_else(|error| error.into_inner());
        while guard.generation <= self.seen && guard.failed.is_none() {
            let now = Instant::now();
            if now >= deadline {
                return Err(CaptureError::Timeout);
            }
            let (next, wait) = self
                .slot
                .ready
                .wait_timeout(guard, deadline - now)
                .unwrap_or_else(|error| error.into_inner());
            guard = next;
            if wait.timed_out() && guard.generation <= self.seen && guard.failed.is_none() {
                return Err(CaptureError::Timeout);
            }
        }
        if let Some(message) = guard.failed.clone() {
            return Err(CaptureError::platform(anyhow::anyhow!(message)));
        }
        let duplicate = guard.duplicate || guard.generation == self.seen;
        self.seen = guard.generation;
        Ok((guard.pixels.clone(), guard.width, guard.height, duplicate))
    }
}

impl Drop for Pump {
    fn drop(&mut self) {
        self.stop
            .store(true, std::sync::atomic::Ordering::Relaxed);
        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }
}

fn fail(slot: &FrameSlot, message: String) {
    let mut guard = slot.inner.lock().unwrap_or_else(|error| error.into_inner());
    guard.failed = Some(message);
    slot.ready.notify_all();
}

fn drive(
    fd: OwnedFd,
    node_id: Option<u32>,
    region: Region,
    stop: Arc<std::sync::atomic::AtomicBool>,
    slot: Arc<FrameSlot>,
) {
    if let Err(error) = drive_stream(fd, node_id, region, &stop, &slot) {
        fail(&slot, error);
    }
}

fn drive_stream(
    fd: OwnedFd,
    node_id: Option<u32>,
    region: Region,
    stop: &Arc<std::sync::atomic::AtomicBool>,
    slot: &Arc<FrameSlot>,
) -> Result<(), String> {
    pw::init();
    let mainloop = pw::main_loop::MainLoop::new(None).map_err(|error| error.to_string())?;
    let context = pw::context::Context::new(&mainloop).map_err(|error| error.to_string())?;
    let core = context
        .connect_fd(fd, None)
        .map_err(|error| error.to_string())?;
    let stream = pw::stream::Stream::new(
        &core,
        "snow-shot-screen",
        properties! {
            *pw::keys::MEDIA_TYPE => "Video",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Screen",
        },
    )
    .map_err(|error| error.to_string())?;
    let data = UserData {
        format: spa::param::video::VideoInfoRaw::default(),
        previous: Vec::new(),
        slot: Arc::clone(slot),
        region,
    };
    let _listener = stream
        .add_local_listener_with_user_data(data)
        .param_changed(|_, user_data, id, param| {
            let Some(param) = param else {
                return;
            };
            if id != pw::spa::param::ParamType::Format.as_raw() {
                return;
            }
            let Ok((media_type, media_subtype)) = pw::spa::param::format_utils::parse_format(param)
            else {
                return;
            };
            if media_type != pw::spa::param::format::MediaType::Video
                || media_subtype != pw::spa::param::format::MediaSubtype::Raw
            {
                return;
            }
            let _ = user_data.format.parse(param);
        })
        .state_changed(|_, user_data, _, new| {
            // A parked stream is reused without a new handshake, so a dead
            // stream has to mark itself as failed.
            if let pw::stream::StreamState::Error(message) = new {
                fail(&user_data.slot, message);
            }
        })
        .process(|stream, user_data| publish_buffer(stream, user_data))
        .register()
        .map_err(|error| error.to_string())?;

    let timer = mainloop.loop_().add_timer({
        let mainloop = mainloop.clone();
        let stop = Arc::clone(stop);
        move |_| {
            if stop.load(std::sync::atomic::Ordering::Relaxed) {
                mainloop.quit();
            }
        }
    });
    let _ = timer.update_timer(Some(Duration::from_millis(200)), Some(Duration::from_millis(200)));

    let values = enum_format_pod();
    let pod = spa::pod::Pod::from_bytes(&values).ok_or_else(|| "pipewire format pod is invalid".to_owned())?;
    let mut params = [pod];
    stream
        .connect(
            spa::utils::Direction::Input,
            node_id,
            pw::stream::StreamFlags::AUTOCONNECT | pw::stream::StreamFlags::MAP_BUFFERS,
            &mut params,
        )
        .map_err(|error| error.to_string())?;
    mainloop.run();
    drop(timer);
    Ok(())
}

fn publish_buffer(stream: &pw::stream::StreamRef, user_data: &mut UserData) {
    let Some(mut buffer) = stream.dequeue_buffer() else {
        return;
    };
    let datas = buffer.datas_mut();
    if datas.is_empty() {
        return;
    }
    let data = &mut datas[0];
    let offset = data.chunk().offset() as usize;
    let size = data.chunk().size() as usize;
    let stride_hint = data.chunk().stride();
    let Some(bytes) = data.data() else {
        return;
    };
    let end = offset.saturating_add(size).min(bytes.len());
    if end <= offset {
        return;
    }
    let payload = &bytes[offset..end];
    let size = user_data.format.size();
    let width = size.width;
    let height = size.height;
    if width == 0 || height == 0 {
        return;
    }
    let stride = if stride_hint > 0 {
        stride_hint as usize
    } else {
        payload.len() / height as usize
    };
    let format = user_data.format.format();
    let converted = if format == spa::param::video::VideoFormat::BGRx {
        bgrx_to_rgba(payload, width, height, stride)
    } else if format == spa::param::video::VideoFormat::BGRA {
        bgra_to_rgba(payload, width, height, stride)
    } else if format == spa::param::video::VideoFormat::RGBA
        || format == spa::param::video::VideoFormat::RGBx
    {
        copy_rgba(payload, width, height, stride).map(|mut pixels| {
            if format == spa::param::video::VideoFormat::RGBx {
                for pixel in pixels.chunks_exact_mut(4) {
                    pixel[3] = 255;
                }
            }
            pixels
        })
    } else {
        bgrx_to_rgba(payload, width, height, stride)
    };
    let Some(full) = converted else {
        return;
    };
    let Some((pixels, width, height)) = crop_to_region(&full, width, height, &user_data.region)
    else {
        return;
    };
    let duplicate = user_data.previous == pixels;
    if !duplicate {
        user_data.previous.clone_from(&pixels);
    }
    let mut guard = user_data
        .slot
        .inner
        .lock()
        .unwrap_or_else(|error| error.into_inner());
    guard.pixels = pixels;
    guard.width = width;
    guard.height = height;
    guard.duplicate = duplicate;
    guard.generation = guard.generation.wrapping_add(1);
    user_data.slot.ready.notify_all();
}

fn crop_to_region(
    full: &[u8],
    width: u32,
    height: u32,
    region: &Region,
) -> Option<(Vec<u8>, u32, u32)> {
    if region.width == 0 || region.height == 0 || (width == region.width && height == region.height)
    {
        return Some((full.to_vec(), width, height));
    }
    let x = u32::try_from(region.x.saturating_sub(region.stream_x)).ok()?;
    let y = u32::try_from(region.y.saturating_sub(region.stream_y)).ok()?;
    let pixels = crop_rgba(full, width, height, x, y, region.width, region.height)?;
    let expected = (region.width as usize)
        .checked_mul(region.height as usize)?
        .checked_mul(4)?;
    if pixels.len() != expected {
        return None;
    }
    Some((pixels, region.width, region.height))
}

fn enum_format_pod() -> Vec<u8> {
    let obj = pw::spa::pod::object!(
        pw::spa::utils::SpaTypes::ObjectParamFormat,
        pw::spa::param::ParamType::EnumFormat,
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::MediaType,
            Id,
            pw::spa::param::format::MediaType::Video
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::MediaSubtype,
            Id,
            pw::spa::param::format::MediaSubtype::Raw
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoFormat,
            Choice,
            Enum,
            Id,
            pw::spa::param::video::VideoFormat::BGRx,
            pw::spa::param::video::VideoFormat::BGRx,
            pw::spa::param::video::VideoFormat::BGRA,
            pw::spa::param::video::VideoFormat::RGBA,
            pw::spa::param::video::VideoFormat::RGBx,
            pw::spa::param::video::VideoFormat::RGB,
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoSize,
            Choice,
            Range,
            Rectangle,
            pw::spa::utils::Rectangle {
                width: 320,
                height: 240
            },
            pw::spa::utils::Rectangle {
                width: 1,
                height: 1
            },
            pw::spa::utils::Rectangle {
                width: 16384,
                height: 16384
            }
        ),
        pw::spa::pod::property!(
            pw::spa::param::format::FormatProperties::VideoFramerate,
            Choice,
            Range,
            Fraction,
            pw::spa::utils::Fraction { num: 30, denom: 1 },
            pw::spa::utils::Fraction { num: 0, denom: 1 },
            pw::spa::utils::Fraction { num: 1000, denom: 1 }
        ),
    );
    pw::spa::pod::serialize::PodSerializer::serialize(
        std::io::Cursor::new(Vec::new()),
        &pw::spa::pod::Value::Object(obj),
    )
    .map(|(cursor, _)| cursor.into_inner())
    .unwrap_or_default()
}
