#pragma once
#include <QWidget>
namespace tb { class Application; }
namespace tb::ui {
class VerifyDialog { public: static bool verify(Application& app, QWidget* parent); };
}
