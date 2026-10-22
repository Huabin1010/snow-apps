//! ScreenCast sources. Mutter records a rectangle without asking the user to
//! pick a screen. The portal is the fallback, and its restore token keeps that
//! permission from being requested again. The caller falls back to X11 after both.

use std::collections::HashMap;
use std::os::fd::OwnedFd;
use std::os::unix::net::UnixStream;
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::mpsc;
use std::thread;
use std::time::Duration;

use zbus::blocking::{Connection, Proxy};
use zbus::zvariant::{OwnedObjectPath, OwnedValue, Value};

use super::pipewire::{Pump, Region};

const PORTAL_WAIT: Duration = Duration::from_secs(60);
const FRAME_WAIT: Duration = Duration::from_millis(800);
const PERSIST_UNTIL_REVOKED: u32 = 2;

struct StreamInfo {
    node_id: u32,
    x: i32,
    y: i32,
}

pub(crate) fn open(x: i32, y: i32, width: u32, height: u32, window: Option<u64>) -> Option<Pump> {
    let region = Region {
        stream_x: x,
        stream_y: y,
        x,
        y,
        width,
        height,
    };
    if let Some(window) = window {
        let mut options = HashMap::new();
        options.insert("window-id".to_owned(), Value::U64(window));
        options.insert("cursor-mode".to_owned(), Value::U32(0));
        let mut window_region = region;
        if let Ok((origin_x, origin_y, _, _)) = super::x11::window_geometry(window) {
            window_region.stream_x = origin_x;
            window_region.stream_y = origin_y;
        }
        if let Some(pump) = mutter("RecordWindow", &options, window_region) {
            return Some(pump);
        }
    }
    // Mutter does not show a share dialog. Starting the portal first leaves that
    // dialog on screen even when this process then records through Mutter.
    for source in area_sources() {
        match source {
            AreaSource::Mutter => {
                let mut options = HashMap::new();
                options.insert("cursor-mode".to_owned(), Value::U32(0));
                if let Some(pump) = mutter(
                    "RecordArea",
                    &(x, y, width as i32, height as i32, options),
                    region,
                ) {
                    return Some(pump);
                }
            }
            AreaSource::Portal => {
                if let Some(pump) = portal(region) {
                    return Some(pump);
                }
            }
        }
    }
    None
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum AreaSource {
    Mutter,
    Portal,
}

fn area_sources() -> &'static [AreaSource] {
    &[AreaSource::Mutter, AreaSource::Portal]
}

fn portal(region: Region) -> Option<Pump> {
    let (sender, receiver) = mpsc::sync_channel(1);
    thread::spawn(move || {
        let _ = sender.send(portal_grant());
    });
    let grant = receiver.recv_timeout(PORTAL_WAIT).ok()?.ok()?;
    let region = Region {
        stream_x: grant.stream.x,
        stream_y: grant.stream.y,
        ..region
    };
    let mut pump = Pump::start(
        grant.fd,
        Some(grant.stream.node_id),
        region,
        Some(grant.connection),
    )
    .ok()?;
    pump.wait_ready(FRAME_WAIT).ok()?;
    Some(pump)
}

struct PortalGrant {
    connection: Connection,
    fd: OwnedFd,
    stream: StreamInfo,
}

