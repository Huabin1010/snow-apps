#include "../../presentation/services/globalshortcutbackend_p.h"

#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QEventLoop>
#include <QGuiApplication>
#include <QKeySequence>
#include <QSocketNotifier>
#include <QTimer>
#include <QVariantMap>

#include <X11/Xlib.h>
#include <X11/keysym.h>

#ifdef None
#undef None
#endif
#ifdef Status
#undef Status
#endif

namespace snow_shot::presentation {

struct PortalShortcut {
    QString id;
    QVariantMap description;
};

QDBusArgument& operator<<(QDBusArgument& argument, const PortalShortcut& shortcut) {
    argument.beginStructure();
    argument << shortcut.id << shortcut.description;
    argument.endStructure();
    return argument;
}

const QDBusArgument& operator>>(const QDBusArgument& argument, PortalShortcut& shortcut) {
    argument.beginStructure();
    argument >> shortcut.id >> shortcut.description;
    argument.endStructure();
    return argument;
}

namespace {

struct PortalResponse {
    uint code = 2;
    QVariantMap results;
};

class PortalWaiter : public QObject {
    Q_OBJECT
  public slots:
    void response(uint code, const QVariantMap& results) {
        value = {code, results};
        emit finished();
    }

  signals:
    void finished();

  public:
    PortalResponse value;
};

struct GrabbedShortcut {
    int id = 0;
    KeyCode keycode = 0;
    unsigned modifiers = 0;
};

GlobalShortcutValidationResult validation(const shortcuts::ShortcutBinding& binding, bool supported,
                                           GlobalShortcutFailureReason reason) {
    GlobalShortcutValidationResult result;
    result.binding = binding;
    result.shortcut = binding.portableText;
    result.supported = supported;
    result.failureReason = supported ? GlobalShortcutFailureReason::None : reason;
    return result;
}

bool modifierKey(Qt::Key key) {
    switch (key) {
    case Qt::Key_Shift:
    case Qt::Key_Control:
    case Qt::Key_Alt:
    case Qt::Key_Meta:
    case Qt::Key_Super_L:
    case Qt::Key_Super_R:
    case Qt::Key_AltGr:
        return true;
    default:
        return false;
    }
}

bool parseShortcut(const shortcuts::ShortcutBinding& binding, QKeyCombination* combination) {
    const QKeySequence sequence =
        QKeySequence::fromString(binding.portableText, QKeySequence::PortableText);
    if (combination == nullptr || sequence.count() != 1 || modifierKey(sequence[0].key()))
        return false;
    *combination = sequence[0];
    return true;
}

QString gtkTrigger(const QKeyCombination& combination) {
    QString trigger;
    const auto modifiers = combination.keyboardModifiers();
    if (modifiers.testFlag(Qt::ControlModifier))
        trigger += QStringLiteral("<Control>");
    if (modifiers.testFlag(Qt::AltModifier))
        trigger += QStringLiteral("<Alt>");
    if (modifiers.testFlag(Qt::ShiftModifier))
        trigger += QStringLiteral("<Shift>");
    if (modifiers.testFlag(Qt::MetaModifier))
        trigger += QStringLiteral("<Super>");
    QString key = QKeySequence(combination.key()).toString(QKeySequence::PortableText);
    if (key.size() == 1)
        key = key.toLower();
    return trigger + key;
}

int shortcutGrabError = 0;

int recordGrabError(Display*, XErrorEvent* event) {
    shortcutGrabError = event->error_code;
    return 0;
}

int ignoreXError(Display*, XErrorEvent*) {
    return 0;
}

unsigned xModifiers(Qt::KeyboardModifiers modifiers) {
    unsigned value = 0;
    if (modifiers.testFlag(Qt::ShiftModifier))
        value |= ShiftMask;
    if (modifiers.testFlag(Qt::ControlModifier))
        value |= ControlMask;
    if (modifiers.testFlag(Qt::AltModifier))
        value |= Mod1Mask;
    if (modifiers.testFlag(Qt::MetaModifier))
        value |= Mod4Mask;
    return value;
}

QString portalToken(const char* prefix) {
    static quint64 next = 1;
    return QStringLiteral("%1%2%3")
        .arg(QLatin1String(prefix), QString::number(QCoreApplication::applicationPid()),
             QString::number(next++));
}

class LinuxGlobalShortcutBackend final : public QObject, public GlobalShortcutBackend {
    Q_OBJECT
  public:
    LinuxGlobalShortcutBackend() {
        XInitThreads();
        m_display = XOpenDisplay(nullptr);
        if (m_display != nullptr) {
            m_notifier = new QSocketNotifier(ConnectionNumber(m_display), QSocketNotifier::Read,
                                             this);
            connect(m_notifier, &QSocketNotifier::activated, this, [this] {
                while (XPending(m_display) != 0) {
                    XEvent event;
                    XNextEvent(m_display, &event);
                    if (event.type != KeyPress || !m_handler)
                        continue;
                    const unsigned state = event.xkey.state & ~(LockMask | Mod2Mask);
                    for (const auto& grabbed : m_grabs) {
                        if (grabbed.keycode == event.xkey.keycode && grabbed.modifiers == state) {
                            m_handler(grabbed.id);
                            break;
                        }
                    }
                }
            });
        }
    }

