#include "snow_shot/platform/windows/shellcontextmenuregistration.h"

#include "snow_shot/app/edition.h"
#include "snow_shot/app/supportedpinfiles.h"
#include "snow_shot/shortcuts/menuaccesskey.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

// Linux file managers read per-user menu entries from XDG data directories:
//   Dolphin (KIO)      <data>/kio/servicemenus/<product>.desktop
//   Nemo               <data>/nemo/actions/<product>.nemo_action
//   Files (Nautilus)   <data>/nautilus/scripts/<menu label>
// The shared registry-shaped snapshot keys each entry by absolute path. A regular file stores its
// bytes under the default value with its permission bits as the value type; anything else is
// recorded as foreign so it is never rewritten or removed.
namespace snow_shot::platform::windows {
namespace {
const QString kForeignValue = QStringLiteral("foreign");
constexpr qint64 kMaximumEntrySize = 64 * 1024;
constexpr quint32 kPermissionMask = 0x7077; // Owner, group and other bits; user bits mirror owner.
constexpr quint32 kEntryPermissions = 0x7055; // rwxr-xr-x: KIO and Nautilus require executables.
const QByteArray kProductMarker = QByteArrayLiteral("X-Snow-Shot-ProductId=");
const QByteArray kExecutableMarker = QByteArrayLiteral("X-Snow-Shot-Executable=");
const QString kEditFlag = QStringLiteral("--edit-with-snow-shot");

QString contextMenuText(const char* source) {
    return QCoreApplication::translate("ShellContextMenuRegistration", source);
}

QString entryText(const char* source, const QString& path) {
    return contextMenuText(source).arg(QDir::toNativeSeparators(path));
}

// File managers render the label literally, so no mnemonic is added on Linux.
QString menuLabel() {
    return app::edition::isMini ? contextMenuText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                                                    "Edit with Snow Shot Mini"))
                                : contextMenuText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                                                    "Edit with Snow Shot"));
}

QString invalidAccessKeyError() {
    return contextMenuText(
        QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                          "The context menu access key must be one letter (A–Z) or digit (0–9)."));
}

QString singleLine(QString value) {
    value.remove(QLatin1Char('\n'));
    value.remove(QLatin1Char('\r'));
    return value;
}

// Desktop Entry string escaping, applied after Exec quoting as the specification requires.
QString keyFileValue(const QString& value) {
    QString escaped;
    escaped.reserve(value.size());
    for (const QChar character : value) {
        if (character == u'\\')
            escaped += QStringLiteral("\\\\");
        else if (character == u'\n')
            escaped += QStringLiteral("\\n");
        else if (character == u'\r')
            escaped += QStringLiteral("\\r");
        else if (character == u'\t')
            escaped += QStringLiteral("\\t");
        else
            escaped += character;
    }
    if (escaped.startsWith(u' '))
        escaped.replace(0, 1, QStringLiteral("\\s"));
    return escaped;
}

// A double-quoted Exec argument. KIO and GLib's shell parser both honour backslash escapes for
// these characters, and both expand "%%" to a literal percent sign.
QString quotedExecArgument(const QString& value) {
    QString quoted = QStringLiteral("\"");
    for (const QChar character : singleLine(value)) {
        if (character == u'"' || character == u'`' || character == u'$' || character == u'\\')
            quoted += u'\\';
        if (character == u'%')
            quoted += u'%';
        quoted += character;
    }
    return quoted + u'"';
}

