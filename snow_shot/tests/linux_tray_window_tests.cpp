#include "snow_shot/presentation/components/contentcardwidget.h"
#include "snow_shot/presentation/components/titlebarwidget.h"
#include "snow_shot/presentation/globalshortcutmanager.h"
#include "snow_shot/presentation/languagemanager.h"
#include "snow_shot/presentation/mainwindow.h"
#include "snow_shot/presentation/settings/settingsbackend.h"
#include "snow_shot/presentation/settings/settingsruntimesession.h"
#include "snow_shot/presentation/styles/thememanager.h"
#include "snow_shot/presentation/systemtraycontroller.h"
#include "snow_shot/storage/applicationstorage.h"
#include "snow_shot/storage/settingsadapters.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>

#include <QDBusConnection>
#include <QDBusInterface>

#include <iostream>

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

QStringList statusItems() {
    QDBusInterface watcher(QStringLiteral("org.kde.StatusNotifierWatcher"),
                           QStringLiteral("/StatusNotifierWatcher"),
                           QStringLiteral("org.kde.StatusNotifierWatcher"),
                           QDBusConnection::sessionBus());
    require(watcher.isValid(), "the shell status watcher must be available");
    return watcher.property("RegisteredStatusNotifierItems").toStringList();
}

QString busctl(const QStringList& arguments) {
    QProcess process;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&process, &QProcess::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    process.start(QStringLiteral("busctl"), arguments);
    timeout.start(8000);
    loop.exec();
    if (process.state() != QProcess::NotRunning)
        process.kill();
    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    if (process.exitCode() != 0)
        std::cerr << error.toStdString() << '\n';
    require(process.exitCode() == 0, "the status item must answer the shell");
    return QString::fromUtf8(process.readAllStandardOutput());
}

QString menuObjectPath(const QString& service, const QString& itemPath) {
    const QString output = busctl({QStringLiteral("--user"), QStringLiteral("--json=short"),
                                   QStringLiteral("get-property"), service, itemPath,
                                   QStringLiteral("org.kde.StatusNotifierItem"),
                                   QStringLiteral("Menu")});
    const QJsonDocument document = QJsonDocument::fromJson(output.toUtf8());
    const QString path = document.object().value(QStringLiteral("data")).toString();
    require(path.startsWith(QLatin1Char('/')), "the status item must publish a menu object");
    return path;
}

void collectMenuEntries(const QJsonValue& node, QList<QPair<int, QString>>* entries) {
    const QJsonArray tuple = node.isArray() ? node.toArray() : node.toObject().value(QStringLiteral("data")).toArray();
    if (tuple.size() < 3)
        return;
    const int id = tuple.at(0).toInt();
    const QJsonObject properties = tuple.at(1).toObject();
    const QString label = properties.value(QStringLiteral("label")).toObject().value(QStringLiteral("data")).toString();
    if (!label.isEmpty())
        entries->append({id, label});
    const QJsonArray children = tuple.at(2).toArray();
    for (const QJsonValue& child : children)
        collectMenuEntries(child, entries);
}

QList<QPair<int, QString>> menuEntries(const QString& service, const QString& menuPath) {
    const QString output = busctl({QStringLiteral("--user"), QStringLiteral("--json=short"),
                                   QStringLiteral("call"), QStringLiteral("--"), service, menuPath,
                                   QStringLiteral("com.canonical.dbusmenu"), QStringLiteral("GetLayout"),
                                   QStringLiteral("iias"), QStringLiteral("0"), QStringLiteral("-1"),
                                   QStringLiteral("1"), QStringLiteral("label")});
    const QJsonArray data = QJsonDocument::fromJson(output.toUtf8()).object().value(QStringLiteral("data")).toArray();
    require(data.size() == 2, "GetLayout must return a revision and a root item");
    QList<QPair<int, QString>> entries;
    collectMenuEntries(data.at(1), &entries);
    return entries;
}

void clickMenuEntry(const QString& service, const QString& menuPath, int id) {
    busctl({QStringLiteral("--user"), QStringLiteral("call"), QStringLiteral("--"), service, menuPath,
            QStringLiteral("com.canonical.dbusmenu"), QStringLiteral("Event"), QStringLiteral("isvu"),
            QString::number(id), QStringLiteral("clicked"), QStringLiteral("i"), QStringLiteral("0"),
            QStringLiteral("0")});
}

} // namespace

