#include "EntryEditDialog.h"
#include "../vault/Crypto.h"
#include <QCheckBox>
#include <QDateEdit>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStackedWidget>
#include <QScrollArea>
#include <QScreen>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <sodium.h>
#include <algorithm>
using namespace tb::model;
namespace {
// Автодополнение последнего незавершённого тега после запятой.
class TagCompleter : public QCompleter {
public:
    using QCompleter::QCompleter;
protected:
    // Стандартный QCompleter дополняет текст поля целиком, а в поле тегов
    // кандидаты должны подбираться только по фрагменту после последней
    // запятой — его и отдаём как «путь» для сопоставления.
    QStringList splitPath(const QString& path) const override {
        const int comma = path.lastIndexOf(QLatin1Char(','));
        return {path.mid(comma + 1).trimmed()};
    }
    // Подстановка не должна затирать уже введённые теги: собираем весь текст
    // поля заново — префикс до запятой, подобранный тег и хвост «, »,
    // чтобы сразу можно было вводить следующий тег.
    QString pathFromIndex(const QModelIndex& index) const override {
        const QString completion = index.data(Qt::DisplayRole).toString();
        const auto* edit = qobject_cast<const QLineEdit*>(widget());
        if (!edit) return completion;
        const QString text = edit->text();
        const int comma = text.lastIndexOf(QLatin1Char(','));
        return (comma >= 0 ? text.left(comma + 1) + QLatin1Char(' ') : QString()) + completion + QLatin1String(", ");
    }
};
}
namespace tb::ui {
void populateGroups(QComboBox& combo, const VaultModel& model) {
    QList<QPair<QString, Id>> groups;
    for (const auto& node : model.nodes()) {
        QStringList parts{node.name};
        for (Id parent = node.parent; !parent.isNull(); parent = model.nodes().value(parent).parent)
            parts.prepend(model.nodes().value(parent).name);
        groups.append({parts.join(QStringLiteral(" › ")), node.id});
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& group : groups) combo.addItem(group.first, group.second);
}
}
namespace {
struct ReadKey { QByteArray bytes; QString error; };
ReadKey readKey(const QString& path) {
    if (!QFileInfo(path).isFile()) return {{}, QStringLiteral("Выберите обычный файл приватного ключа.")};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {{}, QStringLiteral("Не удалось прочитать файл ключа.")};
    if (file.size() > VaultModel::MaxSecretBytes) return {{}, QStringLiteral("Файл ключа превышает 1 МиБ.")};
    QByteArray bytes = file.read(VaultModel::MaxSecretBytes + 1);
    if (file.error() != QFile::NoError) return {{}, QStringLiteral("Не удалось прочитать файл ключа.")};
    if (bytes.size() > VaultModel::MaxSecretBytes || !file.atEnd()) return {{}, QStringLiteral("Файл ключа превышает 1 МиБ.")};
    // Проверяются только границы контейнера; исходные байты не меняются.
    const QByteArray text = bytes.trimmed();
    for (const char* kind : {"OPENSSH PRIVATE KEY", "RSA PRIVATE KEY", "EC PRIVATE KEY", "DSA PRIVATE KEY", "PRIVATE KEY", "ENCRYPTED PRIVATE KEY"}) {
        const QByteArray begin = QByteArray("-----BEGIN ") + kind + "-----";
        const QByteArray end = QByteArray("-----END ") + kind + "-----";
        if ((text.startsWith(begin + '\n') || text.startsWith(begin + "\r\n")) && text.endsWith(end)) {
            const auto body = text.mid(begin.size(), text.size() - begin.size() - end.size()).trimmed();
            if (!body.isEmpty() && !body.contains("-----BEGIN ") && !body.contains("-----END ")) return {std::move(bytes), {}};
        }
    }
    return {{}, QStringLiteral("Поддерживаются контейнеры приватных ключей OpenSSH и PEM. PPK не поддерживается.")};
}
QLineEdit* line(QWidget* parent, const char* name, bool secret = false) {
    auto* field = new QLineEdit(parent); field->setObjectName(QString::fromLatin1(name));
    field->setMaxLength(int(secret ? VaultModel::MaxSecretBytes : VaultModel::MaxPlainBytesCBOR));
    if (secret) {
        field->setEchoMode(QLineEdit::Password);
        field->setInputMethodHints(Qt::ImhSensitiveData | Qt::ImhNoPredictiveText | Qt::ImhNoAutoUppercase);
    }
    return field;
}
}
EntryEditDialog::EntryEditDialog(const VaultModel& model, std::optional<Entry> existing, QWidget* parent)
    : QDialog(parent), model_(model), original_(existing.value_or(Entry{})) {
    setWindowTitle(existing ? QStringLiteral("Изменение записи") : QStringLiteral("Новая запись"));
    auto* layout = new QVBoxLayout(this);
    fields_ = new QWidget(this); auto* fields = new QVBoxLayout(fields_); fields->setContentsMargins(0,0,0,0);
    auto* form = new QFormLayout;
    type_ = new QComboBox(fields_); type_->setObjectName(QStringLiteral("entryType"));
    type_->addItems({QStringLiteral("API-токен"), QStringLiteral("Учётные данные"), QStringLiteral("SSH-ключ")});
    type_->setEnabled(!existing.has_value());
    group_ = new QComboBox(fields_); group_->setObjectName(QStringLiteral("entryGroup")); tb::ui::populateGroups(*group_, model);
    title_ = line(fields_, "entryTitle"); url_ = line(fields_, "entryUrl"); note_ = new QPlainTextEdit(fields_);
    note_->setObjectName(QStringLiteral("entryNote")); note_->setMaximumHeight(100);
    form->addRow(QStringLiteral("&Тип:"), type_); form->addRow(QStringLiteral("&Проект/группа:"), group_);
    form->addRow(QStringLiteral("&Название:"), title_); form->addRow(QStringLiteral("Адрес (&URL):"), url_);
    form->addRow(QStringLiteral("&Заметка:"), note_);
    // Срок действия: опционален; дата выбирается с календарём, хранится UTC-полночь.
    expiry_ = new QCheckBox(QStringLiteral("Действителен до"), fields_); expiry_->setObjectName(QStringLiteral("entryExpiry"));
    expiryDate_ = new QDateEdit(fields_); expiryDate_->setObjectName(QStringLiteral("entryExpiryDate"));
    expiryDate_->setCalendarPopup(true); expiryDate_->setDisplayFormat(QStringLiteral("dd.MM.yyyy"));
    expiryDate_->setDate(QDate::currentDate().addDays(30)); expiryDate_->setEnabled(false);
    connect(expiry_, &QCheckBox::toggled, expiryDate_, &QDateEdit::setEnabled);
    form->addRow(expiry_, expiryDate_);
    // Теги: свободный ввод через запятую; подсказки — все теги хранилища (allTags).
    tags_ = line(fields_, "entryTags"); tags_->setMaxLength(4096);
    tags_->setPlaceholderText(QStringLiteral("теги через запятую, например: прод, сервер"));
    tags_->setAccessibleName(QStringLiteral("Теги записи"));
    auto* tagCompleter = new TagCompleter(model.allTags(), tags_);
    tagCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    tagCompleter->setCompletionMode(QCompleter::InlineCompletion);
    tags_->setCompleter(tagCompleter);
    form->addRow(QStringLiteral("Т&еги:"), tags_);
    fields->addLayout(form);
    auto* pages = new QStackedWidget(fields_);
    token_ = line(pages, "entryToken", true); auto* tokenPage = new QWidget(pages); auto* tokenForm = new QFormLayout(tokenPage);
    tokenForm->addRow(QStringLiteral("Т&окен:"), token_); pages->addWidget(tokenPage);
    auto* credentials = new QWidget(pages); auto* credentialsForm = new QFormLayout(credentials);
    login_ = line(credentials, "entryLogin"); password_ = line(credentials, "entryPassword", true);
    auto* passwordRow = new QWidget(credentials); auto* passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0); passwordLayout->addWidget(password_);
    auto* generate = new QPushButton(QStringLiteral("Сгенерировать"), passwordRow); generate->setObjectName(QStringLiteral("generatePassword"));
    generate->setToolTip(QStringLiteral("Случайный пароль из 24 символов без похожих знаков (0/O, 1/l/I)"));
    passwordLayout->addWidget(generate);
    credentialsForm->addRow(QStringLiteral("&Логин:"), login_); credentialsForm->addRow(QStringLiteral("П&ароль:"), passwordRow); pages->addWidget(credentials);
    auto* ssh = new QWidget(pages); auto* sshForm = new QFormLayout(ssh);
    sshPath_ = line(ssh, "sshPrivatePath"); sshPath_->setPlaceholderText(existing ? QStringLiteral("Пусто — сохранить прежний ключ") : QStringLiteral("Выберите файл приватного ключа"));
    auto* browse = new QPushButton(QStringLiteral("Выбрать файл…"), ssh);
    sshPublic_ = new QPlainTextEdit(ssh); sshPublic_->setObjectName(QStringLiteral("sshPublic")); sshPublic_->setMaximumHeight(70);
    sshPass_ = line(ssh, "sshPassphrase", true); sshComment_ = line(ssh, "sshComment");
    sshForm->addRow(QStringLiteral("Файл п&риватного ключа:"), sshPath_); sshForm->addRow(browse);
    sshForm->addRow(QStringLiteral("Публичный ключ:"), sshPublic_);
    sshForm->addRow(QStringLiteral("Парольная фраза:"), sshPass_); sshForm->addRow(QStringLiteral("Описание ключа:"), sshComment_);
    pages->addWidget(ssh); fields->addWidget(pages);
    for (auto* layout : {form, tokenForm, credentialsForm, sshForm}) layout->setRowWrapPolicy(QFormLayout::WrapLongRows);
    favorite_ = new QCheckBox(QStringLiteral("Избранное"), fields_); favorite_->setObjectName(QStringLiteral("entryFavorite"));
    reverify_ = new QCheckBox(QStringLiteral("Повторная проверка при показе и копировании"), fields_); reverify_->setObjectName(QStringLiteral("entryReverify"));
    fields->addWidget(favorite_); fields->addWidget(reverify_);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(fields_); layout->addWidget(scroll, 1);
    error_ = new QLabel(this); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); error_->setObjectName(QStringLiteral("entryError")); layout->addWidget(error_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить")); buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена")); layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &EntryEditDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &EntryEditDialog::reject);
    connect(type_, &QComboBox::currentIndexChanged, pages, &QStackedWidget::setCurrentIndex);
    connect(browse, &QPushButton::clicked, this, [this] {
        const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("Приватный ключ SSH"));
        if (!path.isEmpty()) sshPath_->setText(path);
    });
    connect(generate, &QPushButton::clicked, this, [this] {
        tb::crypto::init();
        // Алфавит без похожих знаков (0/O, 1/l/I); 24 знака ≈ 140 бит энтропии.
        const QString alphabet = QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789#!%+=-");
        QString password;
        for (int i = 0; i < 24; ++i) password += alphabet.at(int(randombytes_uniform(uint(alphabet.size()))));
        password_->setText(password);
    });
    type_->setCurrentIndex(int(original_.type)); pages->setCurrentIndex(type_->currentIndex());
    if (existing) setGroup(original_.groupId);
    title_->setText(original_.title); url_->setText(original_.url); note_->setPlainText(original_.note);
    tags_->setText(original_.tags.join(QStringLiteral(", ")));
    token_->setText(original_.token); login_->setText(original_.login); password_->setText(original_.password);
    sshPass_->setText(original_.sshPassphrase); sshComment_->setText(original_.sshComment);
    sshPublic_->setPlainText(QString::fromUtf8(original_.sshPublic)); sshPublic_->document()->setModified(false);
    favorite_->setChecked(original_.favorite); reverify_->setChecked(original_.reverify);
    if (original_.expiresAt.isValid()) { expiry_->setChecked(true); expiryDate_->setDate(original_.expiresAt.date()); }
    const auto available = screen()->availableGeometry();
    resize(qMin(580, available.width() - 40), qMin(sizeHint().height(), available.height() - 80)); title_->setFocus();
}
EntryEditDialog::~EntryEditDialog() { clearSecrets(); }
void EntryEditDialog::setGroup(Id id) { group_->setCurrentIndex(id.isNull() ? -1 : group_->findData(id)); }
void EntryEditDialog::clearSecrets() {
    token_->clear(); password_->clear(); sshPass_->clear(); original_ = Entry{}; result_.reset();
}
void EntryEditDialog::reject() { canceled_ = true; clearSecrets(); QDialog::reject(); }
void EntryEditDialog::accept() {
    if (reading_ || canceled_) return;
    Entry e; e.id = original_.id.isNull() ? QUuid::createUuid() : original_.id; e.created = original_.created;
    e.type = EntryType(type_->currentIndex()); e.groupId = group_->currentData().value<Id>();
    e.title = title_->text(); e.url = url_->text(); e.note = note_->toPlainText();
    e.favorite = favorite_->isChecked(); e.reverify = reverify_->isChecked();
    e.expiresAt = expiry_->isChecked()
        ? QDateTime(expiryDate_->date(), QTime(0, 0), Qt::UTC) : QDateTime();
    // Нормализация и лимиты тегов — зона ответственности модели: UI только делит
    // текст по запятым, а пробный validation.setEntry ниже покажет нарушение
    // (больше 16 тегов / длиннее 32 символов) в entryError без дублирования правил.
    e.tags = VaultModel::normalizeTags(tags_->text().split(QLatin1Char(',')));
    if (e.title.isEmpty()) { error_->setText(QStringLiteral("Название обязательно.")); return; }
    if (e.type == EntryType::ApiToken) e.token = token_->text();
    else if (e.type == EntryType::Credentials) { e.login = login_->text(); e.password = password_->text(); }
    else {
        e.sshPrivate = original_.sshPrivate;
        e.sshPublic = sshPublic_->document()->isModified() ? sshPublic_->toPlainText().toUtf8() : original_.sshPublic;
        e.sshPassphrase = sshPass_->text(); e.sshComment = sshComment_->text();
        if (!sshPath_->text().isEmpty()) {
            QScopedValueRollback guard(reading_, true);
            fields_->setEnabled(false); buttons_->button(QDialogButtonBox::Save)->setEnabled(false);
            QEventLoop loop; QFutureWatcher<ReadKey> watcher;
            connect(&watcher, &QFutureWatcher<ReadKey>::finished, &loop, &QEventLoop::quit);
            watcher.setFuture(QtConcurrent::run(readKey, sshPath_->text()));
            if (!watcher.isFinished()) loop.exec();
            const auto read = watcher.result();
            fields_->setEnabled(true); buttons_->button(QDialogButtonBox::Save)->setEnabled(true);
            if (canceled_) return;
            if (!read.error.isEmpty()) { error_->setText(read.error); return; }
            e.sshPrivate = read.bytes;
        }
        if (e.sshPrivate.isEmpty()) { error_->setText(QStringLiteral("Выберите файл приватного ключа.")); return; }
    }
    auto validation = model_; const auto error = validation.setEntry(e);
    if (!error.isEmpty()) { error_->setText(error); return; }
    e.modified = QDateTime::currentDateTimeUtc(); if (!e.created.isValid()) e.created = e.modified;
    result_ = std::move(e); QDialog::accept();
}
