#include "snow_shot/platform/windows/autostartregistration.h"

#include "snow_shot/app/edition.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTextStream>

namespace snow_shot::platform::windows {
namespace {

QString autostartDirectory() {
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    if (config.isEmpty())
        config = QDir::homePath() + QStringLiteral("/.config");
    return config + QStringLiteral("/autostart");
}

QString desktopFilePath() {
    // The filename stays stable. applicationName() is a display string and can
    // contain spaces, so it must not become the XDG autostart id.
    return autostartDirectory() + QStringLiteral("/snow-shot.desktop");
}

QString displayName() {
    QString name = snow_shot::app::edition::productName().simplified();
    name.replace(QLatin1Char('\n'), QLatin1Char(' '));
    name.replace(QLatin1Char('\r'), QLatin1Char(' '));
    return name.isEmpty() ? QStringLiteral("Snow Shot") : name;
}

QString quoteDesktopExec(const QString& path) {
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    escaped.replace(QLatin1Char('\n'), QString());
    escaped.replace(QLatin1Char('\r'), QString());
    return QStringLiteral("\"%1\" --autostart").arg(escaped);
}

QString desktopEntry(const QString& command) {
    QString entry;
    QTextStream stream(&entry);
    stream << "[Desktop Entry]\n"
           << "Type=Application\n"
           << "Version=1.0\n"
           << "Name=" << displayName() << "\n"
           << "Comment=Stay in the status area and wait for a screenshot\n"
           << "Exec=" << command << "\n"
           << "Icon=snow-shot\n"
           << "Terminal=false\n"
           << "StartupNotify=false\n"
           << "X-GNOME-Autostart-enabled=true\n";
    return entry;
}

bool autostartFlagEnabled(const QByteArray& desktopFile) {
    bool gnomeEnabled = true;
    bool hidden = false;
    const QString text = QString::fromUtf8(desktopFile);
    for (const QString& rawLine : text.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line == QStringLiteral("Hidden=true"))
            hidden = true;
        else if (line == QStringLiteral("Hidden=false"))
            hidden = false;
        else if (line == QStringLiteral("X-GNOME-Autostart-enabled=false"))
            gnomeEnabled = false;
        else if (line == QStringLiteral("X-GNOME-Autostart-enabled=true"))
            gnomeEnabled = true;
    }
    return gnomeEnabled && !hidden;
}

bool writeDesktopFile(const QByteArray& bytes, QString* error) {
    QDir directory(autostartDirectory());
    if (!directory.exists() && !directory.mkpath(QStringLiteral("."))) {
        if (error)
            *error = QCoreApplication::translate("AutoStartRegistration",
                                                 "Could not create the autostart directory.");
        return false;
    }
    QSaveFile file(desktopFilePath());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error)
            *error = file.errorString();
        return false;
    }
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error)
            *error = file.errorString().isEmpty()
                         ? QCoreApplication::translate("AutoStartRegistration",
                                                       "Could not write the autostart entry.")
                         : file.errorString();
        return false;
    }
    return true;
}

bool removeDesktopFile(QString* error) {
    const QString path = desktopFilePath();
    if (!QFile::exists(path))
        return true;
    if (QFile::remove(path))
        return true;
    if (error)
        *error = QCoreApplication::translate("AutoStartRegistration",
                                             "Could not remove the autostart entry.");
    return false;
}

} // namespace

bool AutoStartRegistration::isSupported() {
    return !QStandardPaths::writableLocation(QStandardPaths::ConfigLocation).isEmpty() ||
           !QDir::homePath().isEmpty();
}

QString AutoStartRegistration::hint() {
    return QCoreApplication::translate("AutoStartRegistration",
                                       "Start automatically when you log in and stay in the "
                                       "status area.");
}

QString AutoStartRegistration::expectedCommand() {
    return quoteDesktopExec(QCoreApplication::applicationFilePath());
}

AutoStartRegistrationSnapshot AutoStartRegistration::snapshot() {
    AutoStartRegistrationSnapshot snapshot;
    const QString path = desktopFilePath();
    const QFileInfo info(path);
    if (info.exists() && !info.isFile()) {
        snapshot.valid = false;
        snapshot.error = QCoreApplication::translate(
            "AutoStartRegistration", "The autostart entry is not a desktop file.");
        return snapshot;
    }
    snapshot.valid = true;
    snapshot.exists = info.isFile();
    snapshot.state = AutoStartState::Disabled;
    if (!snapshot.exists)
        return snapshot;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        snapshot.valid = false;
        snapshot.exists = true;
        snapshot.error = file.errorString();
        return snapshot;
    }
    snapshot.nativeData = file.readAll();
    snapshot.state = autostartFlagEnabled(snapshot.nativeData) ? AutoStartState::Enabled
                                                               : AutoStartState::Disabled;
    return snapshot;
}

bool AutoStartRegistration::matchesExpectedCommand() {
    const AutoStartRegistrationSnapshot current = snapshot();
    if (!current.valid || current.state != AutoStartState::Enabled)
        return false;
    return current.nativeData == desktopEntry(expectedCommand()).toUtf8();
}

bool AutoStartRegistration::setEnabled(bool enabled, QString* error) {
    const AutoStartRegistrationSnapshot current = snapshot();
    if (!current.valid) {
        if (error)
            *error = current.error;
        return false;
    }
    if (!canChangeState(current.state)) {
        if (error)
            *error = QCoreApplication::translate(
                "AutoStartRegistration", "Login startup cannot be changed in the current state.");
        return false;
    }
    if (!enabled)
        return removeDesktopFile(error);
    return writeDesktopFile(desktopEntry(expectedCommand()).toUtf8(), error);
}

bool AutoStartRegistration::restore(const AutoStartRegistrationSnapshot& previous, QString* error) {
    if (!previous.valid || (previous.exists && previous.nativeData.isEmpty())) {
        if (error)
            *error = QCoreApplication::translate("AutoStartRegistration",
                                                 "The saved autostart state is invalid.");
        return false;
    }
    if (!previous.exists)
        return removeDesktopFile(error);
    return writeDesktopFile(previous.nativeData, error);
}

} // namespace snow_shot::platform::windows