int main(int argc, char* argv[]) {
    QTemporaryDir directory;
    require(directory.isValid(), "a temporary profile must be available");
    qputenv("XDG_DATA_HOME", directory.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", (directory.path() + QStringLiteral("/config")).toUtf8());
    QCoreApplication::setOrganizationName(QStringLiteral("SnowShotTests"));
    QCoreApplication::setApplicationName(QStringLiteral("linux-tray-window"));
    QApplication application(argc, argv);
    require(snow_shot::storage::ApplicationStorage::instance()
                .initialize({directory.path(), directory.path(), 8000})
                .success,
            "storage must initialize");
    snow_shot::presentation::styles::ThemeManager::instance().initialize(application);
    require(snow_shot::presentation::LanguageManager::instance().setLanguage(QStringLiteral("en_US")),
            "English must be available for the menu labels");

    const auto& registry = snow_shot::presentation::settings::builtInSettingsRegistry();
    snow_shot::presentation::GlobalShortcutManager shortcuts;
    snow_shot::presentation::settings::BuiltInSettingsBackend backend(shortcuts);
    snow_shot::presentation::settings::SettingsRuntimeSession session(registry, backend);
    MainWindow window(registry, session);
    require(window.findChild<TitleBarWidget*>() == nullptr,
            "Linux must not show a second SnowShot title bar under the system one");
    require(window.windowTitle() == QStringLiteral("Snow Shot"),
            "the system title must be the product name once, not SnowShot — Snow Shot");
    snow_shot::presentation::SystemTrayController tray;
    tray.setMenuOptions(snow_shot::storage::TraySettings().menuOptions());
    QObject::connect(&tray, &snow_shot::presentation::SystemTrayController::openFunctionSettingsRequested,
                     &window, [&window] { window.showScreenshotSettings(); });
    QObject::connect(&tray, &snow_shot::presentation::SystemTrayController::showMainWindowRequested,
                     &window, [&window] { window.showAndActivate(); });

    const QStringList before = statusItems();
    tray.show();
    QStringList added;
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 5000 && added.size() != 1) {
        added.clear();
        for (const QString& item : statusItems()) {
            if (!before.contains(item))
                added.append(item);
        }
        QApplication::processEvents();
    }
    require(added.size() == 1, "the tray must register one status item");
    const int at = added.first().indexOf(QLatin1Char('@'));
    require(at > 0, "the status item must name its service and menu object");
    const QString service = added.first().left(at);
    const QString path = added.first().mid(at + 1);

    const QString settingsLabel = QCoreApplication::translate("SettingsCatalog", "Settings");
    const QString mainLabel = QCoreApplication::translate("SettingsCatalog", "Show main interface");
    const QString menuPath = menuObjectPath(service, path);
    const QList<QPair<int, QString>> entries = menuEntries(service, menuPath);
    int settingsId = -1;
    int mainId = -1;
    for (const auto& entry : entries) {
        if (entry.second == settingsLabel)
            settingsId = entry.first;
        if (entry.second == mainLabel)
            mainId = entry.first;
    }
    require(settingsId >= 0 && mainId >= 0,
            "the shell menu must list Settings and the main interface");

    clickMenuEntry(service, menuPath, settingsId);
    auto* content = window.findChild<ContentCardWidget*>();
    require(window.isVisible() && content != nullptr, "Settings must open the main window");
    const auto location = content->currentLocation();
    require(location.pageId == QStringLiteral("screenshots") &&
                location.sectionId == QStringLiteral("screenshot-settings"),
            "Settings must show the screenshot settings page");
    require(window.width() > 200 && window.height() > 200,
            "the settings window must have a real visible size");

    window.hide();
    require(!window.isVisible(), "the settings window must close before the next check");
    busctl({QStringLiteral("--user"), QStringLiteral("call"), QStringLiteral("--"), service, path,
            QStringLiteral("org.kde.StatusNotifierItem"), QStringLiteral("Activate"),
            QStringLiteral("ii"), QStringLiteral("0"), QStringLiteral("0")});
    require(window.isVisible() && content->currentLocation().pageId == QStringLiteral("screenshots") &&
                content->currentLocation().sectionId == QStringLiteral("screenshot-settings"),
            "a status-icon double-click must open the screenshot settings page");

    window.hide();
    require(!window.isVisible(), "the settings window must close before the next check");
    clickMenuEntry(service, menuPath, mainId);
    require(window.isVisible() && window.width() > 200 && window.height() > 200,
            "the main interface command must open the operation window");

    tray.hide();
    snow_shot::storage::ApplicationStorage::instance().shutdown();
    return 0;
}
