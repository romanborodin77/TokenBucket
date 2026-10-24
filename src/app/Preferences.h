#pragma once
#include <QKeySequence>
#include <QSettings>
#include <optional>
#include "../platform/QuickPaste.h"
namespace tb::prefs {
enum class CloseBehavior { Hide, LockHide, Exit };
struct Values {
    QString theme = QStringLiteral("system");
    bool notifications = true, clearEnvironmentOnExit = false;
    int idleMinutes = 5, revealSeconds = 15, clipboardSeconds = 20;
    QKeySequence hotkey = QKeySequence(QStringLiteral("Ctrl+Alt+Space"));
    paste::Field defaultField = paste::Field::Secret;
    CloseBehavior closeBehavior = CloseBehavior::Hide;
    std::optional<bool> autostart;
    bool operator==(const Values&) const = default;
};
struct ApplyResult { QStringList appliedKeys; QString failedKey, error; };
Values read(QSettings& settings);
QString validate(const Values& values);
bool writeValue(QSettings& settings,const QString& key,const QVariant& value,QString* error=nullptr);
}
