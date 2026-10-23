#pragma once
#include <QString>
#include <QWidget>
namespace tb { class Application; }
namespace tb::ui {
// Повторная проверка владельца для записей с reverify (см. VerifyDialog.cpp).
class VerifyDialog { public: static bool verify(Application& app, QWidget* parent, const QString& action = {}, bool* checking = nullptr); };
}
