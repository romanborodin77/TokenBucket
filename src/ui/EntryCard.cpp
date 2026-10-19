#include "EntryCard.h"
#include "VerifyDialog.h"
#include <QApplication>
#include <QEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace tb::ui {
EntryCard::EntryCard(Application& app, QWidget* parent) : QFrame(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    title_ = new QLabel(this); metadata_ = new QLabel(this); error_ = new QLabel(this);
    for (auto* label : {title_, metadata_, error_}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    layout->addWidget(title_); layout->addWidget(metadata_);
    value_ = new QPlainTextEdit(this); value_->setObjectName(QStringLiteral("entryDetails")); value_->setReadOnly(true);
    value_->setTextInteractionFlags(Qt::NoTextInteraction); value_->setFocusPolicy(Qt::NoFocus); value_->setContextMenuPolicy(Qt::NoContextMenu);
    value_->setAccessibleName(QStringLiteral("Секретное значение")); layout->addWidget(value_, 1);
    reveal_ = new QPushButton(QStringLiteral("Показать на 15 секунд"), this);
    copySecret_ = new QPushButton(QStringLiteral("Копировать секрет"), this); copySecret_->setObjectName(QStringLiteral("copySecretButton"));
    copyLogin_ = new QPushButton(QStringLiteral("Копировать логин"), this);
    copyPhrase_ = new QPushButton(QStringLiteral("Копировать фразу ключа"), this);
    for (auto* button : {reveal_, copySecret_, copyLogin_, copyPhrase_}) layout->addWidget(button);
    layout->addWidget(error_);
    hideTimer_.setParent(this); hideTimer_.setObjectName(QStringLiteral("secretHideTimer")); hideTimer_.setSingleShot(true); hideTimer_.setInterval(15000);
    connect(&hideTimer_, &QTimer::timeout, this, &EntryCard::hideNowForTest);
    connect(reveal_, &QPushButton::clicked, this, [this] {
        if (!authorize()) return; const auto* entry = app_.model()->entry(id_); if (!entry) return;
        value_->setPlainText(entry->type == model::EntryType::Credentials ? QStringLiteral("Логин: %1\nПароль: %2").arg(entry->login, entry->password) : entry->token);
        hideTimer_.start(15000);
    });
    connect(copySecret_, &QPushButton::clicked, this, [this] { copyField(0); });
    connect(copyLogin_, &QPushButton::clicked, this, [this] { copyField(1); });
    connect(copyPhrase_, &QPushButton::clicked, this, [this] { copyField(2); });
    connect(&app_, &Application::busyChanged, this, &EntryCard::updateEnabled);
    connect(&app_, &Application::stateChanged, this, [this] { if (app_.gate() != Application::Gate::Ready) clear(); else updateEnabled(); });
    qApp->installEventFilter(this); clear();
}
void EntryCard::updateEnabled() {
    const bool allowed = app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id_);
    for (auto* button : {reveal_, copySecret_, copyLogin_, copyPhrase_}) button->setEnabled(allowed);
}
void EntryCard::hideNowForTest() { hideTimer_.stop(); value_->setPlainText(QStringLiteral("••••••••")); }
void EntryCard::clear() { ++selection_; id_ = {}; hideNowForTest(); title_->clear(); metadata_->clear(); error_->clear(); updateEnabled(); }
void EntryCard::showEntry(model::Id id) {
    ++selection_; id_ = id; hideNowForTest(); error_->clear();
    const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(id) : nullptr;
    if (!entry) { clear(); return; }
    title_->setText(entry->title); metadata_->setText(entry->url + QStringLiteral("\n") + entry->note + QStringLiteral("\n") + entry->sshComment);
    const bool ssh = entry->type == model::EntryType::SshKey;
    reveal_->setVisible(!ssh); copySecret_->setText(ssh ? QStringLiteral("Копировать приватный ключ") : entry->type == model::EntryType::Credentials ? QStringLiteral("Копировать пароль") : QStringLiteral("Копировать токен"));
    copyLogin_->setVisible(entry->type == model::EntryType::Credentials); copyPhrase_->setVisible(ssh && !entry->sshPassphrase.isEmpty()); updateEnabled();
}
QString EntryCard::secretText() const { return value_->toPlainText(); }
bool EntryCard::authorize() {
    if (app_.gate() != Application::Gate::Ready || app_.busy()) return false;
    const auto* entry = app_.model()->entry(id_); if (!entry) return false;
    const auto generation = app_.generation(), selection = selection_; const auto id = id_;
    if (entry->reverify && !VerifyDialog::verify(app_, this)) return false;
    return generation == app_.generation() && selection == selection_ && id == id_ && app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id_);
}
void EntryCard::copyField(int field) {
    if (!authorize()) return; const auto* entry = app_.model()->entry(id_); if (!entry) return;
    QString text;
    if (field == 1) text = entry->login;
    else if (field == 2) text = entry->sshPassphrase;
    else if (entry->type == model::EntryType::SshKey) {
        if (!entry->sshPrivate.isValidUtf8()) { error_->setText(QStringLiteral("Ключ не является UTF-8. Для точной выдачи нужен файл.")); return; }
        text = QString::fromUtf8(entry->sshPrivate);
    } else text = entry->type == model::EntryType::ApiToken ? entry->token : entry->password;
    if (text.isEmpty()) { error_->setText(QStringLiteral("Пустое значение не копируется.")); return; }
    emit copyRequested(text);
}
bool EntryCard::eventFilter(QObject* object, QEvent* event) {
    if ((event->type() == QEvent::WindowDeactivate && object == window()) || event->type() == QEvent::ApplicationDeactivate || (event->type() == QEvent::Hide && object == this)) hideNowForTest();
    if (event->type() == QEvent::FocusIn) if (auto* widget = qobject_cast<QWidget*>(object); widget && widget != this && !isAncestorOf(widget)) hideNowForTest();
    return QFrame::eventFilter(object, event);
}
}
