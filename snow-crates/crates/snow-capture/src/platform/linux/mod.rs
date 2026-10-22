//! Linux capture. GNOME records through Mutter without a share dialog. The
//! ScreenCast portal is only the fallback, and a pure X11 session uses
//! XShm/XGetImage.

mod pipewire;
mod pixels;
mod screencast;
mod x11;

use std::time::Duration;

use crate::backend::{CaptureBackend, MonitorCapturer};
use crate::error::{CaptureError, CaptureResult};
use crate::frame::Frame;
use crate::region::{MonitorGeometry, MonitorLayout};
use crate::{CaptureOptions, CaptureTarget, CaptureTargetInfo, MonitorId, WindowId};

const FRAME_WAIT: Duration = Duration::from_secs(2);

pub(crate) struct LinuxBackend;

pub(crate) fn layout_from_monitors(monitors: &[MonitorId]) -> CaptureResult<MonitorLayout> {
    let monitors = monitors
        .iter()
        .filter_map(|monitor| {
            let geometry = monitor.desktop_geometry()?;
            let width = if geometry.pixel_width == 0 {
                geometry.width.round().max(0.0) as u32
            } else {
                geometry.pixel_width
            };
            let height = if geometry.pixel_height == 0 {
                geometry.height.round().max(0.0) as u32
            } else {
                geometry.pixel_height
            };
            if width == 0 || height == 0 {
                return None;
            }
            Some(MonitorGeometry {
                monitor: monitor.clone(),
                x: geometry.x.round() as i32,
                y: geometry.y.round() as i32,
                width,
                height,
            })
        })
        .collect::<Vec<_>>();
    layout_from_geometries(monitors)
}

fn layout_from_geometries(monitors: Vec<MonitorGeometry>) -> CaptureResult<MonitorLayout> {
    let left = monitors
        .iter()
        .map(|monitor| i64::from(monitor.x))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let top = monitors
        .iter()
        .map(|monitor| i64::from(monitor.y))
        .min()
        .ok_or(CaptureError::NoPrimaryMonitor)?;
    let right = monitors
        .iter()
        .map(|monitor| i64::from(monitor.x) + i64::from(monitor.width))
        .max()
        .unwrap_or(left);
    let bottom = monitors
        .iter()
        .map(|monitor| i64::from(monitor.y) + i64::from(monitor.height))
        .max()
        .unwrap_or(top);
    Ok(MonitorLayout {
        monitors,
        virtual_left: left as i32,
        virtual_top: top as i32,
        virtual_width: u32::try_from(right - left).map_err(|_| CaptureError::BufferOverflow)?,
        virtual_height: u32::try_from(bottom - top).map_err(|_| CaptureError::BufferOverflow)?,
    })
}

fn monitors() -> CaptureResult<Vec<x11::Output>> {
    if let Ok(outputs) = x11::outputs() {
        if !outputs.is_empty() {
            return Ok(outputs);
        }
    }
    if let Some((width, height)) = gnome_screen_size() {
        return Ok(vec![x11::synthetic_output(width, height)]);
    }
    Err(CaptureError::NoPrimaryMonitor)
}

fn gnome_screen_size() -> Option<(u32, u32)> {
    let connection = zbus::blocking::Connection::session().ok()?;
    let proxy = zbus::blocking::Proxy::new(
        &connection,
        "org.gnome.Shell.Introspect",
        "/org/gnome/Shell/Introspect",
        "org.gnome.Shell.Introspect",
    )
    .ok()?;
    let (width, height): (i32, i32) = proxy.get_property("ScreenSize").ok()?;
    if width <= 0 || height <= 0 {
        return None;
    }
    Some((width as u32, height as u32))
}

fn monitor_ids() -> CaptureResult<Vec<MonitorId>> {
    Ok(monitors()?.into_iter().map(|output| output.monitor).collect())
}

struct TargetRect {
    x: i32,
    y: i32,
    width: u32,
    height: u32,
    window: Option<u64>,
}