QString singleQuotedShellWord(const QString& value) {
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

// Dolphin offers the entry only when every selected item matches. Plain text is omitted because
// every text type, such as source code, inherits text/plain and would show an entry that fails.
QStringList kioMimeTypes(const QStringList& extensions) {
    static const QList<std::pair<QString, QString>> table{
        {QStringLiteral(".gif"), QStringLiteral("image/gif")},
        {QStringLiteral(".apng"), QStringLiteral("image/apng")},
        {QStringLiteral(".png"), QStringLiteral("image/png")},
        {QStringLiteral(".jpg"), QStringLiteral("image/jpeg")},
        {QStringLiteral(".jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral(".webp"), QStringLiteral("image/webp")},
        {QStringLiteral(".jxl"), QStringLiteral("image/jxl")},
        {QStringLiteral(".avif"), QStringLiteral("image/avif")},
        {QStringLiteral(".bmp"), QStringLiteral("image/bmp")},
        {QStringLiteral(".html"), QStringLiteral("text/html")},
        {QStringLiteral(".htm"), QStringLiteral("text/html")},
        {QStringLiteral(".md"), QStringLiteral("text/markdown")},
        {QStringLiteral(".markdown"), QStringLiteral("text/markdown")},
        {QStringLiteral(".css"), QStringLiteral("text/css")},
    };
    QStringList types;
    for (const auto& extension : extensions)
        for (const auto& [suffix, type] : table)
            if (suffix == extension && !types.contains(type))
                types.append(type);
    return types;
}

QByteArray markerValue(const QByteArray& content, const QByteArray& marker) {
    for (const QByteArray& rawLine : content.split('\n')) {
        QByteArray line = rawLine.trimmed();
        if (line.startsWith('#'))
            line = line.mid(1).trimmed();
        if (line.startsWith(marker))
            return line.mid(marker.size());
    }
    return {};
}

bool isForeign(const ShellContextMenuRegistryKey& key) {
    return key.exists && (key.values.contains(kForeignValue) || !key.values.contains(QString()));
}

QByteArray entryContent(const ShellContextMenuRegistryKey& key) {
    return key.values.value(QString()).data;
}

ShellContextMenuRegistryKey regularEntry(const QByteArray& content) {
    ShellContextMenuRegistryKey key;
    key.exists = true;
    key.values.insert(QString(), {kEntryPermissions, content});
    return key;
}

bool readEntry(const QString& path, ShellContextMenuRegistryKey* key, QString* error) {
    *key = {};
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;
    key->exists = true;
    // This product only creates regular files, so links and oversized files belong to someone else.
    if (info.isSymLink() || !info.isFile() || info.size() > kMaximumEntrySize) {
        key->values.insert(kForeignValue, {});
        return true;
    }
    QFile file(path);
    QByteArray content;
    if (file.open(QIODevice::ReadOnly))
        content = file.read(kMaximumEntrySize + 1);
    if (!file.isOpen() || file.error() != QFileDevice::NoError) {
        if (error)
            *error = entryText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                                 "Could not read the file manager context menu "
                                                 "entry %1."),
                               path);
        return false;
    }
    const auto permissions = static_cast<quint32>(info.permissions().toInt()) & kPermissionMask;
    key->values.insert(QString(), {permissions, std::move(content)});
    return true;
}

bool replaceEntry(const QString& path, const ShellContextMenuRegistryKey& key, QString* error) {
    if (!key.exists) {
        if (QFile::remove(path) || !QFileInfo(path).isFile())
            return true;
        if (error)
            *error = entryText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                                 "Could not remove the file manager context menu "
                                                 "entry %1."),
                               path);
        return false;
    }
    const auto value = key.values.value(QString());
    QSaveFile file(path);
    const bool written =
        QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly) &&
        file.write(value.data) == value.data.size() && file.commit() &&
        QFile::setPermissions(path, QFileDevice::Permissions::fromInt(
                                        static_cast<int>(value.type & kPermissionMask)));
    if (!written && error)
        *error = entryText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                             "Could not write the file manager context menu entry "
                                             "%1."),
                           path);
    return written;
}

struct EntryLayout {
    QString serviceMenu;
    QString nemoAction;
    QString scriptsDirectory;
    QString script;
};

EntryLayout entryLayout(const QString& root, const QString& productId) {
    // Nautilus shows the script's file name as the menu label.
    QString scriptName = singleLine(menuLabel()).trimmed();
    scriptName.replace(QLatin1Char('/'), QLatin1Char('-'));
    while (scriptName.startsWith(QLatin1Char('.')))
        scriptName.remove(0, 1);
    if (scriptName.isEmpty())
        scriptName = productId;
    const QDir data(root);
    const QString scripts = data.filePath(QStringLiteral("nautilus/scripts"));
    return {data.filePath(QStringLiteral("kio/servicemenus/%1.desktop").arg(productId)),
            data.filePath(QStringLiteral("nemo/actions/%1.nemo_action").arg(productId)), scripts,
            QDir(scripts).filePath(scriptName)};
}

