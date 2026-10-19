#include "MiniPanel.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include "../platform/QuickPaste.h"
#include "VerifyDialog.h"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QShortcut>
#include <QVBoxLayout>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
namespace tb::ui {
MiniPanel::MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent)
    : QWidget(parent, Qt::Tool | Qt::FramelessWindowHint), app_(app), clip_(clip) {
    setWindowTitle(QStringLiteral("TokenBucket — быстрая вставка"));
    auto* layout = new QVBoxLayout(this);
    recipientLbl_ = new QLabel(this); recipientLbl_->setTextFormat(Qt::PlainText); recipientLbl_->setWordWrap(true);
    search_ = new QLineEdit(this); search_->setPlaceholderText(QStringLiteral("Быстрый поиск записи…"));
    search_->setAccessibleName(QStringLiteral("Быстрый поиск записи"));
    list_ = new QListWidget(this); list_->setAccessibleName(QStringLiteral("Записи для быстрой вставки"));
    layout->addWidget(recipientLbl_); layout->addWidget(search_); layout->addWidget(list_);
    connect(search_, &QLineEdit::textChanged, this, &MiniPanel::refill);
    connect(new QShortcut(Qt::Key_Return, list_), &QShortcut::activated, this, &MiniPanel::send);
    connect(list_, &QListWidget::itemDoubleClicked, this, &MiniPanel::send);
}
void MiniPanel::refill() {
    list_->clear();
    if (app_.gate() != Application::Gate::Ready) return;
    for (auto id : app_.model()->search(search_->text())) {
        const auto* entry = app_.model()->entry(id);
        auto* item = new QListWidgetItem(entry->title, list_);
        item->setData(Qt::UserRole, id.toString());
    }
    if (list_->count()) list_->setCurrentRow(0);
}
void MiniPanel::popup() {
    if (app_.gate() != Application::Gate::Ready || app_.busy()) return;
#ifdef Q_OS_WIN
    recipient_ = paste::captureRecipient(); // до show(): панель ещё не активна, фокус у получателя
    recipientLbl_->setText(QStringLiteral("Получатель: %1 — «%2»").arg(recipient_.processName, recipient_.windowTitle));
#else
    recipientLbl_->setText(QStringLiteral("Получатель: вставка вручную (Ctrl+V)"));
#endif
    search_->clear(); refill();
    show(); raise(); activateWindow();
    search_->setFocus();
}
void MiniPanel::send() {
    auto* item = list_->currentItem();
    if (!item || app_.gate() != Application::Gate::Ready || app_.busy()) return;
    const auto* entry = app_.model()->entry(QUuid::fromString(item->data(Qt::UserRole).toString()));
    if (!entry) return;
    if (!paste::eligibleForAutoPaste(*entry)) {
        QMessageBox::information(this, QStringLiteral("TokenBucket"), QStringLiteral("Этот секрет недоступен для быстрой вставки. Используйте копирование или файл."));
        return;
    }
    if (entry->reverify && !VerifyDialog::verify(app_, this)) return;
    const auto text = entry->type == model::EntryType::ApiToken ? entry->token : entry->password;
    QString error;
    if (!clip_.copySecret(text, &error)) {
        if (app_.gate() == Application::Gate::Ready) QMessageBox::warning(this, QStringLiteral("TokenBucket"), error);
        return;
    }
#ifdef Q_OS_WIN
    if (recipient_.processName.startsWith(QStringLiteral("(нет доступа"))) {
        QMessageBox::information(this, QStringLiteral("TokenBucket"), QStringLiteral("Окно-получатель принадлежит процессу с повышенными правами. Вставка отменена; секрет скопирован в буфер."));
        return;
    }
    if (!paste::recipientStillValid(recipient_)) {
        QMessageBox::information(this, QStringLiteral("TokenBucket"), QStringLiteral("Окно-получатель изменилось. Вставка отменена; секрет скопирован в буфер."));
        return;
    }
    hide();
    if (recipient_.hwnd) SetForegroundWindow(static_cast<HWND>(recipient_.hwnd));
    paste::sendCtrlV(); // только Ctrl+V, без Enter
#else
    hide();
    QMessageBox::information(this, QStringLiteral("TokenBucket"), QStringLiteral("Секрет скопирован в буфер. Вставьте его вручную (Ctrl+V)."));
#endif
}
}
