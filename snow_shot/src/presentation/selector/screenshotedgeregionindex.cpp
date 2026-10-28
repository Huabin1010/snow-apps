// SPDX-License-Identifier: GPL-3.0-or-later
#include "snow_shot/presentation/screenshotedgeregionindex.h"

#include <algorithm>
#include <cstdlib>

namespace snow_shot::presentation {
namespace {
// Gaps up to this many pixels do not break an edge, so icons or anti-aliased corners that
// cross a window border still count as one straight line.
constexpr int kMaximumEdgeGap = 3;
constexpr int kMaximumWindowLevels = 64;

using Segment = EdgeRegionIndex::Segment;
using SegmentList = std::vector<Segment>;

[[nodiscard]] inline bool differs(const std::uint8_t* a, const std::uint8_t* b, int threshold) {
    return std::abs(int(a[0]) - int(b[0])) + std::abs(int(a[1]) - int(b[1])) +
               std::abs(int(a[2]) - int(b[2])) >
           threshold;
}

// Accumulates consecutive edge flags into segments, tolerating short gaps.
struct RunTracker {
    int start = -1;
    int last = -1;

    void add(int position, bool edge, int minimumLength, SegmentList& out) {
        if (edge) {
            if (start >= 0 && position - last > kMaximumEdgeGap + 1)
                close(minimumLength, out);
            if (start < 0)
                start = position;
            last = position;
        } else if (start >= 0 && position - last > kMaximumEdgeGap) {
            close(minimumLength, out);
        }
    }
    void close(int minimumLength, SegmentList& out) {
        if (start >= 0 && last + 1 - start >= minimumLength)
            out.push_back({start, last + 1});
        start = -1;
        last = -1;
    }
};

[[nodiscard]] bool covers(const SegmentList& segments, int position) {
    const auto it = std::lower_bound(segments.begin(), segments.end(), position,
                                     [](const Segment& s, int p) { return s.end <= p; });
    return it != segments.end() && it->begin <= position;
}

// Walks boundary indices from `from` to `to` inclusive (either direction, both must be valid
// boundaries) and returns the first one with an edge covering `position`, else `fallback`.
[[nodiscard]] int nearestBoundary(const std::vector<SegmentList>& boundaries, int position,
                                  int from, int to, int fallback) {
    const int step = to >= from ? 1 : -1;
    for (int b = from; b != to + step; b += step) {
        if (b >= 1 && b < static_cast<int>(boundaries.size()) && !boundaries[b].empty() &&
            covers(boundaries[b], position))
            return b;
    }
    return fallback;
}
} // namespace

std::shared_ptr<const EdgeRegionIndex> EdgeRegionIndex::build(const std::uint8_t* pixels,
                                                              int width, int height,
                                                              std::ptrdiff_t strideBytes,
                                                              const Options& options) {
    if (pixels == nullptr || width < 2 || height < 2 || strideBytes < width * 4LL)
        return nullptr;
    std::shared_ptr<EdgeRegionIndex> index(new EdgeRegionIndex());
    index->m_width = width;
    index->m_height = height;
    index->m_options = options;
    index->m_horizontal.resize(static_cast<size_t>(height));
    index->m_vertical.resize(static_cast<size_t>(width));

    const int threshold = options.edgeThreshold;
    const int minimum = options.minimumEdgeLength;
    std::vector<RunTracker> columnRuns(static_cast<size_t>(width));

    for (int y = 0; y < height; ++y) {
        const std::uint8_t* row = pixels + static_cast<std::ptrdiff_t>(y) * strideBytes;
        const std::uint8_t* previous = y > 0 ? row - strideBytes : nullptr;
        RunTracker horizontalRun;
        SegmentList& horizontal = index->m_horizontal[static_cast<size_t>(y)];
        for (int x = 0; x < width; ++x) {
            const std::uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 4;
            if (previous != nullptr) {
                horizontalRun.add(x, differs(pixel, previous + static_cast<std::ptrdiff_t>(x) * 4,
                                             threshold),
                                  minimum, horizontal);
            }
            if (x > 0) {
                columnRuns[static_cast<size_t>(x)].add(
                    y, differs(pixel, pixel - 4, threshold), minimum,
                    index->m_vertical[static_cast<size_t>(x)]);
            }
        }
        horizontalRun.close(minimum, horizontal);
    }
    for (int x = 1; x < width; ++x)
        columnRuns[static_cast<size_t>(x)].close(minimum, index->m_vertical[static_cast<size_t>(x)]);
    return index;
}

std::vector<EdgeRegionRect> EdgeRegionIndex::regionsAt(int x, int y, EdgeRegionMode mode) const {
    if (x < 0 || y < 0 || x >= m_width || y >= m_height)
        return {};

    const double frameArea = static_cast<double>(m_width) * m_height;
    const auto acceptable = [&](const EdgeRegionRect& r) {
        return static_cast<double>(r.width()) * r.height() < frameArea * m_options.areaLimit;
    };

    std::vector<EdgeRegionRect> accepted;
    EdgeRegionRect current;
    bool first = true;
    const int levels = mode == EdgeRegionMode::Window ? kMaximumWindowLevels
                                                      : std::max(1, m_options.maximumLevels);
    for (int level = 0; level < levels; ++level) {
        EdgeRegionRect next;
        if (first) {
            // Boundary b separates pixels b - 1 and b, so the first candidates on each side of
            // the point are y / x (towards the origin) and y + 1 / x + 1 (away from it). A point
            // on the frame border has no candidate on that side and uses the frame edge.
            next.top = y >= 1 ? nearestBoundary(m_horizontal, x, y, 1, 0) : 0;
            next.bottom = y + 1 <= m_height - 1
                              ? nearestBoundary(m_horizontal, x, y + 1, m_height - 1, m_height)
                              : m_height;
            next.left = x >= 1 ? nearestBoundary(m_vertical, y, x, 1, 0) : 0;
            next.right = x + 1 <= m_width - 1
                             ? nearestBoundary(m_vertical, y, x + 1, m_width - 1, m_width)
                             : m_width;
        } else {
            next.top = current.top > 1
                           ? nearestBoundary(m_horizontal, x, current.top - 1, 1, 0)
                           : 0;
            next.bottom = current.bottom < m_height - 1
                              ? nearestBoundary(m_horizontal, x, current.bottom + 1, m_height - 1,
                                                m_height)
                              : m_height;
            next.left = current.left > 1
                            ? nearestBoundary(m_vertical, y, current.left - 1, 1, 0)
                            : 0;
            next.right = current.right < m_width - 1
                             ? nearestBoundary(m_vertical, y, current.right + 1, m_width - 1,
                                               m_width)
                             : m_width;
            if (next == current)
                break;
        }
        current = next;
        first = false;
        if (!acceptable(current))
            break;
        accepted.push_back(current);
    }

    if (accepted.empty())
        return {};
    if (mode == EdgeRegionMode::Window)
        return {accepted.back()};
    return accepted;
}

} // namespace snow_shot::presentation
