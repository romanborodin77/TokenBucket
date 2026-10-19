#include "MainWindow.h"
#include "EntryEditDialog.h"
#include "EntryCard.h"
#include "ExportDialog.h"
#include "VerifyDialog.h"
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenuBar>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScopedValueRollback>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "../platform/FileOps.h"
#include "../vault/Container.h"
#include "../vault/SecureBytes.h"
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#include "../platform/QuickPasteWin.h"
#include <QKeySequenceEdit>
#include <QSettings>
#include <QTimer>
#endif
#include <algorithm>
namespace tb::ui {
MainWindow::MainWindow(Application& app, QWidget* parent) : QMainWindow(parent), app_(app) {
    setWindowTitle(QStringLiteral("TokenBucket"));    const auto available = screen()->availableGeometry(); resize(qMin(1080, available.width() - 40), qMin(660, available.height() - 80));
    auto* vault = menuBar()->addMenu(QStringLiteral("&Хранилище"));
    auto* projects = menuBar()->addMenu(QStringLiteral("&Проекты и группы"));
    auto* entries = menuBar()->addMenu(QStringLiteral("&Записи"));
    const auto action = [&](QMenu* menu, const QString& text, auto slot) {
        auto* item = menu->addAction(text, this, slot); mutations_ << item; return item;
    };
    auto* addProjectAction = action(projects, QStringLiteral("Новый проект…"), &MainWindow::addProject);
    action(projects, QStringLiteral("Новая группа…"), &MainWindow::addGroup);
    action(projects, QStringLiteral("Переименовать…"), &MainWindow::renameSelectedNode);
    action(projects, QStringLiteral("Переместить группу…"), &MainWindow::moveSelectedNode);
    action(projects, QStringLiteral("Удалить проект/группу…"), &MainWindow::deleteSelectedNode);
    auto* addEntryAction = action(entries, QStringLiteral("Новая запись…"), &MainWindow::addEntry);
    addEntryAction->setShortcut(QKeySequence::New);
    auto* edit = action(entries, QStringLiteral("Изменить запись…"), &MainWindow::editSelected); edit->setShortcut(QKeySequence(Qt::Key_F2));
    action(entries, QStringLiteral("Удалить запись…"), &MainWindow::deleteSelected);
    action(vault, QStringLiteral("Защищённый экспорт…"), &MainWindow::exportVault);
    action(vault, QStringLiteral("Импорт из файла…"), &MainWindow::importVault);
    vault->addSeparator();
    auto* settings = menuBar()->addMenu(QStringLiteral("&Настройки"));
#ifdef Q_OS_WIN
    switchAction_ = action(settings, QString(), &MainWindow::switchUnlockMode);
    {
        auto* watcher = new QFutureWatcher<platform::hello::Availability>(this);
        connect(watcher, &QFutureWatcher<platform::hello::Availability>::finished, this, [this, watcher] {
            helloAvailable_ = watcher->result() == platform::hello::Availability::Available;
            watcher->deleteLater(); updateSwitchActionText();
        });
        watcher->setFuture(QtConcurrent::run(platform::hello::availability));
    }
#endif
    changePwAction_ = action(settings, QStringLiteral("Сменить мастер-пароль…"), &MainWindow::changeMasterPassword);
#ifdef Q_OS_WIN
    settings->addAction(QStringLiteral("Горячая клавиша быстрой вставки…"), this, &MainWindow::changeHotkey);
#endif
    updateSwitchActionText();
    auto* lock = vault->addAction(QStringLiteral("Заблокировать"), &app_, &Application::lock); lock->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    vault->addAction(QStringLiteral("Закрыть"), this, &QWidget::close, QKeySequence::Quit);
    auto* toolbar = addToolBar(QStringLiteral("Действия")); toolbar->setMovable(false); toolbar->setToolButtonStyle(Qt::ToolButtonTextOnly);
    toolbar->addAction(addProjectAction); toolbar->addAction(addEntryAction); toolbar->addAction(edit); toolbar->addSeparator(); toolbar->addAction(lock);
    auto* central = new QWidget(this); auto* layout = new QVBoxLayout(central);
    search_ = new QLineEdit(central); search_->setObjectName(QStringLiteral("vaultSearch")); search_->setAccessibleName(QStringLiteral("Поиск по хранилищу"));
    search_->setPlaceholderText(QStringLiteral("Поиск по названиям, логинам, проектам и заметкам — во всём хранилище")); layout->addWidget(search_);
    auto* splitter = new QSplitter(central);
    tree_ = new QTreeWidget(splitter); tree_->setHeaderLabel(QStringLiteral("Проекты и группы")); tree_->setAccessibleName(QStringLiteral("Проекты и группы"));
    list_ = new QListWidget(splitter); list_->setAccessibleName(QStringLiteral("Записи выбранной группы")); list_->setMinimumWidth(160);
    right_ = new QStackedWidget(splitter); right_->setObjectName(QStringLiteral("entryPanel"));
    auto* hint = new QLabel(QStringLiteral("Создайте проект и добавьте запись или выберите существующую запись."), right_);
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); right_->addWidget(hint);
    cardScroll_ = new QScrollArea(right_); cardScroll_->setWidgetResizable(true); cardScroll_->setFrameShape(QFrame::NoFrame);
    card_ = new EntryCard(app_, cardScroll_); cardScroll_->setWidget(card_); right_->addWidget(cardScroll_);
    splitter->addWidget(tree_); splitter->addWidget(list_); splitter->addWidget(right_); splitter->setSizes({250,300,500}); layout->addWidget(splitter, 1);
    error_ = new QLabel(central); error_->setObjectName(QStringLiteral("operationError")); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    setCentralWidget(central);
    vaultInfo_ = new QLabel(statusBar()); vaultInfo_->setTextFormat(Qt::PlainText);
    statusBar()->addPermanentWidget(vaultInfo_);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { refillList(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { refillList(); });
    connect(list_, &QListWidget::currentItemChanged, this, &MainWindow::showMetadata);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { if (!narrow_) editSelected(); });
    connect(list_, &QListWidget::itemClicked, this, &MainWindow::openCardDialog);
    connect(list_, &QListWidget::itemActivated, this, &MainWindow::openCardDialog);
    connect(card_, &EntryCard::copyRequested, this, [this](const QString& text) {
        if (!ready()) return; QString error;
        if (!clipboard_.copySecret(text, &error)) { if (app_.gate() == Application::Gate::Ready) report(error); }
        else statusBar()->showMessage(QStringLiteral("Скопировано. Очистка буфера через 20 секунд."), 5000);
    });
    connect(&app_, &Application::busyChanged, this, [this](bool busy) { updateEnabled(); statusBar()->showMessage(busy ? QStringLiteral("Выполняется операция…") : QString()); });
    connect(&app_, &Application::stateChanged, this, [this] {
        if (app_.gate() != Application::Gate::Ready) {
            clipboard_.clearIfOurs(); card_->clear();
            for (auto* dialog : findChildren<QDialog*>()) {
                if (auto* editDialog = qobject_cast<EntryEditDialog*>(dialog)) editDialog->clearSecrets();
                dialog->reject();
            }
            search_->clear(); error_->clear(); hide();
        }
        refreshAll();
    });
#ifdef Q_OS_WIN
    hotkey_ = std::make_unique<platform::QuickPaste>(app_, clipboard_, this);
    connect(hotkey_.get(), &platform::QuickPaste::conflictDetected, this, [this](const QString& combo) {
        QTimer::singleShot(0, this, [this, combo] {
            QMessageBox::warning(this, QStringLiteral("TokenBucket"),
                QStringLiteral("Горячая клавиша %1 занята другим приложением. Выберите другое сочетание.").arg(combo));
            changeHotkey();
        });
    });
    hotkey_->start(nullptr);
#endif
    refreshAll();
}
MainWindow::~MainWindow() = default;
bool MainWindow::ready() const { return app_.gate() == Application::Gate::Ready && !app_.busy(); }
void MainWindow::updateEnabled() {
    centralWidget()->setEnabled(ready()); for (auto* action : mutations_) action->setEnabled(ready());
}
model::Id MainWindow::selectedNode() const { const auto* item = tree_->currentItem(); return item ? item->data(0, Qt::UserRole).value<model::Id>() : model::Id{}; }
model::Id MainWindow::selectedEntry() const { const auto* item = list_->currentItem(); return item ? item->data(Qt::UserRole).value<model::Id>() : model::Id{}; }
void MainWindow::selectNode(model::Id id) {
    for (QTreeWidgetItemIterator item(tree_); *item; ++item) if ((*item)->data(0, Qt::UserRole).value<model::Id>() == id) {
        tree_->setCurrentItem(*item); tree_->scrollToItem(*item); return;
    }
}
void MainWindow::refreshAll() {
    const auto node = selectedNode(), entry = selectedEntry();
    QSignalBlocker block(tree_); tree_->clear();
    if (app_.gate() == Application::Gate::Ready) {
        QTreeWidgetItem* selection = nullptr;
        const std::function<void(model::Id,QTreeWidgetItem*)> add = [&](model::Id parent, QTreeWidgetItem* item) {
            for (auto id : app_.model()->childNodes(parent)) {
                auto* child = new QTreeWidgetItem(QStringList{app_.model()->nodes().value(id).name});
                child->setData(0, Qt::UserRole, id); (item ? item : tree_->invisibleRootItem())->addChild(child);
                if (id == node) selection = child; add(id, child);
            }
        };
        add({}, nullptr); tree_->expandToDepth(0);
        if (selection) tree_->setCurrentItem(selection);
        else if (tree_->topLevelItemCount()) tree_->setCurrentItem(tree_->topLevelItem(0));
    }
    refillList(entry); updateEnabled(); updateSwitchActionText();
    if (app_.gate() == Application::Gate::Ready) {
        const auto mode = app_.mode() == Application::Mode::Hello ? QStringLiteral("Windows Hello") : QStringLiteral("мастер-пароль");
        vaultInfo_->setText(QStringLiteral("Записей: %1 · Проектов и групп: %2 · Разблокировка: %3")
            .arg(app_.model()->entries().size()).arg(app_.model()->nodes().size()).arg(mode));
    } else vaultInfo_->clear();
}
void MainWindow::refillList(model::Id selected) {
    QSignalBlocker block(list_); list_->clear();
    if (app_.gate() == Application::Gate::Ready) {
        auto ids = search_->text().isEmpty() ? app_.model()->entriesInGroup(selectedNode()) : app_.model()->search(search_->text());
        std::sort(ids.begin(), ids.end(), [this](model::Id a, model::Id b) {
            const auto *first = app_.model()->entry(a), *second = app_.model()->entry(b);
            return first->favorite != second->favorite ? first->favorite : first->title < second->title;
        });
        for (auto id : ids) {
            const auto* entry = app_.model()->entry(id);
            auto* item = new QListWidgetItem(entry->favorite ? QStringLiteral("★ ") + entry->title : entry->title, list_); item->setData(Qt::UserRole, id);
            if (id == selected) list_->setCurrentItem(item);
        }
        if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
    }
    showMetadata();
}
void MainWindow::showMetadata() {
    const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(selectedEntry()) : nullptr;
    right_->setCurrentIndex(entry ? 1 : 0); if (!entry) { card_->clear(); return; }
    card_->showEntry(entry->id);
}
void MainWindow::openCardDialog() {
    if (!narrow_ || !ready() || cardOpen_ || selectedEntry().isNull()) return;
    QScopedValueRollback guard(cardOpen_, true); QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("TokenBucket — запись"));
    auto* layout = new QVBoxLayout(&dialog); auto* scroll = new QScrollArea(&dialog); scroll->setWidgetResizable(true);
    cardScroll_->takeWidget(); scroll->setWidget(card_); layout->addWidget(scroll);
    auto restore = qScopeGuard([&] { card_->hideNowForTest(); scroll->takeWidget(); cardScroll_->setWidget(card_); });
    const auto available = screen()->availableGeometry(); dialog.resize(qMin(520, available.width() - 40), qMin(560, available.height() - 80)); dialog.exec();
}
void MainWindow::report(const QString& error) { error_->setText(error); statusBar()->showMessage(QStringLiteral("Операция не выполнена.")); }
bool MainWindow::commit(const std::function<QString(model::VaultModel&)>& mutation) {
    if (!ready()) return false;
    auto before = *app_.model(); const auto generation = app_.generation();
    const auto connection = connect(&app_, &Application::stateChanged, this, [&] { if (app_.generation() != generation) before.clear(); });
    auto cleanup = qScopeGuard([&] { disconnect(connection); });
    const auto error = mutation(*app_.model());
    if (!error.isEmpty()) { *app_.model() = std::move(before); report(error); refreshAll(); return false; }
    QString saveError; const bool saved = app_.save(&saveError);
    if (!saved) {
        if (app_.generation() == generation && app_.gate() == Application::Gate::Ready) *app_.model() = std::move(before);
        before.clear(); refreshAll();
        if (app_.gate() == Application::Gate::Ready) report(saveError);
        return false;
    }
    error_->clear(); refreshAll(); statusBar()->showMessage(QStringLiteral("Сохранено"), 3000); return true;
}
void MainWindow::addProject() {
    if (!ready() || editing_) return; QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    bool ok = false; const auto name = QInputDialog::getText(this, QStringLiteral("Новый проект"), QStringLiteral("Имя проекта:"), QLineEdit::Normal, {}, &ok);
    const auto id = QUuid::createUuid();
    if (ok && ready() && app_.generation() == generation && commit([&](auto& model) { return model.addNode(id, {}, name); })) selectNode(id);
}
void MainWindow::addGroup() {
    if (!ready() || editing_) return; const auto parent = selectedNode(); if (parent.isNull()) { report(QStringLiteral("Сначала выберите проект или группу.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation(); bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Новая группа"), QStringLiteral("Имя группы:"), QLineEdit::Normal, {}, &ok);
    const auto id = QUuid::createUuid();
    if (ok && ready() && app_.generation() == generation && commit([&](auto& model) { return model.addNode(id, parent, name); })) selectNode(id);
}
void MainWindow::addEntry() {
    if (!ready() || editing_) return; const auto parent = selectedNode(); if (parent.isNull()) { report(QStringLiteral("Сначала выберите проект или группу.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    EntryEditDialog dialog(*app_.model(), std::nullopt, this); dialog.setGroup(parent);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto entry = dialog.entry(); if (entry) commit([&](auto& model) { return model.setEntry(*entry); });
    }
}
void MainWindow::editSelected() {
    if (!ready() || editing_) return; const auto* entry = app_.model()->entry(selectedEntry()); if (!entry) { report(QStringLiteral("Выберите запись.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    EntryEditDialog dialog(*app_.model(), *entry, this);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto changed = dialog.entry(); if (changed) commit([&](auto& model) { return model.setEntry(*changed); });
    }
}
void MainWindow::renameSelectedNode() {
    if (!ready() || editing_) return; const auto id = selectedNode(); if (id.isNull()) return;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation(); bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Переименование"), QStringLiteral("Новое имя:"), QLineEdit::Normal, app_.model()->nodes().value(id).name, &ok);
    if (ok && ready() && app_.generation() == generation) commit([&](auto& model) { return model.renameNode(id, name); });
}
void MainWindow::moveSelectedNode() {
    if (!ready() || editing_) return; const auto id = selectedNode();
    if (id.isNull() || app_.model()->nodes().value(id).parent.isNull()) { report(QStringLiteral("Выберите группу для переноса.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("Перемещение группы")); auto* layout = new QVBoxLayout(&dialog);
    auto* combo = new QComboBox(&dialog); combo->setObjectName(QStringLiteral("moveTarget")); combo->setAccessibleName(QStringLiteral("Новый родитель группы")); populateGroups(*combo, *app_.model()); layout->addWidget(combo);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Переместить")); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) commit([&](auto& model) { return model.moveNode(id, combo->currentData().value<model::Id>()); });
}
void MainWindow::deleteSelected() {
    if (!ready() || editing_) return; const auto id = selectedEntry(); if (id.isNull()) { report(QStringLiteral("Выберите запись для удаления.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (QMessageBox::question(this, QStringLiteral("Удаление записи"), QStringLiteral("Удалить выбранную запись?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes
        && ready() && app_.generation() == generation) commit([&](auto& model) { return model.removeEntry(id); });
}
void MainWindow::deleteSelectedNode() {
    if (!ready() || editing_) return; const auto id = selectedNode(); if (id.isNull()) return;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (QMessageBox::question(this, QStringLiteral("Удаление проекта/группы"), QStringLiteral("Удалить выбранный проект или группу?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes
        && ready() && app_.generation() == generation) commit([&](auto& model) { return model.removeNode(id); });
}
void MainWindow::exportVault() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (!VerifyDialog::verify(app_, this, QStringLiteral("защищённый экспорт")) || !ready() || app_.generation() != generation) return;
    ExportDialog dialog(this, QStringLiteral("Защищённый экспорт хранилища"),
        QStringLiteral("Экспорт шифруется отдельным паролем, который не связан с мастер-паролем. "
                       "Полученный файл можно переносить на другие компьютеры; для восстановления понадобятся только файл и этот пароль."),
        QStringLiteral("&Пароль защищённого экспорта (12–1024 байта UTF-8):"));
    if (dialog.exec() != QDialog::Accepted || !ready() || app_.generation() != generation) return;
    SecureBytes password(dialog.password()); dialog.clearSecrets();
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Защищённый экспорт"), {}, QStringLiteral("TokenBucket (*.tb)"));
    if (path.isEmpty() || !ready() || app_.generation() != generation) { password.clear(); return; }
    QString error;
    const QByteArray bytes = app_.exportContainer(password.bytes(), &error);
    password.clear();
    if (bytes.isEmpty()) { QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return; }
    if (!store::realFileOps().writeAll(path, bytes)) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось записать файл экспорта. Если файл существует, выберите другое имя."));
        return;
    }
    statusBar()->showMessage(QStringLiteral("Экспорт выполнен."), 5000);
}
void MainWindow::importVault() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (QMessageBox::question(this, QStringLiteral("Импорт хранилища"),
            QStringLiteral("Импорт заменит текущее хранилище новым без слияния. Продолжить?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт из файла"), {}, QStringLiteral("TokenBucket (*.tb);;Все файлы (*)"));
    if (path.isEmpty() || !ready() || app_.generation() != generation) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > container::MaxFileBytes) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось открыть файл импорта или он больше 80 МиБ.")); return;
    }
    const QByteArray bytes = file.read(container::MaxFileBytes + 1);
    file.close();
    QInputDialog fileDialog(this);
    fileDialog.setWindowTitle(QStringLiteral("Импорт хранилища"));
    fileDialog.setLabelText(QStringLiteral("Пароль файла импорта:")); fileDialog.setTextEchoMode(QLineEdit::Password);
    fileDialog.setOkButtonText(QStringLiteral("Далее")); fileDialog.setCancelButtonText(QStringLiteral("Отмена"));
    if (fileDialog.exec() != QDialog::Accepted || !ready() || app_.generation() != generation) return;
    SecureBytes filePassword(fileDialog.textValue().toUtf8()); fileDialog.setTextValue({});
    ExportDialog localDialog(this, QStringLiteral("Пароль нового локального хранилища"),
        QStringLiteral("Импортируемые данные будут пересохранены в этом приложении с новым мастер-паролем. "
                       "Пароль файла импорта используется один раз — только чтобы прочитать файл."),
        QStringLiteral("&Новый мастер-пароль локального хранилища (12–1024 байта UTF-8):"));
    if (localDialog.exec() != QDialog::Accepted || !ready() || app_.generation() != generation) { filePassword.clear(); return; }
    SecureBytes localPassword(localDialog.password()); localDialog.clearSecrets();
    QString error;
    const bool ok = app_.importContainer(bytes, filePassword.bytes(), localPassword.bytes(), &error);
    filePassword.clear(); localPassword.clear();
    if (!ok) { QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return; }
    refreshAll();
    statusBar()->showMessage(QStringLiteral("Импорт выполнен."), 5000);
}
void MainWindow::switchUnlockMode() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
    if (!VerifyDialog::verify(app_, this, QStringLiteral("смена способа разблокировки")) || !ready()) return;
    QString error;
#ifdef Q_OS_WIN
    if (app_.mode() == Application::Mode::Password) {
        if (!helloAvailable_) {
            QMessageBox::information(this, QStringLiteral("TokenBucket"), QStringLiteral("Windows Hello недоступен или не настроен."));
            return;
        }
        QMessageBox::information(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Хранилище будет зашифровано для вашей учётной записи Windows, и разблокировка станет выполняться по Windows Hello. "
                           "Мастер-пароль использоваться больше не будет. Ключ не переносится на другие компьютеры; "
                           "для переноса заранее сделайте защищённый экспорт."));
        if (app_.switchToHello(&error)) statusBar()->showMessage(QStringLiteral("Разблокировка Windows Hello включена."), 5000);
        else QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
        refreshAll();
        return;
    }
#endif
    QMessageBox::information(this, QStringLiteral("TokenBucket"),
        QStringLiteral("Хранилище будет зашифровано новым мастер-паролем; Windows Hello больше не будет использоваться для разблокировки. "
                       "Запомните новый пароль: восстановить забытый невозможно."));
    ExportDialog dialog(this, QStringLiteral("Переход на мастер-пароль"),
        QStringLiteral("Введите новый мастер-пароль дважды. После сохранения хранилище открывается только этим паролем."),
        QStringLiteral("&Новый мастер-пароль (12–1024 байта UTF-8):"));
    if (dialog.exec() != QDialog::Accepted || !ready()) return;
    SecureBytes password(dialog.password()); dialog.clearSecrets();
    const bool ok = app_.switchToPassword(password.bytes(), &error);
    password.clear();
    if (!ok) QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
    else statusBar()->showMessage(QStringLiteral("Разблокировка по мастер-паролю включена."), 5000);
    refreshAll();
}
void MainWindow::changeMasterPassword() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
    if (app_.mode() != Application::Mode::Password) return;
    if (!VerifyDialog::verify(app_, this, QStringLiteral("смена мастер-пароля")) || !ready()) return;
    ExportDialog dialog(this, QStringLiteral("Смена мастер-пароля"),
        QStringLiteral("Текущий пароль уже подтверждён. Введите новый пароль дважды. "
                       "После сохранения хранилище открывается только новым паролем, старый перестаёт действовать. "
                       "Забытый пароль восстановить невозможно — заранее делайте защищённый экспорт как резервную копию."),
        QStringLiteral("&Новый мастер-пароль (12–1024 байта UTF-8):"));
    if (dialog.exec() != QDialog::Accepted || !ready()) return;
    SecureBytes password(dialog.password()); dialog.clearSecrets();
    QString error;
    const bool ok = app_.switchToPassword(password.bytes(), &error);
    password.clear();
    if (!ok) QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
    else statusBar()->showMessage(QStringLiteral("Мастер-пароль изменён."), 5000);
}
void MainWindow::updateSwitchActionText() {
#ifdef Q_OS_WIN
    if (switchAction_) {
        switchAction_->setVisible(app_.mode() == Application::Mode::Hello || helloAvailable_);
        switchAction_->setText(app_.mode() == Application::Mode::Hello
            ? QStringLiteral("Перейти на мастер-пароль…") : QStringLiteral("Перейти на Windows Hello…"));
    }
#endif
    if (changePwAction_) changePwAction_->setVisible(app_.mode() != Application::Mode::Hello);
}
#ifdef Q_OS_WIN
void MainWindow::changeHotkey() {
    if (!hotkey_) return;
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("Горячая клавиша быстрой вставки"));
    auto* layout = new QVBoxLayout(&dialog);
    auto* label = new QLabel(QStringLiteral("Сочетание для вызова панели быстрой вставки:"), &dialog); label->setWordWrap(true);
    auto* edit = new QKeySequenceEdit(&dialog); edit->setObjectName(QStringLiteral("hotkeyEdit"));
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    edit->setKeySequence(QKeySequence(settings.value(QStringLiteral("hotkey"), QStringLiteral("Ctrl+Alt+Space")).toString()));
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Сохранить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(label); layout->addWidget(edit); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted) return;
    const QKeySequence chosen = edit->keySequence();
    if (chosen.isEmpty()) { QMessageBox::warning(this, QStringLiteral("TokenBucket"), QStringLiteral("Сочетание не задано.")); return; }
    settings.setValue(QStringLiteral("hotkey"), chosen.toString());
    QString error;
    if (!hotkey_->reRegister(&error) && !error.isEmpty()) QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
}
#endif
void MainWindow::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); narrow_ = width() < 720; right_->setVisible(!narrow_); }void MainWindow::closeEvent(QCloseEvent* event) { app_.lock(); QMainWindow::closeEvent(event); emit closing(); }
}