fn target_rect(target: &CaptureTarget) -> CaptureResult<TargetRect> {
    match target {
        CaptureTarget::PrimaryMonitor => {
            let outputs = monitors()?;
            let output = outputs
                .iter()
                .find(|output| output.monitor.is_primary())
                .or_else(|| outputs.first())
                .ok_or(CaptureError::NoPrimaryMonitor)?;
            Ok(TargetRect {
                x: output.x,
                y: output.y,
                width: output.width,
                height: output.height,
                window: None,
            })
        }
        CaptureTarget::Monitor(monitor) => {
            if let Some(geometry) = monitor.desktop_geometry() {
                return Ok(TargetRect {
                    x: geometry.x.round() as i32,
                    y: geometry.y.round() as i32,
                    width: geometry.pixel_width,
                    height: geometry.pixel_height,
                    window: None,
                });
            }
            let outputs = monitors()?;
            let output = outputs
                .into_iter()
                .find(|output| output.monitor.raw_handle() == monitor.raw_handle())
                .ok_or(CaptureError::MonitorLost)?;
            Ok(TargetRect {
                x: output.x,
                y: output.y,
                width: output.width,
                height: output.height,
                window: None,
            })
        }
        CaptureTarget::Region(region) => {
            if region.width == 0 || region.height == 0 {
                return Err(CaptureError::InvalidConfig(
                    "region width and height must be > 0".into(),
                ));
            }
            Ok(TargetRect {
                x: region.x,
                y: region.y,
                width: region.width,
                height: region.height,
                window: None,
            })
        }
        CaptureTarget::Window(window) => {
            let id = window.linux_id()?;
            let (x, y, width, height) = x11::window_geometry(id)?;
            if width == 0 || height == 0 {
                return Err(CaptureError::InvalidTarget(
                    "window has no visible geometry".into(),
                ));
            }
            Ok(TargetRect {
                x,
                y,
                width,
                height,
                window: Some(id),
            })
        }
    }
}

fn target_info(rect: &TargetRect) -> CaptureTargetInfo {
    CaptureTargetInfo {
        origin_x: rect.x,
        origin_y: rect.y,
        width: rect.width,
        height: rect.height,
    }
}

enum Source {
    Pipe(pipewire::Pump),
    X11(x11::X11Source),
}

struct LinuxCapturer {
    rect: TargetRect,
    source: Option<Source>,
}

impl LinuxCapturer {
    fn open(target: &CaptureTarget, options: CaptureOptions) -> CaptureResult<Self> {
        if !matches!(options.backend_tuning, crate::tuning::BackendTuning::Default) {
            return Err(CaptureError::BackendUnavailable(
                "Windows capture tuning is unavailable on Linux".into(),
            ));
        }
        let mut rect = target_rect(target)?;
        // Wayland clients such as Edge and Chrome are not X11 windows. Capturing
        // the X11 window under the cursor misses their pixels. Mutter's area
        // capture composites those windows into the screenshot.
        let wayland = std::env::var_os("WAYLAND_DISPLAY").is_some();
        if !wayland && rect.window.is_none() && !options.excluded_windows.is_empty() {
            let center_x = rect.x.saturating_add(rect.width as i32 / 2);
            let center_y = rect.y.saturating_add(rect.height as i32 / 2);
            if let Ok(Some(window)) =
                x11::content_window(center_x, center_y, &options.excluded_windows)
            {
                rect.window = Some(window);
            }
        }
        Ok(Self { rect, source: None })
    }

    fn ensure(&mut self) -> CaptureResult<()> {
        if self.source.is_some() {
            return Ok(());
        }
        if let Some(pump) = screencast::open(
            self.rect.x,
            self.rect.y,
            self.rect.width,
            self.rect.height,
            self.rect.window,
        ) {
            self.source = Some(Source::Pipe(pump));
            return Ok(());
        }
        self.source = Some(Source::X11(x11::X11Source::new(
            self.rect.x,
            self.rect.y,
            self.rect.width,
            self.rect.height,
        )));
        Ok(())
    }
}

impl MonitorCapturer for LinuxCapturer {
    fn capture(&mut self, _reuse: Option<Frame>) -> CaptureResult<Frame> {
        self.ensure()?;
        let pipe_frame = match self.source.as_mut() {
            Some(Source::Pipe(pump)) => Some(pump.next(FRAME_WAIT)?),
            _ => None,
        };
        if let Some((pixels, width, height, duplicate)) = pipe_frame {
            if width == self.rect.width && height == self.rect.height {
                let mut frame = Frame::from_rgba8(width, height, pixels)?;
                frame.metadata.is_duplicate = duplicate;
                return Ok(frame);
            }
            self.source = Some(Source::X11(x11::X11Source::new(
                self.rect.x,
                self.rect.y,
                self.rect.width,
                self.rect.height,
            )));
        }
        match self.source.as_mut() {
            Some(Source::X11(source)) => {
                let (pixels, duplicate) = source.next_rgba(FRAME_WAIT)?;
                let mut frame = Frame::from_rgba8(self.rect.width, self.rect.height, pixels)?;
                frame.metadata.is_duplicate = duplicate;
                Ok(frame)
            }
            _ => Err(CaptureError::AccessLost),
        }
    }

