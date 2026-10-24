#pragma once
#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include <QDateTime>
#include "../platform/QuickPaste.h"
namespace tb { class Application; class ClipboardGuard; }
class QLineEdit; class QListWidget; class QLabel; class QComboBox; class QPushButton;
namespace tb::ui {
// Одна выдача: выбор -> подтверждение -> возврат фокуса -> публикация/отправка.
// Контекст не содержит секретов и уничтожается при смене поколения хранилища.
class MiniPanel : public QWidget {
    Q_OBJECT
public:
    MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent = nullptr);
    void popup();
    void setDefaultField(paste::Field field);
protected:
    void closeEvent(QCloseEvent* event) override;
    bool eventFilter(QObject* object, QEvent* event) override;
private:
    enum class Phase { Closed, Choosing, Verifying, Restoring };
    void send(bool copyOnly = false);
    void refill();
    void selectField();
    void updateActions();
    void clearContext();
    void cancel();
    void fail(const QString& reason);
    bool contextValid() const;
    void continuePaste();
    paste::Field field() const;
    Application& app_;
    ClipboardGuard& clip_;
    QLineEdit* search_{};
    QListWidget* list_{};
    QLabel *recipientLbl_{}, *status_{};
    QComboBox* field_{};
    QPushButton *paste_{}, *copy_{};
    Phase phase_ = Phase::Closed;
    bool canPaste_ = false;
    bool checking_ = false, publishing_ = false;
    quint64 action_ = 0;
    model::Id operationId_;
    quint64 generation_ = 0;
    QDateTime modified_;
    model::EntryType type_ = model::EntryType::ApiToken;
    bool reverify_ = false;
    paste::Field operationField_ = paste::Field::Secret;
    paste::Field defaultField_ = paste::Field::Secret;
    QTimer restoreTimer_;
    QElapsedTimer restoreBudget_;
#ifdef Q_OS_WIN
    paste::Recipient recipient_{};
#endif
};
}
