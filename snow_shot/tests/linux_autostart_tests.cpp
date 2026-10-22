#include "snow_shot/platform/windows/administratorlaunch.h"
#include "snow_shot/platform/windows/autostartregistration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QString desktopPath(const QString& configHome) {
    return configHome + QStringLiteral("/autostart/snow-shot.desktop");
}

QString readDesktop(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "the autostart desktop file must be readable");
    return QString::fromUtf8(file.readAll());
}

QString field(const QString& desktop, const QString& key) {
    for (const QString& rawLine : desktop.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.startsWith(key + QLatin1Char('=')))
            return line.mid(key.size() + 1).trimmed();
    }
    return {};
}

void requireCanonicalEntry(const QString& desktop, const QString& expectedName) {
    require(desktop.startsWith(QStringLiteral("[Desktop Entry]\n")),
            "the autostart file must be a desktop entry");
    require(field(desktop, QStringLiteral("Type")) == QStringLiteral("Application"),
            "the autostart entry must be an application");
    require(field(desktop, QStringLiteral("Name")) == expectedName,
            "the autostart name must be a single sanitized line");
    require(!field(desktop, QStringLiteral("Name")).contains(QLatin1Char('\n')),
            "the autostart name must not break the desktop file");
    require(field(desktop, QStringLiteral("Icon")) == QStringLiteral("snow-shot"),
            "login startup must use the installed snow-shot icon");
    require(field(desktop, QStringLiteral("Terminal")) == QStringLiteral("false"),
            "login startup must not open a terminal");
    require(field(desktop, QStringLiteral("StartupNotify")) == QStringLiteral("false"),
            "login startup must not raise a startup notification for a tray-only launch");
    require(field(desktop, QStringLiteral("X-GNOME-Autostart-enabled")) == QStringLiteral("true"),
            "GNOME must be told the login entry is enabled");
    const QString exec = field(desktop, QStringLiteral("Exec"));
    const QString binary = QCoreApplication::applicationFilePath();
    require(exec == QStringLiteral("\"%1\" --autostart").arg(binary),
            "login startup must quote this executable and pass --autostart so the main window "
            "stays hidden");
    require(exec.endsWith(QStringLiteral("--autostart")),
            "without --autostart the process would open the main window instead of staying in "
            "the status area");
}

void validateDesktopFile(const QString& path) {
    if (QStandardPaths::findExecutable(QStringLiteral("desktop-file-validate")).isEmpty())
        return;
    QProcess process;
    process.start(QStringLiteral("desktop-file-validate"), {path});
    require(process.waitForFinished(5000), "desktop-file-validate must finish");
    require(process.exitCode() == 0, "desktop-file-validate must accept the login entry");
}

} // namespace

