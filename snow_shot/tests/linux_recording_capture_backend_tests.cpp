#include "snow_capture.h"
#include "snow_recording.h"

#include <cstring>
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool contains(const char* haystack, const char* needle) {
    return haystack != nullptr && needle != nullptr && std::strstr(haystack, needle) != nullptr;
}

SnowCaptureDirectRecordingConfig legacyDxgiConfig(const char* outputPath) {
    SnowCaptureDirectRecordingConfig config{};
    config.version = SNOW_CAPTURE_DIRECT_RECORDING_CONFIG_VERSION;
    config.struct_size = sizeof(SnowCaptureDirectRecordingConfig);
    config.x = 40;
    config.y = 40;
    config.width = 320;
    config.height = 240;
    config.capture_backend = SNOW_CAPTURE_BACKEND_DXGI;
    config.output_file_utf8 = outputPath;
    config.output_format = SNOW_RECORDING_OUTPUT_FORMAT_MP4;
    config.capture_fps = 30;
    config.output_fps = 30;
    config.maximum_width = 1920;
    config.maximum_height = 1080;
    config.codec = SNOW_CAPTURE_VIDEO_CODEC_H264;
    config.preset = SNOW_CAPTURE_VIDEO_ENCODING_PRESET_VERYFAST;
    config.encoder_preference = SNOW_CAPTURE_ENCODER_PREFERENCE_SOFTWARE;
    config.quality = 80;
    config.show_cursor = 1;
    config.mouse_highlight_radius = 41;
    config.mouse_trail_duration_ms = 500;
    config.keyboard_size = 64;
    return config;
}
} // namespace

int main() {
    const char* outputPath = "/tmp/snow-shot-linux-recording-backend-test.mp4";
    const SnowCaptureDirectRecordingConfig config = legacyDxgiConfig(outputPath);
    SnowRecordingSession* session = nullptr;
    const SnowRecordingResult created =
        snow_recording_session_create_direct(&config, &session);
    if (created != SNOW_RECORDING_RESULT_OK) {
        const char* error = snow_recording_last_error_message();
        require(!contains(error, "unavailable on Linux"),
                "legacy DXGI preference must not reject Linux recording backends");
        // Portal, display, or encoder setup may still fail on headless CI.
        return 0;
    }
    require(session != nullptr, "a successful create must return a session");
    snow_recording_session_destroy(session);
    return 0;
}
