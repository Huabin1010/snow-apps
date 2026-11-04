#!/bin/bash
# Build a Debian package from the Linux Snow Shot build.
# The package keeps private libraries next to the executable so a normal
# apt install can launch Snow Shot without the build tree or /opt.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="${SNOW_SHOT_VERSION:-1.3.7}"
arch="$(dpkg --print-architecture)"
preset="${1:-snow-shot-linux-x64-debug}"
build_dir="$root/build/$preset/snow_shot"
jpeg_dir="${SNOW_SHOT_JPEG_DIR:-/opt/libjpeg-turbo-3.1.1/lib}"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

app_name="snow-shot"
pkg_dir="$stage/${app_name}_${version}_${arch}"
app_dir="$pkg_dir/usr/lib/snow-shot"

require() {
    if [[ ! -e "$1" ]]; then
        echo "Missing $1. Build Snow Shot before packaging." >&2
        exit 1
    fi
}

require "$build_dir/snow_shot"
require "$build_dir/snow-ocr-process"
require "$build_dir/snow-shot-mcp"
require "$build_dir/snow-shot-updater"
require "$build_dir/snow-shot-shortcut"
require "$build_dir/libsnow_shot_image_codec_backend.so"
require "$build_dir/assets/qrcode/detect.prototxt"
require "$root/snow_shot/resources/audios/camera_shutter.mp3"
require "$root/snow_shot/resources/app-icon.svg"
require "$jpeg_dir/libjpeg.so.8"
require "$jpeg_dir/libturbojpeg.so.0"

mkdir -p "$app_dir/assets/qrcode" "$app_dir/audios" \
    "$pkg_dir/usr/bin" \
    "$pkg_dir/usr/share/applications" \
    "$pkg_dir/usr/share/icons/hicolor/scalable/apps" \
    "$pkg_dir/DEBIAN"

install -m 0755 "$build_dir/snow_shot" "$app_dir/snow-shot"
# The GNOME key-binding helper starts "snow_shot" next to itself when the
# application is not running yet.
ln -s snow-shot "$app_dir/snow_shot"
install -m 0755 "$build_dir/snow-shot-shortcut" "$app_dir/snow-shot-shortcut"
install -m 0755 "$build_dir/snow-ocr-process" "$app_dir/snow-ocr-process"
install -m 0755 "$build_dir/snow-shot-mcp" "$app_dir/snow-shot-mcp"
install -m 0755 "$build_dir/snow-shot-updater" "$app_dir/snow-shot-updater"
install -m 0644 "$build_dir/libsnow_shot_image_codec_backend.so" \
    "$app_dir/libsnow_shot_image_codec_backend.so"
install -m 0644 "$(readlink -f "$jpeg_dir/libjpeg.so.8")" "$app_dir/libjpeg.so.8"
install -m 0644 "$(readlink -f "$jpeg_dir/libturbojpeg.so.0")" "$app_dir/libturbojpeg.so.0"
install -m 0644 "$build_dir/assets/qrcode/detect.prototxt" \
    "$build_dir/assets/qrcode/detect.caffemodel" \
    "$build_dir/assets/qrcode/sr.prototxt" \
    "$build_dir/assets/qrcode/sr.caffemodel" \
    "$app_dir/assets/qrcode/"
install -m 0644 "$root/snow_shot/resources/audios/camera_shutter.mp3" \
    "$app_dir/audios/camera_shutter.mp3"
install -m 0644 "$root/snow_shot/resources/app-icon.svg" \
    "$pkg_dir/usr/share/icons/hicolor/scalable/apps/snow-shot.svg"
cat > "$pkg_dir/usr/bin/snow-shot" << 'EOF'
#!/bin/sh
exec /usr/lib/snow-shot/snow-shot "$@"
EOF
chmod 0755 "$pkg_dir/usr/bin/snow-shot"

cat > "$pkg_dir/usr/share/applications/snow-shot.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=Snow Shot
Comment=Capture, scroll, and record the screen
Exec=snow-shot %F
Icon=snow-shot
Terminal=false
Categories=Graphics;Utility;
MimeType=image/png;image/jpeg;image/gif;image/webp;image/bmp;image/tiff;
StartupNotify=true
Actions=edit;

[Desktop Action edit]
Name=Edit with Snow Shot
Exec=snow-shot --edit-with-snow-shot %f
EOF

strip --strip-unneeded \
    "$app_dir/snow-shot" \
    "$app_dir/snow-ocr-process" \
    "$app_dir/snow-shot-mcp" \
    "$app_dir/snow-shot-updater" \
    "$app_dir/snow-shot-shortcut" \
    "$app_dir/libsnow_shot_image_codec_backend.so" \
    "$app_dir/libjpeg.so.8" \
    "$app_dir/libturbojpeg.so.0"

for binary in \
    "$app_dir/snow-shot" \
    "$app_dir/snow-ocr-process" \
    "$app_dir/snow-shot-mcp" \
    "$app_dir/snow-shot-updater" \
    "$app_dir/libsnow_shot_image_codec_backend.so"
do
    patchelf --set-rpath '$ORIGIN' "$binary"
done

if ldd "$app_dir/snow-shot" | grep -q 'not found'; then
    echo "The packaged executable is missing a library:" >&2
    ldd "$app_dir/snow-shot" | grep 'not found' >&2
    exit 1
fi

