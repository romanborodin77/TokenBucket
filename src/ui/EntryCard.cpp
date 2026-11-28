#include "EntryCard.h"
#include "EntryLabels.h"
#include "EntryAccess.h"
#include "../model/TextFile.h"
#include <QApplication>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLayout>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QDesktopServices>
#include <QUrl>
#include <QKeyEvent>
#include <QMimeData>
#include <QStyle>
#include <QTextDocument>
#include "../platform/FileOps.h"
namespace tb::ui {
namespace {
class CardActions final : public QLayout {
public:
    explicit CardActions(QWidget* parent) : QLayout(parent) {
        setContentsMargins(0, 0, 0, 0);
        setSpacing(6);
    }
    ~CardActions() override { while (auto* item = takeAt(0)) delete item; }
    void addItem(QLayoutItem* item) override { items_.append(item); }
    int count() const override { return int(items_.size()); }
    QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override {
        return index >= 0 && index < count() ? items_.takeAt(index) : nullptr;
    }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override { return arrange(QRect(0, 0, width, 0), false); }
    QSize minimumSize() const override {
        QSize result(0, 0);
        for (const auto* item : items_) {
            if (!item->isEmpty()) result = result.expandedTo(item->sizeHint());
        }
        return result;
    }
    QSize sizeHint() const override { return minimumSize(); }
    void setGeometry(const QRect& rect) override {
        QLayout::setGeometry(rect);
        arrange(rect, true);
    }

private:
    int arrange(const QRect& rect, bool place) const {
        int x = 0, y = 0, rowHeight = 0;
        for (auto* item : items_) {
            if (item->isEmpty()) continue;
            const auto size = item->sizeHint();
            if (rowHeight && x + size.width() > rect.width()) {
                x = 0;
                y += rowHeight + spacing();
                rowHeight = 0;
            }
            if (place) {
                const QRect geometry(rect.topLeft() + QPoint(x, y), size);
                item->setGeometry(QStyle::visualRect(parentWidget()->layoutDirection(), rect, geometry));
            }
            x += size.width() + spacing();
            rowHeight = qMax(rowHeight, size.height());
        }
        return y + rowHeight;
    }