fn portal_grant() -> Result<PortalGrant, String> {
    let connection = Connection::session().map_err(|error| error.to_string())?;
    let unique = connection
        .unique_name()
        .ok_or_else(|| "portal connection has no unique name".to_owned())?
        .as_str()
        .trim_start_matches(':')
        .replace('.', "_");
    let screencast = Proxy::new(
        &connection,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.portal.ScreenCast",
    )
    .map_err(|error| error.to_string())?;
    let create_token = token();
    let session_token = token();
    let mut create = HashMap::new();
    create.insert("handle_token".to_owned(), string_value(&create_token));
    create.insert(
        "session_handle_token".to_owned(),
        string_value(&session_token),
    );
    let created = call_request(
        &connection,
        &screencast,
        "CreateSession",
        &create,
        &unique,
        &create_token,
    )?;
    let session = object_path(created.get("session_handle")).unwrap_or_else(|| {
        OwnedObjectPath::try_from(format!(
            "/org/freedesktop/portal/desktop/session/{unique}/{session_token}"
        ))
        .expect("session path")
    });
    let sources_token = token();
    let mut sources = portal_select_options(read_restore_token().as_deref());
    sources.insert("handle_token".to_owned(), string_value(&sources_token));
    call_request(
        &connection,
        &screencast,
        "SelectSources",
        &(session.clone(), sources),
        &unique,
        &sources_token,
    )?;
    let start_token = token();
    let mut start = HashMap::new();
    start.insert("handle_token".to_owned(), string_value(&start_token));
    let started = call_request(
        &connection,
        &screencast,
        "Start",
        &(session.clone(), "", start),
        &unique,
        &start_token,
    )?;
    let stream = started
        .get("streams")
        .and_then(|value| first_stream(value.clone()))
        .ok_or_else(|| "portal screencast returned no stream".to_owned())?;
    if let Some(token) = owned_string(started.get("restore_token")) {
        write_restore_token(&token);
    }
    let remote: zbus::zvariant::OwnedFd = screencast
        .call(
            "OpenPipeWireRemote",
            &(session, HashMap::<String, Value>::new()),
        )
        .map_err(|error| error.to_string())?;
    Ok(PortalGrant {
        connection,
        fd: OwnedFd::from(remote),
        stream,
    })
}

fn call_request<B>(
    connection: &Connection,
    screencast: &Proxy<'_>,
    method: &str,
    body: &B,
    sender: &str,
    handle: &str,
) -> Result<HashMap<String, OwnedValue>, String>
where
    B: serde::ser::Serialize + zbus::zvariant::DynamicType,
{
    let path = format!("/org/freedesktop/portal/desktop/request/{sender}/{handle}");
    let request = Proxy::new(
        connection,
        "org.freedesktop.portal.Desktop",
        path.as_str(),
        "org.freedesktop.portal.Request",
    )
    .map_err(|error| error.to_string())?;
    let mut signals = request
        .receive_signal("Response")
        .map_err(|error| error.to_string())?;
    let _returned: OwnedObjectPath = screencast
        .call(method, body)
        .map_err(|error| error.to_string())?;
    let message = signals
        .next()
        .ok_or_else(|| format!("portal {method} produced no response"))?;
    let (code, results): (u32, HashMap<String, OwnedValue>) = message
        .body()
        .deserialize()
        .map_err(|error| error.to_string())?;
    if code == 1 {
        return Err("portal screencast was cancelled".to_owned());
    }
    if code != 0 {
        return Err(format!("portal {method} failed with response {code}"));
    }
    Ok(results)
}

