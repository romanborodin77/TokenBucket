#include "MiniPanel.h"
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include "VerifyDialog.h"
#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QComboBox>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QCloseEvent>
#include <QKeyEvent>
namespace tb::ui {
MiniPanel::MiniPanel(Application& app, ClipboardGuard& clip, QWidget* parent)
    : QWidget(parent, Qt::Tool), app_(app), clip_(clip) {
    setWindowTitle(QStringLiteral("TokenBucket — быстрая вставка"));
    setObjectName(QStringLiteral("miniPanel")); resize(600, 400);
    auto* layout = new QVBoxLayout(this);
    recipientLbl_ = new QLabel(this); recipientLbl_->setTextFormat(Qt::PlainText); recipientLbl_->setWordWrap(true);
    search_ = new QLineEdit(this); search_->setObjectName(QStringLiteral("miniSearch"));
    search_->setPlaceholderText(QStringLiteral("Быстрый поиск записи…"));
    search_->setAccessibleName(QStringLiteral("Быстрый поиск записи"));
    list_ = new QListWidget(this); list_->setObjectName(QStringLiteral("miniEntries"));
    list_->setAccessibleName(QStringLiteral("Записи для быстрой вставки"));
    field_ = new QComboBox(this); field_->setObjectName(QStringLiteral("miniField")); field_->setAccessibleName(QStringLiteral("Поле записи"));
    status_ = new QLabel(this); status_->setObjectName(QStringLiteral("miniStatus")); status_->setTextFormat(Qt::PlainText); status_->setWordWrap(true);
    auto* buttons = new QHBoxLayout;
    auto* cancelButton = new QPushButton(QStringLiteral("Отмена (Esc)"), this); cancelButton->setObjectName(QStringLiteral("miniCancel"));
    copy_ = new QPushButton(QStringLiteral("Скопировать"), this); copy_->setObjectName(QStringLiteral("miniCopy"));
    paste_ = new QPushButton(QStringLiteral("Вставить"), this); paste_->setObjectName(QStringLiteral("miniPaste"));
    buttons->addWidget(cancelButton); buttons->addStretch(); buttons->addWidget(copy_); buttons->addWidget(paste_);
    layout->addWidget(recipientLbl_); layout->addWidget(search_); layout->addWidget(list_, 1); layout->addWidget(field_); layout->addWidget(status_);
    layout->addWidget(new QLabel(QStringLiteral("↑↓ — выбор · Enter — вставить · без отправки Enter получателю"), this)); layout->addLayout(buttons);
    connect(search_, &QLineEdit::textChanged, this, &MiniPanel::refill);
    connect(search_, &QLineEdit::returnPressed, this, [this] { send(); });
    connect(list_, &QListWidget::itemActivated, this, [this] { send(); });
    connect(list_, &QListWidget::currentRowChanged, this, [this] { selectField(); });
    connect(field_, &QComboBox::currentIndexChanged, this, [this] { updateActions(); });
    connect(paste_, &QPushButton::clicked, this, [this] { send(); });
    connect(copy_, &QPushButton::clicked, this, [this] { send(true); });
    connect(cancelButton, &QPushButton::clicked, this, &MiniPanel::cancel);
    connect(new QShortcut(QKeySequence(Qt::Key_Escape), this), &QShortcut::activated, this, &MiniPanel::cancel);
    connect(&app_, &Application::stateChanged, this, [this] {
        if (app_.gate() != Application::Gate::Ready || (phase_ != Phase::Closed && generation_ != app_.generation())) cancel();
    });
    connect(&app_, &Application::busyChanged, this, [this](bool busy) {
        if (busy && ((phase_ == Phase::Verifying && !checking_) || phase_ == Phase::Restoring)) cancel();
        else updateActions();
    });
    restoreTimer_.setInterval(25); connect(&restoreTimer_, &QTimer::timeout, this, &MiniPanel::continuePaste);
    qApp->installEventFilter(this);
}
paste::Field MiniPanel::field() const { return paste::Field(field_->currentData().toInt()); }
void MiniPanel::clearContext() {
    ++action_;
    if (publishing_) clip_.cancelPendingCopy();
    phase_ = Phase::Closed; restoreTimer_.stop(); operationId_ = {}; canPaste_ = false;
    const QSignalBlocker block(search_); search_->clear(); list_->clear(); field_->clear();
    recipientLbl_->clear(); status_->clear(); updateActions();
#ifdef Q_OS_WIN
    recipient_ = {};
#endif
}
void MiniPanel::cancel() { clearContext(); hide(); }
void MiniPanel::closeEvent(QCloseEvent* event) { cancel(); event->ignore(); }
bool MiniPanel::eventFilter(QObject* object, QEvent* event) {
    if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        auto* widget = qobject_cast<QWidget*>(object);
        if (widget && (widget == this || isAncestorOf(widget))) { cancel(); return true; }
    }
    if (phase_ == Phase::Choosing && event->type() == QEvent::ApplicationDeactivate) cancel();
    if (object == search_ && event->type() == QEvent::KeyPress) {
        const auto key = static_cast<QKeyEvent*>(event)->key();
        if (key == Qt::Key_Up || key == Qt::Key_Down) {
            if (list_->count()) list_->setCurrentRow(qBound(0, list_->currentRow() + (key == Qt::Key_Down ? 1 : -1), list_->count() - 1));
            return true;
        }
    }
    return QWidget::eventFilter(object, event);
}
void MiniPanel::refill() {
    if (phase_ != Phase::Choosing) return;
    list_->clear();
    if (app_.gate() != Application::Gate::Ready || app_.busy()) return;
    for (auto id : app_.model()->search(search_->text())) {
        const auto* entry = app_.model()->entry(id);
        if (!entry || entry->type == model::EntryType::SshKey) continue;
        QStringList path;
        for (auto group = entry->groupId; !group.isNull();) {
            const auto node = app_.model()->nodes().constFind(group);
            if (node == app_.model()->nodes().cend()) break;
            path.prepend(node->name); group = node->parent;
        }
        const auto type = entry->type == model::EntryType::ApiToken ? QStringLiteral("Токен") : QStringLiteral("Логин / пароль");
        auto* item = new QListWidgetItem(QStringLiteral("%1\n%2 · %3").arg(entry->title, type, path.join(" / ")), list_);
        item->setData(Qt::UserRole, id.toString());
    }
    if (list_->count()) { status_->clear(); list_->setCurrentRow(0); }
    else { selectField(); status_->setText(QStringLiteral("Подходящих записей нет.")); }
}
void MiniPanel::selectField() {
    status_->clear();
    const QSignalBlocker block(field_); field_->clear();
    auto* item = list_->currentItem();
    const auto* entry = item ? app_.model()->entry(QUuid::fromString(item->data(Qt::UserRole).toString())) : nullptr;
    if (entry) {
        field_->addItem(entry->type == model::EntryType::ApiToken ? QStringLiteral("Токен") : QStringLiteral("Пароль"), int(paste::Field::Secret));
        if (entry->type == model::EntryType::Credentials) field_->addItem(QStringLiteral("Логин"), int(paste::Field::Login));
    }
    updateActions();
}
void MiniPanel::updateActions() {
    const bool choosing = phase_ == Phase::Choosing && app_.gate() == Application::Gate::Ready && !app_.busy();
    search_->setEnabled(choosing); list_->setEnabled(choosing); field_->setEnabled(choosing);
    const auto* item = list_->currentItem();
    const auto* entry = item && choosing ? app_.model()->entry(QUuid::fromString(item->data(Qt::UserRole).toString())) : nullptr;
    const auto* value = entry ? paste::fieldValue(*entry, field()) : nullptr;
    copy_->setEnabled(value && !value->isEmpty() && !value->contains(QChar::Null));
    paste_->setEnabled(entry && canPaste_ && paste::eligibleForAutoPaste(*entry, field()));
    paste_->setText(QStringLiteral("Вставить %1").arg(field_->currentText().toLower()));
    if (choosing && value && status_->text().isEmpty()) {
        if (value->isEmpty() || value->contains(QChar::Null)) status_->setText(QStringLiteral("Пустое поле или нулевой символ: выдача недоступна."));
        else if (!paste::eligibleForAutoPaste(*entry, field())) status_->setText(QStringLiteral("Многострочное значение: только явное копирование, без автовставки."));
    }
}
void MiniPanel::popup() {
    if (phase_ != Phase::Closed) { cancel(); return; }
    clearContext(); phase_ = Phase::Choosing; generation_ = app_.generation();
    if (app_.gate() != Application::Gate::Ready || app_.busy()) {
        recipientLbl_->setText(QStringLiteral("Хранилище заблокировано или занято. Разблокируйте его и вызовите панель заново."));
    } else {
#ifdef Q_OS_WIN
        if (QGuiApplication::platformName() == QLatin1String("windows")) {
            recipient_ = paste::captureRecipient(); QString error;
            canPaste_ = recipient_.pid != GetCurrentProcessId() && paste::recipientAllowed(recipient_, &error);
            if (canPaste_) recipientLbl_->setText(QStringLiteral("Получатель: %1 — «%2»").arg(recipient_.processName, recipient_.windowTitle));
        }
#endif
        if (!canPaste_) recipientLbl_->setText(QStringLiteral("Ручное копирование. Для автовставки активируйте поле другого приложения и вызовите горячую клавишу."));
        refill();
    }
    updateActions(); show(); raise(); activateWindow(); search_->setFocus();
}
bool MiniPanel::contextValid() const {
    if (phase_ == Phase::Closed || app_.gate() != Application::Gate::Ready || app_.busy() || generation_ != app_.generation()) return false;
    const auto* e = app_.model()->entry(operationId_);
    return e && e->modified == modified_ && e->type == type_ && e->reverify == reverify_;
}
void MiniPanel::fail(const QString& reason) {
    if (phase_ == Phase::Closed) return;
    restoreTimer_.stop(); operationId_ = {};
    if (app_.gate() != Application::Gate::Ready || generation_ != app_.generation()) { cancel(); return; }
    phase_ = Phase::Choosing; status_->setText(reason); updateActions();
    if (!isVisible()) { show(); raise(); activateWindow(); }
}
void MiniPanel::send(bool copyOnly) {
    if (phase_ != Phase::Choosing || checking_ || publishing_ || app_.gate() != Application::Gate::Ready || app_.busy()) return;
    auto* item = list_->currentItem(); if (!item) return;
    const auto* entry = app_.model()->entry(QUuid::fromString(item->data(Qt::UserRole).toString()));
    if (!entry) return;
    if (!copyOnly && (!canPaste_ || !paste::eligibleForAutoPaste(*entry, field()))) {
        status_->setText(QStringLiteral("Автовставка недоступна: проверьте получателя и выбранное поле.")); return;
    }
    operationId_ = entry->id; operationField_ = field(); modified_ = entry->modified;
    type_ = entry->type; reverify_ = entry->reverify; generation_ = app_.generation();
    phase_ = Phase::Verifying; updateActions();
    const auto action = ++action_;
    const bool verified = !reverify_ || VerifyDialog::verify(app_, this, copyOnly ? QStringLiteral("Копировать поле") : QStringLiteral("Вставить поле"), &checking_);
    if (action != action_) return;
    if (!verified) {
        fail(QStringLiteral("Подтверждение отменено.")); return;
    }
    if (!contextValid()) { fail(QStringLiteral("Запись или состояние хранилища изменились. Повторите выбор.")); return; }
    if (copyOnly) {
        const auto* value = paste::fieldValue(*app_.model()->entry(operationId_), operationField_);
        QString error;
        if (!value) { fail(QStringLiteral("Поле недоступно.")); return; }
        QString copy = *value;
        publishing_ = true;
        const bool published = clip_.copySecret(copy, &error); copy.clear(); publishing_ = false;
        if (action != action_) return;
        if (!published) { fail(error); return; }
        if (!contextValid()) clip_.clearIfOurs();
        cancel(); return;
    }
#ifdef Q_OS_WIN
    QString error;
    const HWND foreground = GetForegroundWindow();
    const HWND own = reinterpret_cast<HWND>(winId());
    if (!paste::recipientAllowed(recipient_, &error) || (foreground != recipient_.hwnd && foreground != own && GetAncestor(foreground, GA_ROOTOWNER) != own)) {
        fail(error.isEmpty() ? QStringLiteral("Получатель изменился. Ничего не скопировано.") : error); return;
    }
    phase_ = Phase::Restoring; hide(); paste::activateRecipient(recipient_);
    restoreBudget_.start(); restoreTimer_.start();
#endif
}
void MiniPanel::continuePaste() {
#ifdef Q_OS_WIN
    if (phase_ != Phase::Restoring) return;
    if (!contextValid() || !paste::recipientExists(recipient_)) { fail(QStringLiteral("Выдача отменена: запись или получатель изменились.")); return; }
    if (restoreBudget_.elapsed() >= 1500) { fail(QStringLiteral("Не удалось вернуть фокус или дождаться отпускания клавиш. Ничего не скопировано.")); return; }
    const HWND foreground = GetForegroundWindow(), own = reinterpret_cast<HWND>(winId());
    if (foreground && foreground != recipient_.hwnd && foreground != own && GetAncestor(foreground, GA_ROOTOWNER) != own) {
        fail(QStringLiteral("Активировано другое окно. Ничего не скопировано.")); return;
    }
    if (!paste::recipientActive(recipient_) || !paste::modifiersReleased()) return;
    QString error;
    if (!paste::recipientAllowed(recipient_, &error)) { fail(error); return; }
    restoreTimer_.stop();
    const auto* e = app_.model()->entry(operationId_);
    if (!e || !paste::eligibleForAutoPaste(*e, operationField_)) { fail(QStringLiteral("Поле изменилось или недоступно.")); return; }
    QString value = *paste::fieldValue(*e, operationField_);
    const auto action = action_; publishing_ = true;
    const bool published = clip_.copySecret(value, &error); value.clear(); publishing_ = false;
    if (action != action_) return;
    if (!published) { fail(error); return; }
    if (!contextValid() || !paste::recipientActive(recipient_) || !paste::modifiersReleased()) {
        clip_.clearIfOurs(); fail(QStringLiteral("Выдача отменена после публикации; своя публикация снята либо ожидает очистки.")); return;
    }
    const UINT count = paste::sendCtrlV();
    if (count == 4) { cancel(); return; }
    clip_.clearIfOurs();
    fail(count ? QStringLiteral("Передана только часть нажатий. Результат неизвестен; проверьте клавиши Ctrl/V. Автоповтора нет.")
               : QStringLiteral("Windows не приняла нажатия. Автоповтора и автоматического копирования нет."));
#endif
}
}
