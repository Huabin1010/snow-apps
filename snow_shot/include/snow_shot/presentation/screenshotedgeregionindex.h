// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef SNOW_SHOT_PRESENTATION_SCREENSHOTEDGEREGIONINDEX_H
#define SNOW_SHOT_PRESENTATION_SCREENSHOTEDGEREGIONINDEX_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace snow_shot::presentation {

// Half-open pixel rectangle: [left, right) x [top, bottom).
struct EdgeRegionRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    [[nodiscard]] int width() const { return right - left; }
    [[nodiscard]] int height() const { return bottom - top; }
    [[nodiscard]] bool operator==(const EdgeRegionRect&) const = default;
};

enum class EdgeRegionMode {
    // A single rectangle that approximates the enclosing window.
    Window,
    // Nested rectangles ordered from the innermost region to the outermost one.
    Element,
};

// Finds window-like regions in a captured frame without asking the window system. Long straight
// colour discontinuities are indexed once; a query then walks outwards from a point to the nearest
// enclosing edges. Queries are read-only and cheap enough to run on every mouse move.
class EdgeRegionIndex {
  public:
    struct Options {
        // Sum of absolute per-channel differences that counts as an edge between two pixels.
        int edgeThreshold = 30;
        // Shortest straight edge, in pixels, that can bound a region. Text strokes stay below it.
        int minimumEdgeLength = 32;
        int maximumLevels = 8;
        // Regions covering at least this share of the frame are not reported.
        double areaLimit = 0.95;
    };

    struct Segment {
        int begin = 0; // first pixel along the edge
        int end = 0;   // one past the last pixel along the edge
    };

    // `pixels` holds 4 bytes per pixel. Channel order is irrelevant because only differences
    // between neighbouring pixels are compared. Returns null for invalid input.
    [[nodiscard]] static std::shared_ptr<const EdgeRegionIndex>
    build(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t strideBytes,
          const Options& options);
    [[nodiscard]] static std::shared_ptr<const EdgeRegionIndex>
    build(const std::uint8_t* pixels, int width, int height, std::ptrdiff_t strideBytes) {
        return build(pixels, width, height, strideBytes, Options{});
    }

    [[nodiscard]] int width() const { return m_width; }
    [[nodiscard]] int height() const { return m_height; }

    // Empty when the point is outside the frame or no bounded region surrounds it.
    [[nodiscard]] std::vector<EdgeRegionRect> regionsAt(int x, int y, EdgeRegionMode mode) const;

  private:
    EdgeRegionIndex() = default;

    int m_width = 0;
    int m_height = 0;
    Options m_options;
    // Indexed by boundary: horizontal[y] separates pixel rows y - 1 and y, vertical[x] separates
    // pixel columns x - 1 and x. Segments of one boundary are sorted and disjoint.
    std::vector<std::vector<Segment>> m_horizontal;
    std::vector<std::vector<Segment>> m_vertical;
};

} // namespace snow_shot::presentation

#endif // SNOW_SHOT_PRESENTATION_SCREENSHOTEDGEREGIONINDEX_H
