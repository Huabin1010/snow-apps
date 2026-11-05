#include "snow_shot/platform/windows/shellcontextmenuregistration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <cstdlib>
#include <iostream>

namespace {
namespace windows = snow_shot::platform::windows;

const QString kProduct = QStringLiteral("snow-shot");
const QString kLabel = QStringLiteral("Edit with Snow Shot");
const QString kFlag = QStringLiteral("--edit-with-snow-shot");
constexpr QFileDevice::Permissions kExpectedPermissions =
    QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
    QFileDevice::ReadGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther;
constexpr QFileDevice::Permissions kPosixPermissions =
    kExpectedPermissions | QFileDevice::WriteGroup | QFileDevice::WriteOther;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "a context menu entry must be readable");
    return file.readAll();
}

void writeFile(const QString& path, const QByteArray& content) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create a fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
                file.write(content) == content.size(),
            "write a fixture file");
}

bool exists(const QString& path) {
    return QFileInfo(path).exists() || QFileInfo(path).isSymLink();
}

// Reverses Desktop Entry string escaping for the first value of a key.
QString keyValue(const QByteArray& content, const QString& key) {
    for (const QString& line : QString::fromUtf8(content).split(QLatin1Char('\n'))) {
        if (!line.startsWith(key + QLatin1Char('=')))
            continue;
        const QString raw = line.mid(key.size() + 1);
        QString value;
        for (qsizetype index = 0; index < raw.size(); ++index) {
            if (raw.at(index) != u'\\' || index + 1 >= raw.size()) {
                value += raw.at(index);
                continue;
            }
            const QChar next = raw.at(++index);
            value += next == u's'   ? QChar(u' ')
                     : next == u'n' ? QChar(u'\n')
                     : next == u't' ? QChar(u'\t')
                     : next == u'r' ? QChar(u'\r')
                                    : next;
        }
        return value;
    }
    return {};
}

// Expands launcher field codes the way the file managers do before they split the command.
QString expandFieldCodes(const QString& command, QChar code, const QString& insertion) {
    QString expanded;
    for (qsizetype index = 0; index < command.size(); ++index) {
        if (command.at(index) == u'%' && index + 1 < command.size()) {
            const QChar next = command.at(++index);
            if (next == u'%')
                expanded += u'%';
            else if (next == code)
                expanded += insertion;
            continue;
        }
        expanded += command.at(index);
    }
    return expanded;
}

QString shellSingleQuoted(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + value + QLatin1Char('\'');
}

