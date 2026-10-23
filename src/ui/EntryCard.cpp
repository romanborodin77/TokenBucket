#include "EntryCard.h"
#include "VerifyDialog.h"
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include "../platform/FileOps.h"
namespace tb::ui {
EntryCard::EntryCard(Application& app, QWidget* parent) : QFrame(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16); layout->setSpacing(12);
    title_ = new QLabel(this); title_->setObjectName(QStringLiteral("cardTitle"));
    identity_ = new QLabel(this); identity_->setObjectName(QStringLiteral("cardIdentity"));
    flags_ = new QLabel(this); flags_->setObjectName(QStringLiteral("cardFlags"));
    metadata_ = new QLabel(this); metadata_->setObjectName(QStringLiteral("cardMetadata")); error_ = new QLabel(this);
    for (auto* label : {title_, identity_, flags_, metadata_, error_}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    auto titleFont = title_->font(); titleFont.setPointSize(titleFont.pointSize() + 5); titleFont.setBold(true); title_->setFont(titleFont);
    auto* heading = new QHBoxLayout; heading->addWidget(title_, 1);
    edit_ = new QPushButton(QStringLiteral("Изменить"), this); edit_->setObjectName(QStringLiteral("cardEdit")); heading->addWidget(edit_);
    layout->addLayout(heading); layout->addWidget(identity_); layout->addWidget(flags_);
    layout->addWidget(new QLabel(QStringLiteral("СЕКРЕТ"), this));
    value_ = new QPlainTextEdit(this); value_->setObjectName(QStringLiteral("entryDetails")); value_->setReadOnly(true);
    value_->setTextInteractionFlags(Qt::NoTextInteraction); value_->setFocusPolicy(Qt::NoFocus); value_->setContextMenuPolicy(Qt::NoContextMenu);
    value_->setAccessibleName(QStringLiteral("Секретное значение")); value_->setMinimumHeight(80); value_->setMaximumHeight(140); layout->addWidget(value_);
    reveal_ = new QPushButton(QStringLiteral("Показать на 15 секунд"), this);
    copySecret_ = new QPushButton(QStringLiteral("Копировать секрет"), this); copySecret_->setObjectName(QStringLiteral("copySecretButton"));
    copyLogin_ = new QPushButton(QStringLiteral("Копировать логин"), this);
    copyPhrase_ = new QPushButton(QStringLiteral("Копировать фразу ключа"), this);
    sshToFile_ = new QPushButton(QStringLiteral("Сохранить ключ в файл…"), this); sshToFile_->setObjectName(QStringLiteral("sshToFileButton"));
    more_ = new QPushButton(QStringLiteral("Дополнительно…"), this); more_->setObjectName(QStringLiteral("cardMore"));
    for (auto* button : {reveal_, copySecret_, copyLogin_, copyPhrase_, sshToFile_}) layout->addWidget(button);
    layout->addWidget(more_); layout->addSpacing(10); layout->addWidget(metadata_); layout->addStretch(1);
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
    connect(sshToFile_, &QPushButton::clicked, this, &EntryCard::saveSshToFile);
    connect(edit_, &QPushButton::clicked, this, &EntryCard::editRequested);
    connect(more_, &QPushButton::clicked, this, &EntryCard::moreRequested);
    connect(&app_, &Application::busyChanged, this, &EntryCard::updateEnabled);
    // Инвариант: модель может быть очищена из QtConcurrent-потока (lock) — указатели на Entry
    // не живут через диалоги/циклы событий.
    connect(&app_, &Application::stateChanged, this, [this] { if (app_.gate() != Application::Gate::Ready) clear(); else updateEnabled(); });
    applyAccent(); qApp->installEventFilter(this); clear();
}
void EntryCard::updateEnabled() {
    const bool allowed = app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id_);
    for (auto* button : {reveal_, copySecret_, copyLogin_, copyPhrase_, sshToFile_, edit_, more_}) button->setEnabled(allowed);
}
void EntryCard::hideNowForTest() { hideTimer_.stop(); value_->setPlainText(QStringLiteral("••••••••")); }
void EntryCard::clear() { ++selection_; id_ = {}; hideNowForTest(); title_->clear(); identity_->clear(); flags_->clear(); metadata_->clear(); error_->clear(); updateEnabled(); }
void EntryCard::showEntry(model::Id id) {
    ++selection_; id_ = id; hideNowForTest(); error_->clear();
    const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(id) : nullptr;
    if (!entry) { clear(); return; }
    const QString expiryNote = entry->expiresAt.isValid()
        ? QStringLiteral(" · действителен до %1%2").arg(
              entry->expiresAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy")),
              QDateTime::currentDateTimeUtc() > entry->expiresAt ? QStringLiteral(" (просрочен)") : QString())
        : QString();
    title_->setText(entry->title); metadata_->setText(QStringLiteral("Заметки: %1\n\nАдрес: %2\n\nИзменено: %3%4\n%5").arg(
        entry->note, entry->url, entry->modified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm")), expiryNote, entry->sshComment));
    const bool ssh = entry->type == model::EntryType::SshKey;
    QStringList path;
    auto node = entry->groupId;
    for (int depth = 0; !node.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
        const auto it = app_.model()->nodes().constFind(node);
        if (it == app_.model()->nodes().cend()) break;
        path.prepend(it->name); node = it->parent;
    }
    identity_->setText(QStringLiteral("%1 · %2").arg(ssh ? QStringLiteral("SSH-ключ")
        : entry->type == model::EntryType::Credentials ? QStringLiteral("Учётные данные") : QStringLiteral("API-токен"), path.join(QStringLiteral(" / "))));
    QStringList flags;
    if (entry->favorite) flags << QStringLiteral("★ Избранное");
    if (entry->reverify) flags << QStringLiteral("Повторная проверка");
    if (!entry->tags.isEmpty()) flags << entry->tags.join(QStringLiteral(", "));
    flags_->setText(flags.join(QStringLiteral(" · ")));
    reveal_->setVisible(!ssh); copySecret_->setText(ssh ? QStringLiteral("Копировать приватный ключ") : entry->type == model::EntryType::Credentials ? QStringLiteral("Копировать пароль") : QStringLiteral("Копировать токен"));
    copyLogin_->setVisible(entry->type == model::EntryType::Credentials); copyPhrase_->setVisible(ssh && !entry->sshPassphrase.isEmpty());
    sshToFile_->setVisible(ssh && !entry->sshPrivate.isEmpty()); updateEnabled();
}
QString EntryCard::secretText() const { return value_->toPlainText(); }
void EntryCard::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (copySecret_ && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)) applyAccent();
}
void EntryCard::applyAccent() {
    auto p = qApp->palette();
    p.setColor(QPalette::Active, QPalette::Button, p.color(QPalette::Highlight));
    p.setColor(QPalette::Inactive, QPalette::Button, p.color(QPalette::Highlight));
    p.setColor(QPalette::Active, QPalette::ButtonText, p.color(QPalette::HighlightedText));
    p.setColor(QPalette::Inactive, QPalette::ButtonText, p.color(QPalette::HighlightedText));
    copySecret_->setPalette(p);
}
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
bool EntryCard::isSafeTarget(const QString& path, QString* err) {
    const QFileInfo info(path);
    if (info.isSymbolicLink() || info.isShortcut()) {
        if (err) *err = QStringLiteral("Запись по символической ссылке запрещена.");
        return false;
    }
    return true;
}
void EntryCard::saveSshToFile() {
    if (!authorize()) return;
    const auto* entry = app_.model()->entry(id_);
    if (!entry || entry->type != model::EntryType::SshKey || entry->sshPrivate.isEmpty()) return;
    // Копия до диалогов: дальше warning/QFileDialog/question крутят вложенный цикл событий, за который
    // QtConcurrent-операция или автолок могут очистить модель — указатель entry стал бы висячим,
    // поэтому работаем только с локальной копией и после каждого диалога перепроверяем актуальность
    // (страж как в authorize: поколение/выбор не сменились, шлюз готов, запись всё ещё существует).
    const QByteArray key = entry->sshPrivate;
    const auto generation = app_.generation(), selection = selection_; const auto id = id_;
    const auto stillValid = [&] {
        return generation == app_.generation() && selection == selection_ && id == id_
            && app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id);
    };
    if (QMessageBox::warning(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Приватный ключ будет сохранён за пределами хранилища в незашифрованном виде. Продолжить?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!stillValid()) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранение приватного ключа"));
    if (path.isEmpty() || !stillValid()) return;
    QString error;
    const QString temporary = path + QStringLiteral(".tb-new");
    if (!isSafeTarget(path, &error) || !isSafeTarget(temporary, &error)) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return;
    }
    bool written = false;
    if (QFile::exists(path)) {
        if (QMessageBox::question(this, QStringLiteral("TokenBucket"), QStringLiteral("Файл существует. Перезаписать?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        if (!stillValid()) return;
        if (QFile::exists(temporary) && !store::realFileOps().removeFile(temporary)) {
            QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось подготовить замену файла.")); return;
        }
        if ((written = store::realFileOps().writeAll(temporary, key))
            && !(written = store::realFileOps().renameReplace(temporary, path)))
            store::realFileOps().removeFile(temporary);
    } else written = store::realFileOps().writeAll(path, key);
    if (!written) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось записать файл ключа.")); return;
    }
    error_->clear();
}
bool EntryCard::eventFilter(QObject* object, QEvent* event) {
    if ((event->type() == QEvent::WindowDeactivate && object == window()) || event->type() == QEvent::ApplicationDeactivate || (event->type() == QEvent::Hide && object == this)) hideNowForTest();
    if (event->type() == QEvent::FocusIn) if (auto* widget = qobject_cast<QWidget*>(object); widget && widget != this && !isAncestorOf(widget)) hideNowForTest();
    return QFrame::eventFilter(object, event);
}
}
