#include "systemnotificationservice.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusError>
#include <QDBusMessage>
#include <QElapsedTimer>
#include <QObject>
#include <QStringList>
#include <QThread>
#include <QVariantMap>
#include <QVector>

#include <cstdlib>
#include <functional>
#include <iostream>
#include <optional>

using snow_shot::platform::SystemNotificationAction;
using snow_shot::platform::SystemNotificationRequest;
using snow_shot::platform::SystemNotificationResult;
using snow_shot::platform::SystemNotificationSeverity;
using snow_shot::platform::freedesktop::SystemNotificationService;

namespace {
const QString serverPath = QStringLiteral("/org/freedesktop/Notifications");
const QString serverInterface = QStringLiteral("org.freedesktop.Notifications");
// A test-only name keeps D-Bus activation from starting a real desktop notification daemon.
const QString serverName = QStringLiteral("org.snowshot.Tests.Notifications");

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

bool waitFor(const std::function<bool()>& predicate, int timeoutMs = 5000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    return predicate();
}

void drain(int milliseconds = 100) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
}
} // namespace

class FakeNotificationServer final : public QObject, protected QDBusContext {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")
  public:
    struct Call {
        QString appName;
        uint replacesId = 0;
        QString appIcon;
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int timeout = 0;
    };

    explicit FakeNotificationServer(QDBusConnection connection)
        : m_connection(std::move(connection)) {}

    QVector<Call> calls;
    std::optional<QDBusError::ErrorType> failure;

    void invokeAction(uint id, const QString& key) {
        QDBusMessage signal = QDBusMessage::createSignal(serverPath, serverInterface,
                                                         QStringLiteral("ActionInvoked"));
        signal << id << key;
        m_connection.send(signal);
    }

    void close(uint id) {
        QDBusMessage signal = QDBusMessage::createSignal(serverPath, serverInterface,
                                                         QStringLiteral("NotificationClosed"));
        signal << id << uint{2};
        m_connection.send(signal);
    }

  public slots:
    uint Notify(const QString& appName, uint replacesId, const QString& appIcon,
                const QString& summary, const QString& body, const QStringList& actions,
                const QVariantMap& hints, int timeout) {
        if (failure) {
            sendErrorReply(*failure, QStringLiteral("rejected by the test server"));
            return 0;
        }
        calls.append({appName, replacesId, appIcon, summary, body, actions, hints, timeout});
        return m_nextId++;
    }

  private:
    QDBusConnection m_connection;
    uint m_nextId = 41;
};

