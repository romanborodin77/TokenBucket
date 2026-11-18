#pragma once
#include <QKeySequence>
#include <QSettings>
#include <optional>
#include "../platform/QuickPaste.h"
#include "../vault/PasswordGenerator.h"
namespace tb::prefs {
inline constexpr auto McpSshSetting = "mcpSshEnabled";
enum class CloseBehavior { Hide, LockHide, Exit };
struct Values {
    QString theme = QStringLiteral("system");
    bool notifications = true, clearEnvironmentOnExit = false;
    bool mcpSshEnabled = false;
    int idleMinutes = 5, revealSeconds = 15, clipboardSeconds = 20;
    QKeySequence hotkey = QKeySequence(QStringLiteral("Ctrl+Alt+Space"));
    paste::Field defaultField = paste::Field::Secret;
    CloseBehavior closeBehavior = CloseBehavior::Hide;
    std::optional<bool> autostart;
    crypto::GeneratorOptions generator;
    bool operator==(const Values&) const = default;
};
struct ApplyResult { QStringList appliedKeys; QString failedKey, error; };
Values read(QSettings& settings);
bool readMcpSshEnabled(QSettings& settings, bool fallback = false);
QString validate(const Values& values);
bool writeValue(QSettings& settings,const QString& key,const QVariant& value,QString* error=nullptr);
}
