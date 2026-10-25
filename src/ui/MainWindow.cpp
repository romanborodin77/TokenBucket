#include "MainWindow.h"
#include "SettingsDialog.h"
#include "SessionFlow.h"
#include "UnlockDialog.h"
#include "../app/Preferences.h"
#include "EntryEditDialog.h"
#include "EntryAccess.h"
#include "EntryLabels.h"
#include "EntryCard.h"
#include "TextFileDialog.h"
#include "TextImportDialog.h"
#include "RegistryView.h"
#include "../io/RegistryMigration.h"
#include "../model/TextFile.h"
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QKeyEvent>
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
#include <QMap>
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
class MigrationTable:public QTableWidget {
public:
    MigrationTable(int rows,int columns,QWidget* parent):QTableWidget(rows,columns,parent) {qApp->installEventFilter(this);}
    std::function<void()> hideValue;
protected:
    bool eventFilter(QObject* object,QEvent* event) override {
        if(hideValue && (event->type()==QEvent::ApplicationDeactivate || (object==window() && (event->type()==QEvent::WindowDeactivate || event->type()==QEvent::Hide)))) hideValue();
        return QTableWidget::eventFilter(object,event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        if(event->matches(QKeySequence::Copy) || event->matches(QKeySequence::Cut)) {event->accept(); return;}
        QTableWidget::keyPressEvent(event);
    }
};
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
    entries->addSeparator();
    action(entries, QStringLiteral("Переместить выбранные в группу…"), &MainWindow::bulkMoveDialog);
    action(entries, QStringLiteral("Добавить тег выбранным…"), &MainWindow::bulkAddTagDialog);
    action(entries, QStringLiteral("Снять тег у выбранных…"), &MainWindow::bulkRemoveTagDialog);
    entries->addSeparator();
#ifdef Q_OS_WIN
    // Не через mutations_: доступность зависит не только от Ready, но и от выбранной записи
    // (тип и однострочность секрета) — управляем сами в updateEnabled/showMetadata.
    envExportAction_ = entries->addAction(QStringLiteral("В переменную окружения…"), this, &MainWindow::exportToEnvironment);
#endif
    action(vault, QStringLiteral("Защищённый экспорт…"), &MainWindow::exportVault);
    action(vault, QStringLiteral("Импорт из файла…"), &MainWindow::importVault);
    action(vault, QStringLiteral("Импорт из CSV / KeePass XML…"), &MainWindow::importFromCsvKeePass);
    action(vault,QStringLiteral("TXT → предпросмотр и реестр…"),&MainWindow::importTextFile)->setObjectName("importTextFile");
    action(entries,QStringLiteral("Предпросмотр преобразования старых записей…"),&MainWindow::previewRegistryMigration)->setObjectName("previewRegistryMigration");
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
    // «Новая запись» и представления в одной строке: на узком окне заголовок вертикальный,
    // и отдельная строка представлений отняла бы высоту у дерева проектов.
    auto* entryRow = new QWidget(header); auto* entryRowLayout = new QHBoxLayout(entryRow); entryRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* newEntry = new QToolButton(entryRow); newEntry->setDefaultAction(addEntryAction); newEntry->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newEntry->setAccessibleName(QStringLiteral("Новая запись")); entryRowLayout->addWidget(newEntry); headerLayout_->addWidget(entryRow);
    layout->addWidget(header);
    filterLayout_ = controlsLayout;
    typeFilter_ = new QComboBox(controls); typeFilter_->setObjectName(QStringLiteral("typeFilter"));
    typeFilter_->setAccessibleName(QStringLiteral("Фильтр по типу записи"));
    typeFilter_->addItem(QStringLiteral("Все типы"),-1);
    for(int i=0;i<=int(model::EntryType::TextFile);++i) typeFilter_->addItem(entryTypeLabel(model::EntryType(i)),i);
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
    auto* viewsRow = new QWidget(entryRow); auto* viewsLayout = new QHBoxLayout(viewsRow); viewsLayout->setContentsMargins(0, 0, 0, 0);
    savedViews_ = new QComboBox(viewsRow); savedViews_->setObjectName(QStringLiteral("savedViews"));
    savedViews_->setAccessibleName(QStringLiteral("Сохранённые представления")); savedViews_->setPlaceholderText(QStringLiteral("Представления…"));
    savedViews_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); savedViews_->setMinimumContentsLength(8);
    viewActions_ = new QToolButton(viewsRow); viewActions_->setObjectName(QStringLiteral("viewActions")); viewActions_->setText(QStringLiteral("⋯"));
    viewActions_->setAccessibleName(QStringLiteral("Действия с представлениями")); viewActions_->setPopupMode(QToolButton::InstantPopup);
    viewActions_->setToolTip(QStringLiteral("Представление запоминает раздел, группу, тип, срок, теги, сортировку и поиск в зашифрованном хранилище"));
    auto* viewsMenu = new QMenu(viewActions_);
    const QString viewActionNames[4] = {QStringLiteral("Сохранить текущее…"), QStringLiteral("Переименовать…"), QStringLiteral("Обновить из текущего"), QStringLiteral("Удалить")};
    for (int i = 0; i < 4; ++i) {
        auto* action = viewsMenu->addAction(viewActionNames[i]); action->setObjectName(QStringLiteral("viewAction%1").arg(i));
        connect(action, &QAction::triggered, this, [this, i] { viewAction(i); });
    }
    connect(viewsMenu, &QMenu::aboutToShow, this, [this, viewsMenu] {
        for (int i = 1; i < 4; ++i) viewsMenu->actions().at(i)->setEnabled(savedViews_->currentIndex() >= 0);
    });
    viewActions_->setMenu(viewsMenu);
    viewsLayout->addWidget(savedViews_, 1); viewsLayout->addWidget(viewActions_); entryRowLayout->addWidget(viewsRow, 1);
    connect(savedViews_, &QComboBox::activated, this, [this](int index) { if (index >= 0) applyView(savedViews_->itemText(index)); });
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
    list_ = new QListWidget(results); list_->setObjectName("entriesList"); list_->setAccessibleName(QStringLiteral("Список записей")); list_->setMinimumWidth(160); list_->setSpacing(4);
    list_->setWordWrap(true); list_->setUniformItemSizes(true); list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    resultsTabs_=new QTabWidget(results); resultsTabs_->setObjectName("resultsTabs");
    registry_=new RegistryView(app_,resultsTabs_); resultsTabs_->addTab(list_,QStringLiteral("Список")); resultsTabs_->addTab(registry_,QStringLiteral("Реестр"));
    resultsLayout->addWidget(tagsScroll); resultsLayout->addWidget(resultsTabs_, 1);
    const auto selectEntry=[this](model::Id id) {
        for(int i=0;i<list_->count();++i) if(list_->item(i)->data(Qt::UserRole).value<model::Id>()==id) {list_->setCurrentRow(i); return true;} return false;
    };
    connect(registry_,&RegistryView::entryActivated,this,[this,selectEntry](model::Id id) {if(selectEntry(id)) {showMetadata(); openCardDialog();}});
    connect(registry_,&RegistryView::copyRequested,this,[this,selectEntry](model::Id id) {if(ready() && selectEntry(id)) {card_->showEntry(id); card_->copySecret();}});
    connect(registry_,&RegistryView::sourceRequested,this,&MainWindow::openDocument);
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
    connect(list_, &QListWidget::itemSelectionChanged, this, &MainWindow::showMetadata);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { if (!narrow_) editSelected(); });
    connect(list_, &QListWidget::itemClicked, this, &MainWindow::openCardDialog);
    connect(list_, &QListWidget::itemActivated, this, &MainWindow::openCardDialog);
    connect(card_, &EntryCard::editRequested, this, &MainWindow::editSelected);
    connect(card_,&EntryCard::documentRequested,this,&MainWindow::openDocument);
    connect(card_,&EntryCard::sourceRequested,this,&MainWindow::openDocument);
    connect(card_,&EntryCard::compareRequested,this,&MainWindow::compareWithSource);
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
QList<model::Id> MainWindow::selectedEntries() const {
    QList<model::Id> ids;
    for (const auto* item : list_->selectedItems()) ids << item->data(Qt::UserRole).value<model::Id>();
    return ids;
}
// Все изменения — одна мутация commit(): проверка на копии модели, одно сохранение,
// полный откат при первой ошибке (сообщение называет запись).
bool MainWindow::bulkApply(const QList<model::Id>& ids, quint64 generation, const std::function<QString(model::VaultModel&, model::Id)>& change) {
    if (!selectionStill(ids, generation) || ids.isEmpty()) return false;
    return commit([&](model::VaultModel& m) {
        for (auto id : ids) {
            const auto title = m.entry(id) ? m.entry(id)->title : QString();
            if (const auto error = change(m, id); !error.isEmpty()) return QStringLiteral("%1: %2").arg(title, error);
        }
        return QString();
    });
}
bool MainWindow::bulkMove(model::Id group) {
    if (!ready() || !app_.model()->nodes().contains(group)) return false;
    return bulkApply(selectedEntries(), app_.generation(), [&](model::VaultModel& m, model::Id id) {
        auto e = *m.entry(id); if (e.groupId == group) return QString();
        e.groupId = group; e.modified = QDateTime::currentDateTimeUtc(); return m.setEntry(e);
    });
}
bool MainWindow::bulkAddTag(const QString& tag) {
    const auto clean = tag.trimmed(); if (!ready() || clean.isEmpty()) return false;
    return bulkApply(selectedEntries(), app_.generation(), [&](model::VaultModel& m, model::Id id) {
        auto e = *m.entry(id); if (e.tags.contains(clean, Qt::CaseInsensitive)) return QString();
        e.tags << clean; e.modified = QDateTime::currentDateTimeUtc(); return m.setEntry(e);
    });
}
bool MainWindow::bulkRemoveTag(const QString& tag) {
    const auto clean = tag.trimmed(); if (!ready() || clean.isEmpty()) return false;
    return bulkApply(selectedEntries(), app_.generation(), [&](model::VaultModel& m, model::Id id) {
        auto e = *m.entry(id);
        if (!e.tags.removeIf([&](const QString& t) { return t.compare(clean, Qt::CaseInsensitive) == 0; })) return QString();
        e.modified = QDateTime::currentDateTimeUtc(); return m.setEntry(e);
    });
}
bool MainWindow::bulkDelete() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) return false;
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    // Удаление не раскрывает секретов, но защищённые записи не должны исчезать без владельца.
    const bool guarded = std::any_of(ids.cbegin(), ids.cend(), [&](model::Id id) { const auto* e = app_.model()->entry(id); return e && e->reverify; });
    if (guarded && !VerifyDialog::verify(app_, this, QStringLiteral("массовое удаление"))) return false;
    if (!selectionStill(ids, generation)) return false;
    if (QMessageBox::question(this, QStringLiteral("Удаление записей"), QStringLiteral("Удалить выбранные записи: %1? Действие нельзя отменить.").arg(ids.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return false;
    return bulkApply(ids, generation, [](model::VaultModel& m, model::Id id) { return m.removeEntry(id); });
}
bool MainWindow::selectionStill(const QList<model::Id>& ids, quint64 generation) {
    if (!ready() || app_.generation() != generation) return false;
    for (auto id : ids) if (!app_.model()->entry(id)) { report(QStringLiteral("Выбранные записи изменились. Повторите действие.")); return false; }
    return true;
}
void MainWindow::selectEntriesForAction(const QList<model::Id>& ids) {
    const QSignalBlocker block(list_); list_->clearSelection();
    for (int i = 0; i < list_->count(); ++i) if (ids.contains(list_->item(i)->data(Qt::UserRole).value<model::Id>())) list_->item(i)->setSelected(true);
}
void MainWindow::bulkMoveDialog() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) { report(QStringLiteral("Выберите записи.")); return; }
    const auto generation = app_.generation(); bool ok = false;
    QMap<QString, model::Id> groups;
    for (auto id : app_.model()->nodes().keys()) groups.insert(groupPath(id), id);
    QString chosen;
    { QScopedValueRollback guard(editing_, true);
      chosen = QInputDialog::getItem(this, QStringLiteral("Переместить записи"), QStringLiteral("Группа для выбранных записей (%1):").arg(ids.size()), groups.keys(), 0, false, &ok); }
    if (!ok || !selectionStill(ids, generation)) return;
    selectEntriesForAction(ids); bulkMove(groups.value(chosen));
}
void MainWindow::bulkAddTagDialog() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) { report(QStringLiteral("Выберите записи.")); return; }
    const auto generation = app_.generation(); bool ok = false; QString tag;
    { QScopedValueRollback guard(editing_, true);
      tag = QInputDialog::getText(this, QStringLiteral("Добавить тег"), QStringLiteral("Тег для выбранных записей (%1):").arg(ids.size()), QLineEdit::Normal, {}, &ok); }
    if (!ok || !selectionStill(ids, generation)) return;
    selectEntriesForAction(ids); bulkAddTag(tag);
}
void MainWindow::bulkRemoveTagDialog() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) { report(QStringLiteral("Выберите записи.")); return; }
    const auto generation = app_.generation();
    QStringList tags;
    for (auto id : ids) for (const auto& tag : app_.model()->entry(id)->tags) if (!tags.contains(tag, Qt::CaseInsensitive)) tags << tag;
    if (tags.isEmpty()) { report(QStringLiteral("У выбранных записей нет тегов.")); return; }
    bool ok = false; QString tag;
    { QScopedValueRollback guard(editing_, true);
      tag = QInputDialog::getItem(this, QStringLiteral("Снять тег"), QStringLiteral("Тег у выбранных записей (%1):").arg(ids.size()), tags, 0, false, &ok); }
    if (!ok || !selectionStill(ids, generation)) return;
    selectEntriesForAction(ids); bulkRemoveTag(tag);
}
model::SavedView MainWindow::captureView(const QString& name) const {
    model::SavedView v; v.name = name;
    if (libraryView_ == LibraryView::Group) v.group = selectedNode();
    else v.library = libraryView_ == LibraryView::Favorites ? 1 : libraryView_ == LibraryView::Expiring ? 2 : 0;
    v.type = typeFilter_->currentData().toInt(); v.expiry = expiryFilter_->currentIndex(); v.sort = sortMode_->currentIndex();
    v.tags = model::VaultModel::normalizeTags(selectedTags_); v.search = search_->text().left(model::VaultModel::MaxViewSearch);
    return v;
}
// Представления — часть модели: любое изменение идёт обычным commit() (согласие на формат,
// одно сохранение, полный откат при ошибке).
bool MainWindow::changeViews(const std::function<QString(QList<model::SavedView>&)>& change) {
    if (!ready()) return false;
    return commit([&](model::VaultModel& m) { auto views = m.views(); const auto error = change(views); return error.isEmpty() ? m.setViews(views) : error; });
}
bool MainWindow::saveCurrentView(const QString& name) {
    if (!ready()) return false;
    const auto view = captureView(name);
    return changeViews([&](QList<model::SavedView>& views) { views << view; return QString(); });
}
bool MainWindow::applyView(const QString& name) {
    if (!ready()) return false;
    const auto& views = app_.model()->views();
    const auto it = std::find_if(views.cbegin(), views.cend(), [&](const auto& v) { return v.name.compare(name, Qt::CaseInsensitive) == 0; });
    if (it == views.cend()) return false;
    const auto v = *it;
    QTreeWidgetItem* groupItem = nullptr;
    if (!v.group.isNull()) for (QTreeWidgetItemIterator i(tree_); *i; ++i) if ((*i)->data(0, Qt::UserRole).value<model::Id>() == v.group) groupItem = *i;
    {
        QSignalBlocker b1(typeFilter_), b2(expiryFilter_), b3(sortMode_), b4(search_), b5(tree_);
        typeFilter_->setCurrentIndex(qMax(0, typeFilter_->findData(v.type)));
        sortMode_->setCurrentIndex(v.sort); search_->setText(v.search);
        if (groupItem) { libraryView_ = LibraryView::Group; tree_->setCurrentItem(groupItem); }
        else {
            libraryView_ = v.library == 1 ? LibraryView::Favorites : v.library == 2 ? LibraryView::Expiring : LibraryView::All;
            tree_->setCurrentItem(nullptr); tree_->clearSelection();
        }
        expiryFilter_->setCurrentIndex(v.expiry);
        // Отсутствующие теги не применяются: фильтр несуществующим тегом дал бы пустой список без объяснения.
        const auto tags = app_.model()->allTags(); selectedTags_.clear();
        for (const auto& tag : v.tags)
            if (std::any_of(tags.cbegin(), tags.cend(), [&](const QString& t) { return t.compare(tag, Qt::CaseInsensitive) == 0; })) selectedTags_ << tag;
    }
    refillTagBar(); updateLibraryCounts(); refillList();
    if (!v.group.isNull() && !groupItem) statusBar()->showMessage(QStringLiteral("Группа представления удалена — фильтр группы снят."), 5000);
    return true;
}
void MainWindow::refillViews() {
    const QSignalBlocker block(savedViews_); const auto current = savedViews_->currentText();
    savedViews_->clear();
    if (app_.gate() == Application::Gate::Ready) for (const auto& v : app_.model()->views()) savedViews_->addItem(v.name);
    savedViews_->setCurrentIndex(savedViews_->findText(current));
}
void MainWindow::viewAction(int action) {
    if (!ready() || editing_) return;
    const auto name = savedViews_->currentText();
    if (action == 0) {
        bool ok = false;
        const auto chosen = QInputDialog::getText(this, QStringLiteral("Сохранить представление"), QStringLiteral("Имя представления:"), QLineEdit::Normal, {}, &ok).trimmed();
        if (ok && !chosen.isEmpty() && saveCurrentView(chosen)) savedViews_->setCurrentIndex(savedViews_->findText(chosen));
        return;
    }
    if (savedViews_->currentIndex() < 0) return;
    const auto same = [&](const model::SavedView& v) { return v.name.compare(name, Qt::CaseInsensitive) == 0; };
    if (action == 1) {
        bool ok = false;
        const auto renamed = QInputDialog::getText(this, QStringLiteral("Переименовать представление"), QStringLiteral("Новое имя:"), QLineEdit::Normal, name, &ok).trimmed();
        if (ok && !renamed.isEmpty() && changeViews([&](QList<model::SavedView>& views) { for (auto& v : views) if (same(v)) v.name = renamed; return QString(); }))
            savedViews_->setCurrentIndex(savedViews_->findText(renamed));
    } else if (action == 2) {
        const auto current = captureView(name);
        changeViews([&](QList<model::SavedView>& views) { for (auto& v : views) if (same(v)) v = current; return QString(); });
    } else if (action == 3 && QMessageBox::question(this, QStringLiteral("Удаление представления"), QStringLiteral("Удалить представление «%1»? Записи не затрагиваются.").arg(name),
                   QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes)
        changeViews([&](QList<model::SavedView>& views) { views.removeIf(same); return QString(); });
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
    refillTagBar(); updateLibraryCounts(); refillViews();
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
        const int typeFilter = typeFilter_->currentData().toInt();
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
        registry_->setEntries(ids);
        for (auto id : ids) {
            const auto* entry = app_.model()->entry(id);
            QString title = entry->favorite ? QStringLiteral("★ ") + entry->title : entry->title;
            const auto type=entryTypeLabel(entry->type);
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
    if (const int selected = int(list_->selectedItems().size()); selected > 1 && app_.gate() == Application::Gate::Ready) {
        right_->setCurrentIndex(1); displayedEntry_ = {}; card_->showSelection(selected);
#ifdef Q_OS_WIN
        if (envExportAction_) envExportAction_->setEnabled(false);
#endif
        return;
    }
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
bool MainWindow::ensureSchemaUpgradeApproved() {
    if(!ready()) return false;
    if(!app_.schemaUpgradeRequired() || app_.schemaUpgradeApproved()) return true;
    const auto generation=app_.generation(); const auto before=*app_.model();
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    QMessageBox box(QMessageBox::Warning,QStringLiteral("Обновление формата"),QStringLiteral("Данные будут сохранены в формате v%1. Прежняя версия программы не откроет новый формат. Перед записью будет создана уникальная проверенная зашифрованная копия прежнего файла в каталоге хранилища. Продолжить?").arg(model::VaultModel::CurrentVersion),QMessageBox::Yes|QMessageBox::No,this);
    box.setObjectName("schemaUpgradeConfirmation"); box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); box.setEscapeButton(QMessageBox::No);
    if(box.exec()!=QMessageBox::Yes) {error_->clear(); return false;}
    if(!ready() || generation!=app_.generation() || before!=*app_.model()) {if(ready()) report(QStringLiteral("Данные изменились. Повторите действие.")); return false;}
    QString error; if(!app_.approveSchemaUpgrade(&error)) {report(error); return false;} return true;
}
bool MainWindow::commit(const std::function<QString(model::VaultModel&)>& mutation) {
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    if (!ready() || !ensureSchemaUpgradeApproved()) return false;
    auto before = *app_.model(); const auto generation = app_.generation();
    const auto connection = connect(&app_, &Application::stateChanged, this, [&] { if (app_.generation() != generation) before.clear(); });
    auto cleanup = qScopeGuard([&] { disconnect(connection); });
    const auto error = mutation(*app_.model());
    if (!error.isEmpty()) { *app_.model() = std::move(before); report(error); refreshAll(); return false; }
    QString saveError; const QPointer<MainWindow> self(this); auto* core=&app_;
    const bool saved=runVaultOperation(*core,[core](void*,QString* error) {return core->save(error);},&saveError,this);
    if(!self) return false;
    if (!saved) {
        if (app_.generation() == generation && app_.gate() == Application::Gate::Ready) *app_.model() = std::move(before);
        before.clear(); refreshAll();
        if (app_.gate() == Application::Gate::Ready) report(saveError); else if(!saveError.isEmpty()) emit notifyRequested(saveError);
        return false;
    }
    error_->clear(); refreshAll(); statusBar()->showMessage(QStringLiteral("Сохранено"), 3000); return true;
}
bool MainWindow::commitEntry(const std::function<QString(model::VaultModel&)>& mutation, model::Id entryId) {
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    if (!ready()) return false;
    if(const auto* existing=app_.model()->entry(entryId);existing && existing->type==model::EntryType::TextFile) return commit(mutation);
    if(!ensureSchemaUpgradeApproved()) return false;
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
    const QPointer<MainWindow> self(this); auto* core=&app_;
    const bool saved=runVaultOperation(*core,[core](void*,QString* error) {return core->save(error);},&saveError,this);
    if(!self) return false;
    if (!saved) {
        if (app_.generation() == generation && app_.gate() == Application::Gate::Ready)
            rollbackEntry(entryId, backup);
        refreshAll();
        if (app_.gate() == Application::Gate::Ready) report(saveError); else if(!saveError.isEmpty()) emit notifyRequested(saveError);
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
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
    EntryEditDialog dialog(*app_.model(), std::nullopt, this); dialog.setGroup(parent); dialog.setGeneratorSettings(preferences_);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto entry = dialog.entry(); if (entry) commitEntry([&](auto& model) { return model.setEntry(*entry); }, entry->id);
    }
}
void MainWindow::editSelected() {
    if (selectedEntries().size() > 1) { report(QStringLiteral("Для изменения выберите одну запись.")); return; }
    if (!ready() || editing_) return; const auto id=selectedEntry(); if (!app_.model()->entry(id)) { report(QStringLiteral("Выберите запись.")); return; }
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
    // Редактор показывает секреты записи — для записей с повторной проверкой сначала
    // подтверждаем владельца, как при показе/копировании (audit M-1).
    if (!authorizeEntry(app_,id,this) || selectedEntry()!=id || !ready() || generation!=app_.generation()) return;
    const auto* entry=app_.model()->entry(id); if(!entry) return; const auto before=*entry;
    EntryEditDialog dialog(*app_.model(), *entry, this); dialog.setGeneratorSettings(preferences_);
    if (dialog.exec() == QDialog::Accepted && ready() && app_.generation() == generation) {
        const auto* current=app_.model()->entry(id);
        if(!current || *current!=before) {report(QStringLiteral("Запись изменилась. Откройте редактор заново.")); return;}
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
    if (selectedEntries().size() > 1) { bulkDelete(); return; }
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
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
    if (!VerifyDialog::verify(app_, this, QStringLiteral("защищённый экспорт")) || !ready() || app_.generation() != generation) return;
    ExportDialog dialog(this, QStringLiteral("Защищённый экспорт хранилища"),
        QStringLiteral("Экспорт шифруется отдельным паролем, который не связан с мастер-паролем. "
                       "Полученный файл можно переносить на другие компьютеры; для восстановления понадобятся только файл и этот пароль. Формат данных экспорта — v%1: прежняя версия программы его не откроет. Локальный файл при экспорте не обновляется.").arg(model::VaultModel::CurrentVersion),
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
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
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
    if(!ensureSchemaUpgradeApproved()) {filePassword.clear(); localPassword.clear(); return;}
    QString error;
    const QPointer<MainWindow> self(this); auto* core=&app_;
    const bool ok=runVaultOperation(*core,[&](void*,QString* failure) {return core->importContainer(bytes,filePassword.bytes(),localPassword.bytes(),failure);},&error,this);
    filePassword.clear(); localPassword.clear();
    if(!self) return;
    if(!ready()) {if(!error.isEmpty()) emit notifyRequested(error); return;}
    if (!ok) { QMessageBox::critical(this, QStringLiteral("TokenBucket"), error); return; }
    refreshAll();
    statusBar()->showMessage(QStringLiteral("Импорт выполнен."), 5000);
}
void MainWindow::switchUnlockMode() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
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
        if(!ensureSchemaUpgradeApproved()) return;
        const QPointer<MainWindow> self(this); auto* core=&app_;
        const bool ok=runVaultOperation(*core,[core](void*,QString* failure) {return core->switchToHello(failure);},&error,this);
        if(!self) return;
        if(!ready()) {if(!error.isEmpty()) emit notifyRequested(error); return;}
        if(ok) statusBar()->showMessage(QStringLiteral("Разблокировка Windows Hello включена."),5000);
        else QMessageBox::critical(this,QStringLiteral("TokenBucket"),error);
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
    if(!ensureSchemaUpgradeApproved()) {password.clear(); return;}
    const QPointer<MainWindow> self(this); auto* core=&app_;
    const bool ok=runVaultOperation(*core,[&](void*,QString* failure) {return core->switchToPassword(password.bytes(),failure);},&error,this);
    password.clear();
    if(!self) return;
    if(!ready()) {if(!error.isEmpty()) emit notifyRequested(error); return;}
    if (!ok) QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
    else statusBar()->showMessage(QStringLiteral("Разблокировка по мастер-паролю включена."), 5000);
    refreshAll();
}
void MainWindow::changeMasterPassword() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
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
    if(!ensureSchemaUpgradeApproved()) {password.clear(); return;}
    const QPointer<MainWindow> self(this); auto* core=&app_;
    const bool ok=runVaultOperation(*core,[&](void*,QString* failure) {return core->switchToPassword(password.bytes(),failure);},&error,this);
    password.clear();
    if(!self) return;
    if(!ready()) {if(!error.isEmpty()) emit notifyRequested(error); return;}
    if (!ok) QMessageBox::critical(this, QStringLiteral("TokenBucket"), error);
    else statusBar()->showMessage(QStringLiteral("Мастер-пароль изменён."), 5000);
}
bool MainWindow::moveNodeTo(model::Id node, model::Id newParent) {
    if (!ready() || node == newParent || newParent.isNull()) { refreshAll(); return false; }
    return commit([&](auto& model) { return model.moveNode(node, newParent); });
}
void MainWindow::importFromCsvKeePass() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
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
    if (added < 0) {
        if(ready() && !error.isEmpty()) {QMessageBox box(QMessageBox::Critical,QStringLiteral("TokenBucket"),error,QMessageBox::Ok,this); box.setTextFormat(Qt::PlainText); box.exec();}
        return;
    }
    refreshAll();
    statusBar()->showMessage(QStringLiteral("Импортировано записей: %1").arg(added), 5000);
    QMessageBox summary(QMessageBox::Information,QStringLiteral("TokenBucket"),
        QStringLiteral("Импортировано записей: %1%2.").arg(added)
            .arg(error.isEmpty() ? QString() : QStringLiteral(" — Пропуски и замечания: %1").arg(error)),QMessageBox::Ok,this);
    summary.setTextFormat(Qt::PlainText); summary.exec();
}
int MainWindow::applyImport(const QByteArray& bytes, bool keepassXml, QString* err) {
    if(!ready()) {if(err) *err=QStringLiteral("Хранилище недоступно."); return -1;}
    const io::ImportResult parsed = keepassXml ? io::parseKeePassXml(bytes) : io::parseCsv(bytes);
    auto candidate=*app_.model();
    // Все записи селятся под проект «Импорт» (создаётся при необходимости), группы — по пути из файла.
    model::Id root;
    for (auto id : candidate.nodes().keys())
        if (candidate.nodes().value(id).parent.isNull() && candidate.nodes().value(id).name == QLatin1String("Импорт")) root = id;
    if (root.isNull()) {
        root = QUuid::createUuid();
        const QString createError = candidate.addNode(root, {}, QStringLiteral("Импорт"));
        if (!createError.isEmpty()) { if (err) *err = createError; return -1; }
    }
    int added = 0;
    QStringList notes;
    for (const auto& imported : parsed.entries) {
        model::Id parent = root;
        for (const QString& part : imported.groupPath) {
            model::Id child;
            for (auto id : candidate.nodes().keys())
                if (candidate.nodes().value(id).parent == parent && candidate.nodes().value(id).name == part) { child = id; break; }
            if (child.isNull()) {
                child = QUuid::createUuid();
                const QString createError = candidate.addNode(child, parent, part);
                if (!createError.isEmpty()) { notes << createError; child = parent; break; } // упёрлись в лимит глубины — селятся выше
                parent = child;
            } else parent = child;
        }
        model::Entry e = imported.entry;
        e.id = QUuid::createUuid(); e.groupId = parent;
        e.created = e.modified = QDateTime::currentDateTimeUtc();
        const QString entryError = candidate.setEntry(e);
        if (entryError.isEmpty()) ++added; else notes << QStringLiteral("%1: %2").arg(e.title, entryError);
    }
    if (parsed.skipped > 0) notes << QStringLiteral("Пропущено строк файла: %1 (%2)").arg(parsed.skipped).arg(parsed.lastSkipReason);
    if(added>0) {
        if(!ensureSchemaUpgradeApproved()) {if(err) *err=error_->text(); return -1;}
        if(!commit([&](model::VaultModel& model) {model=std::move(candidate); return QString();})) {if(err) *err=error_->text().isEmpty()?QStringLiteral("Импорт не сохранён."):error_->text(); return -1;}
    }
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
    if (item->isSelected() && list_->selectedItems().size() > 1) {
        QMenu menu(this);
        menu.addAction(QStringLiteral("Переместить в группу…"), this, &MainWindow::bulkMoveDialog);
        menu.addAction(QStringLiteral("Добавить тег…"), this, &MainWindow::bulkAddTagDialog);
        menu.addAction(QStringLiteral("Снять тег…"), this, &MainWindow::bulkRemoveTagDialog);
        menu.addSeparator();
        menu.addAction(QStringLiteral("Удалить выбранные (%1)…").arg(list_->selectedItems().size()), this, [this] { bulkDelete(); });
        menu.exec(list_->viewport()->mapToGlobal(pos)); return;
    }
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
void MainWindow::previewRegistryMigration() {
    if(!ready() || editing_) return;
    const auto group=selectedNode(); if(group.isNull() || libraryView_!=LibraryView::Group) {report(QStringLiteral("Выберите ресурс в дереве проектов.")); return;}
    QScopedValueRollback guard(editing_,true); const auto generation=app_.generation();
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    if(!VerifyDialog::verify(app_,this,QStringLiteral("предпросмотр преобразования")) || !ready() || generation!=app_.generation() || group!=selectedNode()) return;
    auto proposal=io::proposeRegistryMigration(*app_.model(),app_.model()->entriesInSubtree(group),generation);
    if(!proposal.error.isEmpty()) {report(proposal.error); return;}
    QDialog dialog(this); dialog.setObjectName("registryMigrationPreview"); dialog.setWindowTitle(QStringLiteral("Старые записи — предпросмотр")); dialog.resize(900,480);
    auto* layout=new QVBoxLayout(&dialog); auto* heading=new QLabel(QStringLiteral("Ресурс: %1\nВыберите преобразования. Неоднозначные записи и SSH сохраняются без изменений. Исходные TXT с неподтверждёнными байтами остаются прежними записями.").arg(groupPath(group)),&dialog);
    heading->setTextFormat(Qt::PlainText); heading->setWordWrap(true); layout->addWidget(heading);
    auto* table=new MigrationTable(proposal.before.size(),5,&dialog); table->setObjectName("registryMigrationEntries"); table->setHorizontalHeaderLabels({QStringLiteral("Применить"),QStringLiteral("Запись"),QStringLiteral("Сейчас"),QStringLiteral("После"),QStringLiteral("Предложение")});
    table->setEditTriggers(QAbstractItemView::NoEditTriggers); table->setDragEnabled(false); table->setContextMenuPolicy(Qt::NoContextMenu); table->setSelectionBehavior(QAbstractItemView::SelectRows);
    int changes=0;
    for(int i=0;i<proposal.before.size();++i) {
        const auto& before=proposal.before[i]; const auto& after=proposal.after[i]; const bool changed=before!=after; changes+=changed;
        auto* check=new QTableWidgetItem; check->setFlags(changed?Qt::ItemIsEnabled|Qt::ItemIsUserCheckable:Qt::NoItemFlags); check->setCheckState(changed?Qt::Checked:Qt::Unchecked); table->setItem(i,0,check);
        table->setItem(i,1,new QTableWidgetItem(before.title)); table->setItem(i,2,new QTableWidgetItem(entryTypeLabel(before.type)));
        table->setItem(i,3,new QTableWidgetItem(changed?entryTypeLabel(after.type):QStringLiteral("Без изменений")));
        table->setItem(i,4,new QTableWidgetItem(QStringLiteral("••••••••")));
    }
    table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch); layout->addWidget(table,1);
    QTimer revealTimer(&dialog); revealTimer.setSingleShot(true); revealTimer.setObjectName("migrationHideTimer"); revealTimer.setInterval(appliedPreferences_.revealSeconds*1000); int revealed=-1;
    const auto hideValue=[&] {revealTimer.stop(); if(revealed>=0 && revealed<table->rowCount()) table->item(revealed,4)->setText(QStringLiteral("••••••••")); revealed=-1;};
    table->hideValue=hideValue; const auto clearCallback=qScopeGuard([&] {table->hideValue={};});
    connect(&revealTimer,&QTimer::timeout,&dialog,hideValue); connect(table,&QTableWidget::currentCellChanged,&dialog,[&] {hideValue();});
    auto* reveal=new QPushButton(QStringLiteral("Показать выбранное на время…"),&dialog); reveal->setObjectName("migrationReveal"); layout->addWidget(reveal);
    auto* error=new QLabel(changes?QString():QStringLiteral("Безопасных автоматических преобразований нет. Новые TXT можно импортировать отдельно через предпросмотр."),&dialog); error->setObjectName("registryMigrationError"); error->setTextFormat(Qt::PlainText); error->setWordWrap(true); layout->addWidget(error);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Ok|QDialogButtonBox::Cancel,&dialog); buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Применить выбранное…")); buttons->button(QDialogButtonBox::Ok)->setEnabled(changes>0); layout->addWidget(buttons);
    bool applying=false,canceled=false; connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    connect(reveal,&QPushButton::clicked,&dialog,[&] {
        const int row=table->currentRow(); if(applying || canceled || !ready() || row<0 || row>=proposal.before.size() || proposal.before[row]==proposal.after[row]) return;
        const auto before=proposal.before[row];
        if(!authorizeEntry(app_,before.id,&dialog,true) || canceled || !ready() || generation!=app_.generation() || row!=table->currentRow() || row>=proposal.before.size()) return;
        const auto* current=app_.model()->entry(before.id); if(!current || *current!=before) {error->setText(QStringLiteral("Предпросмотр устарел. Откройте его заново.")); return;}
        hideValue(); revealed=row; table->item(row,4)->setText(proposal.after[row].plainValue); revealTimer.start();
    });
    connect(&dialog,&QDialog::rejected,&dialog,[&] {canceled=true; if(applying && app_.gate()==Application::Gate::Ready) app_.lock();});
    connect(&app_,&Application::stateChanged,&dialog,[&] {
        if(generation!=app_.generation() || app_.gate()!=Application::Gate::Ready) {
            for(auto* child:dialog.findChildren<QDialog*>()) child->reject(); hideValue(); table->setRowCount(0); proposal={}; dialog.reject();
        }
    });
    connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if(applying || canceled || !ready() || generation!=app_.generation()) return;
        io::MigrationProposal selected; selected.generation=generation;
        for(int i=0;i<proposal.before.size();++i) if(table->item(i,0)->checkState()==Qt::Checked) {selected.before.append(proposal.before[i]); selected.after.append(proposal.after[i]);}
        if(selected.before.isEmpty()) {error->setText(QStringLiteral("Нет выбранных преобразований.")); return;}
        QMessageBox confirm(QMessageBox::Warning,QStringLiteral("Изменение видимости"),QStringLiteral("Выбранные значения станут видимыми и доступными поиску, кроме записей с повторной проверкой. Перед записью будет создана проверенная зашифрованная копия. Затем сеанс заблокируется для повторного открытия. Продолжить?"),QMessageBox::Yes|QMessageBox::No,&dialog);
        confirm.setTextFormat(Qt::PlainText); confirm.setDefaultButton(QMessageBox::No); confirm.setEscapeButton(QMessageBox::No);
        if(confirm.exec()!=QMessageBox::Yes || canceled || !ready() || generation!=app_.generation()) return;
        if(!ensureSchemaUpgradeApproved() || canceled || !ready() || generation!=app_.generation()) return;
        model::VaultModel candidate; QString failure;
        if(!io::buildMigrationCandidate(*app_.model(),selected,&candidate,&failure)) {error->setText(failure); return;}
        applying=true; buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        QString safety;
        runVaultOperation(app_,[&](void*,QString* error) {safety=app_.preserveBeforeRegistryMigration(error); return !safety.isEmpty();},&failure,&dialog);
        if(canceled || !ready() || generation!=app_.generation()) return;
        if(safety.isEmpty() || !io::buildMigrationCandidate(*app_.model(),selected,&candidate,&failure)) {
            applying=false; error->setText(failure); buttons->button(QDialogButtonBox::Ok)->setEnabled(true); return;
        }
        const bool saved=commit([&](model::VaultModel& model) {model=std::move(candidate); return QString();}); applying=false;
        if(!saved) {if(!canceled && ready()) {error->setText(error_->text()); buttons->button(QDialogButtonBox::Ok)->setEnabled(true);} return;}
        dialog.accept(); app_.lock();
    });
    dialog.exec();
}
void MainWindow::compareWithSource(model::Id id) {
    if(!ready() || editing_) return; QScopedValueRollback guard(editing_,true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    if(!authorizeEntry(app_,id,this)) return;
    const auto* e=app_.model()->entry(id); if(!e) return; const auto before=*e; const auto generation=app_.generation();
    const auto* source=app_.model()->entry(before.sourceEntryId);
    if(!source || source->type!=model::EntryType::TextFile || !source->textFile.sourceSnapshot) {report(QStringLiteral("Источник недоступен.")); return;}
    const auto original=*source;
    if(!authorizeEntry(app_,source->id,this,!model::textfile::isVisible(source->textFile,before.sourceLine))) return;
    const auto* current=app_.model()->entry(id); source=app_.model()->entry(original.id);
    if(!ready() || generation!=app_.generation() || !current || *current!=before || !source || *source!=original) return;
    const auto* value=paste::fieldValue(before,paste::Field::Secret);
    if(!value) {report(QStringLiteral("Для этого типа откройте исходную строку.")); return;}
    auto sourceValue=model::textfile::lineText(original.textFile,before.sourceLine);
    const auto parsed=io::parseTextFile(original.textFile.bytes,original.textFile.fileName);
    for(const auto& parameter:parsed.parameters) if(parameter.line==before.sourceLine) {sourceValue=parameter.value; break;}
    QMessageBox::information(this,QStringLiteral("Сравнение с источником"),*value==sourceValue?QStringLiteral("Совпадает"):QStringLiteral("Отличается"));
}
void MainWindow::openDocument(model::Id id) {
    if(!ready() || editing_) return;
    const auto* e=app_.model()->entry(id); if(!e || e->type!=model::EntryType::TextFile) return;
    QScopedValueRollback guard(editing_,true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true);
    const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    TextFileDialog dialog(app_,id,clipboard_,this); dialog.setRevealSeconds(appliedPreferences_.revealSeconds);
    connect(&dialog,&TextFileDialog::requestRegistryImport,this,[this,&dialog](model::Id id) {
        dialog.reject(); QTimer::singleShot(0,this,[this,id] {structureDocument(id);});
    });
    connect(&dialog,&TextFileDialog::requestVisibilityChange,this,[this](model::Id id,int line,bool visible) {
        commitEntry([&](model::VaultModel& model) {
            const auto* current=model.entry(id); if(!current || current->type!=model::EntryType::TextFile) return QStringLiteral("Документ недоступен.");
            auto changed=*current;
            if(visible) {if(!changed.textFile.visibleLines.contains(line)) changed.textFile.visibleLines.append(line); std::sort(changed.textFile.visibleLines.begin(),changed.textFile.visibleLines.end());}
            else changed.textFile.visibleLines.removeAll(line);
            return model.setEntry(changed);
        },id);
    });
    dialog.exec();
}
void MainWindow::previewTextImport(io::TextImportPreview preview) {
    if(!ready()) return;
    const auto baseline=*app_.model(); const auto generation=app_.generation();
    TextImportDialog dialog(app_,std::move(preview),[this,baseline,generation](const io::TextImportPreview& p,const QList<io::RegistrySelection>& choices,QString* error) {
        if(!ready() || generation!=app_.generation() || *app_.model()!=baseline) {if(error) *error=QStringLiteral("Сеанс или данные изменились. Повторите импорт."); return false;}
        model::VaultModel candidate;
        if(!io::buildRegistryCandidate(*app_.model(),p,choices,&candidate,error)) return false;
        const bool saved=commit([&](model::VaultModel& model) {model=std::move(candidate); return QString();});
        if(!saved && error) *error=error_->text(); return saved;
    },this);
    dialog.setRevealSeconds(appliedPreferences_.revealSeconds); dialog.exec();
}
void MainWindow::structureDocument(model::Id id) {
    if(!ready() || editing_) return;
    QScopedValueRollback guard(editing_,true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    if(!authorizeEntry(app_,id,this,true)) return;
    const auto* e=app_.model()->entry(id); if(!e || e->type!=model::EntryType::TextFile) return;
    auto parsed=io::parseTextFile(e->textFile.bytes,e->textFile.fileName);
    if(e->textFile.sourceSnapshot) parsed.source=*e;
    else {
        const auto newId=parsed.source.id; parsed.source=*e; parsed.source.id=newId; parsed.source.textFile.sourceSnapshot=true;
        parsed.source.textFile.visibleLines.clear(); parsed.source.title+=QStringLiteral(" — снимок импорта");
        parsed.source.created=parsed.source.modified=QDateTime::currentDateTimeUtc();
    }
    previewTextImport(std::move(parsed));
}
void MainWindow::importTextFile() {
    if(!ready() || editing_) return;
    const auto group=selectedNode(); if(group.isNull()) {report(QStringLiteral("Сначала выберите существующую группу ресурса.")); return;}
    QScopedValueRollback guard(editing_,true); const auto generation=app_.generation();
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto path=QFileDialog::getOpenFileName(this,QStringLiteral("TXT — источник реестра"),{},QStringLiteral("Текстовые файлы (*.txt *.env);;Все файлы (*)"));
    if(path.isEmpty() || !ready() || generation!=app_.generation()) return;
    QFutureWatcher<io::TextImportPreview> watcher; QEventLoop loop;
    connect(&watcher,&QFutureWatcher<io::TextImportPreview>::finished,&loop,&QEventLoop::quit);
    connect(&app_,&Application::stateChanged,&loop,[&] {if(generation!=app_.generation() || app_.gate()!=Application::Gate::Ready) loop.quit();});
    statusBar()->showMessage(QStringLiteral("Чтение и разбор TXT…"));
    watcher.setFuture(QtConcurrent::run([path] {
        const QFileInfo info(path); io::TextImportPreview result;
        if(!info.isFile() || info.isSymbolicLink() || info.isShortcut() || info.isJunction() || info.size()>model::VaultModel::MaxSecretBytes) {
            result.error=QStringLiteral("Нужен обычный файл UTF-8 не больше 1 МиБ."); return result;
        }
        QFile file(path);
        if(!file.open(QIODevice::ReadOnly)) {result.error=QStringLiteral("Не удалось прочитать TXT."); return result;}
        auto bytes=file.read(model::VaultModel::MaxSecretBytes+1);
        if(file.error()!=QFile::NoError || !file.atEnd() || bytes.size()>model::VaultModel::MaxSecretBytes) {result.error=QStringLiteral("Файл изменился, недоступен или превышает 1 МиБ."); return result;}
        return io::parseTextFile(bytes,info.fileName());
    }));
    if(!watcher.isFinished()) loop.exec();
    if(!ready() || generation!=app_.generation() || !watcher.isFinished()) return;
    statusBar()->clearMessage(); io::TextImportPreview preview;
    try {preview=watcher.result();} catch(...) {report(QStringLiteral("Не удалось подготовить предпросмотр TXT.")); return;}
    if(!preview.error.isEmpty()) {report(preview.error); return;}
    if(!VerifyDialog::verify(app_,this,QStringLiteral("предпросмотр TXT")) || !ready() || generation!=app_.generation()) return;
    preview.source.groupId=group; previewTextImport(std::move(preview));
}
#ifdef Q_OS_WIN
bool MainWindow::canExportEnv() const {
    if (!ready() || list_->selectedItems().size() > 1) return false;
    const auto* entry = app_.model()->entry(selectedEntry());
    // однострочный токен/пароль: eligibleForAutoPaste заодно отсекает SSH-ключи (многострочные)
    return entry && paste::eligibleForAutoPaste(*entry);
}
void MainWindow::exportToEnvironment() {
    if (!ready() || editing_) return;
    QScopedValueRollback guard(editing_, true);
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    const auto generation = app_.generation();
    const auto* entry = app_.model()->entry(selectedEntry());
    if (!entry || !paste::eligibleForAutoPaste(*entry)) { report(QStringLiteral("Выберите непустое однострочное значение, токен или пароль.")); return; }
    const auto before=*entry; const auto id=entry->id;
    const auto stillValid=[&] {const auto* current=app_.model()->entry(id); return ready() && app_.generation()==generation && selectedEntry()==id && current && *current==before;};
    // Копия секрета в локаль ДО диалогов: VerifyDialog/EnvExportDialog крутят вложенные циклы
    // событий, за которые автолок или QtConcurrent-операция могут очистить модель — указатель
    // entry стал бы висячим. Дальше работаем только с копией и после каждого диалога
    // перепроверяем, что шлюз готов и поколение не сменилось (страж как в saveSshToFile).
    const QString secret = *paste::fieldValue(*entry,paste::Field::Secret);
    // NUL в секрете недопустим: и env-блок CreateProcessW, и REG_SZ в реестре молча усекают
    // строку на нём — для security-фичи отказываемся явно, а не отдаём усечённое значение.
    if (secret.contains(QChar(u'\0'))) { report(QStringLiteral("Секрет содержит недопустимые символы для переменной окружения.")); return; }
    if(!authorizeEntry(app_,id,this) || !stillValid()) return;
    EnvExportDialog dialog(this,preferences_);
    if(before.type==model::EntryType::PlainValue) dialog.setSuggestedName(before.parameterName);
    const int outcome=dialog.exec();
    appliedPreferences_.clearEnvironmentOnExit=prefs::read(*preferences_).clearEnvironmentOnExit;
    if (outcome != QDialog::Accepted || !stillValid()) return;
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
    if (!stillValid()) return;
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