    ~LinuxGlobalShortcutBackend() override {
        for (const auto& grabbed : m_grabs)
            ungrab(grabbed);
        if (m_display != nullptr)
            XCloseDisplay(m_display);
    }

    void setActivationHandler(ActivationHandler handler) override {
        m_handler = std::move(handler);
    }

    GlobalShortcutValidationResult
    validateShortcut(const shortcuts::ShortcutBinding& binding) const override {
        QKeyCombination combination;
        return validation(binding, parseShortcut(binding, &combination),
                          GlobalShortcutFailureReason::InvalidShortcut);
    }

    GlobalShortcutBackendResult registerShortcut(int registrationId,
                                                 const shortcuts::ShortcutBinding& binding) override {
        QKeyCombination combination;
        if (!parseShortcut(binding, &combination))
            return {false, GlobalShortcutFailureReason::InvalidShortcut, 0};
        m_bindings.insert(registrationId, combination);
        const bool portal = bindPortal();
        const bool x11 = grabX11(registrationId, combination);
        if (!portal && !x11) {
            m_bindings.remove(registrationId);
            return {false, GlobalShortcutFailureReason::SystemError, 0};
        }
        return {true, GlobalShortcutFailureReason::None, 0};
    }

    void unregisterShortcut(int registrationId) override {
        const auto grabbed = std::find_if(m_grabs.begin(), m_grabs.end(),
                                           [&](const GrabbedShortcut& item) {
                                               return item.id == registrationId;
                                           });
        if (grabbed != m_grabs.end()) {
            ungrab(*grabbed);
            m_grabs.erase(grabbed);
        }
        m_bindings.remove(registrationId);
        bindPortal();
    }

  private:
    bool grabX11(int registrationId, const QKeyCombination& combination) {
        if (m_display == nullptr)
            return false;
        const QString key =
            QKeySequence(combination.key()).toString(QKeySequence::PortableText);
        const KeySym symbol = XStringToKeysym(key.toUtf8().constData());
        if (symbol == NoSymbol)
            return false;
        const KeyCode keycode = XKeysymToKeycode(m_display, symbol);
        if (keycode == 0)
            return false;
        GrabbedShortcut grabbed{registrationId, keycode,
                                xModifiers(combination.keyboardModifiers())};
        const unsigned variants[] = {0u, LockMask, Mod2Mask, LockMask | Mod2Mask};
        shortcutGrabError = 0;
        const auto previous = XSetErrorHandler(recordGrabError);
        for (unsigned extra : variants) {
            XGrabKey(m_display, keycode, grabbed.modifiers | extra, DefaultRootWindow(m_display),
                     False, GrabModeAsync, GrabModeAsync);
        }
        XSync(m_display, False);
        XSetErrorHandler(ignoreXError);
        Q_UNUSED(previous);
        if (shortcutGrabError != 0) {
            ungrab(grabbed);
            return false;
        }
        m_grabs.push_back(grabbed);
        return true;
    }

    void ungrab(const GrabbedShortcut& grabbed) {
        if (m_display == nullptr)
            return;
        const unsigned variants[] = {0u, LockMask, Mod2Mask, LockMask | Mod2Mask};
        const auto previous = XSetErrorHandler(recordGrabError);
        for (unsigned extra : variants) {
            XUngrabKey(m_display, grabbed.keycode, grabbed.modifiers | extra,
                       DefaultRootWindow(m_display));
        }
        XSync(m_display, False);
        XSetErrorHandler(ignoreXError);
        Q_UNUSED(previous);
    }