    fn release_capture_access(&mut self) {
        self.source = None;
    }

    fn capture_access_active(&self) -> bool {
        self.source.is_some()
    }
}

impl CaptureBackend for LinuxBackend {
    fn enumerate_monitors(&self) -> CaptureResult<Vec<MonitorId>> {
        monitor_ids()
    }

    fn primary_monitor(&self) -> CaptureResult<MonitorId> {
        monitor_ids()?
            .into_iter()
            .find(|monitor| monitor.is_primary())
            .ok_or(CaptureError::NoPrimaryMonitor)
    }

    fn monitor_layout(&self) -> CaptureResult<MonitorLayout> {
        let outputs = monitors()?;
        layout_from_geometries(
            outputs
                .into_iter()
                .map(|output| MonitorGeometry {
                    x: output.x,
                    y: output.y,
                    width: output.width,
                    height: output.height,
                    monitor: output.monitor,
                })
                .collect(),
        )
    }

    fn inspect_window(&self, window: &WindowId) -> CaptureResult<CaptureTargetInfo> {
        Ok(target_info(&target_rect(&CaptureTarget::Window(*window))?))
    }

    fn inspect_target(&self, target: &CaptureTarget) -> CaptureResult<Option<CaptureTargetInfo>> {
        Ok(Some(target_info(&target_rect(target)?)))
    }

    fn create_target_capturer(
        &self,
        target: &CaptureTarget,
        options: CaptureOptions,
    ) -> CaptureResult<Option<Box<dyn MonitorCapturer>>> {
        Ok(Some(Box::new(LinuxCapturer::open(target, options)?)))
    }

    fn create_monitor_capturer(
        &self,
        monitor: &MonitorId,
    ) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(LinuxCapturer::open(
            &CaptureTarget::Monitor(monitor.clone()),
            CaptureOptions::default(),
        )?))
    }

    fn create_window_capturer(&self, window: &WindowId) -> CaptureResult<Box<dyn MonitorCapturer>> {
        Ok(Box::new(LinuxCapturer::open(
            &CaptureTarget::Window(*window),
            CaptureOptions::default(),
        )?))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::monitor::MonitorDesktopGeometry;

    #[test]
    fn layout_bounds_cover_every_monitor() {
        let left = MonitorId::from_parts(0, 1, 1, "left", true).with_desktop_geometry(
            MonitorDesktopGeometry {
                x: 0.0,
                y: 0.0,
                width: 1920.0,
                height: 1080.0,
                pixel_width: 1920,
                pixel_height: 1080,
            },
        );
        let right = MonitorId::from_parts(0, 2, 2, "right", false).with_desktop_geometry(
            MonitorDesktopGeometry {
                x: 1920.0,
                y: -120.0,
                width: 1280.0,
                height: 1024.0,
                pixel_width: 1280,
                pixel_height: 1024,
            },
        );
        let layout = layout_from_monitors(&[left, right]).expect("layout");
        assert_eq!(layout.virtual_left, 0);
        assert_eq!(layout.virtual_top, -120);
        assert_eq!(layout.virtual_width, 3200);
        assert_eq!(layout.virtual_height, 1200);
        assert_eq!(layout.monitors.len(), 2);
    }

    #[test]
    fn live_session_capture_returns_a_nonempty_frame() {
        if std::env::var_os("WAYLAND_DISPLAY").is_none() && std::env::var_os("DISPLAY").is_none() {
            return;
        }
        let backend = LinuxBackend;
        let monitors = backend.enumerate_monitors().expect("monitors");
        let monitor = monitors.first().expect("a monitor");
        let mut capturer = backend
            .create_monitor_capturer(monitor)
            .expect("capturer");
        let frame = capturer.capture(None).expect("frame");
        assert!(frame.width() > 0 && frame.height() > 0);
        let pixels = frame.as_bytes();
        assert_eq!(pixels.len(), frame.width() as usize * frame.height() as usize * 4);
        assert!(
            pixels.iter().any(|byte| *byte != 0),
            "a live desktop frame must contain visible pixels"
        );
    }
}
