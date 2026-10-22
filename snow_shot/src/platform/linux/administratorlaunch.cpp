#include "snow_shot/platform/windows/administratorlaunch.h"

#include "snow_shot/platform/windows/autostartregistration.h"

#include <QCoreApplication>

namespace snow_shot::platform::windows {
namespace {

AdministratorResult failure(const QString& error) {
    AdministratorResult result;
    result.success = false;
    result.error = error;
    return result;
}

AdministratorResult ok() {
    return {true, false, {}};
}

bool desktopFileBelongsToRoot(const QByteArray& desktopFile, const QString& root) {
    return !root.isEmpty() && desktopFile.contains(root.toUtf8());
}

} // namespace

// Linux login startup is an XDG autostart desktop file. Administrator
// elevation has no equivalent and stays an explicit failure.

AdministratorPresentation administratorPresentation(AdministratorState state, bool autoStart,
                                                    bool pending) {
    Q_UNUSED(state);
    Q_UNUSED(autoStart);
    Q_UNUSED(pending);
    AdministratorPresentation presentation;
    presentation.launchEnabled = false;
    presentation.restartEnabled = false;
    presentation.elevated = false;
    presentation.launchHint = QCoreApplication::translate(
        "AdministratorLaunch", "Launching as administrator is not available on Linux.");
    presentation.restartHint = presentation.launchHint;
    presentation.restartLabel = QCoreApplication::translate("AdministratorLaunch",
                                                            "Restart as administrator");
    return presentation;
}

AdministratorResult runRestartTransaction(
    const std::function<AdministratorResult(const std::function<bool()>&)>& prepare,
    const std::function<bool()>& flush, const std::function<void()>& quit) {
    Q_UNUSED(prepare);
    Q_UNUSED(flush);
    Q_UNUSED(quit);
    AdministratorResult result;
    result.success = false;
    result.error = QCoreApplication::translate(
        "AdministratorLaunch", "Administrator restart is not supported on Linux.");
    return result;
}

AdministratorResult runStartupTransaction(const StartupTransactionOperations& operations) {
    Q_UNUSED(operations);
    AdministratorResult result;
    result.success = false;
    result.error = QCoreApplication::translate(
        "AdministratorLaunch", "Startup transactions are not supported on Linux.");
    return result;
}

AdministratorState administratorState() {
    AdministratorState state;
    state.valid = false;
    state.member = false;
    state.elevated = false;
    return state;
}

std::optional<StartupMode> observedStartupMode() {
    const AutoStartRegistrationSnapshot current = AutoStartRegistration::snapshot();
    if (!current.valid)
        return std::nullopt;
    return current.state == AutoStartState::Enabled ? StartupMode::Registry : StartupMode::Off;
}

QString canonicalAccountSid(const QString& accountOrSid) {
    return accountOrSid;
}

bool sameAccountSid(const QString& left, const QString& right) {
    return left == right;
}

bool administratorOperationPending() {
    return false;
}

void setAdministratorRestartGuard(std::function<bool()> guard) {
    Q_UNUSED(guard);
}

AdministratorResult changeStartupMode(StartupMode mode, const std::function<bool()>& persist) {
    if (mode == StartupMode::ElevatedTask) {
        return failure(QCoreApplication::translate(
            "AdministratorLaunch", "Launching as administrator is not available on Linux."));
    }
    const AutoStartRegistrationSnapshot previous = AutoStartRegistration::snapshot();
    if (!previous.valid)
        return failure(previous.error);
    QString error;
    if (!AutoStartRegistration::setEnabled(mode == StartupMode::Registry, &error))
        return failure(error);
    if (!persist || persist())
        return ok();
    QString restoreError;
    if (!AutoStartRegistration::restore(previous, &restoreError))
        return failure(restoreError);
    return failure(QCoreApplication::translate("AdministratorLaunch",
                                               "Startup settings could not be saved."));
}

AdministratorResult restartAsAdministrator(const std::function<bool()>& flush) {
    Q_UNUSED(flush);
    AdministratorResult result;
    result.success = false;
    result.error = QCoreApplication::translate(
        "AdministratorLaunch", "Restarting as administrator is not supported on Linux.");
    return result;
}

int dispatchAdministratorHelper(const QStringList& arguments) {
    Q_UNUSED(arguments);
    return -1;
}

AdministratorResult reconcileStartupMode(StartupMode mode) {
    if (mode == StartupMode::ElevatedTask) {
        return failure(QCoreApplication::translate(
            "AdministratorLaunch", "Launching as administrator is not available on Linux."));
    }
    if (mode == StartupMode::Registry && AutoStartRegistration::matchesExpectedCommand())
        return ok();
    QString error;
    if (!AutoStartRegistration::setEnabled(mode == StartupMode::Registry, &error))
        return failure(error);
    return ok();
}

void reconcileStartupRunValue(const QString& usersRunKey, const QString& expectedCommand,
                              const QString& replacementCommand) {
    Q_UNUSED(usersRunKey);
    Q_UNUSED(expectedCommand);
    Q_UNUSED(replacementCommand);
}

AdministratorResult removeInstallationStartup(const QString& root) {
    const AutoStartRegistrationSnapshot current = AutoStartRegistration::snapshot();
    if (!current.valid)
        return failure(current.error);
    if (!current.exists || !desktopFileBelongsToRoot(current.nativeData, root))
        return ok();
    QString error;
    if (!AutoStartRegistration::setEnabled(false, &error))
        return failure(error);
    return ok();
}

AdministratorResult migrateInstallationStartup(const QString& previousRoot, const QString& root) {
    Q_UNUSED(root);
    const AutoStartRegistrationSnapshot current = AutoStartRegistration::snapshot();
    if (!current.valid)
        return failure(current.error);
    if (!current.exists || !desktopFileBelongsToRoot(current.nativeData, previousRoot))
        return ok();
    QString error;
    if (!AutoStartRegistration::setEnabled(true, &error))
        return failure(error);
    return ok();
}

bool launchOnInteractiveDesktop(const QString& executable) {
    Q_UNUSED(executable);
    return false;
}

} // namespace snow_shot::platform::windows
