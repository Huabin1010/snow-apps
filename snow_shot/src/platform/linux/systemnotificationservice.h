#ifndef SNOW_SHOT_PLATFORM_LINUX_SYSTEMNOTIFICATIONSERVICE_H
#define SNOW_SHOT_PLATFORM_LINUX_SYSTEMNOTIFICATIONSERVICE_H

#include <QObject>
#include <QString>
#include <memory>
#include "snow_shot/platform/systemnotification.h"

class QDBusConnection;

namespace snow_shot::platform::freedesktop {

// Delivers notifications through org.freedesktop.Notifications so each delivered
// notification keeps its own click payload, independent of the tray icon.
class SystemNotificationService final : public QObject {
    Q_OBJECT
  public:
    using Action = SystemNotificationAction;
    using Result = SystemNotificationResult;
    using Completion = SystemNotificationCompletion;

    static QString defaultServiceName();

    explicit SystemNotificationService(QObject* parent = nullptr);
    // Inject the bus and server name so tests can run against a private fake server.
    SystemNotificationService(const QDBusConnection& connection, const QString& service,
                              QObject* parent);
    ~SystemNotificationService() override;
    void show(SystemNotificationRequest request, Completion completed = {});

  signals:
    void activated(snow_shot::platform::SystemNotificationAction action, const QString& filePath);

  private slots:
    void handleActionInvoked(uint id, const QString& actionKey);
    void handleNotificationClosed(uint id, uint reason);

  private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace snow_shot::platform::freedesktop
#endif
