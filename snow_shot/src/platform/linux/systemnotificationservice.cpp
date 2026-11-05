#include "systemnotificationservice.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <utility>

namespace snow_shot::platform::freedesktop {
namespace {
const QString notificationsPath = QStringLiteral("/org/freedesktop/Notifications");
const QString notificationsInterface = QStringLiteral("org.freedesktop.Notifications");
const QString defaultActionKey = QStringLiteral("default");
// Matches the installed desktop entry and icon name (packaging/linux/snow-shot.desktop).
const QString desktopEntry = QStringLiteral("snow-shot");
// Servers that never emit NotificationClosed must not grow the payload table without bound.
constexpr qsizetype maximumTrackedNotifications = 64;

uchar urgency(SystemNotificationSeverity severity) {
    // freedesktop urgency levels: 0 low, 1 normal, 2 critical.
    return severity == SystemNotificationSeverity::Error ? 2 : 1;
}

QString applicationDisplayName() {
    // QGuiApplication exposes this as a property; reading it keeps the service Core-only.
    const QCoreApplication* application = QCoreApplication::instance();
    const QString display = application != nullptr
                                ? application->property("applicationDisplayName").toString()
                                : QString();
    return display.isEmpty() ? QCoreApplication::applicationName() : display;
}

SystemNotificationResult errorResult(const QDBusError& error) {
    using Status = SystemNotificationResult::Status;
    Status status = Status::Failed;
    switch (error.type()) {
    case QDBusError::ServiceUnknown:
    case QDBusError::NoServer:
    case QDBusError::NoNetwork:
    case QDBusError::Disconnected:
    case QDBusError::NoReply:
    case QDBusError::Timeout:
    case QDBusError::TimedOut:
    case QDBusError::UnknownObject:
    case QDBusError::UnknownInterface:
    case QDBusError::UnknownMethod:
        status = Status::Unavailable;
        break;
    case QDBusError::AccessDenied:
        status = Status::Denied;
        break;
    default:
        break;
    }
    return {status, QStringLiteral("%1: %2").arg(error.name(), error.message())};
}
} // namespace

class SystemNotificationService::Impl {
  public:
    struct Payload {
        Action action = Action::None;
        QString filePath;
    };

    Impl(const QDBusConnection& bus, QString name) : connection(bus), service(std::move(name)) {}

    static void complete(const Completion& completed, Result result) {
        if (!result.accepted())
            qWarning().noquote() << "freedesktop notification:" << result.diagnostic;
        if (completed)
            completed(std::move(result));
    }

    void remember(uint id, Payload payload) {
        if (id == 0 || payload.action == Action::None)
            return;
        payloads.insert(id, std::move(payload));
        // Server ids increase monotonically, so the first key is the oldest notification.
        while (payloads.size() > maximumTrackedNotifications)
            payloads.erase(payloads.begin());
    }

    QDBusConnection connection;
    QString service;
    QMap<uint, Payload> payloads;
};

QString SystemNotificationService::defaultServiceName() {
    return QStringLiteral("org.freedesktop.Notifications");
}

SystemNotificationService::SystemNotificationService(QObject* parent)
    : SystemNotificationService(QDBusConnection::sessionBus(), defaultServiceName(), parent) {}

SystemNotificationService::SystemNotificationService(const QDBusConnection& connection,
                                                     const QString& service, QObject* parent)
    : QObject(parent), m_impl(std::make_unique<Impl>(connection, service)) {
    setObjectName(QStringLiteral("systemNotificationService"));
    if (!m_impl->connection.isConnected())
        return;
    // Signals are broadcast to every client; ids are unique per server, so the payload
    // table only matches notifications this service submitted.
    m_impl->connection.connect(service, notificationsPath, notificationsInterface,
                               QStringLiteral("ActionInvoked"), this,
                               SLOT(handleActionInvoked(uint, QString)));
    m_impl->connection.connect(service, notificationsPath, notificationsInterface,
                               QStringLiteral("NotificationClosed"), this,
                               SLOT(handleNotificationClosed(uint, uint)));
}

SystemNotificationService::~SystemNotificationService() = default;

void SystemNotificationService::show(SystemNotificationRequest request, Completion completed) {
    if (!m_impl->connection.isConnected()) {
        Impl::complete(completed, {Result::Status::Unavailable,
                                   QStringLiteral("The D-Bus session bus is not connected.")});
        return;
    }
    QStringList actions;
    if (request.action != Action::None) {
        // Servers activate "default" when the notification body is clicked.
        actions << defaultActionKey << tr("Open");
    }
    QVariantMap hints;
    hints.insert(QStringLiteral("urgency"), QVariant::fromValue(urgency(request.severity)));
    hints.insert(QStringLiteral("desktop-entry"), desktopEntry);

    QDBusMessage call = QDBusMessage::createMethodCall(
        m_impl->service, notificationsPath, notificationsInterface, QStringLiteral("Notify"));
    call << applicationDisplayName() << uint{0} << desktopEntry << request.title << request.body
         << actions << hints << int{-1};

    auto* watcher = new QDBusPendingCallWatcher(m_impl->connection.asyncCall(call), this);
    Impl::Payload payload{request.action, request.filePath};
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, payload = std::move(payload),
             completed = std::move(completed)](QDBusPendingCallWatcher* finished) mutable {
                finished->deleteLater();
                const QDBusPendingReply<uint> reply = *finished;
                if (reply.isError()) {
                    Impl::complete(completed, errorResult(reply.error()));
                    return;
                }
                m_impl->remember(reply.value(), std::move(payload));
                Impl::complete(completed, {});
            });
}

void SystemNotificationService::handleActionInvoked(uint id, const QString& actionKey) {
    if (actionKey != defaultActionKey)
        return;
    const auto found = m_impl->payloads.constFind(id);
    if (found == m_impl->payloads.cend())
        return;
    const Impl::Payload payload = found.value();
    if (payload.action == Action::OpenAbout ||
        (payload.action == Action::OpenRecording && !payload.filePath.isEmpty()))
        emit activated(payload.action, payload.filePath);
}

void SystemNotificationService::handleNotificationClosed(uint id, uint reason) {
    Q_UNUSED(reason);
    m_impl->payloads.remove(id);
}

} // namespace snow_shot::platform::freedesktop