// Nemo's Quote=double inserts each path as a double-quoted word with " and \ escaped.
QString nemoQuoted(QString value) {
    value.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

struct Fixture {
    QTemporaryDir directory;
    QString root;
    QString executable;
    QString output;
    int notifications = 0;

    Fixture() {
        require(directory.isValid(), "a temporary directory must be available");
        root = directory.filePath(QStringLiteral("data"));
        output = directory.filePath(QStringLiteral("arguments"));
        // Spaces, a double quote and a dollar sign exercise every quoting layer.
        executable = directory.filePath(QStringLiteral("bin dir/snow \"shot\" $HOME"));
        writeFile(executable, "#!/bin/sh\nfor argument do printf '%s\\0' \"$argument\"; done "
                              "> \"$SNOW_SHOT_TEST_OUTPUT\"\n");
        require(QFile::setPermissions(executable, kExpectedPermissions),
                "the fake executable must be runnable");
    }

    windows::ShellContextMenuRegistration::Options options(const QString& path = {}) {
        windows::ShellContextMenuRegistration::Options result;
        result.classesRoot = root;
        result.executablePath = path.isEmpty() ? executable : path;
        result.productId = kProduct;
        result.registry.notifyChanged = [this] { ++notifications; };
        return result;
    }

    [[nodiscard]] QString serviceMenu() const {
        return root + QStringLiteral("/kio/servicemenus/snow-shot.desktop");
    }
    [[nodiscard]] QString nemoAction() const {
        return root + QStringLiteral("/nemo/actions/snow-shot.nemo_action");
    }
    [[nodiscard]] QString script(const QString& name = kLabel) const {
        return root + QStringLiteral("/nautilus/scripts/") + name;
    }
    [[nodiscard]] QStringList entries() const {
        return {serviceMenu(), nemoAction(), script()};
    }

    [[nodiscard]] QStringList run(const QString& program, const QStringList& arguments,
                                  const QString& workingDirectory = {}) const {
        QFile::remove(output);
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.insert(QStringLiteral("SNOW_SHOT_TEST_OUTPUT"), output);
        process.setProcessEnvironment(environment);
        if (!workingDirectory.isEmpty())
            process.setWorkingDirectory(workingDirectory);
        process.start(program, arguments);
        require(process.waitForFinished(10000) && process.exitStatus() == QProcess::NormalExit &&
                    process.exitCode() == 0,
                "the generated launcher command must exit successfully");
        if (!QFileInfo::exists(output))
            return {};
        QStringList result;
        const QByteArray bytes = readFile(output);
        for (const QByteArray& item : bytes.split('\0'))
            result.append(QString::fromUtf8(item));
        result.removeLast();
        return result;
    }
};

QStringList pairs(const QStringList& paths) {
    QStringList result;
    for (const QString& path : paths)
        result << kFlag << path;
    return result;
}

void enableWritesEveryFileManagerEntry() {
    Fixture fixture;
    const windows::ShellContextMenuRegistration registration(fixture.options());
    require(windows::ShellContextMenuRegistration::isSupported(), "Linux supports context menus");
    QString error;
    require(registration.setEnabled(true, &error) && error.isEmpty() && fixture.notifications == 1,
            "enabling writes the entries and notifies once");
    for (const QString& path : fixture.entries()) {
        const QFileInfo info(path);
        require(info.isFile() && !info.isSymLink() &&
                    (info.permissions() & kPosixPermissions) == kExpectedPermissions,
                "each entry is a regular rwxr-xr-x file");
        const QByteArray content = readFile(path);
        require(content.contains("X-Snow-Shot-ProductId=snow-shot\n") &&
                    content.contains("X-Snow-Shot-Executable=" + fixture.executable.toUtf8()),
                "each entry records its product and installation");
    }

    const QByteArray service = readFile(fixture.serviceMenu());
    const QString mimeTypes = keyValue(service, QStringLiteral("MimeType"));
    require(keyValue(service, QStringLiteral("Type")) == u"Service" &&
                keyValue(service, QStringLiteral("Actions")) == u"edit;" &&
                keyValue(service, QStringLiteral("Name")) == kLabel &&
                keyValue(service, QStringLiteral("Exec")) == registration.expectedCommand() &&
                mimeTypes.contains(QStringLiteral("image/png;")) &&
                mimeTypes.contains(QStringLiteral("text/markdown;")) &&
                !mimeTypes.contains(QStringLiteral("text/plain")),
            "the Dolphin service menu targets supported types without plain text");
    const QByteArray nemo = readFile(fixture.nemoAction());
    const QString extensions = keyValue(nemo, QStringLiteral("Extensions"));
    require(keyValue(nemo, QStringLiteral("Name")) == kLabel &&
                keyValue(nemo, QStringLiteral("Selection")) == u"notnone" &&
                keyValue(nemo, QStringLiteral("Quote")) == u"double" &&
                extensions.contains(QStringLiteral(".png;")) &&
                extensions.contains(QStringLiteral(".txt;")),
            "the Nemo action lists every supported extension");
    require(readFile(fixture.script()).startsWith("#!/bin/sh\n"),
            "the Nautilus script is named after the menu label");

    require(registration.setEnabled(true) && fixture.notifications == 1,
            "re-enabling an identical registration writes nothing");
    require(registration.setEnabled(true, QStringLiteral("e"), &error) &&
                fixture.notifications == 1,
            "file managers have no access keys, so a valid key changes nothing");
    require(!registration.setEnabled(true, QStringLiteral("!"), &error) && !error.isEmpty(),
            "an invalid access key is still rejected");

    require(registration.setEnabled(false, &error) && fixture.notifications == 2,
            "disabling removes the entries and notifies once");
    for (const QString& path : fixture.entries())
        require(!exists(path), "disabling removes every owned entry");
    require(registration.setEnabled(false) && fixture.notifications == 2,
            "disabling twice is a no-op");
}

void generatedCommandsPassPathPairs() {
    Fixture fixture;
    const windows::ShellContextMenuRegistration registration(fixture.options());
    require(registration.setEnabled(true), "enable the entries");
    const QString folder = fixture.directory.filePath(QStringLiteral("pictures"));
    require(QDir().mkpath(folder), "create the selection folder");
    const QStringList paths{folder + QStringLiteral("/a b.png"),
                            folder + QStringLiteral("/it's \"q\".jpg"),
                            folder + QStringLiteral("/图片 50%.md")};

    // Nemo expands %F and hands the command to GLib's POSIX-compatible shell parser.
    QStringList quoted;
    for (const QString& path : paths)
        quoted.append(nemoQuoted(path));
    const QString nemo =
        expandFieldCodes(keyValue(readFile(fixture.nemoAction()), QStringLiteral("Exec")), u'F',
                         quoted.join(QLatin1Char(' ')));
    require(fixture.run(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), nemo}) == pairs(paths),
            "the Nemo action passes one flag and path pair per selected file");

    // Dolphin starts one process per file for %f.
    const QString kio =
        expandFieldCodes(keyValue(readFile(fixture.serviceMenu()), QStringLiteral("Exec")), u'f',
                         shellSingleQuoted(paths.at(1)));
    require(fixture.run(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), kio}) ==
                pairs({paths.at(1)}),
            "the Dolphin service menu passes the selected file after the edit flag");

    // Nautilus runs from the open folder with relative operands where it can.
    require(fixture.run(fixture.script(), {QStringLiteral("a b.png"), paths.at(2)}, folder) ==
                pairs({paths.at(0), paths.at(2)}),
            "the Nautilus script resolves relative selections against the open folder");
    require(fixture.run(fixture.script(), {}, folder).isEmpty() &&
                !QFileInfo::exists(fixture.output),
            "the Nautilus script does not start Snow Shot without a local selection");
}