    QList<QLayoutItem*> items_;
};

class CardValue final : public QPlainTextEdit {
public:
    using QPlainTextEdit::QPlainTextEdit;
protected:
    QMimeData* createMimeDataFromSelection() const override { return new QMimeData; }
};
QUrl openableUrl(const model::Entry* e) {
    if(!e || e->type!=model::EntryType::PlainValue || e->valueFormat!=model::ValueFormat::Url) return {};
    const QUrl url(e->plainValue,QUrl::StrictMode);
    return url.isValid() && !url.host().isEmpty() && (url.scheme()=="http" || url.scheme()=="https")?url:QUrl();
}
}
EntryCard::EntryCard(Application& app, QWidget* parent) : QFrame(parent), app_(app) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 6, 8, 6); layout->setSpacing(6);
    title_ = new QLabel(this); title_->setObjectName(QStringLiteral("cardTitle"));
    identity_ = new QLabel(this); identity_->setObjectName(QStringLiteral("cardIdentity"));
    flags_ = new QLabel(this); flags_->setObjectName(QStringLiteral("cardFlags"));
    metadata_ = new QLabel(this); metadata_->setObjectName(QStringLiteral("cardMetadata")); error_ = new QLabel(this);
    sshTargets_ = new QLabel(this); sshTargets_->setObjectName(QStringLiteral("cardSshTargets"));
    for (auto* label : {title_, identity_, flags_, metadata_, sshTargets_, error_}) { label->setTextFormat(Qt::PlainText); label->setWordWrap(true); }
    sshTargets_->setTextInteractionFlags(Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard);
    auto titleFont = title_->font(); titleFont.setPointSize(titleFont.pointSize() + 3); titleFont.setBold(true); title_->setFont(titleFont);
    edit_ = new QPushButton(QStringLiteral("Изменить"), this); edit_->setObjectName(QStringLiteral("cardEdit"));
    layout->addWidget(title_); layout->addWidget(identity_); layout->addWidget(flags_);
    valueLabel_=new QLabel(QStringLiteral("СЕКРЕТ"),this); valueLabel_->setObjectName("cardValueLabel"); layout->addWidget(valueLabel_);
    value_ = new CardValue(this); value_->setObjectName(QStringLiteral("entryDetails")); value_->setReadOnly(true);
    value_->setTextInteractionFlags(Qt::NoTextInteraction); value_->setFocusPolicy(Qt::NoFocus); value_->setContextMenuPolicy(Qt::NoContextMenu);
    value_->setAccessibleName(QStringLiteral("Секретное значение"));
    updateValueHeight();
    layout->addWidget(value_);
    reveal_ = new QPushButton(QStringLiteral("Показать на 15 с"), this);
    reveal_->setToolTip(QStringLiteral("Показать на 15 секунд"));
    copySecret_ = new QPushButton(QStringLiteral("Копировать секрет"), this); copySecret_->setObjectName(QStringLiteral("copySecretButton"));
    copyLogin_ = new QPushButton(QStringLiteral("Копировать логин"), this);
    copyPhrase_ = new QPushButton(QStringLiteral("Копировать фразу ключа"), this);
    sshToFile_ = new QPushButton(QStringLiteral("Сохранить ключ в файл…"), this); sshToFile_->setObjectName(QStringLiteral("sshToFileButton"));
    more_ = new QPushButton(QStringLiteral("Дополнительно…"), this); more_->setObjectName(QStringLiteral("cardMore"));
    document_=new QPushButton(QStringLiteral("Открыть строки…"),this); document_->setObjectName("textFileOpen");
    source_=new QPushButton(QStringLiteral("Открыть источник…"),this); source_->setObjectName("cardSource");
    compare_=new QPushButton(QStringLiteral("Сравнить с источником…"),this); compare_->setObjectName("cardCompare");
    openUrl_=new QPushButton(QStringLiteral("Открыть HTTP(S)-адрес…"),this); openUrl_->setObjectName("cardOpenUrl");
    actions_ = new QWidget(this);
    QSizePolicy actionsPolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    actionsPolicy.setHeightForWidth(true);
    actions_->setSizePolicy(actionsPolicy);
    auto* actions = new CardActions(actions_);
    for (auto* button : {edit_, reveal_, copySecret_, copyLogin_, copyPhrase_, sshToFile_, document_, source_, compare_, openUrl_, more_})
        actions->addWidget(button);
    layout->addWidget(actions_);
    layout->addWidget(sshTargets_); layout->addWidget(metadata_); layout->addStretch(1);
    layout->addWidget(error_);
    hideTimer_.setParent(this); hideTimer_.setObjectName(QStringLiteral("secretHideTimer")); hideTimer_.setSingleShot(true); hideTimer_.setInterval(15000);
    connect(&hideTimer_, &QTimer::timeout, this, &EntryCard::hideNowForTest);
    connect(reveal_, &QPushButton::clicked, this, [this] {
        if (!authorize()) return; const auto* entry = app_.model()->entry(id_); if (!entry) return;
        switch(entry->type) {
        case model::EntryType::ApiToken: value_->setPlainText(entry->token); break;
        case model::EntryType::Credentials: value_->setPlainText(QStringLiteral("Логин: %1\nПароль: %2").arg(entry->login,entry->password)); break;
        case model::EntryType::PlainValue: value_->setPlainText(entry->plainValue.isEmpty()?QStringLiteral("Пусто"):entry->plainValue); break;
        default: return;
        }
        if(entry->type!=model::EntryType::PlainValue || entry->reverify) hideTimer_.start();
    });
    connect(copySecret_, &QPushButton::clicked, this, [this] { copyField(0); });
    connect(copyLogin_, &QPushButton::clicked, this, [this] { copyField(1); });
    connect(copyPhrase_, &QPushButton::clicked, this, [this] { copyField(2); });
    connect(sshToFile_, &QPushButton::clicked, this, &EntryCard::saveSshToFile);
    connect(edit_, &QPushButton::clicked, this, &EntryCard::editRequested);
    connect(more_, &QPushButton::clicked, this, &EntryCard::moreRequested);
    connect(document_,&QPushButton::clicked,this,[this] {if(app_.gate()==Application::Gate::Ready && !app_.busy()) emit documentRequested(id_);});
    connect(source_,&QPushButton::clicked,this,[this] {if(app_.gate()==Application::Gate::Ready && !app_.busy()) if(const auto* e=app_.model()->entry(id_);e && !e->sourceEntryId.isNull()) emit sourceRequested(e->sourceEntryId);});
    connect(compare_,&QPushButton::clicked,this,[this] {if(app_.gate()==Application::Gate::Ready && !app_.busy()) emit compareRequested(id_);});
    connect(openUrl_,&QPushButton::clicked,this,[this] {
        if(app_.gate()!=Application::Gate::Ready || app_.busy() || openableUrl(app_.model()->entry(id_)).isEmpty() || !authorize()) return;
        const auto url=openableUrl(app_.model()->entry(id_)); if(url.isEmpty()) return;
        if(!QDesktopServices::openUrl(url)) error_->setText(QStringLiteral("Не удалось открыть адрес."));
    });
    connect(&app_, &Application::busyChanged, this, &EntryCard::updateEnabled);
    // Инвариант: модель может быть очищена из QtConcurrent-потока (lock) — указатели на Entry
    // не живут через диалоги/циклы событий.
    connect(&app_, &Application::stateChanged, this, [this] { if (app_.gate() != Application::Gate::Ready) clear(); else updateEnabled(); });
    applyAccent(); qApp->installEventFilter(this); clear();
}
void EntryCard::updateEnabled() {
    const bool allowed = app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id_);
    for (auto* button : {reveal_, copySecret_, copyLogin_, copyPhrase_, sshToFile_, edit_, more_,document_,source_,compare_,openUrl_}) button->setEnabled(allowed);
    const auto* entry=allowed?app_.model()->entry(id_):nullptr;
    openUrl_->setEnabled(allowed && !openableUrl(entry).isEmpty());
    source_->setEnabled(entry && !entry->sourceEntryId.isNull() && app_.model()->entry(entry->sourceEntryId));
}
void EntryCard::hideNowForTest() {
    hideTimer_.stop(); const auto* entry=app_.gate()==Application::Gate::Ready?app_.model()->entry(id_):nullptr;
    if (entry && entry->type == model::EntryType::TextFile) {
        value_->setPlainText(QStringLiteral("%1\nСтрок: %2 · видимых: %3").arg(entry->textFile.fileName)
            .arg(model::textfile::lineCount(entry->textFile.bytes)).arg(entry->textFile.visibleLines.size()));
        return;
    }
    value_->setPlainText(entry && entry->type==model::EntryType::PlainValue && !entry->reverify
        ?entry->plainValue.isEmpty()?QStringLiteral("Пусто"):entry->plainValue:QStringLiteral("••••••••"));
}
void EntryCard::setRevealSeconds(int seconds) {
    if(seconds!=5 && seconds!=10 && seconds!=15 && seconds!=30) seconds=15;
    if(hideTimer_.interval()!=seconds*1000) hideNowForTest();
    hideTimer_.setInterval(seconds*1000);
    reveal_->setText(QStringLiteral("Показать на %1 с").arg(seconds));
    reveal_->setToolTip(QStringLiteral("Показать на %1 секунд").arg(seconds));
}
void EntryCard::clear() {
    ++selection_;
    id_ = {};
    hideNowForTest();
    title_->clear();
    for (auto* label : {identity_, flags_, metadata_, sshTargets_, error_}) {
        label->clear();
        label->hide();
    }
    valueLabel_->hide();
    value_->hide();
    actions_->hide();
    updateEnabled();
}
void EntryCard::showSelection(int count) { clear(); title_->setText(QStringLiteral("Выбрано записей: %1").arg(count)); }
void EntryCard::showEntry(model::Id id) {
    ++selection_; id_ = id; hideNowForTest(); error_->clear();
    const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(id) : nullptr;
    if (!entry) { clear(); return; }
    const auto expiryDate = entry->expiresAt.date();
    const QString expiryNote = expiryDate.isValid()
        ? QStringLiteral(" · действителен до %1%2").arg(
              expiryDate.toString(QStringLiteral("dd.MM.yyyy")),
              expiryDate < QDate::currentDate() ? QStringLiteral(" (просрочен)") : QString())
        : QString();
    title_->setText(entry->title);
    QStringList details;
    if (!entry->note.isEmpty()) details << QStringLiteral("Заметки: %1").arg(entry->note);
    if (!entry->url.isEmpty()) details << QStringLiteral("Адрес: %1").arg(entry->url);
    details << QStringLiteral("Изменено: %1%2").arg(entry->modified.toLocalTime().toString(QStringLiteral("yyyy-MM-dd hh:mm")), expiryNote);
    if (!entry->sshComment.isEmpty()) details << entry->sshComment;
    metadata_->setText(details.join('\n'));
    const bool ssh = entry->type == model::EntryType::SshKey;
    QStringList targets;
    for (const auto& target : entry->sshTargets) {
        const auto host = target.host.contains(':') ? QStringLiteral("[%1]").arg(target.host) : target.host;
        auto line = QStringLiteral("%1:%2").arg(host).arg(target.port);
        if (!target.alias.isEmpty()) line.prepend(target.alias+QStringLiteral(" · "));
        if (!target.username.isEmpty()) line.append(QStringLiteral(" · ")+target.username);
        targets << line;
    }
    sshTargets_->setText(targets.isEmpty()?QString():QStringLiteral("SSH-подключения\n%1").arg(targets.join('\n')));
    sshTargets_->setVisible(!targets.isEmpty());
    QStringList path;
    auto node = entry->groupId;
    for (int depth = 0; !node.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
        const auto it = app_.model()->nodes().constFind(node);
        if (it == app_.model()->nodes().cend()) break;
        path.prepend(it->name); node = it->parent;
    }
    identity_->setText(path.isEmpty() ? entryTypeLabel(entry->type)
        : QStringLiteral("%1 · %2").arg(entryTypeLabel(entry->type), path.join(QStringLiteral(" / "))));
    QStringList flags;
    if (entry->favorite) flags << QStringLiteral("★ Избранное");
    if (entry->reverify) flags << QStringLiteral("Повторная проверка");
    if (!entry->tags.isEmpty()) flags << entry->tags.join(QStringLiteral(", "));
    flags_->setText(flags.join(QStringLiteral(" · ")));
    const bool plain=entry->type==model::EntryType::PlainValue,file=entry->type==model::EntryType::TextFile;
    document_->setVisible(file);
    source_->setVisible(!entry->sourceEntryId.isNull()); compare_->setVisible(!entry->sourceEntryId.isNull());
    openUrl_->setVisible(!openableUrl(entry).isEmpty());
    valueLabel_->setText(plain?QStringLiteral("ЗНАЧЕНИЕ"):file?QStringLiteral("ДОКУМЕНТ"):QStringLiteral("СЕКРЕТ"));
    value_->setAccessibleName(plain?QStringLiteral("Значение"):file?QStringLiteral("Сведения о документе"):QStringLiteral("Секретное значение"));
    value_->setFocusPolicy(plain && !entry->reverify?Qt::StrongFocus:Qt::NoFocus);
    value_->setTextInteractionFlags(plain && !entry->reverify?Qt::TextSelectableByMouse|Qt::TextSelectableByKeyboard:Qt::NoTextInteraction);
    if(plain && !entry->parameterName.isEmpty()) metadata_->setText(QStringLiteral("Параметр: %1\n").arg(entry->parameterName)+metadata_->text());
    reveal_->setVisible(!ssh && !file && (!plain || entry->reverify)); copySecret_->setVisible(!file);
    copySecret_->setText(plain?QStringLiteral("Копировать значение"):ssh ? QStringLiteral("Копировать приватный ключ") : entry->type == model::EntryType::Credentials ? QStringLiteral("Копировать пароль") : QStringLiteral("Копировать токен"));
    if(!entry->sourceEntryId.isNull()) metadata_->setText(metadata_->text()+QStringLiteral("\n%1\nИсточник — снимок импорта; рабочее значение могло быть изменено.")
        .arg(app_.model()->entry(entry->sourceEntryId)?QStringLiteral("Источник, строка %1").arg(entry->sourceLine+1):QStringLiteral("Источник недоступен")));
    copyLogin_->setVisible(entry->type == model::EntryType::Credentials); copyPhrase_->setVisible(ssh && !entry->sshPassphrase.isEmpty());
    sshToFile_->setVisible(ssh && !entry->sshPrivate.isEmpty());
    identity_->show();
    flags_->setVisible(!flags.isEmpty());
    metadata_->show();
    valueLabel_->show();
    value_->show();
    actions_->show();
    error_->show();
    updateEnabled();
}
QString EntryCard::secretText() const { return value_->toPlainText(); }
void EntryCard::changeEvent(QEvent* event) {
    QFrame::changeEvent(event);
    if (copySecret_ && (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::PaletteChange)) applyAccent();
    if (value_ && (event->type() == QEvent::FontChange || event->type() == QEvent::ApplicationFontChange)) updateValueHeight();
}
void EntryCard::updateValueHeight() {
    value_->setFixedHeight(2 * value_->fontMetrics().lineSpacing() + 12);
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
    if (!authorizeEntry(app_,id,this)) return false;
    return generation == app_.generation() && selection == selection_ && id == id_ && app_.gate() == Application::Gate::Ready && !app_.busy() && app_.model()->entry(id_);
}
void EntryCard::copyField(int field) {
    if (!authorize()) return; const auto* entry = app_.model()->entry(id_); if (!entry) return;
    QString text;
    if (field==1) {if(entry->type!=model::EntryType::Credentials) return; text=entry->login;}
    else if(field==2) {if(entry->type!=model::EntryType::SshKey) return; text=entry->sshPassphrase;}
    else if (field==0 && entry->type == model::EntryType::SshKey) {
        if (!entry->sshPrivate.isValidUtf8()) { error_->setText(QStringLiteral("Ключ нельзя скопировать как текст. Сохраните его в файл.")); return; }
        text = QString::fromUtf8(entry->sshPrivate);
    } else if(field==0) switch(entry->type) {
        case model::EntryType::ApiToken: text=entry->token; break;
        case model::EntryType::Credentials: text=entry->password; break;
        case model::EntryType::PlainValue: text=entry->plainValue; break;
        default: return;
    }
    if (text.isEmpty()) { error_->setText(QStringLiteral("Пустое значение не копируется.")); return; }
    emit copyRequested(text);
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
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Сохранение приватного ключа"), {}, {}, nullptr,
        QFileDialog::DontConfirmOverwrite | QFileDialog::DontResolveSymlinks);
    if (path.isEmpty() || !stillValid()) return;
    const bool replace = QFileInfo::exists(path);
    if (replace) {
        if (QMessageBox::question(this, QStringLiteral("TokenBucket"), QStringLiteral("Файл существует. Перезаписать?"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        if (!stillValid()) return;
    }
    const QString error = store::exportFile(path, key, replace);
    if (!error.isEmpty()) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return;
    }
    error_->clear();
}
bool EntryCard::eventFilter(QObject* object, QEvent* event) {
    if(object==value_ && (event->type()==QEvent::ShortcutOverride || event->type()==QEvent::KeyPress)) {
        auto* key=static_cast<QKeyEvent*>(event);
        if(key->matches(QKeySequence::Copy) || key->matches(QKeySequence::Cut)) {
            event->accept();
            if(event->type()==QEvent::KeyPress && app_.gate()==Application::Gate::Ready && !app_.busy()) {
                const auto* entry=app_.model()->entry(id_); const auto cursor=value_->textCursor();
                if(entry && entry->type==model::EntryType::PlainValue && !entry->reverify && cursor.hasSelection()
                    && value_->document()->toRawText()==entry->plainValue) {
                    const auto text=entry->plainValue.mid(cursor.selectionStart(),cursor.selectionEnd()-cursor.selectionStart());
                    if(!text.isEmpty() && authorize()) emit copyRequested(text);
                }
            }
            return true;
        }
    }
    if (!hideTimer_.isActive()) return QFrame::eventFilter(object, event);
    if ((event->type() == QEvent::WindowDeactivate && object == window()) || event->type() == QEvent::ApplicationDeactivate || (event->type() == QEvent::Hide && object == this)) hideNowForTest();
    if (event->type() == QEvent::FocusIn) if (auto* widget = qobject_cast<QWidget*>(object); widget && widget != this && !isAncestorOf(widget)) hideNowForTest();
    return QFrame::eventFilter(object, event);
}
}
