#include "snow_shot/presentation/globalmousemanager.h"

#include <QCursor>
#include <QGuiApplication>
#include <QMutex>

#include <thread>

#include <X11/Xlib.h>
#include <X11/extensions/record.h>

#ifdef Status
#undef Status
#endif

#include <atomic>

namespace snow_shot::presentation {
namespace {
int ignoreXError(Display*, XErrorEvent*) {
    return 0;
}

constexpr unsigned long kStopTimeoutMilliseconds = 2000;

Qt::MouseButton qtButton(int button) {
    switch (button) {
    case 1:
        return Qt::LeftButton;
    case 2:
        return Qt::MiddleButton;
    case 3:
        return Qt::RightButton;
    case 8:
        return Qt::BackButton;
    case 9:
        return Qt::ForwardButton;
    default:
        return Qt::NoButton;
    }
}

GlobalMouseModifiers modifiersFromState(unsigned state) {
    GlobalMouseModifiers modifiers;
    if ((state & ControlMask) != 0)
        modifiers |= GlobalMouseModifier::Control;
    if ((state & Mod1Mask) != 0)
        modifiers |= GlobalMouseModifier::Alt;
    if ((state & ShiftMask) != 0)
        modifiers |= GlobalMouseModifier::Shift;
    if ((state & Mod4Mask) != 0)
        modifiers |= GlobalMouseModifier::Super;
    return modifiers;
}

struct MouseRecord {
    std::atomic_bool stop{false};
    QMutex mutex;
    GlobalMouseConfiguration configuration;
    GlobalMouseGesture gesture;
    GlobalMouseBackend::Handler handler;
    Display* control = nullptr;
    XRecordContext context = 0;
};

void recordCallback(XPointer closure, XRecordInterceptData* data) {
    auto* record = reinterpret_cast<MouseRecord*>(closure);
    if (record == nullptr || data == nullptr || data->category != XRecordFromServer ||
        data->data == nullptr) {
        if (data != nullptr)
            XRecordFreeData(data);
        return;
    }
    const unsigned char* event = data->data;
    const int kind = event[0];
    const int detail = event[1];
    const auto x = static_cast<int>(static_cast<short>(event[20] | (event[21] << 8)));
    const auto y = static_cast<int>(static_cast<short>(event[22] | (event[23] << 8)));
    const unsigned state = event[28] | (event[29] << 8);
    GlobalMouseInput input;
    input.position = QPointF(x, y);
    input.modifiers = modifiersFromState(state);
    if (kind == ButtonPress || kind == ButtonRelease) {
        input.kind = kind == ButtonPress ? GlobalMouseInput::Kind::Press
                                          : GlobalMouseInput::Kind::Release;
        input.button = qtButton(detail);
    } else if (kind == MotionNotify) {
        input.kind = GlobalMouseInput::Kind::Move;
    }
    GlobalMouseBackend::Handler handler;
    std::optional<GlobalMouseDragEvent> drag;
    {
        QMutexLocker lock(&record->mutex);
        if (input.kind != GlobalMouseInput::Kind::Other) {
            const auto result = record->gesture.handle(input, record->configuration);
            drag = result.event;
        }
        handler = record->handler;
    }
    if (drag && handler)
        handler(*drag);
    XRecordFreeData(data);
}

class LinuxGlobalMouseBackend final : public GlobalMouseBackend {
  public:
    ~LinuxGlobalMouseBackend() override {
        stop();
    }

    void start(Handler handler, FailureHandler failure) override {
        if (m_thread.joinable())
            return;
        m_record.handler = std::move(handler);
        m_record.stop = false;
        m_thread = std::thread([this, failure = std::move(failure)] {
            XInitThreads();
            Display* data = XOpenDisplay(nullptr);
            m_record.control = XOpenDisplay(nullptr);
            if (data == nullptr || m_record.control == nullptr) {
                if (failure)
                    failure(1);
                return;
            }
            int major = 0;
            int minor = 0;
            if (XRecordQueryVersion(m_record.control, &major, &minor) == 0) {
                if (failure)
                    failure(2);
                return;
            }
            XRecordRange* range = XRecordAllocRange();
            if (range == nullptr) {
                if (failure)
                    failure(3);
                return;
            }
            range->device_events.first = ButtonPress;
            range->device_events.last = MotionNotify;
            XRecordClientSpec clients = XRecordAllClients;
            XSetErrorHandler(ignoreXError);
            m_record.context = XRecordCreateContext(m_record.control, 0, &clients, 1, &range, 1);
            XFlush(m_record.control);
            if (m_record.context != 0) {
                XRecordEnableContext(data, m_record.context, recordCallback,
                                     reinterpret_cast<XPointer>(&m_record));
            } else if (failure) {
                failure(4);
            }
            XCloseDisplay(data);
            Q_UNUSED(failure);
        });
    }

    void stop() override {
        m_record.stop = true;
        if (m_record.control != nullptr && m_record.context != 0) {
            XRecordDisableContext(m_record.control, m_record.context);
            XRecordFreeContext(m_record.control, m_record.context);
            XFlush(m_record.control);
            m_record.context = 0;
        }
        if (m_thread.joinable()) {
            if (m_thread.get_id() == std::this_thread::get_id())
                return;
            m_thread.join();
        }
        if (m_record.control != nullptr) {
            XCloseDisplay(m_record.control);
            m_record.control = nullptr;
        }
        m_record.handler = {};
    }

    void configure(const GlobalMouseConfiguration& configuration) override {
        QMutexLocker lock(&m_record.mutex);
        m_record.configuration = configuration;
    }

    void cancel(quint64 id) override {
        std::optional<GlobalMouseDragEvent> drag;
        GlobalMouseBackend::Handler handler;
        {
            QMutexLocker lock(&m_record.mutex);
            m_record.gesture.cancel(id);
            handler = m_record.handler;
        }
        Q_UNUSED(drag);
        Q_UNUSED(handler);
    }

    void beginButtonDrag(settings::SettingsGlobalMouseAction action) override {
        GlobalMouseBackend::Handler handler;
        std::optional<GlobalMouseDragEvent> drag;
        {
            QMutexLocker lock(&m_record.mutex);
            const auto result = m_record.gesture.beginButtonDrag(action, QCursor::pos());
            drag = result.event;
            handler = m_record.handler;
        }
        if (drag && handler)
            handler(*drag);
    }

    GlobalMousePermissionState permissionState() const override {
        if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY") || qEnvironmentVariableIsSet("DISPLAY"))
            return {GlobalMousePermissionState::Status::Ready, true, true, true};
        return {GlobalMousePermissionState::Status::Unavailable};
    }

  private:
    MouseRecord m_record;
    std::thread m_thread;
};
} // namespace

std::unique_ptr<GlobalMouseBackend> createLinuxGlobalMouseBackend() {
    return std::make_unique<LinuxGlobalMouseBackend>();
}
} // namespace snow_shot::presentation
