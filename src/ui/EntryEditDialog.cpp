#include "EntryEditDialog.h"
#include "EntryLabels.h"
#include "../vault/Crypto.h"
#include "../model/TextFile.h"
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
#include <QMessageBox>
#include <QKeyEvent>
#include <QMimeData>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScreen>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <sodium.h>
#include <algorithm>
#include <QUrl>
using namespace tb::model;
namespace {
class FileBodyEditor : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
protected:
    QMimeData* createMimeDataFromSelection() const override {return new QMimeData;}
};
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
bool confirmChange(QWidget* parent,const QString& title,const QString& message) {
    QMessageBox dialog(QMessageBox::Warning,title,message,QMessageBox::Yes|QMessageBox::No,parent);
    dialog.setTextFormat(Qt::PlainText); dialog.setDefaultButton(QMessageBox::No); dialog.setEscapeButton(QMessageBox::No);
    return dialog.exec()==QMessageBox::Yes;
}
int payloadCount(const Entry& e) {
    return !e.token.isEmpty()+!e.password.isEmpty()+!e.login.isEmpty()+!e.sshPrivate.isEmpty()+!e.sshPublic.isEmpty()
        +!e.sshPassphrase.isEmpty()+!e.sshComment.isEmpty()+!e.plainValue.isEmpty()+!e.textFile.bytes.isEmpty();
}
}
EntryEditDialog::EntryEditDialog(const VaultModel& model, std::optional<Entry> existing, QWidget* parent)
    : QDialog(parent), model_(model), original_(existing.value_or(Entry{})) {
    setWindowTitle(existing ? QStringLiteral("Изменение записи") : QStringLiteral("Новая запись"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12); layout->setSpacing(8);
    fields_ = new QWidget(this); auto* fields = new QVBoxLayout(fields_); fields->setContentsMargins(0,0,0,0); fields->setSpacing(8);
    fields->setSizeConstraint(QLayout::SetMinAndMaxSize);
    auto* form = new QFormLayout;
    type_ = new QComboBox(fields_); type_->setObjectName(QStringLiteral("entryType"));
    for(int i=0;i<=int(EntryType::TextFile);++i) type_->addItem(tb::ui::entryTypeLabel(EntryType(i)),i);
    type_->setEnabled(!original_.textFile.sourceSnapshot);
    group_ = new QComboBox(fields_); group_->setObjectName(QStringLiteral("entryGroup")); tb::ui::populateGroups(*group_, model);
    title_ = line(fields_, "entryTitle"); url_ = line(fields_, "entryUrl"); note_ = new QPlainTextEdit(fields_);
    note_->setObjectName(QStringLiteral("entryNote")); note_->setFixedHeight(note_->fontMetrics().lineSpacing() * 3 + 12);
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
    auto* pages = new QWidget(fields_); auto* pageLayout = new QVBoxLayout(pages); pageLayout->setContentsMargins(0,0,0,0);
    token_ = line(pages, "entryToken", true); auto* tokenPage = new QWidget(pages); auto* tokenForm = new QFormLayout(tokenPage);
    tokenForm->addRow(QStringLiteral("Т&окен:"), token_); pageLayout->addWidget(tokenPage);
    auto* credentials = new QWidget(pages); auto* credentialsForm = new QFormLayout(credentials);
    login_ = line(credentials, "entryLogin"); password_ = line(credentials, "entryPassword", true);
    auto* passwordRow = new QWidget(credentials); auto* passwordLayout = new QHBoxLayout(passwordRow);
    passwordLayout->setContentsMargins(0, 0, 0, 0); passwordLayout->addWidget(password_);
    auto* generate = new QPushButton(QStringLiteral("Сгенерировать"), passwordRow); generate->setObjectName(QStringLiteral("generatePassword"));
    generate->setToolTip(QStringLiteral("Случайный пароль из 24 символов без похожих знаков (0/O, 1/l/I)"));
    passwordLayout->addWidget(generate);
    credentialsForm->addRow(QStringLiteral("&Логин:"), login_); credentialsForm->addRow(QStringLiteral("П&ароль:"), passwordRow); pageLayout->addWidget(credentials);
    auto* ssh = new QWidget(pages); auto* sshForm = new QFormLayout(ssh);
    sshPath_ = line(ssh, "sshPrivatePath"); sshPath_->setPlaceholderText(existing ? QStringLiteral("Пусто — сохранить прежний ключ") : QStringLiteral("Выберите файл приватного ключа"));
    sshPath_->setAccessibleName(QStringLiteral("Файл приватного ключа"));
    auto* browse = new QPushButton(QStringLiteral("Выбрать файл…"), ssh);
    browse->setObjectName(QStringLiteral("browseSshKey"));
    auto* keyRow = new QWidget(ssh); auto* keyLayout = new QHBoxLayout(keyRow); keyLayout->setContentsMargins(0,0,0,0);
    keyLayout->addWidget(sshPath_, 1); keyLayout->addWidget(browse);
    sshPublic_ = new QPlainTextEdit(ssh); sshPublic_->setObjectName(QStringLiteral("sshPublic")); sshPublic_->setFixedHeight(sshPublic_->fontMetrics().lineSpacing() * 3 + 12);
    sshPass_ = line(ssh, "sshPassphrase", true); sshComment_ = line(ssh, "sshComment");
    sshForm->addRow(QStringLiteral("Файл п&риватного ключа:"), keyRow);
    qobject_cast<QLabel*>(sshForm->labelForField(keyRow))->setBuddy(sshPath_);
    sshForm->addRow(QStringLiteral("Публичный ключ:"), sshPublic_);
    sshForm->addRow(QStringLiteral("Парольная фраза:"), sshPass_); sshForm->addRow(QStringLiteral("Описание ключа:"), sshComment_);
    pageLayout->addWidget(ssh);
    auto* plain=new QWidget(pages); auto* plainForm=new QFormLayout(plain);
    parameter_=line(plain,"entryParameterName"); parameter_->setMaxLength(128); parameter_->setAccessibleName(QStringLiteral("Имя параметра"));
    plainValue_=line(plain,"entryPlainValue"); plainValue_->setMaxLength(int(VaultModel::MaxSecretBytes)); plainValue_->setAccessibleName(QStringLiteral("Простое значение"));
    valueFormat_=new QComboBox(plain); valueFormat_->setObjectName("entryValueFormat"); valueFormat_->setAccessibleName(QStringLiteral("Формат значения"));
    valueFormat_->addItem(QStringLiteral("Текст"),int(ValueFormat::Text)); valueFormat_->addItem(QStringLiteral("IP-адрес"),int(ValueFormat::Ip)); valueFormat_->addItem(QStringLiteral("URL"),int(ValueFormat::Url));
    plainForm->addRow(QStringLiteral("Имя параметра:"),parameter_); plainForm->addRow(QStringLiteral("Значение:"),plainValue_); plainForm->addRow(QStringLiteral("Формат:"),valueFormat_);
    pageLayout->addWidget(plain);
    auto* file=new QWidget(pages); auto* fileForm=new QFormLayout(file);
    fileName_=line(file,"entryTextFileName"); fileName_->setMaxLength(255); fileName_->setAccessibleName(QStringLiteral("Имя текстового файла"));
    fileBody_=new FileBodyEditor(file); fileBody_->setObjectName("entryTextFileBody"); fileBody_->setAccessibleName(QStringLiteral("Содержимое текстового файла"));
    fileBody_->installEventFilter(this); fileBody_->setContextMenuPolicy(Qt::NoContextMenu);
    fileBody_->setInputMethodHints(Qt::ImhSensitiveData|Qt::ImhNoPredictiveText|Qt::ImhNoAutoUppercase);
    fileBody_->setFixedHeight(fileBody_->fontMetrics().lineSpacing()*4+12);
    editBody_=new QPushButton(original_.textFile.sourceSnapshot?QStringLiteral("Редактировать копию…"):QStringLiteral("Открыть тело для изменения…"),file);
    editBody_->setObjectName("editTextFileBody");
    auto* fileInfo=new QLabel(QStringLiteral("UTF-8; до правки байты сохраняются точно. После правки — LF, BOM сохраняется, видимость сбрасывается. Вставка: Ctrl+V; копирование — из просмотра строк."),file);
    fileInfo->setTextFormat(Qt::PlainText); fileInfo->setWordWrap(true);
    fileForm->addRow(QStringLiteral("Имя файла:"),fileName_); fileForm->addRow(QStringLiteral("Тело файла:"),fileBody_); fileForm->addRow(editBody_); fileForm->addRow(fileInfo);
    pageLayout->addWidget(file); fields->addWidget(pages);
    auto* options = new QFormLayout;
    for (auto* layout : {form, tokenForm, credentialsForm, sshForm, plainForm,fileForm,options}) {
        layout->setRowWrapPolicy(QFormLayout::WrapLongRows); layout->setContentsMargins(0,0,0,0); layout->setVerticalSpacing(6);
    }
    favorite_ = new QCheckBox(QStringLiteral("Избранное"), fields_); favorite_->setObjectName(QStringLiteral("entryFavorite"));
    reverify_ = new QCheckBox(QStringLiteral("Повторная проверка при показе и копировании"), fields_); reverify_->setObjectName(QStringLiteral("entryReverify"));
    connect(reverify_,&QCheckBox::toggled,this,[this](bool protectedValue) {
        plainValue_->setEchoMode(protectedValue?QLineEdit::Password:QLineEdit::Normal);
        plainValue_->setInputMethodHints(protectedValue?Qt::ImhSensitiveData|Qt::ImhNoPredictiveText|Qt::ImhNoAutoUppercase:Qt::ImhNone);
    });
    options->addRow(favorite_, reverify_); fields->addLayout(options);
    auto* scroll = new QScrollArea(this); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setWidget(fields_); layout->addWidget(scroll, 1);
    error_ = new QLabel(this); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); error_->setObjectName(QStringLiteral("entryError")); error_->hide(); layout->addWidget(error_);
    buttons_ = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить")); buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена")); layout->addWidget(buttons_);
    connect(buttons_, &QDialogButtonBox::accepted, this, &EntryEditDialog::accept);
    connect(buttons_, &QDialogButtonBox::rejected, this, &EntryEditDialog::reject);
    const auto showType = [this, pages, tokenPage, credentials, ssh,plain,file](int) {
        if(canceled_ || !type_->currentData().isValid()) return;
        const auto type=EntryType(type_->currentData().toInt());
        const auto revert=[&] {const QSignalBlocker block(type_); type_->setCurrentIndex(type_->findData(int(shownType_)));};
        if(type!=original_.type && !original_.id.isNull()
            && (payloadCount(original_)>1 || original_.type==EntryType::TextFile || type==EntryType::TextFile)) {
            showError(QStringLiteral("Эта смена типа потеряет полезные поля. Создайте отдельную запись; исходная останется без изменений.")); revert(); return;
        }
        QString source;
        if(shownType_==EntryType::ApiToken) source=token_->text();
        else if(shownType_==EntryType::Credentials) source=password_->text().isEmpty()?login_->text():password_->text();
        else if(shownType_==EntryType::PlainValue) source=plainValue_->text();
        if(type==EntryType::PlainValue && shownType_!=type && (!original_.id.isNull() || !source.isEmpty())) {
            if(source.contains('\r') || source.contains('\n') || source.contains(QChar(0)) || source.contains(QChar(0x2028)) || source.contains(QChar(0x2029))) {
                showError(QStringLiteral("Многострочные данные сохраняйте текстовым файлом; автоматического сокращения нет.")); revert(); return;
            }
            if(!confirmChange(this,QStringLiteral("Сделать значение видимым"),QStringLiteral("Значение станет видимым после разблокировки и доступным поиску. Если включена повторная проверка, она продолжит скрывать значение. Продолжить?"))) {revert(); return;}
        }
        if(canceled_) return;
        if(type==EntryType::PlainValue && plainValue_->text().isEmpty()) plainValue_->setText(source);
        if(type==EntryType::ApiToken && token_->text().isEmpty()) token_->setText(source);
        if(type==EntryType::Credentials && password_->text().isEmpty()) password_->setText(source);
        shownType_=type;
        tokenPage->setVisible(type==EntryType::ApiToken); credentials->setVisible(type==EntryType::Credentials); ssh->setVisible(type==EntryType::SshKey);
        plain->setVisible(type==EntryType::PlainValue); file->setVisible(type==EntryType::TextFile);
        pages->layout()->activate(); pages->updateGeometry();
        fitToContents();
    };
    connect(type_, &QComboBox::currentIndexChanged, this, showType);
    connect(editBody_,&QPushButton::clicked,this,[this] {
        const auto message=original_.textFile.sourceSnapshot
            ?QStringLiteral("Открыть всё содержимое, включая скрытые строки, и редактировать отдельную копию с новым UUID? Исходный снимок не изменится.")
            :QStringLiteral("Открыть всё содержимое, включая скрытые строки, в редакторе? При изменении тела окончания строк станут LF, а разрешённая видимость строк будет сброшена.");
        if(!confirmChange(this,QStringLiteral("Редактирование текстового файла"),message) || canceled_) return;
        editingCopy_=original_.textFile.sourceSnapshot;
        if(editingCopy_) title_->setText(original_.title+QStringLiteral(" — копия"));
        fileBody_->setPlainText(QString::fromUtf8(original_.textFile.bytes)); bodyBaseline_=fileBody_->document()->toRawText().replace(QChar(0x2029),'\n');
        fileBody_->document()->setModified(false); fileBody_->setReadOnly(false); bodyLoaded_=true; editBody_->hide(); fitToContents();
    });
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
    shownType_=original_.type; type_->setCurrentIndex(type_->findData(int(original_.type)));
    if (existing) setGroup(original_.groupId);
    title_->setText(original_.title); url_->setText(original_.url); note_->setPlainText(original_.note);
    tags_->setText(original_.tags.join(QStringLiteral(", ")));
    token_->setText(original_.token); login_->setText(original_.login); password_->setText(original_.password);
    sshPass_->setText(original_.sshPassphrase); sshComment_->setText(original_.sshComment);
    sshPublic_->setPlainText(QString::fromUtf8(original_.sshPublic)); sshPublic_->document()->setModified(false);
    parameter_->setText(original_.parameterName); plainValue_->setText(original_.plainValue); valueFormat_->setCurrentIndex(valueFormat_->findData(int(original_.valueFormat)));
    fileName_->setText(original_.textFile.fileName.isEmpty()?QStringLiteral("document.txt"):original_.textFile.fileName);
    const bool storedFile=existing && original_.type==EntryType::TextFile;
    fileBody_->setReadOnly(storedFile); fileBody_->setPlaceholderText(storedFile?QStringLiteral("Содержимое скрыто. Откройте тело отдельно для изменения."):QString()); editBody_->setVisible(storedFile);
    favorite_->setChecked(original_.favorite); reverify_->setChecked(original_.reverify);
    if (original_.expiresAt.isValid()) { expiry_->setChecked(true); expiryDate_->setDate(original_.expiresAt.date()); }
    showType(type_->currentIndex()); title_->setFocus();
}
void EntryEditDialog::fitToContents() {
    const auto available = screen()->availableGeometry();
    const int targetWidth = qMin(isVisible() ? width() : 580, available.width() - 40);
    const auto margins = layout()->contentsMargins();
    const int contentWidth = targetWidth - margins.left() - margins.right();
    fields_->layout()->activate();
    int contentHeight = fields_->heightForWidth(contentWidth);
    if (contentHeight < 0) contentHeight = fields_->sizeHint().height();
    int height = contentHeight + margins.top() + margins.bottom() + buttons_->sizeHint().height() + layout()->spacing();
    if (!error_->isHidden()) height += error_->heightForWidth(contentWidth) + layout()->spacing();
    resize(targetWidth, qMin(height, available.height() - 80));
    if (isVisible()) {
        const auto frame = frameGeometry();
        move(qBound(available.left(), frame.left(), available.right() - frame.width() + 1),
             qBound(available.top(), frame.top(), available.bottom() - frame.height() + 1));
    }
}
void EntryEditDialog::showError(const QString& message) { error_->setText(message); error_->show(); fitToContents(); }
EntryEditDialog::~EntryEditDialog() { clearSecrets(); }
bool EntryEditDialog::eventFilter(QObject* object,QEvent* event) {
    if(object==fileBody_ && (event->type()==QEvent::KeyPress || event->type()==QEvent::ShortcutOverride)) {
        const auto* key=static_cast<QKeyEvent*>(event);
        if(key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut)) {
            event->accept();
            if(event->type()==QEvent::KeyPress) showError(QStringLiteral("Для выдачи строк или целого файла используйте просмотр документа. В редакторе работают вставка и удаление."));
            return true;
        }
    }
    return QDialog::eventFilter(object,event);
}
void EntryEditDialog::setGroup(Id id) { group_->setCurrentIndex(id.isNull() ? -1 : group_->findData(id)); }
void EntryEditDialog::clearSecrets() {
    token_->clear(); password_->clear(); sshPass_->clear(); plainValue_->clear(); fileBody_->clear(); bodyBaseline_.clear();
    login_->clear(); note_->clear(); sshPublic_->clear(); original_ = Entry{}; result_.reset();
}
void EntryEditDialog::reject() { canceled_ = true; clearSecrets(); QDialog::reject(); }
void EntryEditDialog::accept() {
    if (reading_ || canceled_) return;
    if(!type_->currentData().isValid()) {showError(QStringLiteral("Выберите тип записи.")); return;}
    const auto type=EntryType(type_->currentData().toInt());
    Entry e=type==original_.type?original_:Entry{};
    e.id = original_.id.isNull() || editingCopy_ ? QUuid::createUuid() : original_.id; e.created = editingCopy_?QDateTime():original_.created;
    e.sourceEntryId=original_.sourceEntryId; e.sourceLine=original_.sourceLine;
    e.type=type; e.groupId = group_->currentData().value<Id>();
    e.title = title_->text(); e.url = url_->text(); e.note = note_->toPlainText();
    e.favorite = favorite_->isChecked(); e.reverify = reverify_->isChecked();
    e.expiresAt = expiry_->isChecked()
        ? QDateTime(expiryDate_->date(), QTime(0, 0), Qt::UTC) : QDateTime();
    // Нормализация и лимиты тегов — зона ответственности модели: UI только делит
    // текст по запятым, а пробный validation.setEntry ниже покажет нарушение
    // (больше 16 тегов / длиннее 32 символов) в entryError без дублирования правил.
    e.tags = VaultModel::normalizeTags(tags_->text().split(QLatin1Char(',')));
    if (e.title.isEmpty()) { showError(QStringLiteral("Название обязательно.")); return; }
    if (e.type == EntryType::ApiToken) e.token = token_->text();
    else if (e.type == EntryType::Credentials) { e.login = login_->text(); e.password = password_->text(); }
    else if(e.type==EntryType::SshKey) {
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
            if (!read.error.isEmpty()) { showError(read.error); return; }
            e.sshPrivate = read.bytes;
        }
        if (e.sshPrivate.isEmpty()) { showError(QStringLiteral("Выберите файл приватного ключа.")); return; }
    } else if(e.type==EntryType::PlainValue) {
        e.parameterName=parameter_->text(); e.plainValue=plainValue_->text(); e.valueFormat=ValueFormat(valueFormat_->currentData().toInt());
    } else if(e.type==EntryType::TextFile) {
        e.textFile.fileName=fileName_->text();
        const auto edited=fileBody_->document()->toRawText().replace(QChar(0x2029),'\n');
        if(original_.id.isNull() || (bodyLoaded_ && edited!=bodyBaseline_)) {
            if(!original_.textFile.visibleLines.isEmpty() && !confirmChange(this,QStringLiteral("Сбросить видимость строк"),
                QStringLiteral("Тело изменилось. Окончания строк станут LF, все строки будут скрыты до новой классификации. Сохранить изменение?"))) return;
            if(canceled_) return;
            e.textFile.bytes=edited.toUtf8();
            if(original_.textFile.bytes.startsWith("\xef\xbb\xbf")) e.textFile.bytes.prepend("\xef\xbb\xbf");
            e.textFile.visibleLines.clear();
        }
        if(editingCopy_) e.textFile.sourceSnapshot=false;
    } else {showError(QStringLiteral("Неизвестный тип записи.")); return;
    }
    auto validation = model_; const auto error = validation.setEntry(e);
    if (!error.isEmpty()) { showError(error); return; }
    if(e.type==EntryType::PlainValue && e.valueFormat==ValueFormat::Url) {
        const QUrl url(e.plainValue,QUrl::StrictMode);
        if((!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
            && !confirmChange(this,QStringLiteral("URL может содержать секрет"),QStringLiteral("В адресе есть учётные данные, параметры или фрагмент. Рекомендуется хранить такой адрес как API-токен. Оставить простым значением?"))) return;
        if(canceled_) return;
    }
    e.modified = QDateTime::currentDateTimeUtc(); if (!e.created.isValid()) e.created = e.modified;
    result_ = std::move(e); QDialog::accept();
}