    bool bindPortal() {
        if (!qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
            return false;
        auto bus = QDBusConnection::sessionBus();
        if (!m_session.isEmpty() || !createPortalSession(bus))
            return !m_session.isEmpty() && sendPortalBindings(bus);
        return sendPortalBindings(bus);
    }

    bool createPortalSession(QDBusConnection& bus) {
        const QString handle = portalToken("h");
        const QString session = portalToken("s");
        QVariantMap options;
        options.insert(QStringLiteral("handle_token"), handle);
        options.insert(QStringLiteral("session_handle_token"), session);
        const QString request = callPortal(bus, QStringLiteral("CreateSession"), {options});
        if (request.isEmpty())
            return false;
        const auto response = waitForPortal(bus, request);
        if (response.code != 0)
            return false;
        const QString unique = bus.baseService().mid(1).replace(u'.', u'_');
        const QVariant sessionHandle = response.results.value(QStringLiteral("session_handle"));
        m_session = sessionHandle.canConvert<QDBusObjectPath>()
                        ? sessionHandle.value<QDBusObjectPath>().path()
                        : sessionHandle.toString();
        if (m_session.isEmpty()) {
            m_session = QStringLiteral("/org/freedesktop/portal/desktop/session/%1/%2")
                            .arg(unique, session);
        }
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), m_session,
                    QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
                    QStringLiteral("Activated"), this,
                    SLOT(portalActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        return true;
    }

    bool sendPortalBindings(QDBusConnection& bus) {
        if (m_session.isEmpty())
            return false;
        qDBusRegisterMetaType<PortalShortcut>();
        qDBusRegisterMetaType<QList<PortalShortcut>>();
        QList<PortalShortcut> shortcuts;
        for (auto it = m_bindings.cbegin(); it != m_bindings.cend(); ++it) {
            QVariantMap description;
            description.insert(QStringLiteral("description"),
                               QStringLiteral("Snow Shot shortcut %1").arg(it.key()));
            description.insert(QStringLiteral("preferred_trigger"), gtkTrigger(it.value()));
            shortcuts.push_back({QString::number(it.key()), description});
        }
        const QString handle = portalToken("b");
        QVariantMap options;
        options.insert(QStringLiteral("handle_token"), handle);
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
            QStringLiteral("BindShortcuts"));
        message << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariant::fromValue(shortcuts)
                << QString() << options;
        const QDBusMessage reply = bus.call(message);
        if (reply.type() == QDBusMessage::ErrorMessage)
            return false;
        const QString request = reply.arguments().isEmpty()
                                    ? QString()
                                    : reply.arguments().constFirst().value<QDBusObjectPath>().path();
        return !request.isEmpty() && waitForPortal(bus, request).code == 0;
    }

    QString callPortal(QDBusConnection& bus, const QString& method, const QVariantMap& options) {
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"), method);
        message << options;
        const QDBusMessage reply = bus.call(message);
        if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
            return {};
        return reply.arguments().constFirst().value<QDBusObjectPath>().path();
    }

    PortalResponse waitForPortal(QDBusConnection& bus, const QString& request) {
        PortalWaiter waiter;
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), request,
                    QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                    &waiter, SLOT(response(uint,QVariantMap)));
        QEventLoop loop;
        QTimer timer;
        timer.setSingleShot(true);
        connect(&waiter, &PortalWaiter::finished, &loop, &QEventLoop::quit);
        connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        timer.start(2000);
        if (waiter.value.code == 2)
            loop.exec();
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), request,
                       QStringLiteral("org.freedesktop.portal.Request"),
                       QStringLiteral("Response"), &waiter, SLOT(response(uint,QVariantMap)));
        return waiter.value;
    }

  private slots:
    void portalActivated(const QDBusObjectPath&, const QString& shortcutId, qulonglong,
                         const QVariantMap&) {
        bool ok = false;
        const int id = shortcutId.toInt(&ok);
        if (ok && m_handler)
            m_handler(id);
    }

  private:
    ActivationHandler m_handler;
    QHash<int, QKeyCombination> m_bindings;
    QList<GrabbedShortcut> m_grabs;
    Display* m_display = nullptr;
    QSocketNotifier* m_notifier = nullptr;
    QString m_session;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createLinuxGlobalShortcutBackend() {
    return std::make_unique<LinuxGlobalShortcutBackend>();
}

} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::PortalShortcut)

#include "linuxglobalshortcutbackend.moc"
