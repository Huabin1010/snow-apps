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
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QSocketNotifier>
#include <QTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
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
        if (qEnvironmentVariableIsSet("WAYLAND_DISPLAY")) {
            // GNOME shows a permission dialog for every portal BindShortcuts call.
            // Keep the binding here and publish the whole set once from
            // flushRegistrations(), through the same media-key path Snipaste uses.
            return {true, GlobalShortcutFailureReason::None, 0};
        }
        const bool x11 = grabX11(registrationId, combination);
        if (!x11) {
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
    }

    void flushRegistrations() override {
        if (!qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
            return;
        syncGnomeMediaKeys();
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

    void syncGnomeMediaKeys() {
        QStringList kept;
        QSet<QString> takenBindings;
        QProcess current;
        current.start(QStringLiteral("gsettings"),
                      {QStringLiteral("get"),
                       QStringLiteral("org.gnome.settings-daemon.plugins.media-keys"),
                       QStringLiteral("custom-keybindings")});
        if (current.waitForFinished(1500) && current.exitCode() == 0) {
            const QString text = QString::fromUtf8(current.readAllStandardOutput());
            static const QRegularExpression pathPattern(QStringLiteral("'([^']+)'"));
            auto match = pathPattern.globalMatch(text);
            while (match.hasNext()) {
                const QString path = match.next().captured(1);
                if (path.contains(QStringLiteral("snow-shot-")))
                    continue;
                kept.push_back(path);
                QProcess binding;
                binding.start(QStringLiteral("gsettings"),
                              {QStringLiteral("get"),
                               QStringLiteral(
                                   "org.gnome.settings-daemon.plugins.media-keys.custom-keybinding:%1")
                                   .arg(path),
                               QStringLiteral("binding")});
                if (binding.waitForFinished(1500) && binding.exitCode() == 0) {
                    QString value = QString::fromUtf8(binding.readAllStandardOutput()).trimmed();
                    if (value.size() >= 2 && value.front() == u'\'' && value.back() == u'\'')
                        value = value.mid(1, value.size() - 2);
                    if (!value.isEmpty())
                        takenBindings.insert(value);
                }
            }
        }

        const QString executable = QCoreApplication::applicationFilePath();
        QList<QKeyCombination> combinations = m_bindings.values();
        std::sort(combinations.begin(), combinations.end(),
                  [](const QKeyCombination& left, const QKeyCombination& right) {
                      return QKeySequence(left).toString(QKeySequence::PortableText) <
                             QKeySequence(right).toString(QKeySequence::PortableText);
                  });
        struct GnomeKey {
            QString path;
            QString name;
            QString command;
            QString binding;
        };
        QList<GnomeKey> keys;
        QStringList owned;
        int index = 0;
        QStringList fingerprintParts = kept;
        for (const QKeyCombination& combination : combinations) {
            const QString binding = gtkTrigger(combination);
            if (takenBindings.contains(binding))
                continue;
            const QString slot = QStringLiteral("snow-shot-%1").arg(index++);
            const QString path =
                QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings/%1/")
                    .arg(slot);
            const QString portable =
                QKeySequence(combination).toString(QKeySequence::PortableText);
            const QString helper =
                QFileInfo(executable).dir().filePath(QStringLiteral("snow-shot-shortcut"));
            // The full debug binary takes the better part of a second to start.
            // The key binding only needs to poke the running instance.
            const QString command =
                QFileInfo::exists(helper)
                    ? QStringLiteral("%1 --shortcut %2").arg(helper, portable)
                    : QStringLiteral("%1 --shortcut %2").arg(executable, portable);
            keys.push_back(GnomeKey{path, QStringLiteral("Snow Shot %1").arg(portable), command,
                                    binding});
            owned.push_back(path);
            fingerprintParts.push_back(path + command + binding);
        }
        const QString fingerprint = fingerprintParts.join(QStringLiteral("\n"));
        if (fingerprint == m_publishedGnomeKeys)
            return;
        for (const GnomeKey& key : keys) {
            writeDconf(key.path + QStringLiteral("name"), key.name);
            writeDconf(key.path + QStringLiteral("command"), key.command);
            writeDconf(key.path + QStringLiteral("binding"), key.binding);
        }
        m_publishedGnomeKeys = fingerprint;

        QStringList all = kept;
        all.append(owned);
        QStringList quoted;
        for (const QString& path : all)
            quoted.push_back(QStringLiteral("'%1'").arg(path));
        QProcess write;
        write.start(QStringLiteral("dconf"),
                    {QStringLiteral("write"),
                     QStringLiteral("/org/gnome/settings-daemon/plugins/media-keys/custom-keybindings"),
                     QStringLiteral("[%1]").arg(quoted.join(QStringLiteral(", ")))});
        write.waitForFinished(1500);
    }

    static void writeDconf(const QString& key, const QString& value) {
        QProcess process;
        process.start(QStringLiteral("dconf"),
                      {QStringLiteral("write"), key,
                       QStringLiteral("'%1'").arg(value)});
        process.waitForFinished(1500);
    }

    bool bindPortal() {
        if (!qEnvironmentVariableIsSet("WAYLAND_DISPLAY"))
            return false;
        auto bus = QDBusConnection::sessionBus();
        // BindShortcuts can be called only once per session. Close the previous
        // session and publish the full list, otherwise later shortcuts such as
        // Ctrl+Shift+C never reach the compositor.
        closePortalSession(bus);
        if (m_bindings.isEmpty())
            return true;
        if (!createPortalSession(bus))
            return false;
        return sendPortalBindings(bus);
    }

    void closePortalSession(QDBusConnection& bus) {
        if (m_session.isEmpty())
            return;
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), m_session,
                       QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
                       QStringLiteral("Activated"), this,
                       SLOT(portalActivated(QDBusObjectPath,QString,qulonglong,QVariantMap)));
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_session,
            QStringLiteral("org.freedesktop.portal.Session"), QStringLiteral("Close"));
        bus.call(message);
        m_session.clear();
    }

    bool createPortalSession(QDBusConnection& bus) {
        const QString handle = portalToken("h");
        const QString session = portalToken("s");
        QVariantMap options;
        options.insert(QStringLiteral("handle_token"), handle);
        options.insert(QStringLiteral("session_handle_token"), session);
        PortalWaiter waiter;
        const QString request = portalRequestPath(bus, handle);
        listenForPortal(bus, request, waiter);
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
            QStringLiteral("CreateSession"));
        message << options;
        const PortalResponse response = finishPortalCall(bus, request, waiter, message);
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
        PortalWaiter waiter;
        const QString request = portalRequestPath(bus, handle);
        listenForPortal(bus, request, waiter);
        QDBusMessage message = QDBusMessage::createMethodCall(
            QStringLiteral("org.freedesktop.portal.Desktop"),
            QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.GlobalShortcuts"),
            QStringLiteral("BindShortcuts"));
        message << QVariant::fromValue(QDBusObjectPath(m_session)) << QVariant::fromValue(shortcuts)
                << QString() << options;
        return finishPortalCall(bus, request, waiter, message).code == 0;
    }

    QString portalRequestPath(const QDBusConnection& bus, const QString& token) const {
        const QString sender = bus.baseService().mid(1).replace(u'.', u'_');
        return QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    }

    void listenForPortal(QDBusConnection& bus, const QString& request, PortalWaiter& waiter) {
        bus.connect(QStringLiteral("org.freedesktop.portal.Desktop"), request,
                    QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                    &waiter, SLOT(response(uint,QVariantMap)));
    }

    PortalResponse finishPortalCall(QDBusConnection& bus, const QString& request,
                                    PortalWaiter& waiter, const QDBusMessage& message) {
        const QDBusMessage reply = bus.call(message);
        PortalResponse response;
        if (reply.type() != QDBusMessage::ErrorMessage) {
            QEventLoop loop;
            QTimer timer;
            timer.setSingleShot(true);
            connect(&waiter, &PortalWaiter::finished, &loop, &QEventLoop::quit);
            connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
            timer.start(2000);
            if (waiter.value.code == 2)
                loop.exec();
            response = waiter.value;
        }
        bus.disconnect(QStringLiteral("org.freedesktop.portal.Desktop"), request,
                       QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                       &waiter, SLOT(response(uint,QVariantMap)));
        return response;
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
    QString m_publishedGnomeKeys;
};

} // namespace

std::unique_ptr<GlobalShortcutBackend> createLinuxGlobalShortcutBackend() {
    return std::make_unique<LinuxGlobalShortcutBackend>();
}

} // namespace snow_shot::presentation

Q_DECLARE_METATYPE(snow_shot::presentation::PortalShortcut)

#include "linuxglobalshortcutbackend.moc"
