#pragma once
#include <QWidget>
#include "../platform/QuickPaste.h"
namespace tb { class Application; class ClipboardGuard; }
class QLineEdit; class QListWidget; class QLabel;
namespace tb::ui {
class MiniPanel : public QWidget {
    Q_OBJECT
public:
    MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent = nullptr);
    void popup();
private:
    void send();
    void refill();
    Application& app_;
    ClipboardGuard& clip_;
    QLineEdit* search_{};
    QListWidget* list_{};
    QLabel* recipientLbl_{};
#ifdef Q_OS_WIN
    paste::Recipient recipient_{};
#endif
};
}
