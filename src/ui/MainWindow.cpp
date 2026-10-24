#include "MainWindow.h"
#include "SettingsDialog.h"
#include "SessionFlow.h"
#include "UnlockDialog.h"
#include "../app/Preferences.h"
#include "EntryEditDialog.h"
#include "EntryCard.h"
#include "AboutDialog.h"
#include "../io/Import.h"
#include "TrayController.h"
#include "ExportDialog.h"
#include "Theme.h"
#include "VerifyDialog.h"
#include "../app/IdleTimer.h"
#include <QGridLayout>
#include <QTimer>
#include <QAction>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFutureWatcher>
#include <QHBoxLayout>
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
#include <QSettings>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStatusBar>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QScreen>
#include <QScrollArea>
#include <QShortcut>
#include <QStyle>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "../platform/FileOps.h"
#include "../vault/Container.h"
#include "../vault/SecureBytes.h"
#include <sodium.h>
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#include "../platform/QuickPasteWin.h"
#include "../platform/QuickPaste.h"
#include "../platform/EnvOps.h"
#include "EnvExportDialog.h"
#include <QTimer>
#endif
#include <algorithm>
namespace tb::ui {
namespace {
// Перехватывает перетаскивание: базовая обработка двигает элемент в дереве,
// затем намерение (узел → целевой родитель) проверяется правилами модели.
class VaultTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<bool(model::Id, model::Id)> moveRequest;
protected:
    void dropEvent(QDropEvent* event) override {
        const auto* item = itemAt(event->position().toPoint());
        const model::Id dragged = currentItem() ? currentItem()->data(0, Qt::UserRole).value<model::Id>() : model::Id{};
        model::Id target;
        switch (dropIndicatorPosition()) {
        case OnItem:
            target = item ? item->data(0, Qt::UserRole).value<model::Id>() : model::Id{}; break;
        case AboveItem: case BelowItem:
            target = item && item->parent() ? item->parent()->data(0, Qt::UserRole).value<model::Id>() : model::Id{}; break;
        default: break;
        }
        QTreeWidget::dropEvent(event);
        if (!dragged.isNull() && moveRequest) moveRequest(dragged, target);
    }
};
}
MainWindow::MainWindow(Application& app, QWidget* parent, QSettings* uiPreferences,QSettings* runPreferences,
                       std::function<bool()> trayAvailable)
    : QMainWindow(parent), preferences_(uiPreferences ? uiPreferences : &uiSettings_),trayAvailable_(std::move(trayAvailable)), app_(app) {
    appliedPreferences_=prefs::read(*preferences_);
#ifdef Q_OS_WIN
    runPreferences_=runPreferences?runPreferences:&runSettings_;
    appliedPreferences_.autostart=platform::autostart::enabled(*runPreferences_);
#else
    Q_UNUSED(runPreferences);
#endif
    setWindowTitle(QStringLiteral("TokenBucket"));
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
#ifdef Q_OS_WIN
    // Не через mutations_: доступность зависит не только от Ready, но и от выбранной записи
    // (тип и однострочность секрета) — управляем сами в updateEnabled/showMetadata.
    envExportAction_ = entries->addAction(QStringLiteral("В переменную окружения…"), this, &MainWindow::exportToEnvironment);
#endif
    action(vault, QStringLiteral("Защищённый экспорт…"), &MainWindow::exportVault);
    action(vault, QStringLiteral("Импорт из файла…"), &MainWindow::importVault);
    action(vault, QStringLiteral("Импорт из CSV / KeePass XML…"), &MainWindow::importFromCsvKeePass);
    vault->addSeparator();
    auto* settings = menuBar()->addMenu(QStringLiteral("&Настройки"));
    auto* openSettings=settings->addAction(QStringLiteral("Настройки…"),this,&MainWindow::showSettings);
    openSettings->setObjectName("openSettings"); openSettings->setShortcut(QKeySequence(QStringLiteral("Ctrl+,")));
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
    updateSwitchActionText();
    auto* help = menuBar()->addMenu(QStringLiteral("&Справка"));
    auto* about = help->addAction(QStringLiteral("О программе…"), this, &MainWindow::showAbout);
    about->setShortcut(QKeySequence::HelpContents); // F1
    auto* lock = vault->addAction(QStringLiteral("Заблокировать"), &app_, &Application::lock); lock->setShortcut(QKeySequence(QStringLiteral("Ctrl+L")));
    // Настоящий выход из приложения: requestExit ставит exiting_ — closeEvent блокирует
    // хранилище и завершает работу. Крестик окна по-прежнему лишь сворачивает в трей.
    vault->addAction(QStringLiteral("Выход"), this, &MainWindow::requestExit, QKeySequence::Quit);
    addProjectAction->setIcon(style()->standardIcon(QStyle::SP_FileDialogNewFolder));
    addEntryAction->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
    edit->setIcon(style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    lock->setIcon(style()->standardIcon(QStyle::SP_VistaShield));
    auto* central = new QWidget(this); auto* layout = new QVBoxLayout(central);
    // The wide splitter's size hint must not prevent entering the narrow dialog mode.
    central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->setSizeConstraint(QLayout::SetNoConstraint); setMinimumSize(480, 400);
    layout->setContentsMargins(18, 16, 18, 12); layout->setSpacing(14);
    auto* header = new QWidget(central); header->setObjectName(QStringLiteral("mainHeader"));
    headerLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, header); headerLayout_->setContentsMargins(0, 0, 0, 0); headerLayout_->setSpacing(14);
    auto* heading = new QLabel(QStringLiteral("Личное хранилище"), header);
    auto headingFont = heading->font(); headingFont.setPointSize(headingFont.pointSize() + 5); headingFont.setBold(true); heading->setFont(headingFont);
    headerLayout_->addWidget(heading, 1);
    auto* controls = new QWidget(central); auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    search_ = new QLineEdit(controls); search_->setObjectName(QStringLiteral("vaultSearch")); search_->setAccessibleName(QStringLiteral("Поиск по хранилищу"));
    search_->setPlaceholderText(QStringLiteral("Поиск по названиям, логинам, проектам и заметкам — во всём хранилище"));
    search_->setPlaceholderText(QStringLiteral("Поиск по хранилищу…  Ctrl+K")); search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(180); headerLayout_->addWidget(search_, 2);
    auto* newEntry = new QToolButton(header); newEntry->setDefaultAction(addEntryAction); newEntry->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newEntry->setAccessibleName(QStringLiteral("Новая запись")); headerLayout_->addWidget(newEntry);
    layout->addWidget(header);
    filterLayout_ = controlsLayout;
    typeFilter_ = new QComboBox(controls); typeFilter_->setObjectName(QStringLiteral("typeFilter"));
    typeFilter_->setAccessibleName(QStringLiteral("Фильтр по типу записи"));
    typeFilter_->addItems({QStringLiteral("Все типы"), QStringLiteral("API-токены"), QStringLiteral("Учётные данные"), QStringLiteral("SSH-ключи")});
    sortMode_ = new QComboBox(controls); sortMode_->setObjectName(QStringLiteral("sortMode"));
    sortMode_->setAccessibleName(QStringLiteral("Сортировка записей"));
    sortMode_->addItems({QStringLiteral("По названию"), QStringLiteral("По дате изменения")});
    expiryFilter_ = new QComboBox(controls); expiryFilter_->setObjectName(QStringLiteral("expiryFilter"));
    expiryFilter_->setAccessibleName(QStringLiteral("Фильтр по сроку действия"));
    expiryFilter_->addItems({QStringLiteral("Любой срок"), QStringLiteral("Истекают (30 дн.)"), QStringLiteral("Просрочены")});
    controlsLayout->addWidget(typeFilter_); controlsLayout->addWidget(sortMode_); controlsLayout->addWidget(expiryFilter_);
    tagBar_ = new QWidget(central); tagBar_->setObjectName(QStringLiteral("tagBar"));
    auto* tagLayout = new QHBoxLayout(tagBar_); tagLayout->setContentsMargins(0, 0, 0, 0);
    tagLayout->addStretch(1);
    connect(typeFilter_, &QComboBox::currentIndexChanged, this, [this](int) { refillList(); });
    connect(sortMode_, &QComboBox::currentIndexChanged, this, [this](int) { refillList(); });
    connect(expiryFilter_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (libraryView_ == LibraryView::Expiring) {
            libraryView_ = LibraryView::All;
            libraryAll_->setChecked(true); libraryExpiring_->setChecked(false);
        }
        refillList();
    });
    auto* splitter = new QSplitter(central); splitter_ = splitter;
    splitter->setObjectName(QStringLiteral("mainSplitter")); splitter->setChildrenCollapsible(false);
    splitter->setHandleWidth(10);
    auto* library = new QWidget(splitter); auto* libraryLayout = new QVBoxLayout(library);
    libraryLayout->setContentsMargins(0, 0, 0, 0);
    libraryLayout->addWidget(new QLabel(QStringLiteral("Библиотека"), library));
    libraryAll_ = new QPushButton(library); libraryAll_->setObjectName(QStringLiteral("libraryAll"));
    libraryFavorites_ = new QPushButton(library); libraryFavorites_->setObjectName(QStringLiteral("libraryFavorites"));
    libraryExpiring_ = new QPushButton(library); libraryExpiring_->setObjectName(QStringLiteral("libraryExpiring"));
    for (auto* button : {libraryAll_, libraryFavorites_, libraryExpiring_}) {
        button->setCheckable(true); libraryLayout->addWidget(button);
    }
    connect(libraryAll_, &QPushButton::clicked, this, [this] { setLibraryView(LibraryView::All); });
    connect(libraryFavorites_, &QPushButton::clicked, this, [this] { setLibraryView(LibraryView::Favorites); });
    connect(libraryExpiring_, &QPushButton::clicked, this, [this] { setLibraryView(LibraryView::Expiring); });
    auto* vaultTree = new VaultTree(library); tree_ = vaultTree; libraryLayout->addWidget(tree_, 1);
    library->setMinimumWidth(170); tree_->setMinimumWidth(150);
    auto* newProject = new QToolButton(library); newProject->setDefaultAction(addProjectAction); newProject->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newProject->setAccessibleName(QStringLiteral("Новый проект")); libraryLayout->addWidget(newProject);
    vaultTree->moveRequest = [this](model::Id node, model::Id target) { return moveNodeTo(node, target); };
    vaultTree->setDragEnabled(true); vaultTree->setAcceptDrops(true);
    vaultTree->setDragDropMode(QAbstractItemView::InternalMove); vaultTree->setDefaultDropAction(Qt::MoveAction);
    vaultTree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setHeaderLabel(QStringLiteral("Проекты и группы")); tree_->setAccessibleName(QStringLiteral("Проекты и группы"));
    auto* results = new QWidget(splitter); results->setMinimumWidth(260);
    auto* resultsLayout = new QVBoxLayout(results); resultsLayout->setContentsMargins(0, 0, 0, 0); resultsLayout->setSpacing(10);
    list_ = new QListWidget(results); list_->setAccessibleName(QStringLiteral("Список записей")); list_->setMinimumWidth(160); list_->setSpacing(4);
    list_->setWordWrap(true); list_->setUniformItemSizes(true);
    right_ = new QStackedWidget(splitter); right_->setObjectName(QStringLiteral("entryPanel"));
    auto* hint = new QLabel(QStringLiteral("Создайте проект и добавьте запись или выберите существующую запись."), right_);
    emptyHint_ = hint;
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); right_->addWidget(hint);
    cardScroll_ = new QScrollArea(right_); cardScroll_->setWidgetResizable(true); cardScroll_->setFrameShape(QFrame::NoFrame);
    card_ = new EntryCard(app_, cardScroll_); cardScroll_->setWidget(card_); right_->addWidget(cardScroll_);
    right_->setMinimumWidth(260);
    splitter->addWidget(library); splitter->addWidget(results); splitter->addWidget(right_); splitter->setSizes({250,350,640});
    searchScope_ = new QLabel(central); searchScope_->setObjectName(QStringLiteral("searchScope")); searchScope_->setTextFormat(Qt::PlainText);
    listSummary_ = new QLabel(central); listSummary_->setObjectName(QStringLiteral("listSummary")); listSummary_->setTextFormat(Qt::PlainText);
    searchScope_->setWordWrap(true); listSummary_->setWordWrap(true);
    resultsLayout->addWidget(searchScope_); resultsLayout->addWidget(listSummary_); resultsLayout->addWidget(controls);
    auto* tagsScroll = new QScrollArea(results); tagsScroll->setWidgetResizable(true); tagsScroll->setFrameShape(QFrame::NoFrame);
    tagsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); tagsScroll->setFixedHeight(44); tagsScroll->setWidget(tagBar_);
    resultsLayout->addWidget(tagsScroll); resultsLayout->addWidget(list_, 1);
    layout->addWidget(splitter, 1);
    for (auto* combo : {typeFilter_, sortMode_, expiryFilter_}) { combo->setMinimumContentsLength(7); combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); }
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::treeMenu);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, &MainWindow::listMenu);
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, this, [this] { search_->setFocus(Qt::ShortcutFocusReason); search_->selectAll(); });
    connect(new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this), &QShortcut::activated, this, [this] { search_->setFocus(Qt::ShortcutFocusReason); search_->selectAll(); });
    auto* clearSearch = new QShortcut(QKeySequence(Qt::Key_Escape), search_); clearSearch->setContext(Qt::WidgetShortcut);
    connect(clearSearch, &QShortcut::activated, search_, &QLineEdit::clear);
    auto* deleteEntry = new QShortcut(QKeySequence::Delete, list_); deleteEntry->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteEntry, &QShortcut::activated, this, &MainWindow::deleteSelected);
    auto* deleteNode = new QShortcut(QKeySequence::Delete, tree_); deleteNode->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteNode, &QShortcut::activated, this, &MainWindow::deleteSelectedNode);
    error_ = new QLabel(central); error_->setObjectName(QStringLiteral("operationError")); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_);
    setCentralWidget(central);
    auto* statuses = new QWidget(central); auto* statusLayout = new QGridLayout(statuses);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    vaultInfo_ = new QLabel(statuses); vaultInfo_->setObjectName(QStringLiteral("vaultState"));
    idleStatus_ = new QLabel(statuses); idleStatus_->setObjectName(QStringLiteral("idleStatus"));
    clipboardStatus_ = new QLabel(statuses); clipboardStatus_->setObjectName(QStringLiteral("clipboardStatus"));
    hotkeyStatus_ = new QLabel(statuses); hotkeyStatus_->setObjectName(QStringLiteral("hotkeyStatus"));
    int statusIndex = 0;
    for (auto* label : {vaultInfo_, idleStatus_, clipboardStatus_, hotkeyStatus_}) {
        label->setTextFormat(Qt::PlainText); label->setWordWrap(true); label->setMinimumWidth(0);
        statusLayout->addWidget(label, statusIndex / 2, statusIndex % 2); ++statusIndex;
    }
    statusLayout->setColumnStretch(0, 1); statusLayout->setColumnStretch(1, 1);
    layout->addWidget(statuses);
    auto* statusTimer = new QTimer(this); statusTimer->setObjectName(QStringLiteral("statusRefreshTimer")); statusTimer->setInterval(1000);
    connect(statusTimer, &QTimer::timeout, this, &MainWindow::refreshStatus); statusTimer->start();
    connect(tree_, &QTreeWidget::currentItemChanged, this, [this] { if (tree_->currentItem()) setLibraryView(LibraryView::Group); });
    connect(search_, &QLineEdit::textChanged, this, [this] { refillList(); });
    connect(list_, &QListWidget::currentItemChanged, this, &MainWindow::showMetadata);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { if (!narrow_) editSelected(); });
    connect(list_, &QListWidget::itemClicked, this, &MainWindow::openCardDialog);
    connect(list_, &QListWidget::itemActivated, this, &MainWindow::openCardDialog);
    connect(card_, &EntryCard::editRequested, this, &MainWindow::editSelected);
    connect(card_, &EntryCard::moreRequested, this, [this] {
        const auto* more = card_->findChild<QPushButton*>(QStringLiteral("cardMore"));
        showEntryMenu(more->mapToGlobal(QPoint(0, more->height())));
    });
    connect(card_, &EntryCard::copyRequested, this, [this](const QString& text) {
        if (!ready()) return; QString error;
        if (!clipboard_.copySecret(text, &error)) { if (app_.gate() == Application::Gate::Ready) report(error); }
        else {
            statusBar()->showMessage(QStringLiteral("Скопировано. Очистка буфера через %1 секунд.").arg(clipboard_.clearSeconds()), 5000);
            emit notifyRequested(QStringLiteral("Секрет скопирован. Буфер очистится через %1 секунд.").arg(clipboard_.clearSeconds()));
        }
    });
    connect(&clipboard_, &ClipboardGuard::cleared, this, [this] { emit notifyRequested(QStringLiteral("Буфер обмена очищен")); });
    connect(&app_, &Application::busyChanged, this, [this](bool busy) { updateEnabled(); refreshStatus(); statusBar()->showMessage(busy ? QStringLiteral("Выполняется операция…") : QString()); });
    connect(&app_, &Application::stateChanged, this, [this] {
        if (app_.gate() != Application::Gate::Ready) {
            clipboard_.clearIfOurs(); card_->clear();
            for (auto* dialog : findChildren<QDialog*>()) {
                if (auto* editDialog = qobject_cast<EntryEditDialog*>(dialog)) editDialog->clearSecrets();
                dialog->reject();
            }
            search_->clear(); error_->clear(); saveUiState(); hide();
        }
        refreshAll();
    });