void foreignEntriesAreNeverChanged() {
    Fixture fixture;
    const windows::ShellContextMenuRegistration registration(fixture.options());
    const QByteArray foreign = "[Desktop Entry]\nType=Service\nActions=other;\n";
    writeFile(fixture.serviceMenu(), foreign);
    QString error;
    require(!registration.setEnabled(true, &error) &&
                error.contains(QStringLiteral("owned by another application")) &&
                fixture.notifications == 0,
            "a foreign entry at a target path rejects the registration");
    require(readFile(fixture.serviceMenu()) == foreign && !exists(fixture.nemoAction()) &&
                !exists(fixture.script()),
            "a rejected registration leaves no partial entries behind");
    require(registration.setEnabled(false) && readFile(fixture.serviceMenu()) == foreign,
            "disabling keeps an entry owned by another application");

    QFile::remove(fixture.serviceMenu());
    const QString target = fixture.directory.filePath(QStringLiteral("linked script"));
    writeFile(target, "#!/bin/sh\n# X-Snow-Shot-ProductId=snow-shot\n");
    require(QDir().mkpath(QFileInfo(fixture.script()).absolutePath()) &&
                QFile::link(target, fixture.script()),
            "create a symbolic link at the script path");
    require(!registration.setEnabled(true, &error) && QFileInfo(fixture.script()).isSymLink() &&
                !exists(fixture.serviceMenu()),
            "a symbolic link is treated as foreign even when its target looks owned");
    require(registration.setEnabled(false) && QFileInfo(fixture.script()).isSymLink() &&
                readFile(target).contains("snow-shot"),
            "disabling never follows or removes a symbolic link");

    QFile::remove(fixture.script());
    const QString mini = fixture.script(QStringLiteral("Edit with Snow Shot Mini"));
    const QByteArray miniScript = "#!/bin/sh\n# X-Snow-Shot-ProductId=snow-shot-mini\n";
    writeFile(mini, miniScript);
    require(registration.setEnabled(true) && registration.setEnabled(false) &&
                readFile(mini) == miniScript,
            "another edition's Nautilus script survives enabling and disabling");
}

