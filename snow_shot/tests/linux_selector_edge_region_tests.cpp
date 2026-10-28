// SPDX-License-Identifier: GPL-3.0-or-later
#include "../src/presentation/selector/screenshotselectorserviceclient.h"

#include <QCoreApplication>
#include <QImage>
#include <QPainter>

#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

struct Recorder {
    int refreshCount = 0;
    bool refreshOk = false;
    int resultCount = 0;
    ScreenshotSelectorResult last;
};

QImage nestedImage(int& released) {
    struct Storage {
        QImage source;
        int* released;
    };
    QImage source(400, 300, QImage::Format_RGB32);
    source.fill(QColor(200, 200, 200));
    QPainter painter(&source);
    painter.fillRect(QRect(50, 40, 300, 220), QColor(40, 40, 40));
    painter.fillRect(QRect(100, 80, 200, 120), QColor(120, 120, 120));
    painter.end();
    // Re-wrap the pixels so the test can observe when the last reference disappears.
    auto* storage = new Storage{source, &released};
    return QImage(
        storage->source.constBits(), source.width(), source.height(), source.bytesPerLine(),
        QImage::Format_RGB32,
        [](void* data) {
            auto* s = static_cast<Storage*>(data);
            ++*s->released;
            delete s;
        },
        storage);
}

CapturedDisplayModel display(const QImage& image, quint32 id, const QPoint& origin) {
    CapturedDisplayModel model;
    model.image = image;
    model.nativeDisplayId = id;
    model.physicalRect = QRect(origin, image.size());
    return model;
}

void runClient() {
    Recorder recorder;
    ScreenshotSelectorServiceClient client(ScreenshotSelectorServiceClientCallbacks{
        [&recorder](quint64, bool ok) {
            ++recorder.refreshCount;
            recorder.refreshOk = ok;
        },
        [&recorder](const ScreenshotSelectorResult& result) {
            ++recorder.resultCount;
            recorder.last = result;
        }});

    int released = 0;
    {
        const QVector<CapturedDisplayModel> displays{
            display(nestedImage(released), 7, QPoint(0, 0)),
            display(nestedImage(released), 9, QPoint(1000, 0))};
        require(client.startRefreshWithDisplays(1, {}, displays), "refresh must start");
    }
    require(released == 2, "the client must not keep the captured pixels after a refresh");
    QCoreApplication::sendPostedEvents();
    require(recorder.refreshCount == 1 && recorder.refreshOk, "refresh must report success");
    require(client.hasService(), "service must be active after a refresh");

    // A point inside the inner panel of the second display maps back to desktop coordinates.
    const QRectF window(1050, 40, 300, 220);
    for (const auto mode : {ScreenshotSelectorHitTestMode::Window,
                            ScreenshotSelectorHitTestMode::WindowSubElement}) {
        recorder.resultCount = 0;
        require(client.startHitTest(1, 5, 3, QPoint(1200, 140), mode, 9), "hit test must start");
        QCoreApplication::sendPostedEvents();
        require(recorder.resultCount == 1, "hit test must deliver exactly one result");
        const auto& result = recorder.last;
        require(result.ok && result.epoch == 1 && result.requestId == 5 && result.generation == 3,
                "result must echo the request identity");
        require(result.displayId == 9, "result must name the display that was hit");
        require(!result.rects.isEmpty() && result.rects.last() == window,
                "the outermost rectangle must be the enclosing window in desktop coordinates");
        if (mode == ScreenshotSelectorHitTestMode::Window)
            require(result.rects.size() == 1, "window mode must report one rectangle");
    }

    recorder.resultCount = 0;
    require(client.startHitTest(1, 6, 4, QPoint(5000, 5000),
                                ScreenshotSelectorHitTestMode::Window, 0),
            "off-screen hit test must still answer");
    QCoreApplication::sendPostedEvents();
    require(recorder.resultCount == 1 && !recorder.last.ok && recorder.last.rects.isEmpty(),
            "a point outside every display must report no region");

    require(client.releaseCache(), "releaseCache must succeed");
    recorder.resultCount = 0;
    require(client.startHitTest(1, 7, 5, QPoint(1200, 140), ScreenshotSelectorHitTestMode::Window,
                                9),
            "hit test after releasing the cache must answer");
    QCoreApplication::sendPostedEvents();
    require(recorder.resultCount == 1 && !recorder.last.ok,
            "a released cache must not report stale regions");

    client.destroyService();
    require(!client.hasService(), "destroyed service must be inactive");
    require(!client.startHitTest(1, 8, 6, QPoint(0, 0), ScreenshotSelectorHitTestMode::Window, 0),
            "destroyed service must reject hit tests");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    runClient();
    return 0;
}