namespace {
std::optional<SystemNotificationResult> showAndWait(SystemNotificationService& service,
                                                    SystemNotificationRequest request) {
    std::optional<SystemNotificationResult> result;
    service.show(std::move(request), [&result](SystemNotificationResult value) { result = value; });
    waitFor([&] { return result.has_value(); });
    return result;
}

uint hintByte(const QVariantMap& hints, const QString& key) {
    return hints.value(key).toUInt();
}
} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("snow-shot-linux-notification-tests"));

    // A disconnected bus must fail fast instead of hanging the delivery controller.
    {
        SystemNotificationService offline(QDBusConnection(QStringLiteral("snow-shot-missing-bus")),
                                          serverName, nullptr);
        const auto result = showAndWait(offline, {QStringLiteral("Title"), QStringLiteral("Body")});
        require(result && result->status == SystemNotificationResult::Status::Unavailable,
                "a disconnected bus must report unavailable delivery");
    }

    QDBusConnection client = QDBusConnection::sessionBus();
    if (!client.isConnected()) {
        std::cout << "SKIP: no D-Bus session bus is available\n";
        return 0;
    }
    QDBusConnection serverBus =
        QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("fake-server"));
    require(serverBus.isConnected(), "the fake server needs its own bus connection");

    // No owner yet: the bus answers ServiceUnknown, which must surface as unavailable.
    {
        SystemNotificationService service(client, serverName, nullptr);
        const auto result = showAndWait(service, {QStringLiteral("Title"), QStringLiteral("Body")});
        require(result && result->status == SystemNotificationResult::Status::Unavailable,
                "a missing notification server must report unavailable delivery");
    }

    FakeNotificationServer server(serverBus);
    require(serverBus.registerObject(serverPath, &server, QDBusConnection::ExportAllSlots),
            "the fake server object must register");
    require(serverBus.registerService(serverName), "the fake server name must register");

    SystemNotificationService service(client, serverName, nullptr);
    require(service.objectName() == QStringLiteral("systemNotificationService"),
            "the tray controller locates the service by object name");
    QVector<QPair<SystemNotificationAction, QString>> activations;
    QObject::connect(&service, &SystemNotificationService::activated, &service,
                     [&](SystemNotificationAction action, const QString& path) {
                         activations.append({action, path});
                     });

    const QString first = QStringLiteral("/tmp/snow shot/first clip.mp4");
    const QString second = QStringLiteral("/tmp/snow shot/second clip.mp4");
    auto result = showAndWait(service, {QStringLiteral("Video export completed"), first,
                                        SystemNotificationSeverity::Information,
                                        SystemNotificationAction::OpenRecording, first});
    require(result && result->accepted(), "an accepted Notify call must report acceptance");
    require(server.calls.size() == 1, "the server must receive exactly one Notify call");
    const auto& call = server.calls.first();
    require(call.summary == QStringLiteral("Video export completed") && call.body == first,
            "Notify must carry the title as summary and the message as body");
    require(call.replacesId == 0 && call.timeout == -1,
            "each notification must be new and use the server default timeout");
    require(call.appIcon == QStringLiteral("snow-shot") &&
                call.hints.value(QStringLiteral("desktop-entry")).toString() ==
                    QStringLiteral("snow-shot"),
            "Notify must identify the installed desktop entry and icon");
    require(!call.appName.isEmpty(), "Notify must name the application");
    require(call.actions.size() == 2 && call.actions.first() == QStringLiteral("default"),
            "clickable notifications must register the default action");
    require(hintByte(call.hints, QStringLiteral("urgency")) == 1,
            "informational notifications must use normal urgency");

    result = showAndWait(service, {QStringLiteral("Video export completed"), second,
                                   SystemNotificationSeverity::Information,
                                   SystemNotificationAction::OpenRecording, second});
    require(result && result->accepted(), "a second notification must be accepted");

    // Clicking an older notification must open that notification's own export.
    server.invokeAction(41, QStringLiteral("default"));
    require(waitFor([&] { return activations.size() == 1; }),
            "clicking the first notification must activate it");
    require(activations.last().first == SystemNotificationAction::OpenRecording &&
                activations.last().second == first,
            "the first notification must keep its own file path");
    server.invokeAction(42, QStringLiteral("default"));
    require(waitFor([&] { return activations.size() == 2; }) && activations.last().second == second,
            "the second notification must keep its own file path");

    // Non-default actions and ids from other applications are ignored.
    server.invokeAction(42, QStringLiteral("other"));
    server.invokeAction(7, QStringLiteral("default"));
    drain();
    require(activations.size() == 2, "unknown actions and foreign ids must not activate");

    result = showAndWait(service, {QStringLiteral("Capture"), QStringLiteral("Capture failed"),
                                   SystemNotificationSeverity::Error});
    require(result && result->accepted(), "an error notification must be accepted");
    require(server.calls.last().actions.isEmpty(),
            "notifications without an action must not advertise a default action");
    require(hintByte(server.calls.last().hints, QStringLiteral("urgency")) == 2,
            "error notifications must use critical urgency");
    server.invokeAction(43, QStringLiteral("default"));
    drain();
    require(activations.size() == 2, "notifications without an action must not activate");

    result = showAndWait(service, {QStringLiteral("Update"), QStringLiteral("Update ready"),
                                   SystemNotificationSeverity::Warning,
                                   SystemNotificationAction::OpenAbout});
    require(result && result->accepted(), "an update notification must be accepted");
    require(hintByte(server.calls.last().hints, QStringLiteral("urgency")) == 1,
            "warnings must stay at normal urgency so they do not persist on screen");
    server.invokeAction(44, QStringLiteral("default"));
    require(waitFor([&] { return activations.size() == 3; }) &&
                activations.last().first == SystemNotificationAction::OpenAbout,
            "update notifications must request the About page");

    // A closed notification can no longer be activated.
    server.close(41);
    drain();
    server.invokeAction(41, QStringLiteral("default"));
    drain();
    require(activations.size() == 3, "closed notifications must forget their payload");

    server.failure = QDBusError::AccessDenied;
    result = showAndWait(service, {QStringLiteral("Title"), QStringLiteral("Body")});
    require(result && result->status == SystemNotificationResult::Status::Denied,
            "an access-denied reply must report denied delivery");
    server.failure = QDBusError::InvalidArgs;
    result = showAndWait(service, {QStringLiteral("Title"), QStringLiteral("Body")});
    require(result && result->status == SystemNotificationResult::Status::Failed &&
                !result->diagnostic.isEmpty(),
            "other server errors must report failed delivery with a diagnostic");

    std::cout << "linux system notification tests passed\n";
    return 0;
}

#include "linux_system_notification_tests.moc"
