// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotedgeregionindex.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {
using snow_shot::presentation::EdgeRegionIndex;
using snow_shot::presentation::EdgeRegionMode;
using snow_shot::presentation::EdgeRegionRect;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Frame {
    int width;
    int height;
    int stride;
    std::vector<std::uint8_t> data;

    Frame(int w, int h, std::uint8_t value, int padding = 0)
        : width(w), height(h), stride(w * 4 + padding),
          data(static_cast<size_t>(stride) * static_cast<size_t>(h), value) {}

    void fill(const EdgeRegionRect& r, std::uint8_t value) {
        for (int y = r.top; y < r.bottom; ++y)
            for (int x = r.left; x < r.right; ++x)
                for (int c = 0; c < 4; ++c)
                    data[static_cast<size_t>(y) * static_cast<size_t>(stride) +
                         static_cast<size_t>(x) * 4 + static_cast<size_t>(c)] = value;
    }
    [[nodiscard]] std::shared_ptr<const EdgeRegionIndex> index() const {
        return EdgeRegionIndex::build(data.data(), width, height, stride);
    }
};

constexpr EdgeRegionRect kWindow{50, 40, 350, 260};
constexpr EdgeRegionRect kPanel{100, 80, 300, 200};

Frame nestedFrame(int padding = 0) {
    Frame frame(400, 300, 200, padding);
    frame.fill(kWindow, 40);
    frame.fill(kPanel, 120);
    return frame;
}

void windowModeReturnsOutermostWindow() {
    const auto index = nestedFrame().index();
    require(index != nullptr, "index must build");
    const auto regions = index->regionsAt(200, 140, EdgeRegionMode::Window);
    require(regions.size() == 1 && regions[0] == kWindow,
            "window mode must report the enclosing window");
}

void elementModeReturnsNestedRegions() {
    const auto index = nestedFrame().index();
    const auto regions = index->regionsAt(200, 140, EdgeRegionMode::Element);
    require(regions.size() == 2, "element mode must report panel and window");
    require(regions[0] == kPanel, "innermost region must come first");
    require(regions[1] == kWindow, "outermost region must come last");
}

void pointInWindowOutsidePanelStopsAtWindow() {
    const auto index = nestedFrame().index();
    const auto regions = index->regionsAt(70, 60, EdgeRegionMode::Element);
    require(regions.size() == 1 && regions[0] == kWindow,
            "a point outside the panel must resolve to the window");
}

void desktopBackgroundIsNotReported() {
    const auto index = nestedFrame().index();
    require(index->regionsAt(10, 10, EdgeRegionMode::Window).empty(),
            "background that spans the whole frame must not be a region");
    require(index->regionsAt(-1, 5, EdgeRegionMode::Window).empty(), "negative x is outside");
    require(index->regionsAt(5, 300, EdgeRegionMode::Window).empty(), "y past the frame is outside");
}

void shortStrokesAreIgnored() {
    Frame frame = nestedFrame();
    // A glyph-sized stroke inside the panel is shorter than the minimum edge length.
    frame.fill({150, 120, 160, 140}, 250);
    const auto index = frame.index();
    const auto regions = index->regionsAt(155, 130, EdgeRegionMode::Element);
    require(!regions.empty() && regions[0] == kPanel, "short strokes must not form regions");
}

void strideWithPaddingIsHonoured() {
    const auto index = nestedFrame(24).index();
    require(index != nullptr, "padded stride must build");
    const auto regions = index->regionsAt(200, 140, EdgeRegionMode::Window);
    require(regions.size() == 1 && regions[0] == kWindow, "padding must not leak into pixels");
}

void invalidInputIsRejected() {
    std::vector<std::uint8_t> pixels(64, 0);
    require(EdgeRegionIndex::build(nullptr, 4, 4, 16) == nullptr, "null pixels");
    require(EdgeRegionIndex::build(pixels.data(), 1, 4, 16) == nullptr, "width below 2");
    require(EdgeRegionIndex::build(pixels.data(), 4, 4, 8) == nullptr, "stride below row size");
}

void fullHdBuildAndQueryAreFast() {
    Frame frame(3440, 1440, 235);
    frame.fill({200, 100, 1800, 1300}, 30);
    frame.fill({300, 200, 1700, 700}, 160);
    const auto begin = std::chrono::steady_clock::now();
    const auto index = frame.index();
    const auto built = std::chrono::steady_clock::now();
    std::vector<EdgeRegionRect> regions;
    for (int i = 0; i < 1000; ++i)
        regions = index->regionsAt(400 + i, 300, EdgeRegionMode::Element);
    const auto queried = std::chrono::steady_clock::now();
    require(regions.size() == 2, "wide frame must resolve nested regions");
    using Ms = std::chrono::duration<double, std::milli>;
    std::cout << "build " << Ms(built - begin).count() << " ms, 1000 queries "
              << Ms(queried - built).count() << " ms\n";
}
} // namespace

int main() {
    windowModeReturnsOutermostWindow();
    elementModeReturnsNestedRegions();
    pointInWindowOutsidePanelStopsAtWindow();
    desktopBackgroundIsNotReported();
    shortStrokesAreIgnored();
    strideWithPaddingIsHonoured();
    invalidInputIsRejected();
    fullHdBuildAndQueryAreFast();
    return 0;
}