# OCR uses the bundled runtime layout: assets/ocr/asset-manifest.json describes
# the stripped snow-ocr-process next to the executable, and the default small
# model ships under assets/ocr/models. Other model sizes download on demand.
# Hash the worker only after strip and patchelf have rewritten it.
case "$arch" in
    amd64) ocr_platform="linux-x64" ;;
    arm64) ocr_platform="linux-arm64" ;;
    *) echo "Unsupported OCR architecture: $arch" >&2; exit 1 ;;
esac
python3 - "$root/snow_shot/packaging/snow-shot-ocr-asset-manifest.json" \
    "${SNOW_SHOT_OCR_CACHE:-$root/artifacts}" "$app_dir" "$ocr_platform" << 'EOF'
import hashlib, json, shutil, subprocess, sys, tempfile, urllib.request
from pathlib import Path

source = json.loads(Path(sys.argv[1]).read_text())
cache, app, platform = Path(sys.argv[2]), Path(sys.argv[3]), sys.argv[4]
worker = app / 'snow-ocr-process'

def descriptor(path):
    digest = hashlib.sha256()
    with path.open('rb') as data:
        for chunk in iter(lambda: data.read(1 << 20), b''):
            digest.update(chunk)
    return dict(name=path.name, size=path.stat().st_size, sha256=digest.hexdigest())

def valid(path, item):
    if not path.is_file() or path.stat().st_size != item['size']:
        return False
    return descriptor(path)['sha256'] == item['sha256']

def fetch(item, destination):
    if valid(destination, item):
        return
    if not item['url'].startswith('https://'):
        raise SystemExit('OCR models require HTTPS')
    destination.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=destination.parent, delete=False) as output:
        partial = Path(output.name)
        try:
            print(f'Downloading {item["name"]}', flush=True)
            request = urllib.request.Request(
                item['url'], headers={'Referer': 'https://www.modelscope.cn/'})
            with urllib.request.urlopen(request, timeout=120) as response:
                shutil.copyfileobj(response, output)
            output.close()
            if not valid(partial, item):
                raise SystemExit(f'OCR model integrity check failed: {item["name"]}')
            partial.replace(destination)
        finally:
            partial.unlink(missing_ok=True)

version = source['runtime']['version']
machine = 'x86_64' if platform == 'linux-x64' else 'aarch64'
expected = f'snow-ocr-process {version} linux-{machine} protocol 6'
actual = subprocess.check_output([str(worker), '--version'], text=True, timeout=60).strip()
if actual != expected:
    raise SystemExit(f'OCR worker reports "{actual}", expected "{expected}"')

ocr = app / 'assets/ocr'
for model in source['models']:
    if model['type'] != 'small':
        continue
    directory = ocr / 'models' / model['id']
    directory.mkdir(parents=True, exist_ok=True)
    for item in model['files']:
        cached = cache / ('ocr-models-' + model['id']) / item['name']
        fetch(item, cached)
        shutil.copyfile(cached, directory / item['name'])
        (directory / item['name']).chmod(0o644)
    (directory / '.complete.json').write_text(
        json.dumps(dict(schema=1, component=model['id'])) + '\n')

manifest = dict(schema=3, default_model='small', runtime=dict(
    version=version, platform=platform, delivery='bundled', protocol=6,
    executable=worker.name, files=[descriptor(worker)]), models=source['models'])
(ocr / 'asset-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
EOF
find "$app_dir/assets/ocr" -type d -exec chmod 0755 {} +
find "$app_dir/assets/ocr" -type f -exec chmod 0644 {} +

mkdir -p "$stage/debian"
printf 'Source: snow-shot\n' > "$stage/debian/control"
depends="$(
    cd "$stage"
    dpkg-shlibdeps --ignore-missing-info -O \
        "$app_dir/snow-shot" \
        "$app_dir/snow-ocr-process" \
        "$app_dir/snow-shot-mcp" \
        "$app_dir/snow-shot-updater" \
        "$app_dir/snow-shot-shortcut" \
        "$app_dir/libsnow_shot_image_codec_backend.so" \
        | sed -n 's/^shlibs:Depends=//p'
)"
runtime_depends="libqt6waylandclient6, qt6-gtk-platformtheme, qt6-svg-plugins, pipewire, xdg-desktop-portal, xdg-desktop-portal-gnome"
if [[ -n "$depends" ]]; then
    depends="${depends}, ${runtime_depends}"
else
    depends="$runtime_depends"
fi

installed_size="$(du -sk "$pkg_dir" | awk '{print $1}')"
cat > "$pkg_dir/DEBIAN/control" << EOF
Package: snow-shot
Version: ${version}
Section: graphics
Priority: optional
Architecture: ${arch}
Maintainer: Snow Shot <chao@mgchao.top>
Homepage: https://snowshot.top
Installed-Size: ${installed_size}
Depends: ${depends}
Description: Capture, scroll, and record the screen
 Snow Shot captures a region, window, or scrolling page and records the
 screen on GNOME Wayland and X11.
EOF

cat > "$pkg_dir/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor || true
fi
EOF
cat > "$pkg_dir/DEBIAN/postrm" << 'EOF'
#!/bin/sh
set -e
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q /usr/share/applications || true
fi
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q /usr/share/icons/hicolor || true
fi
EOF
chmod 0755 "$pkg_dir/DEBIAN/postinst" "$pkg_dir/DEBIAN/postrm"

mkdir -p "$root/dist"
output="$root/dist/${app_name}_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$pkg_dir" "$output"
echo "$output"