fn mutter<B>(method: &str, body: &B, region: Region) -> Option<Pump>
where
    B: serde::ser::Serialize + zbus::zvariant::DynamicType,
{
    let connection = match Connection::session() {
        Ok(connection) => connection,
        Err(error) => {
            eprintln!("mutter session bus: {error}");
            return None;
        }
    };
    let root = match Proxy::new(
        &connection,
        "org.gnome.Mutter.ScreenCast",
        "/org/gnome/Mutter/ScreenCast",
        "org.gnome.Mutter.ScreenCast",
    ) {
        Ok(root) => root,
        Err(error) => {
            eprintln!("mutter proxy: {error}");
            return None;
        }
    };
    let empty: HashMap<String, Value> = HashMap::new();
    let session: OwnedObjectPath = match root.call("CreateSession", &empty) {
        Ok(session) => session,
        Err(error) => {
            eprintln!("mutter CreateSession: {error}");
            return None;
        }
    };
    let session_proxy = match Proxy::new(
        &connection,
        "org.gnome.Mutter.ScreenCast",
        session.as_str(),
        "org.gnome.Mutter.ScreenCast.Session",
    ) {
        Ok(proxy) => proxy,
        Err(error) => {
            eprintln!("mutter session proxy: {error}");
            return None;
        }
    };
    let stream_path: OwnedObjectPath = match session_proxy.call(method, body) {
        Ok(path) => path,
        Err(error) => {
            eprintln!("mutter {method}: {error}");
            return None;
        }
    };
    let stream = match Proxy::new(
        &connection,
        "org.gnome.Mutter.ScreenCast",
        stream_path.as_str(),
        "org.gnome.Mutter.ScreenCast.Stream",
    ) {
        Ok(stream) => stream,
        Err(error) => {
            eprintln!("mutter stream proxy: {error}");
            return None;
        }
    };
    let added = stream.receive_signal("PipeWireStreamAdded").ok();
    if let Err(error) = session_proxy.call::<_, _, ()>("Start", &()) {
        eprintln!("mutter session start: {error}");
        return None;
    }
    let node_id = added.and_then(|mut signals| {
        let message = signals.next()?;
        message.body().deserialize::<u32>().ok()
    });
    let remote = if let Some(node_id) = node_id {
        match pipewire_socket() {
            Some(fd) => (fd, Some(node_id)),
            None => return None,
        }
    } else {
        match stream.call::<_, _, zbus::zvariant::OwnedFd>("Start", &()) {
            Ok(fd) => (OwnedFd::from(fd), None),
            Err(error) => {
                eprintln!("mutter stream start: {error}");
                return None;
            }
        }
    };
    let mut pump = match Pump::start(remote.0, remote.1, region, Some(connection)) {
        Ok(pump) => pump,
        Err(error) => {
            eprintln!("mutter pipewire: {error}");
            return None;
        }
    };
    if let Err(error) = pump.wait_ready(FRAME_WAIT) {
        eprintln!("mutter frame: {error}");
        return None;
    }
    Some(pump)
}

fn pipewire_socket() -> Option<OwnedFd> {
    let runtime = std::env::var("XDG_RUNTIME_DIR").ok()?;
    let remote = std::env::var("PIPEWIRE_REMOTE").unwrap_or_else(|_| "pipewire-0".to_owned());
    let path = if remote.starts_with('/') {
        remote
    } else {
        format!("{runtime}/{remote}")
    };
    UnixStream::connect(path).ok().map(OwnedFd::from)
}

fn restore_token_file() -> Option<std::path::PathBuf> {
    let cache = std::env::var_os("XDG_CACHE_HOME")
        .map(std::path::PathBuf::from)
        .filter(|path| !path.as_os_str().is_empty())
        .or_else(|| {
            std::env::var_os("HOME").map(|home| std::path::PathBuf::from(home).join(".cache"))
        })?;
    Some(cache.join("snow-shot").join("screencast-restore-token"))
}

fn read_restore_token() -> Option<String> {
    read_token_file(&restore_token_file()?)
}

fn write_restore_token(token: &str) {
    if let Some(path) = restore_token_file() {
        let _ = write_token_file(&path, token);
    }
}

fn read_token_file(path: &std::path::Path) -> Option<String> {
    let token = std::fs::read_to_string(path).ok()?.trim().to_owned();
    if token.is_empty() { None } else { Some(token) }
}

fn write_token_file(path: &std::path::Path, token: &str) -> bool {
    let token = token.trim();
    if token.is_empty() {
        return false;
    }
    let Some(parent) = path.parent() else {
        return false;
    };
    if std::fs::create_dir_all(parent).is_err() {
        return false;
    }
    let temporary = path.with_extension("tmp");
    if std::fs::write(&temporary, token.as_bytes()).is_err() {
        return false;
    }
    std::fs::rename(temporary, path).is_ok()
}

fn owned_string(value: Option<&OwnedValue>) -> Option<String> {
    let value: Value = value?.try_clone().ok()?.into();
    match value {
        Value::Str(text) => {
            let token = text.as_str().trim().to_owned();
            if token.is_empty() { None } else { Some(token) }
        }
        _ => None,
    }
}

