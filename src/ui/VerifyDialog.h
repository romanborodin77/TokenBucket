#pragma once
#include <QString>
#include <QWidget>
namespace tb { class Application; }
namespace tb::ui {
class VerifyDialog { public: static bool verify(Application& app, QWidget* parent, const QString& action = {}); };
}