#ifdef Q_OS_WIN
    hotkey_ = std::make_unique<platform::QuickPaste>(app_, clipboard_, this);
    connect(hotkey_.get(), &platform::QuickPaste::conflictDetected, this, [this](const QString& combo) {
        report(QStringLiteral("Горячая клавиша %1 не зарегистрирована. Выберите сочетание в настройках.").arg(combo));
    });
    hotkey_->setDefaultField(appliedPreferences_.defaultField);
    QString hotkeyError; hotkey_->start(appliedPreferences_.hotkey,&hotkeyError);
#endif
    card_->setRevealSeconds(appliedPreferences_.revealSeconds);
    clipboard_.setClearSeconds(appliedPreferences_.clipboardSeconds);
    refreshAll(); restoreUiState();
}
MainWindow::~MainWindow() { saveUiState(); }
void MainWindow::showSettings() {
    if(!ready() || restoring_) return;
    card_->hideNowForTest();
    appliedPreferences_.clearEnvironmentOnExit=prefs::read(*preferences_).clearEnvironmentOnExit;
    SettingsDialog dialog(appliedPreferences_,app_.backupInfo(),
#ifdef Q_OS_WIN
        hotkey_->sequence().toString(QKeySequence::NativeText),
#else
        QString(),
#endif
        this);
    dialog.setUnlockMode(app_.mode(),helloAvailable_);
    connect(&dialog,&SettingsDialog::saveRequested,this,[&] {
        if(restoring_) return;
        if(!ready()) {dialog.reject(); return;}
        const auto result=applyPreferences(dialog.draft());
        if(ready()) dialog.setApplyResult(result,appliedPreferences_);
    });
    connect(&dialog,&SettingsDialog::changePasswordRequested,this,[&] {
        changeMasterPassword(); if(ready()) dialog.setUnlockMode(app_.mode(),helloAvailable_);
    });
    connect(&dialog,&SettingsDialog::changeUnlockModeRequested,this,[&] {
        switchUnlockMode(); if(ready()) dialog.setUnlockMode(app_.mode(),helloAvailable_);
    });
    connect(&dialog,&SettingsDialog::restoreBackupRequested,this,&MainWindow::restoreBackup);
#ifdef Q_OS_WIN
    const bool suspended=hotkey_->suspended(); hotkey_->setSuspended(true);
    const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
    connect(&dialog,&SettingsDialog::clearEnvironmentRequested,this,&MainWindow::removeExportedVariables);
#endif
    dialog.exec();
}
void MainWindow::restoreBackup() {
    QPointer<SettingsDialog> dialog(qobject_cast<SettingsDialog*>(sender()));
    if(!dialog || dialog->parentWidget()!=this || !ready() || restoring_
        || dialog->draft()!=appliedPreferences_) return;
    QPointer<MainWindow> self(this); auto* core=&app_; const auto generation=core->generation();
    restoring_=true;
    const auto cancelOnClose=connect(dialog.data(),&QDialog::finished,core,&Application::cancelBackupRestore);
    const auto stop=qScopeGuard([core,self,cancelOnClose] {
        QObject::disconnect(cancelOnClose); core->cancelBackupRestore(); if(self) self->restoring_=false;
    });
    const auto allowed=[&] {
        return self && dialog && dialog->isVisible() && self->ready() && core->generation()==generation
            && dialog->draft()==self->appliedPreferences_;
    };
    dialog->setOperationError({}); core->cancelBackupRestore();
    Application::Mode mode=Application::Mode::None; QString error;
    const bool hinted=runVaultOperation(*core,[&](void*,QString* err) {
        mode=core->backupMode(err); return mode!=Application::Mode::None;
    },&error,this);
    if(!allowed()) return;
    if(!hinted) {dialog->setOperationError(error); return;}
    const auto backupInfo=core->backupInfo();
    if(backupInfo.state!=Application::BackupInfo::State::Available) {
        dialog->setOperationError(QStringLiteral("Резервная копия недоступна.")); return;
    }
#ifndef Q_OS_WIN
    if(mode==Application::Mode::Hello) {dialog->setOperationError(QStringLiteral("Копия Windows Hello требует Windows.")); return;}
#endif
    SecureBytes password;
    {
        UnlockDialog auth(mode==Application::Mode::Hello?UnlockDialog::Kind::Hello:UnlockDialog::Kind::Password,this);
        auth.setWindowTitle(QStringLiteral("Проверка резервной копии"));
        auth.setPrompt(mode==Application::Mode::Password
            ?QStringLiteral("Введите пароль резервной копии. Он может отличаться от текущего мастер-пароля. Пока основной файл не изменяется.")
            :QStringLiteral("Подтвердите доступ к резервной копии через Windows Hello на этом компьютере. Пока основной файл не изменяется."),
            QStringLiteral("Проверить копию"));
        if(auth.exec()!=QDialog::Accepted || !allowed()) {auth.clearSecrets(); return;}
        password=SecureBytes(auth.password()); auth.clearSecrets();
    }
    const bool prepared=runVaultOperation(*core,[&](void* hwnd,QString* err) {
#ifdef Q_OS_WIN
        if(mode==Application::Mode::Hello) return core->prepareHelloBackupRestore(hwnd,err);
#else
        Q_UNUSED(hwnd);
#endif
        return core->prepareBackupRestore(password.bytes(),err);
    },&error,this);
    password.clear();
    if(!allowed()) return;
    if(!prepared) {dialog->setOperationError(error); return;}
    QMessageBox confirm(QMessageBox::Warning,QStringLiteral("Подтвердить восстановление"),
        QStringLiteral("Заменить текущее хранилище проверенной резервной копией?\nКопия изменена: %1\n\nБудет создана зашифрованная страховочная копия текущего файла. Изменения после резервной копии исчезнут из основного хранилища. После восстановления потребуется разблокировка паролем или Hello этой копии.").arg(backupInfo.modified.toLocalTime().toString(Qt::ISODate)),
        QMessageBox::Yes|QMessageBox::No,this);
    confirm.setTextFormat(Qt::PlainText); confirm.setDefaultButton(QMessageBox::No); confirm.setEscapeButton(QMessageBox::No);
    if(confirm.exec()!=QMessageBox::Yes || !allowed()) return;
    store::RestoreResult result;
    runVaultOperation(*core,[&](void*,QString* err) {
        result=core->restorePreparedBackup(); if(err) *err=result.error;
        return result.committed && result.error.isEmpty();
    },&error,this);
    if(!self || self->exiting_) return;
    if(!result.committed) {if(dialog && dialog->isVisible()) dialog->setOperationError(error); return;}
    const auto message=(result.error.isEmpty()?QStringLiteral("Резервная копия восстановлена. Хранилище заблокировано."):result.error)
        +QStringLiteral("\n\nСтраховочная копия: %1").arg(result.safetyPath);
    QMessageBox outcome(result.error.isEmpty()?QMessageBox::Information:QMessageBox::Warning,
        QStringLiteral("Результат восстановления"),message,QMessageBox::Ok,this);
    outcome.setTextFormat(Qt::PlainText); outcome.exec();
}
prefs::ApplyResult MainWindow::applyPreferences(const prefs::Values& requested) {
    prefs::ApplyResult result;
    result.error=prefs::validate(requested);
    if(!result.error.isEmpty()) {result.failedKey=QStringLiteral("hotkey"); return result;}
    const auto allowed=[&](const QString& key) {
        if(ready()) return true;
        result.failedKey=key; result.error=QStringLiteral("Хранилище заблокировано или занято."); return false;
    };
#ifdef Q_OS_WIN
    if(requested.hotkey!=appliedPreferences_.hotkey) {
        if(!allowed("hotkey")) return result;
        if(!hotkey_->changeSequence(requested.hotkey,*preferences_,&result.error)) {result.failedKey="hotkey"; return result;}
        appliedPreferences_.hotkey=requested.hotkey; result.appliedKeys<<"hotkey"; refreshStatus();
    }
    if(appliedPreferences_.autostart && requested.autostart && requested.autostart!=appliedPreferences_.autostart) {
        if(!allowed("autostart")) return result;
        if(!platform::autostart::setEnabled(*requested.autostart,*runPreferences_,&result.error)) {result.failedKey="autostart"; return result;}
        appliedPreferences_.autostart=requested.autostart; result.appliedKeys<<"autostart";
    }
#endif
    const auto persist=[&](const QString& key,const auto& value,auto& current,const QVariant& stored,auto effect) {
        if(value==current) return true;
        if(!allowed(key)) return false;
        if(!prefs::writeValue(*preferences_,key,stored,&result.error)) {result.failedKey=key; return false;}
        current=value; result.appliedKeys<<key; effect(); return true;
    };
    if(!persist("theme",requested.theme,appliedPreferences_.theme,requested.theme,[&] {
        theme::apply(requested.theme=="dark"?theme::Mode::Dark:requested.theme=="light"?theme::Mode::Light:theme::Mode::System);
    })) return result;
    if(!persist("trayNotifications",requested.notifications,appliedPreferences_.notifications,requested.notifications,[] {})) return result;
    if(!persist("idleTimeoutMinutes",requested.idleMinutes,appliedPreferences_.idleMinutes,requested.idleMinutes,[&] {emit idleTimeoutChanged(requested.idleMinutes);})) return result;
    if(!persist("secretRevealSeconds",requested.revealSeconds,appliedPreferences_.revealSeconds,requested.revealSeconds,[&] {card_->setRevealSeconds(requested.revealSeconds);})) return result;
    if(!persist("clipboardClearSeconds",requested.clipboardSeconds,appliedPreferences_.clipboardSeconds,requested.clipboardSeconds,[&] {clipboard_.setClearSeconds(requested.clipboardSeconds);})) return result;
#ifdef Q_OS_WIN
    if(!persist("quickPasteDefaultField",requested.defaultField,appliedPreferences_.defaultField,
        requested.defaultField==paste::Field::Login?"login":"secret",[&] {hotkey_->setDefaultField(requested.defaultField);})) return result;
#endif
    const auto closing=requested.closeBehavior==prefs::CloseBehavior::Exit?"exit":requested.closeBehavior==prefs::CloseBehavior::LockHide?"lock-hide":"hide";
    if(!persist("closeBehavior",requested.closeBehavior,appliedPreferences_.closeBehavior,closing,[] {})) return result;
#ifdef Q_OS_WIN
    if(!persist("env-clear-on-exit",requested.clearEnvironmentOnExit,appliedPreferences_.clearEnvironmentOnExit,requested.clearEnvironmentOnExit,[] {})) return result;
#endif
    refreshStatus(); return result;
}
void MainWindow::setIdleTimer(IdleTimer* timer) { idleTimer_ = timer; refreshStatus(); }
void MainWindow::refreshStatus() {
    vaultInfo_->setText(app_.gate() != Application::Gate::Ready ? QStringLiteral("Заблокировано")
        : app_.busy() ? QStringLiteral("Выполняется операция…") : QStringLiteral("Разблокировано"));
    const int remaining = idleTimer_ ? idleTimer_->remainingMs() : -1;
    idleStatus_->setText(!idleTimer_ ? QStringLiteral("Автоблокировка: нет данных")
        : remaining < 0 ? QStringLiteral("Автоблокировка: неактивна")
        : QStringLiteral("Автоблокировка через %1:%2").arg((remaining + 999) / 1000 / 60)
            .arg((remaining + 999) / 1000 % 60, 2, 10, QLatin1Char('0')));
    const auto clip = clipboard_.snapshot();
    clipboardStatus_->setText(!clip.owned ? QStringLiteral("Буфер: нет своей публикации")
        : clip.clearPending ? QStringLiteral("Буфер: очистка ожидает освобождения")
        : QStringLiteral("Очистка своего буфера через %1 с").arg(qMax(0, (clip.remainingMs + 999) / 1000)));
#ifdef Q_OS_WIN
    const auto sequence = hotkey_ ? hotkey_->sequence() : QKeySequence();
    hotkeyStatus_->setText(sequence.isEmpty() ? QStringLiteral("Быстрая вставка: не зарегистрировано")
        : QStringLiteral("Быстрая вставка: %1").arg(sequence.toString(QKeySequence::NativeText)));
#else
    hotkeyStatus_->setText(QStringLiteral("Глобальная вставка недоступна на этой платформе"));
#endif
    const auto date = QDateTime::currentDateTimeUtc().date();
    if (date != statusDate_) { statusDate_ = date; updateLibraryCounts(); refillList(); }
}
void MainWindow::restoreUiState() {
    resize(QSize(1240, 800).boundedTo(screen()->availableGeometry().size()));
    const auto geometry = preferences_->value(QStringLiteral("ui/mainGeometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto area = screen()->availableGeometry().adjusted(8, 32, -8, -8);
    const auto state = preferences_->value(QStringLiteral("ui/mainSplitter")).toByteArray();
    if (state.isEmpty() || !splitter_->restoreState(state)) splitter_->setSizes({250,350,640});
    resize(size().boundedTo(area.size()));
    move(qBound(area.left(), x(), area.right() - width() + 1), qBound(area.top(), y(), area.bottom() - height() + 1));
}
void MainWindow::saveUiState() {
    preferences_->setValue(QStringLiteral("ui/mainGeometry"), saveGeometry());
    preferences_->setValue(QStringLiteral("ui/mainSplitter"), splitter_->saveState());
    preferences_->sync();
    if (preferences_->status() != QSettings::NoError && !geometryErrorReported_) {
        geometryErrorReported_ = true;
        report(QStringLiteral("Положение окна не сохранено."));
        emit notifyRequested(QStringLiteral("Положение окна не сохранено."));
    }
}
bool MainWindow::ready() const { return app_.gate() == Application::Gate::Ready && !app_.busy(); }
void MainWindow::updateEnabled() {
    centralWidget()->setEnabled(ready()); for (auto* action : mutations_) action->setEnabled(ready());
#ifdef Q_OS_WIN
    if (envExportAction_) envExportAction_->setEnabled(canExportEnv());
#endif
}
model::Id MainWindow::selectedNode() const { const auto* item = tree_->currentItem(); return item ? item->data(0, Qt::UserRole).value<model::Id>() : model::Id{}; }
model::Id MainWindow::selectedEntry() const { const auto* item = list_->currentItem(); return item ? item->data(Qt::UserRole).value<model::Id>() : model::Id{}; }
void MainWindow::selectNode(model::Id id) {
    for (QTreeWidgetItemIterator item(tree_); *item; ++item) if ((*item)->data(0, Qt::UserRole).value<model::Id>() == id) {
        tree_->setCurrentItem(*item); tree_->scrollToItem(*item); return;
    }
}
QString MainWindow::groupPath(model::Id id) const {
    QStringList parts;
    for (int depth = 0; !id.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
        const auto it = app_.model()->nodes().constFind(id);
        if (it == app_.model()->nodes().cend()) break;
        parts.prepend(it->name); id = it->parent;
    }
    return parts.join(QStringLiteral(" / "));
}
void MainWindow::setLibraryView(LibraryView view) {
    libraryView_ = view;
    if (view != LibraryView::Group) { QSignalBlocker block(tree_); tree_->setCurrentItem(nullptr); tree_->clearSelection(); }
    libraryAll_->setChecked(view == LibraryView::All);
    libraryFavorites_->setChecked(view == LibraryView::Favorites);
    libraryExpiring_->setChecked(view == LibraryView::Expiring);
    { QSignalBlocker block(expiryFilter_); expiryFilter_->setCurrentIndex(view == LibraryView::Expiring ? 1 : 0); }
    refillList();
}
void MainWindow::updateLibraryCounts() {
    int all = 0, favorites = 0, expiring = 0;
    if (app_.gate() == Application::Gate::Ready) {
        for (const auto& entry : app_.model()->entries()) {
            ++all; favorites += entry.favorite;
            const auto days = QDateTime::currentDateTimeUtc().daysTo(entry.expiresAt);
            expiring += entry.expiresAt.isValid() && days >= 0 && days <= model::VaultModel::ExpiryWarningDays;
        }
    }
    libraryAll_->setText(QStringLiteral("Все записи · %1").arg(all));
    libraryFavorites_->setText(QStringLiteral("Избранное · %1").arg(favorites));
    libraryExpiring_->setText(QStringLiteral("Истекают · %1").arg(expiring));
    libraryAll_->setChecked(libraryView_ == LibraryView::All);
    libraryFavorites_->setChecked(libraryView_ == LibraryView::Favorites);
    libraryExpiring_->setChecked(libraryView_ == LibraryView::Expiring);
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
        if (selection && libraryView_ == LibraryView::Group) tree_->setCurrentItem(selection);
        else if (libraryView_ == LibraryView::Group) libraryView_ = LibraryView::All;
    }
    // Порядок важен: refillTagBar сначала обрезает «мёртвые» выбранные теги (после
    // переименования/удаления выбранного чипа) и лишь затем refillList рисует список —
    // иначе список фильтруется по устаревшему набору уже несуществующим тегом.
    displayedEntry_ = {}; // Explicit model refresh may change metadata of the same UUID.
    refillTagBar(); updateLibraryCounts();
    refillList(entry); updateEnabled(); updateSwitchActionText();
    if (app_.gate() == Application::Gate::Ready) {
        const auto mode = app_.mode() == Application::Mode::Hello ? QStringLiteral("Windows Hello") : QStringLiteral("мастер-пароль");
        vaultInfo_->setText(QStringLiteral("Записей: %1 · Проектов и групп: %2 · Разблокировка: %3")
            .arg(app_.model()->entries().size()).arg(app_.model()->nodes().size()).arg(mode));
    } else vaultInfo_->clear();
    refreshStatus();
}
void MainWindow::refillList(model::Id selected) {
    if (selected.isNull()) selected = selectedEntry();
    QSignalBlocker block(list_); list_->clear();
    int baseCount = 0;
    if (app_.gate() == Application::Gate::Ready) {
        auto ids = !search_->text().isEmpty() ? app_.model()->search(search_->text())
            : libraryView_ == LibraryView::Group ? app_.model()->entriesInGroup(selectedNode()) : app_.model()->entries().keys();
        if (libraryView_ == LibraryView::Favorites)
            ids.erase(std::remove_if(ids.begin(), ids.end(), [this](model::Id id) { return !app_.model()->entry(id)->favorite; }), ids.end());
        baseCount = ids.size();
        const int typeFilter = typeFilter_->currentIndex() - 1; // -1 — все типы
        if (typeFilter >= 0)
            ids.erase(std::remove_if(ids.begin(), ids.end(), [this, typeFilter](model::Id id) {
                const auto* entry = app_.model()->entry(id);
                return !entry || int(entry->type) != typeFilter;
            }), ids.end());
        // Семантика И: запись проходит, только если содержит каждый выбранный тег (без учёта регистра);
        // фильтр типа и теги действуют одновременно.
        // Срок действия: 1 — истекают в горизонте 30 дней, 2 — просрочены (дата UTC-полночь).
        const int expiryMode = expiryFilter_->currentIndex();
        if (expiryMode > 0)
            ids.erase(std::remove_if(ids.begin(), ids.end(), [this, expiryMode](model::Id id) {
                const auto* entry = app_.model()->entry(id);
                if (!entry || !entry->expiresAt.isValid()) return true; // без срока не попадают ни в одну ветку
                const qint64 days = QDateTime::currentDateTimeUtc().daysTo(entry->expiresAt);
                return expiryMode == 1 ? !(days >= 0 && days <= model::VaultModel::ExpiryWarningDays) : days >= 0;
            }), ids.end());
        if (!selectedTags_.isEmpty())
            ids.erase(std::remove_if(ids.begin(), ids.end(), [this](model::Id id) {
                const auto* entry = app_.model()->entry(id);
                if (!entry) return true;
                for (const QString& selected : selectedTags_) {
                    const bool has = std::any_of(entry->tags.cbegin(), entry->tags.cend(), [&](const QString& tag) {
                        return tag.compare(selected, Qt::CaseInsensitive) == 0; });
                    if (!has) return true;
                }
                return false;
            }), ids.end());
        const bool byDate = sortMode_->currentIndex() == 1;
        std::sort(ids.begin(), ids.end(), [this, byDate](model::Id a, model::Id b) {
            const auto *first = app_.model()->entry(a), *second = app_.model()->entry(b);
            if (first->favorite != second->favorite) return first->favorite;
            return byDate ? first->modified > second->modified : first->title < second->title;
        });
        for (auto id : ids) {
            const auto* entry = app_.model()->entry(id);
            QString title = entry->favorite ? QStringLiteral("★ ") + entry->title : entry->title;
            const auto type = entry->type == model::EntryType::ApiToken ? QStringLiteral("API-токен")
                : entry->type == model::EntryType::Credentials ? QStringLiteral("Учётные данные") : QStringLiteral("SSH-ключ");
            title += QStringLiteral("\n%1 · %2%3").arg(type, groupPath(entry->groupId), entry->reverify ? QStringLiteral(" · Повторная проверка") : QString());
            // Пометка срока: ⏳ истекает в горизонте 30 дней, ⏰ просрочен (с датой).
            if (entry->expiresAt.isValid()) {
                const qint64 days = QDateTime::currentDateTimeUtc().daysTo(entry->expiresAt);
                const QString dateText = entry->expiresAt.toLocalTime().toString(QStringLiteral("dd.MM.yyyy"));
                if (days < 0) title += QStringLiteral("  ⏰ просрочен %1").arg(dateText);
                else if (days <= model::VaultModel::ExpiryWarningDays) title += QStringLiteral("  ⏳ до %1").arg(dateText);
            }
            auto* item = new QListWidgetItem(title, list_); item->setData(Qt::UserRole, id);
            if (id == selected) list_->setCurrentItem(item);
            if (entry->expiresAt.isValid() && QDateTime::currentDateTimeUtc() > entry->expiresAt)
                item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text)); // просроченные приглушены
        }
        if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
    }
    searchScope_->setText(!search_->text().isEmpty() ? QStringLiteral("Поиск по всему хранилищу")
        : libraryView_ == LibraryView::Group ? groupPath(selectedNode()) : QStringLiteral("Библиотека"));
    listSummary_->setText(QStringLiteral("Показано %1 из %2").arg(list_->count()).arg(baseCount));
    if (!list_->count()) {
        const auto message = app_.model()->nodes().isEmpty() ? QStringLiteral("Создайте проект кнопкой «Новый проект», затем добавьте запись.")
            : !search_->text().isEmpty() || typeFilter_->currentIndex() || expiryFilter_->currentIndex() || !selectedTags_.isEmpty()
                ? QStringLiteral("Нет записей с выбранными фильтрами.")
            : libraryView_ == LibraryView::Favorites ? QStringLiteral("Избранных записей пока нет.")
            : libraryView_ == LibraryView::Group ? QStringLiteral("В этой группе пока нет записей.") : QStringLiteral("Добавьте первую запись.");
        emptyHint_->setText(message);
    }
    list_->setAccessibleName(QStringLiteral("Список записей"));
    showMetadata();
}
void MainWindow::refillTagBar() {
    const auto tags = app_.gate() == Application::Gate::Ready ? app_.model()->allTags() : QStringList();
    // Обрезка «мёртвых» выбранных тегов: после переименования/удаления или блокировки
    // тега может уже не быть — выбор не должен фильтровать список несуществующим тегом.
    QStringList alive;
    for (const QString& selected : selectedTags_)
        if (std::any_of(tags.cbegin(), tags.cend(), [&](const QString& tag) { return tag.compare(selected, Qt::CaseInsensitive) == 0; }))
            alive << selected;
    selectedTags_ = alive;
    // Пересборка: чипы заменяются целиком; stretch вынимается отдельно и возвращается в конец —
    // иначе он был бы удалён вместе с чипами и чипы перестали бы прижиматься к левому краю.
    QLayoutItem* stretch = nullptr;
    while (auto* item = tagBar_->layout()->takeAt(0)) {
        if (item->widget()) {
            item->widget()->deleteLater();
            delete item; // после takeAt обёртка QLayoutItem принадлежит нам — удаляем, чтобы не утекала
        } else stretch = item;
    }
    auto* layout = static_cast<QHBoxLayout*>(tagBar_->layout());
    for (const QString& tag : tags) {
        int uses = 0;
        for (const auto& entry : app_.model()->entries()) uses += entry.tags.contains(tag, Qt::CaseInsensitive) ? 1 : 0;
        auto* chip = new QToolButton(tagBar_);
        chip->setText(tag); chip->setCheckable(true); chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chip->setToolTip(QStringLiteral("Записей: %1").arg(uses));
        chip->setChecked(std::any_of(selectedTags_.cbegin(), selectedTags_.cend(),
            [&](const QString& selected) { return selected.compare(tag, Qt::CaseInsensitive) == 0; }));
        connect(chip, &QToolButton::clicked, this, [this, tag] {
            auto match = std::find_if(selectedTags_.begin(), selectedTags_.end(), [&](const QString& selected) {
                return selected.compare(tag, Qt::CaseInsensitive) == 0; });
            if (match != selectedTags_.end()) selectedTags_.erase(match); else selectedTags_ << tag;
            refillList();
        });
        chip->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(chip, &QToolButton::customContextMenuRequested, this, [this, tag, chip] {
            QMenu menu(chip);
            menu.addAction(QStringLiteral("Переименовать тег…"), this, [this, tag] { renameTag(tag); });
            menu.addAction(QStringLiteral("Удалить тег…"), this, [this, tag] { removeTag(tag); });
            menu.exec(QCursor::pos());
        });
        layout->addWidget(chip);
    }
    if (stretch) layout->addItem(stretch); else layout->addStretch(1);
}
void MainWindow::renameTag(const QString& tag) {
    bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Переименование тега"),
        QStringLiteral("Новое имя тега «%1»:").arg(tag), QLineEdit::Normal, tag, &ok);
    if (!ok) return;
    commit([&](auto& model) { return model.renameTag(tag, name); });
}
void MainWindow::removeTag(const QString& tag) {
    if (QMessageBox::question(this, QStringLiteral("Удаление тега"),
            QStringLiteral("Удалить тег «%1» со всех записей?").arg(tag),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    commit([&](auto& model) { return model.removeTag(tag); });
}
void MainWindow::showMetadata() {
    const auto* entry = app_.gate() == Application::Gate::Ready ? app_.model()->entry(selectedEntry()) : nullptr;
    right_->setCurrentIndex(entry ? 1 : 0); if (!entry) { displayedEntry_ = {}; card_->clear(); return; }
    if (displayedEntry_ != entry->id) { displayedEntry_ = entry->id; card_->showEntry(entry->id); }
#ifdef Q_OS_WIN
    if (envExportAction_) envExportAction_->setEnabled(canExportEnv()); // доступность зависит от выбора
#endif
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
bool MainWindow::commitEntry(const std::function<QString(model::VaultModel&)>& mutation, model::Id entryId) {
    if (!ready()) return false;
    std::optional<model::Entry> backup;
    if (const auto* existing = app_.model()->entry(entryId)) backup = *existing;
    const auto generation = app_.generation();
    // lock очищает модель во время мутации/сохранения — откатывать уже нечем и незачем.
    const auto connection = connect(&app_, &Application::stateChanged, this,
        [&backup, &app = app_, generation] { if (app.generation() != generation) backup.reset(); });
    auto cleanup = qScopeGuard([&] { disconnect(connection); });
    const QString error = mutation(*app_.model());
    if (!error.isEmpty()) { rollbackEntry(entryId, backup); report(error); refreshAll(); return false; }
    QString saveError;
    if (!app_.save(&saveError)) {
        if (app_.generation() == generation && app_.gate() == Application::Gate::Ready)
            rollbackEntry(entryId, backup);
        refreshAll();
        if (app_.gate() == Application::Gate::Ready) report(saveError);
        return false;
    }
    error_->clear(); refreshAll(); statusBar()->showMessage(QStringLiteral("Сохранено"), 3000); return true;
}
void MainWindow::rollbackEntry(model::Id entryId, const std::optional<model::Entry>& backup) {
    // Была запись — вернуть как была; новой записи не было — удалить черновик мутации.
    if (backup) app_.model()->setEntry(*backup);
    else app_.model()->removeEntry(entryId);
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
    if (!ready() || editing_) return; const auto parent = selectedNode();
    if (app_.model()->nodes().isEmpty()) { report(QStringLiteral("Создайте проект кнопкой «Новый проект».")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    EntryEditDialog dialog(*app_.model(), std::nullopt, this); dialog.setGroup(parent);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto entry = dialog.entry(); if (entry) commitEntry([&](auto& model) { return model.setEntry(*entry); }, entry->id);
    }
}
void MainWindow::editSelected() {
    if (!ready() || editing_) return; const auto* entry = app_.model()->entry(selectedEntry()); if (!entry) { report(QStringLiteral("Выберите запись.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    // Редактор показывает секреты записи — для записей с повторной проверкой сначала
    // подтверждаем владельца, как при показе/копировании (audit M-1).
    if (entry->reverify && !VerifyDialog::verify(app_, this, QStringLiteral("изменение записи"))) return;
    EntryEditDialog dialog(*app_.model(), *entry, this);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto changed = dialog.entry(); if (changed) commitEntry([&](auto& model) { return model.setEntry(*changed); }, changed->id);
    }
}
void MainWindow::renameSelectedNode() {
    if (!ready() || editing_) return; const auto id = selectedNode(); if (id.isNull()) { report(QStringLiteral("Выберите проект или группу.")); return; }
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
        && ready() && app_.generation() == generation) commitEntry([&](auto& model) { return model.removeEntry(id); }, id);
}
void MainWindow::deleteSelectedNode() {
    if (!ready() || editing_) return; const auto id = selectedNode(); if (id.isNull()) { report(QStringLiteral("Выберите проект или группу.")); return; }
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
        sodium_memzero(const_cast<char*>(bytes.constData()), size_t(bytes.size())); // копия секрета в куче не переживает шаг
        return;
    }
    sodium_memzero(const_cast<char*>(bytes.constData()), size_t(bytes.size()));
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
bool MainWindow::moveNodeTo(model::Id node, model::Id newParent) {
    if (!ready() || node == newParent || newParent.isNull()) { refreshAll(); return false; }
    return commit([&](auto& model) { return model.moveNode(node, newParent); });
}
void MainWindow::importFromCsvKeePass() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт из CSV / KeePass XML"), {},
        QStringLiteral("CSV (*.csv);;KeePass XML (*.xml);;Все файлы (*.*)"));
    if (path.isEmpty() || !ready() || app_.generation() != generation) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 80 * 1024 * 1024) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось открыть файл импорта (или он больше 80 МиБ)."));
        return;
    }
    const QByteArray bytes = file.readAll();
    file.close();
    const bool keepass = path.endsWith(QStringLiteral(".xml"), Qt::CaseInsensitive);
    QString error;
    const int added = applyImport(bytes, keepass, &error);
    if (added < 0) { QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return; }
    refreshAll();
    statusBar()->showMessage(QStringLiteral("Импортировано записей: %1").arg(added), 5000);
    QMessageBox::information(this, QStringLiteral("TokenBucket"),
        QStringLiteral("Импортировано записей: %1%2.").arg(added)
            .arg(error.isEmpty() ? QString() : QStringLiteral(" — Пропуски и замечания: %1").arg(error)));
}
int MainWindow::applyImport(const QByteArray& bytes, bool keepassXml, QString* err) {
    const io::ImportResult parsed = keepassXml ? io::parseKeePassXml(bytes) : io::parseCsv(bytes);
    // Все записи селятся под проект «Импорт» (создаётся при необходимости), группы — по пути из файла.
    model::Id root;
    for (auto id : app_.model()->nodes().keys())
        if (app_.model()->nodes().value(id).parent.isNull() && app_.model()->nodes().value(id).name == QLatin1String("Импорт")) root = id;
    if (root.isNull()) {
        root = QUuid::createUuid();
        const QString createError = app_.model()->addNode(root, {}, QStringLiteral("Импорт"));
        if (!createError.isEmpty()) { if (err) *err = createError; return -1; }
    }
    int added = 0;
    QStringList notes;
    for (const auto& imported : parsed.entries) {
        model::Id parent = root;
        for (const QString& part : imported.groupPath) {
            model::Id child;
            for (auto id : app_.model()->nodes().keys())
                if (app_.model()->nodes().value(id).parent == parent && app_.model()->nodes().value(id).name == part) { child = id; break; }
            if (child.isNull()) {
                child = QUuid::createUuid();
                const QString createError = app_.model()->addNode(child, parent, part);
                if (!createError.isEmpty()) { notes << createError; child = parent; break; } // упёрлись в лимит глубины — селятся выше
                parent = child;
            } else parent = child;
        }
        model::Entry e = imported.entry;
        e.id = QUuid::createUuid(); e.groupId = parent;
        e.created = e.modified = QDateTime::currentDateTimeUtc();
        const QString entryError = app_.model()->setEntry(e);
        if (entryError.isEmpty()) ++added; else notes << QStringLiteral("%1: %2").arg(e.title, entryError);
    }
    if (parsed.skipped > 0) notes << QStringLiteral("Пропущено строк файла: %1 (%2)").arg(parsed.skipped).arg(parsed.lastSkipReason);
    // Импорт меняет модель — фиксируем одним сохранением (откат commit() не нужен: ошибки не отменяют уже добавленное).
    QString saveError;
    if (added > 0 && !app_.save(&saveError)) notes << saveError;
    if (err) *err = notes.join(QLatin1Char('\n'));
    return added;
}
void MainWindow::treeMenu(const QPoint& pos) {
    const auto item = tree_->itemAt(pos);
    if (item) tree_->setCurrentItem(item);
    QMenu menu(this);
    menu.addAction(QStringLiteral("Новый проект…"), this, &MainWindow::addProject);
    if (item) {
        menu.addAction(QStringLiteral("Новая группа…"), this, &MainWindow::addGroup);
        menu.addAction(QStringLiteral("Переименовать…"), this, &MainWindow::renameSelectedNode);
        if (!app_.model()->nodes().value(selectedNode()).parent.isNull())
            menu.addAction(QStringLiteral("Переместить группу…"), this, &MainWindow::moveSelectedNode);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Удалить"), this, &MainWindow::deleteSelectedNode);
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}
void MainWindow::listMenu(const QPoint& pos) {
    const auto item = list_->itemAt(pos);
    if (!item || !ready()) return;
    list_->setCurrentItem(item);
    showEntryMenu(list_->viewport()->mapToGlobal(pos));
}
void MainWindow::showEntryMenu(const QPoint& globalPos) {
    if (!ready() || selectedEntry().isNull()) return;
    const auto id = selectedEntry();
    QMenu menu(this);
    menu.addAction(QStringLiteral("Изменить…\tF2"), this, &MainWindow::editSelected);
    menu.addAction(QStringLiteral("Копировать секрет"), this, [this] { card_->copySecret(); });
    const auto* entry = app_.model()->entry(id);
#ifdef Q_OS_WIN
    // Тот же критерий, что и для пункта меню: ApiToken/Credentials с однострочным секретом
    // (eligibleForAutoPaste отсекает и SSH-ключи, и переводы строк).
    if (entry && paste::eligibleForAutoPaste(*entry))
        menu.addAction(QStringLiteral("В переменную окружения…"), this, &MainWindow::exportToEnvironment);
#endif
    QAction* favorite = menu.addAction(QStringLiteral("В избранное"));
    favorite->setCheckable(true); favorite->setChecked(entry && entry->favorite);
    connect(favorite, &QAction::toggled, this, &MainWindow::toggleFavorite);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Удалить\tDel"), this, &MainWindow::deleteSelected);
    menu.exec(globalPos);
}
void MainWindow::toggleFavorite() {
    const auto id = selectedEntry();
    commitEntry([&](auto& model) {
        const auto* current = model.entry(id);
        if (!current) return QString();
        auto changed = *current;
        changed.favorite = !changed.favorite;
        return model.setEntry(changed);
    }, id);
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
void MainWindow::showAbout() { ui::AboutDialog dialog(this); dialog.exec(); }
#ifdef Q_OS_WIN
bool MainWindow::canExportEnv() const {
    if (!ready()) return false;
    const auto* entry = app_.model()->entry(selectedEntry());
    // однострочный токен/пароль: eligibleForAutoPaste заодно отсекает SSH-ключи (многострочные)
    return entry && paste::eligibleForAutoPaste(*entry);
}
void MainWindow::exportToEnvironment() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
    const auto generation = app_.generation();
    const auto* entry = app_.model()->entry(selectedEntry());
    if (!entry || !paste::eligibleForAutoPaste(*entry)) { report(QStringLiteral("Выберите запись с однострочным токеном или паролем.")); return; }
    // Копия секрета в локаль ДО диалогов: VerifyDialog/EnvExportDialog крутят вложенные циклы
    // событий, за которые автолок или QtConcurrent-операция могут очистить модель — указатель
    // entry стал бы висячим. Дальше работаем только с копией и после каждого диалога
    // перепроверяем, что шлюз готов и поколение не сменилось (страж как в saveSshToFile).
    const QString secret = entry->type == model::EntryType::ApiToken ? entry->token : entry->password;
    // NUL в секрете недопустим: и env-блок CreateProcessW, и REG_SZ в реестре молча усекают
    // строку на нём — для security-фичи отказываемся явно, а не отдаём усечённое значение.
    if (secret.contains(QChar(u'\0'))) { report(QStringLiteral("Секрет содержит недопустимые символы для переменной окружения.")); return; }
    if (entry->reverify && !VerifyDialog::verify(app_, this, QStringLiteral("выдача секрета в переменную окружения"))) return;
    if (!ready() || app_.generation() != generation) return;
    EnvExportDialog dialog(this,preferences_);
    const int outcome=dialog.exec();
    appliedPreferences_.clearEnvironmentOnExit=prefs::read(*preferences_).clearEnvironmentOnExit;
    if (outcome != QDialog::Accepted || !ready() || app_.generation() != generation) return;
    QString error;
    if (dialog.mode() == EnvExportDialog::Mode::Temporary) {
        if (!platform::envops::startWithVariable(dialog.program(), dialog.arguments(), dialog.name(), secret, &error)) {
            QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return;
        }
        QMessageBox::information(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Приложение запущено с переменной %1 в своём окружении. Секрет нигде не сохранён и исчезнет вместе с процессом.").arg(dialog.name()));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Значение будет храниться в реестре в открытом виде и будет доступно любому процессу "
                           "от вашего имени. Новые процессы увидят переменную после перезапуска оболочки/проводника "
                           "или выхода из системы. Установить переменную %1?").arg(dialog.name()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if (!ready() || app_.generation() != generation) return;
    if (!platform::envops::setUserVariable(dialog.name(), secret, &error)) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return;
    }
    if(!platform::envops::rememberExported(dialog.name(),preferences_,&error)) {
        QMessageBox::critical(this,QStringLiteral("TokenBucket"),
            QStringLiteral("Переменная установлена, но её имя не сохранено для очистки: %1. %2").arg(dialog.name(),error)); return;
    }
    QMessageBox::information(this, QStringLiteral("TokenBucket"),
        QStringLiteral("Переменная %1 установлена. Новые процессы увидят её после перезапуска оболочки/проводника или выхода из системы.").arg(dialog.name()));
}
void MainWindow::removeExportedVariables() {
    if(!ready()) return;
    const auto generation=app_.generation();
    const QStringList names = platform::envops::exportedNames(preferences_);
    if (names.isEmpty()) {
        QMessageBox::information(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Переменные окружения, установленные TokenBucket, не найдены."));
        return;
    }
    if (QMessageBox::question(this, QStringLiteral("TokenBucket"),
            QStringLiteral("Удалить %1 пользовательских переменных, учтённых TokenBucket?").arg(names.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    if(!ready() || app_.generation()!=generation) return;
    const int removed=platform::envops::clearAllExported(preferences_);
    const int remaining=platform::envops::exportedNames(preferences_).size();
    const auto message=QStringLiteral("Удалено: %1. Осталось в учёте: %2.").arg(removed).arg(remaining);
    if(remaining) QMessageBox::warning(this,QStringLiteral("TokenBucket"),message);
    else QMessageBox::information(this,QStringLiteral("TokenBucket"),message);
}
#endif
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event); narrow_ = width() < 720;
    if (right_) right_->setVisible(!narrow_);
    if (headerLayout_) headerLayout_->setDirection(width() < 960 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    if (filterLayout_) filterLayout_->setDirection(width() < 1240 ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
}
void MainWindow::requestExit() { exiting_ = true; close(); }
void MainWindow::closeEvent(QCloseEvent* event) {
    saveUiState();
    if (exiting_ || !trayAvailable_() || appliedPreferences_.closeBehavior==prefs::CloseBehavior::Exit) {
        app_.lock(); QMainWindow::closeEvent(event); emit closing(); return;
    }
    const bool lockHidden=appliedPreferences_.closeBehavior==prefs::CloseBehavior::LockHide;
    if(lockHidden) emit lockedHiddenRequested();
    hide(); event->ignore();
    if(lockHidden) app_.lock();
}
void MainWindow::popupQuickPanel() {
#ifdef Q_OS_WIN
    if (hotkey_) hotkey_->popupPanel();
#endif
}
void MainWindow::applyIdleTimeout(int minutes) {
    // Кламп 1–60 минут: границы те же, что у спинбокса диалога; защита от
    // испорченных значений настроек и от прямых вызовов вне диапазона.
    minutes = qBound(1, minutes, 60);
    QString error;
    if(!prefs::writeValue(*preferences_,QStringLiteral("idleTimeoutMinutes"),minutes,&error)) {report(error); return;}
    appliedPreferences_.idleMinutes=minutes;
    emit idleTimeoutChanged(minutes);
}
}