fn portal_select_options(saved_token: Option<&str>) -> HashMap<String, Value<'static>> {
    let mut sources = HashMap::new();
    sources.insert("types".to_owned(), Value::U32(1));
    sources.insert("cursor_mode".to_owned(), Value::U32(1));
    sources.insert("multiple".to_owned(), Value::Bool(false));
    // Keep the user's screen choice until they revoke it. The next screenshot
    // passes the token back and the portal does not open the picker again.
    sources.insert("persist_mode".to_owned(), Value::U32(PERSIST_UNTIL_REVOKED));
    if let Some(token) = saved_token.map(str::trim).filter(|token| !token.is_empty()) {
        sources.insert("restore_token".to_owned(), string_value(token));
    }
    sources
}

fn token() -> String {
    static NEXT: AtomicU64 = AtomicU64::new(1);
    format!(
        "snow{}{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    )
}

fn string_value(text: &str) -> Value<'static> {
    Value::new(text.to_owned())
}

fn object_path(value: Option<&OwnedValue>) -> Option<OwnedObjectPath> {
    let value: Value = value?.try_clone().ok()?.into();
    match value {
        Value::ObjectPath(path) => OwnedObjectPath::try_from(path.as_str()).ok(),
        Value::Str(text) => OwnedObjectPath::try_from(text.as_str()).ok(),
        _ => None,
    }
}

fn first_stream(value: OwnedValue) -> Option<StreamInfo> {
    let value: Value = value.into();
    let Value::Array(array) = value else {
        return None;
    };
    let item: Value = array.get(0).ok()??;
    let Value::Structure(structure) = item else {
        return None;
    };
    let fields = structure.fields();
    let node_id = match fields.first()? {
        Value::U32(id) => *id,
        _ => return None,
    };
    let mut info = StreamInfo { node_id, x: 0, y: 0 };
    if let Some(Value::Dict(dict)) = fields.get(1) {
        if let Some((x, y)) = dict_pair(dict, "position") {
            info.x = x;
            info.y = y;
        }
    }
    Some(info)
}

fn dict_pair(dict: &zbus::zvariant::Dict, key: &str) -> Option<(i32, i32)> {
    let value: Value = dict.get(&key).ok()??;
    let Value::Structure(structure) = value else {
        return None;
    };
    let fields = structure.fields();
    Some((as_i32(fields.first()?)?, as_i32(fields.get(1)?)?))
}

fn as_i32(value: &Value) -> Option<i32> {
    match value {
        Value::I32(value) => Some(*value),
        Value::U32(value) => i32::try_from(*value).ok(),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::{
        area_sources, portal_select_options, read_token_file, write_token_file, AreaSource,
        PERSIST_UNTIL_REVOKED,
    };
    use zbus::zvariant::Value;

    #[test]
    fn desktop_capture_uses_mutter_before_the_share_dialog() {
        assert_eq!(area_sources(), &[AreaSource::Mutter, AreaSource::Portal]);
    }

    #[test]
    fn portal_remembers_a_screen_and_restores_it_next_time() {
        let first = portal_select_options(None);
        assert!(matches!(first.get("persist_mode"), Some(Value::U32(mode)) if *mode == PERSIST_UNTIL_REVOKED));
        assert!(!first.contains_key("restore_token"));

        let again = portal_select_options(Some(" remembered-screen "));
        assert!(matches!(again.get("restore_token"), Some(Value::Str(token)) if token.as_str() == "remembered-screen"));
        assert!(matches!(again.get("types"), Some(Value::U32(kind)) if *kind == 1));
    }

    #[test]
    fn restore_token_round_trips_and_ignores_blanks() {
        let directory = std::env::temp_dir().join(format!(
            "snow-shot-screencast-token-{}-{}",
            std::process::id(),
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .expect("clock")
                .as_nanos()
        ));
        let path = directory.join("screencast-restore-token");
        assert!(!write_token_file(&path, "  "));
        assert!(read_token_file(&path).is_none());
        assert!(write_token_file(&path, " remembered-screen \n"));
        assert_eq!(
            read_token_file(&path).as_deref(),
            Some("remembered-screen")
        );
        let _ = std::fs::remove_dir_all(directory);
    }
}