void failedPreferenceCommitRestoresEntries() {
    Fixture fixture;
    const QString otherExecutable = fixture.directory.filePath(QStringLiteral("old/snow-shot"));
    const windows::ShellContextMenuRegistration previousInstallation(
        fixture.options(otherExecutable));
    const windows::ShellContextMenuRegistration registration(fixture.options());

    auto result = registration.changeEnabled(true, [] { return false; });
    require(!result.success && result.error.contains(QStringLiteral("preference")),
            "a failed preference commit reports the failure");
    for (const QString& path : fixture.entries())
        require(!exists(path), "rollback removes entries created by the failed transaction");

    require(previousInstallation.setEnabled(true), "another installation registers first");
    QList<QByteArray> before;
    for (const QString& path : fixture.entries())
        before.append(readFile(path));
    bool committedAfterNativeChange = false;
    result = registration.changeEnabled(true, [&] {
        committedAfterNativeChange =
            readFile(fixture.script()).contains(fixture.executable.toUtf8());
        return false;
    });
    require(!result.success && committedAfterNativeChange,
            "the preference is committed after the entries change");
    for (qsizetype index = 0; index < before.size(); ++index) {
        const QString& path = fixture.entries().at(index);
        require(readFile(path) == before.at(index) &&
                    (QFileInfo(path).permissions() & kPosixPermissions) == kExpectedPermissions,
                "rollback restores the previous installation's entries and permissions");
    }

    result = registration.changeEnabled(false, [] { return true; });
    require(result.success && !exists(fixture.serviceMenu()),
            "a successful commit keeps the requested state");
}

void renamedScriptsAndLabelRefresh() {
    Fixture fixture;
    const windows::ShellContextMenuRegistration registration(fixture.options());
    require(registration.refreshLabels() && fixture.notifications == 0 &&
                !exists(fixture.serviceMenu()),
            "refreshing without a registration creates nothing");

    require(registration.setEnabled(true), "enable the entries");
    const QString oldScript = fixture.script(QStringLiteral("Mit Snow Shot bearbeiten"));
    require(QFile::rename(fixture.script(), oldScript), "simulate a script from another language");
    const QByteArray staleService = readFile(fixture.serviceMenu()) + "# stale\n";
    writeFile(fixture.serviceMenu(), staleService);
    const int notifications = fixture.notifications;
    require(registration.refreshLabels() && fixture.notifications == notifications + 1,
            "refreshing an owned registration rewrites it");
    require(!exists(oldScript) && QFileInfo(fixture.script()).isFile() &&
                readFile(fixture.serviceMenu()) != staleService,
            "refreshing renames the script and rewrites stale entries");
    QString error;
    require(!registration.refreshLabels(QStringLiteral("!"), &error) && !error.isEmpty(),
            "refreshing rejects an invalid access key");

    const QString otherExecutable = fixture.directory.filePath(QStringLiteral("other/snow-shot"));
    const windows::ShellContextMenuRegistration otherInstallation(fixture.options(otherExecutable));
    require(otherInstallation.setEnabled(true), "another installation takes over the entries");
    const QByteArray otherService = readFile(fixture.serviceMenu());
    require(registration.refreshLabels() && readFile(fixture.serviceMenu()) == otherService,
            "refreshing leaves entries written by another installation");
}

void defaultRootFollowsXdgDataHome() {
    Fixture fixture;
    const QString dataHome = fixture.directory.filePath(QStringLiteral("xdg data"));
    qputenv("XDG_DATA_HOME", dataHome.toUtf8());
    auto options = fixture.options();
    options.classesRoot.clear();
    const windows::ShellContextMenuRegistration registration(options);
    require(
        registration.setEnabled(true) &&
            QFileInfo(dataHome + QStringLiteral("/kio/servicemenus/snow-shot.desktop")).isFile() &&
            QFileInfo(dataHome + QStringLiteral("/nautilus/scripts/") + kLabel).isFile(),
        "the default root is the XDG data home");
    require(
        registration.setEnabled(false) &&
            !QFileInfo::exists(dataHome + QStringLiteral("/nemo/actions/snow-shot.nemo_action")),
        "disabling cleans the XDG data home");
}
} // namespace

int main(int argc, char** argv) {
    QCoreApplication application(argc, argv);
    enableWritesEveryFileManagerEntry();
    generatedCommandsPassPathPairs();
    foreignEntriesAreNeverChanged();
    failedPreferenceCommitRestoresEntries();
    renamedScriptsAndLabelRefresh();
    defaultRootFollowsXdgDataHome();
    return EXIT_SUCCESS;
}