QByteArray ownershipLines(const QString& productId, const QString& executable, const char* prefix) {
    return QByteArray(prefix) + kProductMarker + productId.toUtf8() + '\n' + QByteArray(prefix) +
           kExecutableMarker + singleLine(executable).toUtf8() + '\n';
}

QByteArray serviceMenuEntry(const QString& productId, const QString& executable,
                            const QStringList& mimeTypes, const QString& command) {
    QByteArray entry =
        "[Desktop Entry]\nType=Service\nMimeType=" + mimeTypes.join(QLatin1Char(';')).toUtf8() +
        // KF5 still matches service menus by this service type; KF6 ignores it.
        ";\nActions=edit;\nX-KDE-ServiceTypes=KonqPopupMenu/Plugin\n"
        "X-KDE-Priority=TopLevel\n" +
        ownershipLines(productId, executable, "");
    entry += "\n[Desktop Action edit]\nName=" + keyFileValue(menuLabel()).toUtf8() +
             "\nIcon=" + productId.toUtf8() + "\nExec=" + keyFileValue(command).toUtf8() + '\n';
    return entry;
}

// Nemo expands %F to one quoted path per selected file without a per-file flag. The POSIX shell
// rewrites that list into the repeated "--edit-with-snow-shot <path>" pairs the parser expects.
QByteArray nemoActionEntry(const QString& productId, const QString& executable,
                           const QStringList& extensions) {
    const QString command =
        QStringLiteral("/bin/sh -c 'for f do set -- \"$@\" %1 \"$f\"; shift; done; "
                       "exec \"$0\" \"$@\"' %2 %F")
            .arg(kEditFlag, quotedExecArgument(executable));
    return "[Nemo Action]\nName=" + keyFileValue(menuLabel()).toUtf8() +
           "\nExec=" + keyFileValue(command).toUtf8() + "\nIcon-Name=" + productId.toUtf8() +
           "\nSelection=notnone\nExtensions=" + extensions.join(QLatin1Char(';')).toUtf8() +
           ";\nQuote=double\n" + ownershipLines(productId, executable, "");
}

// Nautilus runs scripts from the current folder with the selected paths as operands. They are
// relative to that folder when possible and absolute otherwise; non-local selections pass none.
QByteArray nautilusScript(const QString& productId, const QString& executable) {
    return "#!/bin/sh\n" + ownershipLines(productId, executable, "# ") +
           "executable=" + singleQuotedShellWord(singleLine(executable)).toUtf8() +
           "\nfor file do\n"
           "    case $file in\n"
           "        /*) ;;\n"
           "        *) file=$PWD/$file ;;\n"
           "    esac\n"
           "    set -- \"$@\" " +
           kEditFlag.toUtf8() +
           " \"$file\"\n"
           "    shift\n"
           "done\n"
           "[ \"$#\" -gt 0 ] || exit 0\n"
           "exec \"$executable\" \"$@\"\n";
}

ShellContextMenuRegistryKey snapshotKey(const ShellContextMenuRegistrationSnapshot& snapshot,
                                        const QString& path) {
    return snapshot.keys.value(path);
}

QString ownedByAnotherApplicationError(const QString& path) {
    return entryText(QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                       "The file manager context menu entry %1 is owned by "
                                       "another application."),
                     path);
}
} // namespace

ShellContextMenuRegistration::ShellContextMenuRegistration()
    : ShellContextMenuRegistration(Options{}) {}

