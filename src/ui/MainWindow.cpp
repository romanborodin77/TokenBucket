#include "MainWindow.h"
#include "EntryEditDialog.h"
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QDialogButtonBox>
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
#include <QVBoxLayout>
#include <algorithm>
namespace tb::ui {
MainWindow::MainWindow(Application& app, QWidget* parent) : QMainWindow(parent), app_(app) {
    setWindowTitle(QStringLiteral("TokenBucket"));
    const auto available = screen()->availableGeometry(); resize(qMin(1080, available.width() - 40), qMin(660, available.height() - 80));
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
    details_ = new QPlainTextEdit(right_); details_->setReadOnly(true); details_->setObjectName(QStringLiteral("entryDetails")); details_->setAccessibleName(QStringLiteral("Сведения о записи без секретных значений")); right_->addWidget(details_);
    splitter->addWidget(tree_); splitter->addWidget(list_); splitter->addWidget(right_); splitter->setSizes({250,300,500}); layout->addWidget(splitter, 1);
    error_ = new QLabel(central); error_->setObjectName(QStringLiteral("operationError")); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    setCentralWidget(central);
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { refillList(); });
    connect(search_, &QLineEdit::textChanged, this, [this] { refillList(); });
    connect(list_, &QListWidget::currentItemChanged, this, &MainWindow::showMetadata);
    connect(list_, &QListWidget::itemDoubleClicked, this, &MainWindow::editSelected);
    connect(&app_, &Application::busyChanged, this, [this](bool busy) { updateEnabled(); if (busy) statusBar()->showMessage(QStringLiteral("Сохранение…")); });
    connect(&app_, &Application::stateChanged, this, [this] {
        if (app_.gate() != Application::Gate::Ready) {
            for (auto* dialog : findChildren<QDialog*>()) {
                if (auto* editDialog = qobject_cast<EntryEditDialog*>(dialog)) editDialog->clearSecrets();
                dialog->reject();
            }
            search_->clear(); error_->clear(); hide();
        }
        refreshAll();
    });
    refreshAll();
}
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
    refillList(entry); updateEnabled();
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
    details_->clear(); const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(selectedEntry()) : nullptr;
    right_->setCurrentIndex(entry ? 1 : 0); if (!entry) return;
    const QString type = entry->type == model::EntryType::ApiToken ? QStringLiteral("API-токен") : entry->type == model::EntryType::Credentials ? QStringLiteral("Учётные данные") : QStringLiteral("SSH-ключ");
    QStringList text{entry->title, type};
    if (!entry->login.isEmpty()) text << QStringLiteral("Логин: ") + entry->login;
    if (!entry->url.isEmpty()) text << QStringLiteral("Адрес: ") + entry->url;
    if (!entry->note.isEmpty()) text << QStringLiteral("\n") + entry->note;
    if (!entry->sshComment.isEmpty()) text << entry->sshComment;
    if (entry->modified.isValid()) text << QStringLiteral("Изменено: %1").arg(entry->modified.toLocalTime().toString(QStringLiteral("dd.MM.yyyy HH:mm")));
    text << QStringLiteral("\nСекретные значения скрыты."); details_->setPlainText(text.join('\n'));
}
void MainWindow::report(const QString& error) { error_->setText(error); statusBar()->showMessage(QStringLiteral("Изменение не сохранено.")); }
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
void MainWindow::resizeEvent(QResizeEvent* event) { QMainWindow::resizeEvent(event); narrow_ = width() < 720; right_->setVisible(!narrow_); }
void MainWindow::closeEvent(QCloseEvent* event) { app_.lock(); QMainWindow::closeEvent(event); emit closing(); }
}