int main(int argc, char* argv[]) {
    QTemporaryDir configHome;
    require(configHome.isValid(), "a temporary XDG config home must be available");
    qputenv("XDG_CONFIG_HOME", configHome.path().toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("Snow\nShot"));
    QCoreApplication application(argc, argv);

    using snow_shot::platform::windows::AutoStartRegistration;
    using snow_shot::platform::windows::AutoStartState;
    using snow_shot::platform::windows::StartupMode;
    using snow_shot::platform::windows::changeStartupMode;
    using snow_shot::platform::windows::migrateInstallationStartup;
    using snow_shot::platform::windows::observedStartupMode;
    using snow_shot::platform::windows::reconcileStartupMode;
    using snow_shot::platform::windows::removeInstallationStartup;

    const QString desktop = desktopPath(configHome.path());
    require(AutoStartRegistration::isSupported(), "XDG autostart must be supported");
    require(!AutoStartRegistration::hint().isEmpty(), "the login hint must explain the setting");

    auto initial = AutoStartRegistration::snapshot();
    require(initial.valid && !initial.exists && initial.state == AutoStartState::Disabled,
            "a fresh config home must have no login entry");
    require(!AutoStartRegistration::matchesExpectedCommand(),
            "a missing login entry must not match this executable");
    require(observedStartupMode() == StartupMode::Off,
            "a missing login entry must be observed as off");
    require(!QFile::exists(configHome.path() + QStringLiteral("/autostart/Snow Shot.desktop")),
            "the display name must not be used as the autostart filename");

    QString error;
    require(AutoStartRegistration::setEnabled(true, &error), "creating the login entry must work");
    require(error.isEmpty(), "a successful write must not leave an error");
    require(QFileInfo(desktop).isFile(), "the login entry must be snow-shot.desktop");
    const QString sanitized = readDesktop(desktop);
    requireCanonicalEntry(sanitized, QStringLiteral("Snow Shot"));
    validateDesktopFile(desktop);
    require(AutoStartRegistration::matchesExpectedCommand(),
            "the new login entry must match this executable and --autostart");
    require(observedStartupMode() == StartupMode::Registry,
            "an enabled login entry must be observed as the normal startup mode");

    QCoreApplication::setApplicationName(QStringLiteral("Snow Shot Tests"));
    require(AutoStartRegistration::setEnabled(true, &error), "rewriting the login entry must work");
    const QString named = readDesktop(desktop);
    requireCanonicalEntry(named, QStringLiteral("Snow Shot"));
    require(!named.contains(QStringLiteral("Snow Shot Tests")),
            "the login entry must show the product name, not the internal application id");
    require(AutoStartRegistration::setEnabled(true, &error), "enabling twice must stay successful");
    require(readDesktop(desktop) == named, "enabling an existing entry must keep the same text");
    const QStringList names = QDir(configHome.path() + QStringLiteral("/autostart")).entryList(
        QDir::Files | QDir::NoDotAndDotDot);
    require(names == QStringList{QStringLiteral("snow-shot.desktop")},
            "autostart must contain only the stable snow-shot.desktop id");

    QString disabledByGnome = named;
    disabledByGnome.replace(QStringLiteral("X-GNOME-Autostart-enabled=true"),
                            QStringLiteral("X-GNOME-Autostart-enabled=false"));
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to mark the entry disabled");
        file.write(disabledByGnome.toUtf8());
    }
    auto gnomeDisabled = AutoStartRegistration::snapshot();
    require(gnomeDisabled.valid && gnomeDisabled.exists &&
                gnomeDisabled.state == AutoStartState::Disabled,
            "X-GNOME-Autostart-enabled=false must be observed as off while the file remains");
    require(!AutoStartRegistration::matchesExpectedCommand(),
            "a GNOME-disabled entry must not count as a matching login command");
    require(observedStartupMode() == StartupMode::Off,
            "a GNOME-disabled entry must not keep the setting switch on");

    QString lastFlagWins = disabledByGnome + QStringLiteral("X-GNOME-Autostart-enabled=true\n");
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to append a later autostart flag");
        file.write(lastFlagWins.toUtf8());
    }
    require(AutoStartRegistration::snapshot().state == AutoStartState::Enabled,
            "the last X-GNOME-Autostart-enabled value must win");
    QString hidden = named;
    hidden.replace(QStringLiteral("X-GNOME-Autostart-enabled=true"),
                   QStringLiteral("Hidden=true\nX-GNOME-Autostart-enabled=true"));
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to hide the entry");
        file.write(hidden.toUtf8());
    }
    require(AutoStartRegistration::snapshot().state == AutoStartState::Disabled,
            "Hidden=true must turn the observed login state off");

    QString renamed = named;
    renamed.replace(QStringLiteral("Name=Snow Shot"), QStringLiteral("Name=Other"));
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to change the login entry name");
        file.write(renamed.toUtf8());
    }
    require(!AutoStartRegistration::matchesExpectedCommand(),
            "a renamed login entry must not count as the canonical entry");
    auto renamedReconcile = reconcileStartupMode(StartupMode::Registry);
    require(renamedReconcile.success && readDesktop(desktop) == named,
            "reconciliation must restore the canonical name, icon, and --autostart command");

    QString wrongExec = named;
    wrongExec.replace(field(named, QStringLiteral("Exec")), QStringLiteral("/usr/bin/false"));
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to point the entry at another program");
        file.write(wrongExec.toUtf8());
    }
    require(AutoStartRegistration::snapshot().state == AutoStartState::Enabled &&
                !AutoStartRegistration::matchesExpectedCommand(),
            "a foreign Exec must stay enabled in the file but must not match Snow Shot");
    auto reconciled = reconcileStartupMode(StartupMode::Registry);
    require(reconciled.success, "startup reconciliation must rewrite a foreign login command");
    require(AutoStartRegistration::matchesExpectedCommand(),
            "reconciliation must put this executable and --autostart back");
    requireCanonicalEntry(readDesktop(desktop), QStringLiteral("Snow Shot"));
    const QString alreadyMatching = readDesktop(desktop);
    auto reconciledAgain = reconcileStartupMode(StartupMode::Registry);
    require(reconciledAgain.success && readDesktop(desktop) == alreadyMatching,
            "reconciliation must leave a matching login command unchanged");
    const QByteArray matchingBytes = readDesktop(desktop).toUtf8();
    auto reconciledOff = reconcileStartupMode(StartupMode::Off);
    require(reconciledOff.success && !QFile::exists(desktop),
            "reconciliation to off must remove the login entry");
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly), "the test must restore the matching entry");
        file.write(matchingBytes);
    }

    const auto saved = AutoStartRegistration::snapshot();
    require(saved.valid && saved.exists && saved.nativeData == matchingBytes,
            "a snapshot must keep the exact desktop bytes");
    require(AutoStartRegistration::setEnabled(false, &error) && !QFile::exists(desktop),
            "turning login startup off must delete the desktop file");
    require(observedStartupMode() == StartupMode::Off, "a removed entry must be observed as off");
    require(AutoStartRegistration::setEnabled(false, &error),
            "turning login startup off when it is already off must succeed");
    require(AutoStartRegistration::restore(saved, &error), "restore must write the saved bytes");
    {
        QFile file(desktop);
        require(file.open(QIODevice::ReadOnly), "the restored file must be readable");
        require(file.readAll() == saved.nativeData,
                "restore must put back the exact saved desktop entry, not a regenerated one");
    }

    snow_shot::platform::windows::AutoStartRegistrationSnapshot invalid;
    invalid.valid = false;
    invalid.exists = true;
    require(!AutoStartRegistration::restore(invalid, &error) && !error.isEmpty(),
            "an invalid snapshot must be rejected");
    snow_shot::platform::windows::AutoStartRegistrationSnapshot emptyBytes;
    emptyBytes.valid = true;
    emptyBytes.exists = true;
    require(!AutoStartRegistration::restore(emptyBytes, &error),
            "a snapshot that claims a file but has no bytes must be rejected");
    require(readDesktop(desktop).toUtf8() == saved.nativeData,
            "a rejected restore must leave the current login entry untouched");

    bool persistCalled = false;
    auto elevated = changeStartupMode(StartupMode::ElevatedTask, [&] {
        persistCalled = true;
        return true;
    });
    require(!elevated.success && !elevated.error.isEmpty() && !persistCalled,
            "administrator login must fail without saving settings or changing the file");
    require(readDesktop(desktop).toUtf8() == saved.nativeData,
            "a rejected administrator login must leave the XDG entry unchanged");

    auto rolledBack = changeStartupMode(StartupMode::Off, [] { return false; });
    require(!rolledBack.success, "a failed settings save must fail the startup change");
    require(readDesktop(desktop).toUtf8() == saved.nativeData,
            "a failed settings save must restore the previous login entry");
    auto removed = changeStartupMode(StartupMode::Off, [] { return true; });
    require(removed.success && !QFile::exists(desktop) && observedStartupMode() == StartupMode::Off,
            "turning the setting off must remove the login entry after settings are saved");
    auto rolledForward = changeStartupMode(StartupMode::Registry, [] { return false; });
    require(!rolledForward.success && !QFile::exists(desktop),
            "a failed save while turning login on must roll back to no desktop file");
    auto enabled = changeStartupMode(StartupMode::Registry, [] { return true; });
    require(enabled.success && AutoStartRegistration::matchesExpectedCommand(),
            "turning the setting on must write the login entry for this executable");
    require(observedStartupMode() == StartupMode::Registry,
            "the observed mode must follow the desktop file after the setting is saved");

    const QByteArray currentBytes = readDesktop(desktop).toUtf8();
    auto untouched = removeInstallationStartup(QStringLiteral("/definitely/not/this/install"));
    require(untouched.success && readDesktop(desktop).toUtf8() == currentBytes,
            "removing another installation must not delete this login entry");
    auto removedInstall = removeInstallationStartup(QCoreApplication::applicationDirPath());
    require(removedInstall.success && !QFile::exists(desktop),
            "removing this installation must delete a login entry that points at it");
    require(removeInstallationStartup(QCoreApplication::applicationDirPath()).success,
            "removing a missing login entry must succeed");

    require(AutoStartRegistration::setEnabled(true, &error), "the migration fixture must exist");
    QString migratedAway = readDesktop(desktop);
    migratedAway.replace(field(migratedAway, QStringLiteral("Exec")),
                         QStringLiteral("\"/opt/old-snow/snow-shot\" --autostart"));
    {
        QFile file(desktop);
        require(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                "the test must be able to simulate an old install path");
        file.write(migratedAway.toUtf8());
    }
    const QByteArray oldBytes = readDesktop(desktop).toUtf8();
    auto ignored = migrateInstallationStartup(QString(), QStringLiteral("/usr/lib"));
    require(ignored.success && readDesktop(desktop).toUtf8() == oldBytes,
            "an empty previous install root must not rewrite the login entry");
    auto migrated = migrateInstallationStartup(QStringLiteral("/opt/old-snow"),
                                               QStringLiteral("/usr/lib/snow-shot"));
    require(migrated.success && AutoStartRegistration::matchesExpectedCommand(),
            "moving the installation must rewrite Exec to the current executable");
    require(readDesktop(desktop).toUtf8() != oldBytes,
            "a real install move must change the desktop entry");

    require(QDir(configHome.path() + QStringLiteral("/autostart")).removeRecursively(),
            "the test must be able to replace the autostart directory");
    {
        QFile blocking(configHome.path() + QStringLiteral("/autostart"));
        require(blocking.open(QIODevice::WriteOnly),
                "the test must be able to block the autostart directory");
        blocking.write("not a directory");
    }
    error.clear();
    require(!AutoStartRegistration::setEnabled(true, &error) && !error.isEmpty(),
            "a non-directory autostart path must fail with an error");
    require(!QFile::exists(desktop), "a failed create must not invent a desktop file");
    require(QFile::remove(configHome.path() + QStringLiteral("/autostart")),
            "the blocking file must be removable");
    require(QDir().mkpath(configHome.path() + QStringLiteral("/autostart/snow-shot.desktop")),
            "the test must be able to put a directory where the desktop file belongs");
    auto blocked = AutoStartRegistration::snapshot();
    require(!blocked.valid && !blocked.error.isEmpty(),
            "a directory sitting on snow-shot.desktop must be an invalid snapshot");
    require(!observedStartupMode().has_value(),
            "an unreadable login entry must not invent an observed mode");
    error.clear();
    require(!AutoStartRegistration::setEnabled(false, &error) && !error.isEmpty(),
            "startup changes must fail while the desktop path is not a file");
    auto cannotChange = changeStartupMode(StartupMode::Off, [] { return true; });
    require(!cannotChange.success, "the setting toggle must surface an unreadable login entry");

    return 0;
}