ShellContextMenuRegistration::ShellContextMenuRegistration(Options options)
    : m_associationsRoot(QDir::cleanPath([&options] {
          if (!options.classesRoot.isEmpty())
              return options.classesRoot;
          const QString data =
              QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
          return data.isEmpty() ? QDir::homePath() + QStringLiteral("/.local/share") : data;
      }())),
      m_executable(QDir::cleanPath(options.executablePath.isEmpty()
                                       ? QCoreApplication::applicationFilePath()
                                       : options.executablePath)),
      m_productId(options.productId.isEmpty() ? app::edition::productId() : options.productId),
      m_verbName(std::move(options.verbName)), m_registry(std::move(options.registry)),
      m_packagedPreferencePath(std::move(options.packagedPreferencePath)) {
    const auto extensions = options.fileExtensions.isEmpty() ? app::supportedPinFileExtensions()
                                                             : std::move(options.fileExtensions);
    for (auto extension : extensions) {
        extension = extension.toLower();
        if (!extension.startsWith(QLatin1Char('.')))
            extension.prepend(QLatin1Char('.'));
        // Extensions become key-file list items and file-name suffixes.
        if (extension.size() > 1 &&
            std::all_of(extension.cbegin() + 1, extension.cend(),
                        [](QChar character) { return character.isLetterOrNumber(); }) &&
            !m_extensions.contains(extension))
            m_extensions.append(extension);
    }
}

bool ShellContextMenuRegistration::isSupported() {
    return true;
}

QString ShellContextMenuRegistration::expectedCommand() const {
    return QStringLiteral("%1 %2 %f").arg(quotedExecArgument(m_executable), kEditFlag);
}

bool ShellContextMenuRegistration::ownedByProduct(const ShellContextMenuRegistryKey& key) const {
    return key.exists && !isForeign(key) &&
           QString::fromUtf8(markerValue(entryContent(key), kProductMarker)) == m_productId;
}

ShellContextMenuRegistrationSnapshot ShellContextMenuRegistration::snapshot() const {
    ShellContextMenuRegistrationSnapshot result;
    const auto layout = entryLayout(m_associationsRoot, m_productId);
    // Script names follow the interface language, so earlier labels are found by ownership.
    const auto scripts =
        QDir(layout.scriptsDirectory)
            .entryInfoList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                           QDir::Name);
    for (const QFileInfo& info : scripts) {
        ShellContextMenuRegistryKey key;
        if (readEntry(info.absoluteFilePath(), &key, nullptr) && ownedByProduct(key))
            result.keys.insert(QDir::cleanPath(info.absoluteFilePath()), std::move(key));
    }
    for (const auto& path : {layout.serviceMenu, layout.nemoAction, layout.script}) {
        ShellContextMenuRegistryKey key;
        if (!readEntry(path, &key, &result.error))
            return result;
        result.keys.insert(path, std::move(key));
    }
    result.valid = true;
    return result;
}

bool ShellContextMenuRegistration::apply(bool enabled, const QString& accessKey,
                                         const ShellContextMenuRegistrationSnapshot& previous,
                                         QString* error) const {
    Q_UNUSED(accessKey);
    const auto layout = entryLayout(m_associationsRoot, m_productId);
    QMap<QString, QByteArray> requested;
    if (enabled) {
        if (const auto mimeTypes = kioMimeTypes(m_extensions); !mimeTypes.isEmpty())
            requested.insert(layout.serviceMenu, serviceMenuEntry(m_productId, m_executable,
                                                                  mimeTypes, expectedCommand()));
        if (!m_extensions.isEmpty()) {
            requested.insert(layout.nemoAction,
                             nemoActionEntry(m_productId, m_executable, m_extensions));
            requested.insert(layout.script, nautilusScript(m_productId, m_executable));
        }
    }
    // Check every target first so a foreign entry does not leave a partial registration behind.
    for (auto item = requested.cbegin(); item != requested.cend(); ++item) {
        const auto current = snapshotKey(previous, item.key());
        if (current.exists && !ownedByProduct(current)) {
            if (error)
                *error = ownedByAnotherApplicationError(item.key());
            return false;
        }
    }
    bool changed = false;
    for (auto item = requested.cbegin(); item != requested.cend(); ++item) {
        const auto target = regularEntry(item.value());
        if (snapshotKey(previous, item.key()) == target)
            continue;
        if (!replaceEntry(item.key(), target, error))
            return false;
        changed = true;
    }
    for (auto item = previous.keys.cbegin(); item != previous.keys.cend(); ++item) {
        if (requested.contains(item.key()) || !ownedByProduct(item.value()))
            continue;
        if (!replaceEntry(item.key(), {}, error))
            return false;
        changed = true;
    }
    if (changed && m_registry.notifyChanged)
        m_registry.notifyChanged();
    return true;
}

