#!/bin/bash
# Build a Debian package from the Linux Snow Shot build.
# The package keeps private libraries next to the executable so a normal
# apt install can launch Snow Shot without the build tree or /opt.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
version="${SNOW_SHOT_VERSION:-1.3.4}"
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

mkdir -p "$stage/debian"
printf 'Source: snow-shot\n' > "$stage/debian/control"
depends="$(
    cd "$stage"
    dpkg-shlibdeps --ignore-missing-info -O \
        "$app_dir/snow-shot" \
        "$app_dir/snow-ocr-process" \
        "$app_dir/snow-shot-mcp" \
        "$app_dir/snow-shot-updater" \
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