bool ShellContextMenuRegistration::restore(const ShellContextMenuRegistrationSnapshot& previous,
                                           QString* error) const {
    if (!previous.valid) {
        if (error)
            *error = contextMenuText(
                QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                                  "Could not read the file manager context menu entries."));
        return false;
    }
    bool success = true;
    bool changed = false;
    for (auto item = previous.keys.cbegin(); item != previous.keys.cend(); ++item) {
        ShellContextMenuRegistryKey current;
        QString itemError;
        if (!readEntry(item.key(), &current, &itemError)) {
            success = false;
        } else if (current == item.value()) {
            continue;
        } else if ((current.exists && !ownedByProduct(current)) || isForeign(item.value())) {
            // Entries another application wrote, before or since the snapshot, are left alone.
            continue;
        } else if (!replaceEntry(item.key(), item.value(), &itemError)) {
            success = false;
        } else {
            changed = true;
            continue;
        }
        if (error)
            *error = itemError;
    }
    if (changed && m_registry.notifyChanged)
        m_registry.notifyChanged();
    return success;
}

ShellContextMenuResult
ShellContextMenuRegistration::changeEnabled(bool enabled,
                                            const std::function<bool()>& commitPreference) const {
    return changeEnabled(enabled, QString(), commitPreference);
}

ShellContextMenuResult
ShellContextMenuRegistration::changeEnabled(bool enabled, const QString& accessKey,
                                            const std::function<bool()>& commitPreference) const {
    const QString canonicalKey = shortcuts::canonicalMenuAccessKey(accessKey);
    if (!accessKey.isEmpty() && canonicalKey.isEmpty())
        return {false, invalidAccessKeyError()};
    const auto previous = snapshot();
    if (!previous.valid)
        return {false, previous.error};
    QString error;
    if (apply(enabled, canonicalKey, previous, &error)) {
        if (!commitPreference || commitPreference())
            return {true, {}};
        error = contextMenuText(
            QT_TRANSLATE_NOOP("ShellContextMenuRegistration",
                              "Could not save the file manager context menu preference."));
    }
    QString recoveryError;
    if (!restore(previous, &recoveryError))
        error += QLatin1Char(' ') +
                 contextMenuText(QT_TRANSLATE_NOOP(
                                     "ShellContextMenuRegistration",
                                     "Could not restore the file manager context menu entries. %1"))
                     .arg(recoveryError);
    return {false, error};
}

bool ShellContextMenuRegistration::setEnabled(bool enabled, QString* error) const {
    return setEnabled(enabled, QString(), error);
}

bool ShellContextMenuRegistration::setEnabled(bool enabled, const QString& accessKey,
                                              QString* error) const {
    const auto result = changeEnabled(enabled, accessKey, {});
    if (error)
        *error = result.error;
    return result.success;
}

bool ShellContextMenuRegistration::refreshLabels(QString* error) const {
    return refreshLabels(QString(), error);
}

// Rewrites labels only while this installation owns a registration, matching Windows. Other
// installations of the same product keep the entries they wrote.
bool ShellContextMenuRegistration::refreshLabels(const QString& accessKey, QString* error) const {
    const QString canonicalKey = shortcuts::canonicalMenuAccessKey(accessKey);
    if (!accessKey.isEmpty() && canonicalKey.isEmpty()) {
        if (error)
            *error = invalidAccessKeyError();
        return false;
    }
    const auto previous = snapshot();
    if (!previous.valid) {
        if (error)
            *error = previous.error;
        return false;
    }
    const bool registered =
        std::any_of(previous.keys.cbegin(), previous.keys.cend(), [this](const auto& key) {
            return ownedByProduct(key) &&
                   QString::fromUtf8(markerValue(entryContent(key), kExecutableMarker)) ==
                       m_executable;
        });
    if (!registered || apply(true, canonicalKey, previous, error))
        return true;
    QString recoveryError;
    if (!restore(previous, &recoveryError) && error)
        *error += QLatin1Char(' ') + recoveryError;
    return false;
}

} // namespace snow_shot::platform::windows
