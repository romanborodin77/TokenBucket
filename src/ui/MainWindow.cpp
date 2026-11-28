#include "MainWindow.h"
#include <QApplication>
#include "CollectionDialog.h"
#include "SettingsDialog.h"
#ifdef Q_OS_WIN
#include "AgentAccess.h"
#endif
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
#include <QTabBar>
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
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QCursor>
#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
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
#include <QStyleOption>
#include <QStylePainter>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include "../platform/FileOps.h"
#include "../platform/QuickPaste.h"
#include "../vault/Container.h"
#include "../vault/SecureBytes.h"
#include <sodium.h>
#ifdef Q_OS_WIN
#include "../platform/hello_win.h"
#include "../platform/QuickPasteWin.h"
#include "../platform/EnvOps.h"
#include "EnvExportDialog.h"
#include <QTimer>
#endif
#include <algorithm>
namespace tb::ui {
namespace {
constexpr int CollectionProjectRole = Qt::UserRole + 1;

bool isCollectionProject(const QTreeWidgetItem* item) {
    return item && item->data(0, CollectionProjectRole).isValid();
}

QString collectionProjectLabel(const QString& project) {
    return project.isEmpty() ? QStringLiteral("Без проекта")
                            : QStringLiteral("%1 · агент").arg(project);
}

class WorkspaceFilter final : public QComboBox {
public:
    explicit WorkspaceFilter(QWidget* parent) : QComboBox(parent) {
        setMinimumContentsLength(4);
        setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        connect(this, &QComboBox::currentTextChanged, this, &QWidget::setToolTip);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QStyleOptionComboBox option;
        initStyleOption(&option);
        const auto field = style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxEditField, this);
        option.currentText = fontMetrics().elidedText(option.currentText, Qt::ElideRight, field.width());
        QStylePainter painter(this);
        painter.drawComplexControl(QStyle::CC_ComboBox, option);
        painter.drawControl(QStyle::CE_ComboBoxLabel, option);
    }
};

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
        if (isCollectionProject(currentItem()) || isCollectionProject(item)) { event->ignore(); return; }
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
    const auto personal = [&](QMenu* menu, const QString& text, auto slot) {
        auto* item = action(menu, text, slot); personalActions_ << item; return item;
    };
    auto* addProjectAction = action(projects, QStringLiteral("Новый проект…"), &MainWindow::addProject);
    action(projects, QStringLiteral("Новая группа…"), &MainWindow::addGroup);
    action(projects, QStringLiteral("Переименовать…"), &MainWindow::renameSelectedNode);
    action(projects, QStringLiteral("Переместить группу…"), &MainWindow::moveSelectedNode);
    action(projects, QStringLiteral("Удалить проект/группу…"), &MainWindow::deleteSelectedNode);
    auto* addEntryAction = personal(entries, QStringLiteral("Новая запись…"), &MainWindow::addEntry);
    addEntryAction->setShortcut(QKeySequence::New);
    auto* edit = personal(entries, QStringLiteral("Изменить запись…"), &MainWindow::editSelected); edit->setShortcut(QKeySequence(Qt::Key_F2));
    personal(entries, QStringLiteral("Удалить запись…"), &MainWindow::deleteSelected);
    entries->addSeparator();
    personal(entries, QStringLiteral("Переместить выбранные в группу…"), &MainWindow::bulkMoveDialog);
    personal(entries, QStringLiteral("Добавить тег выбранным…"), &MainWindow::bulkAddTagDialog);
    personal(entries, QStringLiteral("Снять тег у выбранных…"), &MainWindow::bulkRemoveTagDialog);
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
    action(settings,QStringLiteral("Коллекция агента…"),&MainWindow::showCollection)->setObjectName("openCollection");
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
    // The full workspace can scroll without imposing its minimum height on the window.
    central->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout->setSizeConstraint(QLayout::SetNoConstraint); setMinimumSize(480, 400);
    layout->setContentsMargins(10, 6, 10, 4); layout->setSpacing(6);
    auto* header = new QWidget(central); header->setObjectName(QStringLiteral("mainHeader"));
    headerLayout_ = new QBoxLayout(QBoxLayout::LeftToRight, header); headerLayout_->setContentsMargins(0, 0, 0, 0); headerLayout_->setSpacing(6);
    auto headingFont = font(); headingFont.setBold(true);
    auto* controls = new QWidget(central); auto* controlsLayout = new QHBoxLayout(controls);
    controlsLayout->setContentsMargins(0, 0, 0, 0);
    controlsLayout->setSpacing(6);
    search_ = new QLineEdit(controls); search_->setObjectName(QStringLiteral("vaultSearch")); search_->setAccessibleName(QStringLiteral("Поиск по хранилищу"));
    search_->setPlaceholderText(QStringLiteral("Поиск по названиям, логинам, проектам и заметкам — во всём хранилище"));
    search_->setPlaceholderText(QStringLiteral("Поиск по хранилищу…  Ctrl+K")); search_->setClearButtonEnabled(true);
    search_->setMinimumWidth(120); headerLayout_->addWidget(search_, 1);
    auto* entryRow = new QWidget(header); auto* entryRowLayout = new QHBoxLayout(entryRow); entryRowLayout->setContentsMargins(0, 0, 0, 0);
    auto* newEntry = new QToolButton(entryRow); newEntry->setDefaultAction(addEntryAction); newEntry->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newEntry->setAccessibleName(QStringLiteral("Новая запись")); entryRowLayout->addWidget(newEntry); headerLayout_->addWidget(entryRow);
    layout->addWidget(header);
    typeFilter_ = new WorkspaceFilter(controls); typeFilter_->setObjectName(QStringLiteral("typeFilter"));
    typeFilter_->setAccessibleName(QStringLiteral("Фильтр по типу записи"));
    typeFilter_->addItem(QStringLiteral("Все типы"),-1);
    for(int i=0;i<=int(model::EntryType::TextFile);++i) typeFilter_->addItem(entryTypeLabel(model::EntryType(i)),i);
    sortMode_ = new WorkspaceFilter(controls); sortMode_->setObjectName(QStringLiteral("sortMode"));
    sortMode_->setAccessibleName(QStringLiteral("Сортировка записей"));
    sortMode_->addItems({QStringLiteral("По названию"), QStringLiteral("По дате изменения")});
    expiryFilter_ = new WorkspaceFilter(controls); expiryFilter_->setObjectName(QStringLiteral("expiryFilter"));
    expiryFilter_->setAccessibleName(QStringLiteral("Фильтр по сроку действия"));
    expiryFilter_->addItems({QStringLiteral("Любой срок"), QStringLiteral("Истекают (30 дн.)"), QStringLiteral("Просрочены")});
    controlsLayout->addWidget(typeFilter_, 1); controlsLayout->addWidget(sortMode_, 1); controlsLayout->addWidget(expiryFilter_, 1);
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
    splitter->setHandleWidth(6);
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
    library->setMinimumWidth(140); tree_->setMinimumWidth(120);
    auto* newProject = new QToolButton(library); newProject->setDefaultAction(addProjectAction); newProject->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    newProject->setAccessibleName(QStringLiteral("Новый проект")); libraryLayout->addWidget(newProject);
    vaultTree->moveRequest = [this](model::Id node, model::Id target) { return moveNodeTo(node, target); };
    vaultTree->setDragEnabled(true); vaultTree->setAcceptDrops(true);
    vaultTree->setDragDropMode(QAbstractItemView::InternalMove); vaultTree->setDefaultDropAction(Qt::MoveAction);
    vaultTree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree_->setHeaderLabel(QStringLiteral("Проекты и группы")); tree_->setAccessibleName(QStringLiteral("Проекты и группы"));
    auto* workspaceScroll = new QScrollArea(splitter);
    workspaceScroll->setObjectName(QStringLiteral("workspaceScroll"));
    workspaceScroll->setFrameShape(QFrame::NoFrame);
    workspaceScroll->setWidgetResizable(true);
    workspaceScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* workspace = new QWidget(workspaceScroll);
    workspace->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(4);
    auto* workspaceSplit = new QSplitter(Qt::Vertical, workspace);
    workspaceSplit_ = workspaceSplit;
    workspaceSplit->setObjectName(QStringLiteral("workspaceSplit"));
    workspaceSplit->setChildrenCollapsible(false);
    workspaceSplit->setHandleWidth(6);
    auto* columns = new QWidget(workspaceSplit);
    columns->setObjectName(QStringLiteral("workspaceLists"));
    workspaceLists_ = new QBoxLayout(QBoxLayout::LeftToRight, columns);
    workspaceLists_->setContentsMargins(0, 0, 0, 0);
    workspaceLists_->setSpacing(6);
    auto* results = new QWidget(columns);
    results->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* resultsLayout = new QVBoxLayout(results); resultsLayout->setContentsMargins(0, 0, 0, 0); resultsLayout->setSpacing(4);
    auto* personalHeading = new QLabel(QStringLiteral("Личное хранилище"), results);
    personalHeading->setObjectName(QStringLiteral("personalHeading"));
    resultsLayout->addWidget(personalHeading);
    list_ = new QListWidget(results); list_->setObjectName("entriesList"); list_->setAccessibleName(QStringLiteral("Список записей")); list_->setSpacing(2);
    list_->setWordWrap(true); list_->setUniformItemSizes(true); list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    right_ = new QStackedWidget(workspaceSplit); right_->setObjectName(QStringLiteral("entryPanel"));
    auto* hint = new QLabel(QStringLiteral("Создайте проект и добавьте запись или выберите существующую запись."), right_);
    emptyHint_ = hint;
    hint->setWordWrap(true); hint->setTextFormat(Qt::PlainText); right_->addWidget(hint);
    cardScroll_ = new QScrollArea(right_); cardScroll_->setWidgetResizable(true); cardScroll_->setFrameShape(QFrame::NoFrame);
    card_ = new EntryCard(app_, cardScroll_); cardScroll_->setWidget(card_); right_->addWidget(cardScroll_);
    collectionScroll_ = new QScrollArea(right_);
    collectionScroll_->setWidgetResizable(true);
    collectionScroll_->setFrameShape(QFrame::NoFrame);
    collection_ = new CollectionPane(app_, [this](QWidget* parent) {
        return ensureSchemaUpgradeApproved(parent);
    }, appliedPreferences_.revealSeconds, clipboard_, collectionScroll_);
    collectionScroll_->setWidget(collection_);
    right_->addWidget(collectionScroll_);
    auto* collectionList = collection_->listWidget();
    collectionList->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    workspaceLists_->addWidget(results, 1);
    workspaceLists_->addWidget(collectionList, 1);
    workspaceSplit->addWidget(columns);
    workspaceSplit->addWidget(right_);
    workspaceSplit->setSizes({240, 360});
    splitter->addWidget(library); splitter->addWidget(workspaceScroll); splitter->setSizes({220, 1000});
    workspaceHeading_ = new QLabel(workspace);
    workspaceHeading_->setObjectName(QStringLiteral("workspaceHeading"));
    workspaceHeading_->setFont(headingFont);
    workspaceHeading_->setTextFormat(Qt::PlainText);
    workspaceHeading_->setWordWrap(true);
    workspaceLayout->addWidget(workspaceHeading_);
    searchScope_ = new QLabel(central); searchScope_->setObjectName(QStringLiteral("searchScope")); searchScope_->setTextFormat(Qt::PlainText);
    listSummary_ = new QLabel(central); listSummary_->setObjectName(QStringLiteral("listSummary")); listSummary_->setTextFormat(Qt::PlainText);
    searchScope_->setWordWrap(true); listSummary_->setWordWrap(true);
    workspaceLayout->addWidget(searchScope_);
    workspaceLayout->addWidget(controls);
    auto* tagsScroll = new QScrollArea(workspace); tagsScroll_ = tagsScroll; tagsScroll->setWidgetResizable(true); tagsScroll->setFrameShape(QFrame::NoFrame);
    tagsScroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff); tagsScroll->setFixedHeight(44); tagsScroll->setWidget(tagBar_);
    resultsTabs_=new QTabWidget(results); resultsTabs_->setObjectName("resultsTabs");
    registry_=new RegistryView(app_,resultsTabs_); resultsTabs_->addTab(list_,QStringLiteral("Список")); resultsTabs_->addTab(registry_,QStringLiteral("Реестр"));
    workspaceLayout->addWidget(tagsScroll);
    workspaceLayout->addWidget(workspaceSplit, 1);
    resultsLayout->addWidget(resultsTabs_, 1);
    resultsLayout->addWidget(listSummary_);
    columns->installEventFilter(this);
    updateWorkspaceLayout();
    workspaceScroll->setWidget(workspace);
    connect(collection_, &CollectionPane::accessRequested, this, &MainWindow::showCollection);
    connect(collection_, &CollectionPane::selectionActivated, this, [this] { setCollectionActive(true); });
    connect(collection_, &CollectionPane::projectsChanged, this, &MainWindow::refreshAll);
    connect(collection_, &CollectionPane::vaultChanged, this, &MainWindow::refreshAll);
    connect(resultsTabs_, &QTabWidget::currentChanged, this, [this] {
        if (resultsTabs_->currentWidget() == registry_) {
            if (const auto* item = list_->currentItem())
                registry_->selectEntry(item->data(Qt::UserRole).value<model::Id>());
        } else if (const auto id = registry_->currentEntry(); !id.isNull()) {
            const QSignalBlocker block(list_);
            for (int row = 0; row < list_->count(); ++row)
                if (list_->item(row)->data(Qt::UserRole).value<model::Id>() == id) {
                    list_->setCurrentRow(row, QItemSelectionModel::ClearAndSelect);
                    break;
                }
        }
        updateWorkspaceLayout();
        activatePersonal();
    });
    connect(registry_, &RegistryView::currentEntryChanged, this, [this] {
        if (resultsTabs_->currentWidget() != registry_) return;
        if (auto* focus = QApplication::focusWidget(); focus && registry_->isAncestorOf(focus)) activatePersonal();
        else showMetadata();
    });
    connect(registry_, &RegistryView::entryActivated, this, [this] { activatePersonal(); });
    connect(registry_, &RegistryView::copyRequested, this, [this](model::Id id) {
        if (ready() && selectedEntry() == id) { showMetadata(); card_->copySecret(); }
    });
    connect(registry_, &RegistryView::sourceRequested, this, [this](model::Id id) {
        if (!collectionActive_) openDocument(id);
    });
    layout->addWidget(splitter, 1);
    tree_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(tree_, &QTreeWidget::customContextMenuRequested, this, &MainWindow::treeMenu);
    list_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(list_, &QListWidget::customContextMenuRequested, this, &MainWindow::listMenu);
    connect(new QShortcut(QKeySequence::Find, this), &QShortcut::activated, this, [this] { search_->setFocus(Qt::ShortcutFocusReason); search_->selectAll(); });
    connect(new QShortcut(QKeySequence(QStringLiteral("Ctrl+K")), this), &QShortcut::activated, this, [this] { search_->setFocus(Qt::ShortcutFocusReason); search_->selectAll(); });
    auto* clearSearch = new QShortcut(QKeySequence(Qt::Key_Escape), search_); clearSearch->setContext(Qt::WidgetShortcut);
    connect(clearSearch, &QShortcut::activated, search_, &QLineEdit::clear);
    auto* deleteEntry = new QShortcut(QKeySequence::Delete, resultsTabs_); deleteEntry->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteEntry, &QShortcut::activated, this, &MainWindow::deleteSelected);
    auto* deleteNode = new QShortcut(QKeySequence::Delete, tree_); deleteNode->setContext(Qt::WidgetWithChildrenShortcut);
    connect(deleteNode, &QShortcut::activated, this, &MainWindow::deleteSelectedNode);
    error_ = new QLabel(central); error_->setObjectName(QStringLiteral("operationError")); error_->setTextFormat(Qt::PlainText); error_->setWordWrap(true); layout->addWidget(error_); error_->hide();
    setCentralWidget(central);
    auto* statuses = new QWidget(central); auto* statusLayout = new QGridLayout(statuses);
    statusLayout->setContentsMargins(6, 2, 6, 2);
    statusLayout->setVerticalSpacing(0);
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
    connect(tree_, &QTreeWidget::currentItemChanged, this, &MainWindow::selectLibraryTreeItem);
    connect(search_, &QLineEdit::textChanged, this, [this](const QString& text) {
        const QString requested = text;
        const auto apply = [this, requested] {
            const QSignalBlocker block(search_);
            acceptedSearch_ = requested;
            if (search_->text() != requested) search_->setText(requested);
            refillList();
            updateCollectionContext();
        };
        if (collectionActive_ && ready()) {
            { const QSignalBlocker block(search_); search_->setText(acceptedSearch_); }
            collection_->confirmNavigation(apply);
        } else apply();
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this] { activatePersonal(); });
    connect(list_, &QListWidget::itemSelectionChanged, this, &MainWindow::showMetadata);
    connect(list_, &QListWidget::itemDoubleClicked, this, [this] { if (!collectionActive_) editSelected(); });
    connect(list_, &QListWidget::itemClicked, this, [this] { activatePersonal(); });
    connect(list_, &QListWidget::itemActivated, this, [this] { activatePersonal(); });
    connect(card_, &EntryCard::editRequested, this, &MainWindow::editSelected);
    connect(card_,&EntryCard::documentRequested,this,&MainWindow::openDocument);
    connect(card_,&EntryCard::sourceRequested,this,&MainWindow::openDocument);
    connect(card_,&EntryCard::compareRequested,this,&MainWindow::compareWithSource);
    connect(card_, &EntryCard::moreRequested, this, [this] {
        const auto* more = card_->findChild<QPushButton*>(QStringLiteral("cardMore"));
        showEntryMenu(more->mapToGlobal(QPoint(0, more->height())));
    });
    connect(card_, &EntryCard::copyRequested, this, [this](const QString& text) {
        if (!ready() || collectionActive_) return; QString error;
        if (!clipboard_.copySecret(text, &error)) { if (app_.gate() == Application::Gate::Ready) report(error); }
        else {
            statusBar()->showMessage(QStringLiteral("Скопировано. Очистка буфера через %1 секунд.").arg(clipboard_.clearSeconds()), 5000);
            emit notifyRequested(QStringLiteral("Секрет скопирован. Буфер очистится через %1 секунд.").arg(clipboard_.clearSeconds()));
        }
    });
    connect(&clipboard_, &ClipboardGuard::cleared, this, [this] { emit notifyRequested(QStringLiteral("Буфер обмена очищен")); });
    connect(&app_, &Application::busyChanged, this, [this](bool busy) { updateEnabled(); refreshStatus(); statusBar()->showMessage(busy ? QStringLiteral("Выполняется операция…") : QString()); });
    connect(&app_, &Application::stateChanged, this, [this] {
        if (collectionAccess_ && (app_.gate() != Application::Gate::Ready || *collectionAccess_ != app_.generation())) {
            collectionAccess_.reset();
            collection_->clearSession();
            setCollectionActive(false);
        }
        if (app_.gate() != Application::Gate::Ready) {
            clipboard_.clearIfOurs(); card_->clear();
            for (auto* dialog : findChildren<QDialog*>()) {
                if (auto* editDialog = qobject_cast<EntryEditDialog*>(dialog)) editDialog->clearSecrets();
                dialog->reject();
            }
            search_->setText({}); report({}); saveUiState(); hide();
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
#ifdef Q_OS_WIN
void MainWindow::setAgentAccess(agent::Access* access) {
    agentAccess_=access;
    if(access) appliedPreferences_.mcpSshEnabled=access->sshEnabled();
}
bool MainWindow::sshOffSavePending() const {
    return agentAccess_ && !agentAccess_->sshEnabled() &&
        (agentAccess_->sshOffSavePending() || prefs::readMcpSshEnabled(*preferences_));
}
#endif
void MainWindow::showSettings() {
    if(!ready() || restoring_) return;
    card_->hideNowForTest();
    appliedPreferences_.clearEnvironmentOnExit=prefs::read(*preferences_).clearEnvironmentOnExit;
#ifdef Q_OS_WIN
    if(agentAccess_) appliedPreferences_.mcpSshEnabled=agentAccess_->sshEnabled();
#endif
    SettingsDialog dialog(appliedPreferences_,app_.backupInfo(),
#ifdef Q_OS_WIN
        hotkey_->sequence().toString(QKeySequence::NativeText),
#else
        QString(),
#endif
        this);
    dialog.setUnlockMode(app_.mode(),helloAvailable_);
#ifdef Q_OS_WIN
    dialog.setSshOffSavePending(sshOffSavePending());
#endif
    connect(&dialog,&SettingsDialog::saveRequested,this,[&] {
        if(restoring_) return;
        if(!ready()) {dialog.reject(); return;}
        const auto result=applyPreferences(dialog.draft());
        if(ready()) {
#ifdef Q_OS_WIN
            dialog.setSshOffSavePending(sshOffSavePending());
#endif
            dialog.setApplyResult(result,appliedPreferences_);
        }
    });
    connect(&dialog,&SettingsDialog::changePasswordRequested,this,[&] {
        changeMasterPassword(); if(ready()) dialog.setUnlockMode(app_.mode(),helloAvailable_);
    });
    connect(&dialog,&SettingsDialog::changeUnlockModeRequested,this,[&] {
        switchUnlockMode(); if(ready()) dialog.setUnlockMode(app_.mode(),helloAvailable_);
    });
    connect(&dialog,&SettingsDialog::restoreBackupRequested,this,&MainWindow::restoreBackup);
    bool openCollection = false;
    connect(&dialog,&SettingsDialog::manageCollectionRequested,this,[&] {
        if (!dialog.isVisible() || dialog.draft() != appliedPreferences_) return;
        openCollection = true;
        dialog.reject();
    });
    connect(&dialog,&SettingsDialog::issueCollectionTokenRequested,this,[&] {changeCollectionToken(dialog,false);});
    connect(&dialog,&SettingsDialog::revokeCollectionTokenRequested,this,[&] {changeCollectionToken(dialog,true);});
#ifdef Q_OS_WIN
    const bool suspended=hotkey_->suspended(); hotkey_->setSuspended(true);
    const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
    connect(&dialog,&SettingsDialog::clearEnvironmentRequested,this,&MainWindow::removeExportedVariables);
#endif
    dialog.exec();
    if (openCollection) QTimer::singleShot(0, this, &MainWindow::showCollection);
}
void MainWindow::showCollection() {
    if(!ready() || restoring_) return;
    const auto generation = app_.generation();
    if (collectionAccess_ == generation) {
        setCollectionActive(true);
        collection_->focusList();
        return;
    }
    const QPointer<MainWindow> self(this);
    card_->hideNowForTest();
    bool verified = false;
    {
#ifdef Q_OS_WIN
        const bool suspended = hotkey_->suspended();
        hotkey_->setSuspended(true);
        const auto resume = qScopeGuard([&] { if(self) self->hotkey_->setSuspended(suspended); });
#endif
        verified = VerifyDialog::verify(app_, this, QStringLiteral("Коллекция агента"));
    }
    if (!verified || !self || !ready() || restoring_ || app_.generation() != generation) return;
    collectionAccess_ = generation;
    updateCollectionContext();
    setCollectionActive(true);
    collection_->authorize(generation);
    collection_->focusList();
}
void MainWindow::changeCollectionToken(SettingsDialog& settings,bool revoke) {
    const QPointer<MainWindow> self(this);
    auto* core=&app_;
    const auto generation=core->generation();
    const auto allowed=[&] {
        return self && settings.isVisible() && self->ready() && !self->restoring_
            && core->generation()==generation && settings.draft()==self->appliedPreferences_;
    };
    if(!allowed()) return;
    settings.setOperationError({});
    const auto title=revoke?QStringLiteral("Отзыв токена агента"):QStringLiteral("Выпуск токена агента");
    const auto prompt=revoke
        ? QStringLiteral("Отозвать токен? Агент потеряет доступ к коллекции. Копии секретов останутся в коллекции.")
        : QStringLiteral("Выпустить новый токен? Предыдущий токен перестанет работать. Новый токен позволяет читать и изменять всю коллекцию агента.");
    if(QMessageBox::question(&settings,title,prompt,QMessageBox::Yes|QMessageBox::No,QMessageBox::No)!=QMessageBox::Yes
        || !allowed() || !VerifyDialog::verify(*core,&settings,title) || !allowed()) return;
    SecureBytes token;
    const auto error=revoke?core->revokeCollectionToken():core->issueCollectionToken(token);
    if(!allowed()) return;
    if(!error.isEmpty() || revoke) {
        settings.setOperationError(error.isEmpty()?QStringLiteral("Токен агента отозван."):error);
        return;
    }
    QDialog issued(&settings);
    issued.setObjectName("issuedCollectionToken");
    issued.setWindowTitle(title);
    auto* layout=new QVBoxLayout(&issued);
    auto* note=new QLabel(QStringLiteral("Токен показан один раз. Передайте его только доверенному агенту."),&issued);
    note->setWordWrap(true); layout->addWidget(note);
    auto* value=new QLabel(QString::fromLatin1(token.bytes()),&issued);
    value->setObjectName("collectionTokenValue"); value->setTextFormat(Qt::PlainText);
    layout->addWidget(value);
    auto* status=new QLabel(&issued); status->setTextFormat(Qt::PlainText); status->setWordWrap(true);
    layout->addWidget(status);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close,&issued);
    auto* copy=buttons->addButton(QStringLiteral("Копировать токен"),QDialogButtonBox::ActionRole);
    layout->addWidget(buttons);
    connect(buttons,&QDialogButtonBox::rejected,&issued,&QDialog::reject);
    connect(&issued,&QDialog::finished,&issued,[&] {value->clear(); token.clear();});
    connect(copy,&QPushButton::clicked,&issued,[&] {
        if(!allowed() || token.isEmpty()) return;
        QString copyError;
        if(self->clipboard_.copySecret(QString::fromLatin1(token.bytes()),&copyError))
            copyError=QStringLiteral("Токен скопирован. Буфер очистится автоматически.");
        if(allowed() && issued.isVisible()) status->setText(copyError);
    });
    issued.exec();
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
    if(!persist("secretRevealSeconds",requested.revealSeconds,appliedPreferences_.revealSeconds,requested.revealSeconds,[&] {
        card_->setRevealSeconds(requested.revealSeconds);
        collection_->setRevealSeconds(requested.revealSeconds);
    })) return result;
    if(!persist("clipboardClearSeconds",requested.clipboardSeconds,appliedPreferences_.clipboardSeconds,requested.clipboardSeconds,[&] {clipboard_.setClearSeconds(requested.clipboardSeconds);})) return result;
#ifdef Q_OS_WIN
    if(!persist("quickPasteDefaultField",requested.defaultField,appliedPreferences_.defaultField,
        requested.defaultField==paste::Field::Login?"login":"secret",[&] {hotkey_->setDefaultField(requested.defaultField);})) return result;
#endif
    const auto closing=requested.closeBehavior==prefs::CloseBehavior::Exit?"exit":requested.closeBehavior==prefs::CloseBehavior::LockHide?"lock-hide":"hide";
    if(!persist("closeBehavior",requested.closeBehavior,appliedPreferences_.closeBehavior,closing,[] {})) return result;
#ifdef Q_OS_WIN
    if(!persist("env-clear-on-exit",requested.clearEnvironmentOnExit,appliedPreferences_.clearEnvironmentOnExit,requested.clearEnvironmentOnExit,[] {})) return result;
    if(requested.mcpSshEnabled!=appliedPreferences_.mcpSshEnabled || sshOffSavePending()) {
        const auto key=QString::fromLatin1(prefs::McpSshSetting);
        if(!allowed(key)) return result;
        if(requested.mcpSshEnabled && (!agentAccess_ || !agentAccess_->start())) {
            result.failedKey=key;
            result.error=QStringLiteral("Не удалось запустить локальный интерфейс агента.");
            return result;
        }
        const bool fallbacks=preferences_->fallbacksEnabled();
        preferences_->setFallbacksEnabled(false);
        const auto restoreFallbacks=qScopeGuard([&] {preferences_->setFallbacksEnabled(fallbacks);});
        if(!requested.mcpSshEnabled && agentAccess_) {
            agentAccess_->setSshEnabled(false);
            appliedPreferences_.mcpSshEnabled=false;
        }
        if(!prefs::writeValue(*preferences_,key,requested.mcpSshEnabled,&result.error)) {
            result.failedKey=key;
            if(!requested.mcpSshEnabled && agentAccess_) {
                agentAccess_->setSshOffSavePending(true);
                result.error+=QStringLiteral(" SSH-команды выключены в текущем сеансе. Повторите сохранение, чтобы выключить их и после перезапуска.");
            }
            return result;
        }
        if(agentAccess_) {
            agentAccess_->setSshOffSavePending(false);
            if(requested.mcpSshEnabled) agentAccess_->setSshEnabled(true);
        }
        appliedPreferences_.mcpSshEnabled=requested.mcpSshEnabled;
        result.appliedKeys<<key;
    }
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
    const auto date = QDate::currentDate();
    if (date != statusDate_) {
        statusDate_ = date;
        displayedEntry_ = {};
        updateLibraryCounts();
        refillList();
    }
}
void MainWindow::restoreUiState() {
    resize(QSize(1240, 800).boundedTo(screen()->availableGeometry().size()));
    const auto geometry = preferences_->value(QStringLiteral("ui/mainGeometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    const auto area = screen()->availableGeometry().adjusted(8, 32, -8, -8);
    const auto state = preferences_->value(QStringLiteral("ui/workspaceSidebar")).toByteArray();
    if (state.isEmpty() || !splitter_->restoreState(state)) splitter_->setSizes({220,1000});
    const auto workspaceState = preferences_->value(QStringLiteral("ui/workspaceDetails")).toByteArray();
    if (!workspaceState.isEmpty()) workspaceSplit_->restoreState(workspaceState);
    const auto registryState=preferences_->value(QStringLiteral("ui/registryHeader")).toByteArray();
    if (!registryState.isEmpty()) registry_->header()->restoreState(registryState);
    resize(size().boundedTo(area.size()));
    move(qBound(area.left(), x(), area.right() - width() + 1), qBound(area.top(), y(), area.bottom() - height() + 1));
}
void MainWindow::saveUiState() {
    preferences_->setValue(QStringLiteral("ui/mainGeometry"), saveGeometry());
    preferences_->setValue(QStringLiteral("ui/workspaceSidebar"), splitter_->saveState());
    preferences_->setValue(QStringLiteral("ui/workspaceDetails"), workspaceSplit_->saveState());
    preferences_->setValue(QStringLiteral("ui/registryHeader"), registry_->header()->saveState());
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
    for (auto* action : personalActions_) action->setEnabled(ready() && !collectionActive_ && libraryView_ != LibraryView::CollectionProject);
#ifdef Q_OS_WIN
    if (envExportAction_) envExportAction_->setEnabled(canExportEnv());
#endif
}
model::Id MainWindow::selectedNode() const { const auto* item = tree_->currentItem(); return item ? item->data(0, Qt::UserRole).value<model::Id>() : model::Id{}; }
model::Id MainWindow::selectedEntry() const {
    if (collectionActive_ || libraryView_ == LibraryView::CollectionProject) return {};
    if (resultsTabs_->currentWidget() == registry_) return registry_->currentEntry();
    const auto* item = list_->currentItem();
    return item ? item->data(Qt::UserRole).value<model::Id>() : model::Id{};
}
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
    updateLibraryCounts();
    collection_->confirmNavigation([this, view] {
        libraryView_ = view;
        collectionProject_.clear();
        { const QSignalBlocker block(tree_); tree_->setCurrentItem(nullptr); tree_->clearSelection(); }
        { const QSignalBlocker block(expiryFilter_); expiryFilter_->setCurrentIndex(view == LibraryView::Expiring ? 1 : 0); }
        setCollectionActive(false);
        updateLibraryCounts();
        updateCollectionContext();
        refillList();
    });
}
void MainWindow::selectLibraryTreeItem(QTreeWidgetItem* item, QTreeWidgetItem* previous) {
    if (!item || !ready()) return;
    const bool agentProject = isCollectionProject(item);
    const auto project = item->data(0, CollectionProjectRole).toString();
    const auto id = item->data(0, Qt::UserRole).value<model::Id>();
    {
        const QSignalBlocker block(tree_);
        tree_->setCurrentItem(previous);
        if (!previous) tree_->clearSelection();
    }
    collection_->confirmNavigation([this, agentProject, project, id] {
        for (QTreeWidgetItemIterator it(tree_); *it; ++it) {
            const auto* candidate = *it;
            const bool match = agentProject
                ? isCollectionProject(candidate) && candidate->data(0, CollectionProjectRole).toString() == project
                : !isCollectionProject(candidate) && candidate->data(0, Qt::UserRole).value<model::Id>() == id;
            if (!match) continue;
            {
                const QSignalBlocker block(tree_);
                tree_->setCurrentItem(*it);
                tree_->scrollToItem(*it);
            }
            libraryView_ = agentProject ? LibraryView::CollectionProject : LibraryView::Group;
            collectionProject_ = project;
            { const QSignalBlocker block(expiryFilter_); expiryFilter_->setCurrentIndex(0); }
            collection_->deactivate();
            setCollectionActive(agentProject);
            updateLibraryCounts();
            updateCollectionContext();
            refillList();
            return;
        }
    });
}
void MainWindow::updateCollectionContext() {
    std::optional<QString> project;
    bool showEntries = true;
    QString heading = QStringLiteral("Библиотека");
    const auto node = selectedNode();
    if (libraryView_ == LibraryView::CollectionProject) {
        project = collectionProject_;
        heading = collectionProjectLabel(collectionProject_);
    } else if (libraryView_ == LibraryView::Group) {
        const auto& nodes = app_.model()->nodes();
        auto id = node;
        for (int depth = 0; !id.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
            const auto found = nodes.constFind(id);
            if (found == nodes.cend()) break;
            if (found->parent.isNull()) project = found->name;
            id = found->parent;
        }
        if (project) {
            const auto roots = std::count_if(nodes.cbegin(), nodes.cend(), [&project](const auto& candidate) {
                return candidate.parent.isNull() && candidate.name == *project;
            });
            showEntries = roots == 1;
        }
        heading = groupPath(node);
    } else if (libraryView_ == LibraryView::Favorites) heading = QStringLiteral("Избранное");
    else if (libraryView_ == LibraryView::Expiring) heading = QStringLiteral("Истекают");
    workspaceHeading_->setText(heading);
    collection_->setContext(project, node, search_->text(), showEntries);
}
void MainWindow::setCollectionActive(bool active) {
    if (collectionActive_ != active) {
        collectionActive_ = active;
        tree_->setDragEnabled(!active);
        tree_->setAcceptDrops(!active);
        for (const auto* name : {"registryCopy", "registrySource"})
            if (auto* button = registry_->findChild<QPushButton*>(QString::fromLatin1(name)))
                button->setVisible(!active);
        if (active) {
            const QSignalBlocker block(list_);
            list_->clearSelection();
            displayedEntry_ = {};
            card_->clear();
        }
        else collection_->deactivate();
#ifdef Q_OS_WIN
        if (hotkey_) hotkey_->setSuspended(active);
#endif
        updateEnabled();
    }
    showMetadata();
}
void MainWindow::activatePersonal() {
    if (!collectionActive_) { showMetadata(); return; }
    if (libraryView_ == LibraryView::CollectionProject) return;
    collection_->confirmNavigation([this] { setCollectionActive(false); });
}
void MainWindow::updateLibraryCounts() {
    int all = 0, favorites = 0, expiring = 0;
    if (app_.gate() == Application::Gate::Ready) {
        const auto today = QDate::currentDate();
        for (const auto& entry : app_.model()->entries()) {
            ++all; favorites += entry.favorite;
            const auto expiryDate = entry.expiresAt.date();
            const auto days = today.daysTo(expiryDate);
            expiring += expiryDate.isValid() && days >= 0 && days <= model::VaultModel::ExpiryWarningDays;
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
    if (collectionActive_ || libraryView_ == LibraryView::CollectionProject) return {};
    if (resultsTabs_->currentWidget() == registry_) {
        const auto id = registry_->currentEntry();
        return id.isNull() ? QList<model::Id>{} : QList<model::Id>{id};
    }
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
    if (!ready() || collectionActive_ || app_.generation() != generation) return false;
    for (auto id : ids) if (!app_.model()->entry(id)) { report(QStringLiteral("Выбранные записи изменились. Повторите действие.")); return false; }
    return true;
}
bool MainWindow::restoreSelectionForAction(const QList<model::Id>& ids, quint64 generation) {
    if (!selectionStill(ids, generation)) return false;
    if (resultsTabs_->currentWidget() == registry_) {
        registry_->selectEntry(ids.value(0));
    } else {
        const QSet<model::Id> wanted(ids.cbegin(), ids.cend());
        const QSignalBlocker block(list_);
        list_->clearSelection();
        for (int i = 0; i < list_->count(); ++i) {
            auto* item = list_->item(i);
            item->setSelected(wanted.contains(item->data(Qt::UserRole).value<model::Id>()));
        }
    }
    if (selectedEntries().size() == ids.size()) return true;
    showMetadata();
    report(QStringLiteral("Список изменился. Выберите записи ещё раз."));
    return false;
}
void MainWindow::bulkMoveDialog() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) { report(QStringLiteral("Выберите записи.")); return; }
    const auto generation = app_.generation();
    QMap<QString, QList<model::Id>> groups;
    for (const auto& node : app_.model()->nodes()) groups[groupPath(node.id)].append(node.id);
    QStringList labels;
    QList<model::Id> destinations;
    for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
        auto groupIds = it.value();
        std::sort(groupIds.begin(), groupIds.end());
        for (const auto id : groupIds) {
            labels.append(groupIds.size() == 1 ? it.key() : QStringLiteral("%1 [%2]").arg(it.key(), id.toString(QUuid::WithoutBraces)));
            destinations.append(id);
        }
    }
    QInputDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Переместить записи"));
    dialog.setLabelText(QStringLiteral("Группа для выбранных записей (%1):").arg(ids.size()));
    dialog.setComboBoxItems(labels);
    auto* choices = dialog.findChild<QComboBox*>();
    if (!choices) return;
    for (int i = 0; i < destinations.size(); ++i) choices->setItemData(i, destinations[i]);
    {
        QScopedValueRollback guard(editing_, true);
        if (dialog.exec() != QDialog::Accepted) return;
    }
    if (!restoreSelectionForAction(ids, generation)) return;
    bulkMove(choices->currentData().value<model::Id>());
}
void MainWindow::bulkAddTagDialog() {
    const auto ids = selectedEntries(); if (!ready() || editing_ || ids.isEmpty()) { report(QStringLiteral("Выберите записи.")); return; }
    const auto generation = app_.generation(); bool ok = false; QString tag;
    { QScopedValueRollback guard(editing_, true);
      tag = QInputDialog::getText(this, QStringLiteral("Добавить тег"), QStringLiteral("Тег для выбранных записей (%1):").arg(ids.size()), QLineEdit::Normal, {}, &ok); }
    if (ok && restoreSelectionForAction(ids, generation)) bulkAddTag(tag);
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
    if (ok && restoreSelectionForAction(ids, generation)) bulkRemoveTag(tag);
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
    if (!ready() || libraryView_ == LibraryView::CollectionProject) return false;
    const auto view = captureView(name);
    return changeViews([&](QList<model::SavedView>& views) { views << view; return QString(); });
}
bool MainWindow::applyView(const QString& name) {
    if (!ready()) return false;
    if (collectionActive_) {
        collection_->confirmNavigation([this, name] { setCollectionActive(false); applyView(name); });
        return true;
    }
    const auto& views = app_.model()->views();
    const auto it = std::find_if(views.cbegin(), views.cend(), [&](const auto& v) { return v.name.compare(name, Qt::CaseInsensitive) == 0; });
    if (it == views.cend()) return false;
    const auto v = *it;
    QTreeWidgetItem* groupItem = nullptr;
    if (!v.group.isNull()) for (QTreeWidgetItemIterator i(tree_); *i; ++i) if ((*i)->data(0, Qt::UserRole).value<model::Id>() == v.group) groupItem = *i;
    {
        QSignalBlocker b1(typeFilter_), b2(expiryFilter_), b3(sortMode_), b4(search_), b5(tree_);
        typeFilter_->setCurrentIndex(qMax(0, typeFilter_->findData(v.type)));
        sortMode_->setCurrentIndex(v.sort); search_->setText(v.search); acceptedSearch_ = v.search;
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
    refillTagBar(); updateLibraryCounts(); updateCollectionContext(); refillList();
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
        QHash<QString, QList<QTreeWidgetItem*>> rootsByName;
        const std::function<void(model::Id,QTreeWidgetItem*)> add = [&](model::Id parent, QTreeWidgetItem* item) {
            for (auto id : app_.model()->childNodes(parent)) {
                const auto name = app_.model()->nodes().value(id).name;
                auto* child = new QTreeWidgetItem(QStringList{name});
                child->setData(0, Qt::UserRole, id); (item ? item : tree_->invisibleRootItem())->addChild(child);
                if (!item) rootsByName[name].append(child);
                if (id == node) selection = child; add(id, child);
            }
        };
        add({}, nullptr);
        if (collectionAccess_ == app_.generation()) {
            for (const auto& project : collection_->projects()) {
                const auto matching = rootsByName.value(project);
                QTreeWidgetItem* item = nullptr;
                if (!project.isEmpty() && matching.size() == 1) item = matching.first();
                else {
                    item = new QTreeWidgetItem(tree_, {collectionProjectLabel(project)});
                    item->setData(0, CollectionProjectRole, project);
                    item->setData(0, Qt::AccessibleDescriptionRole, QStringLiteral("Проект коллекции агента"));
                    item->setToolTip(0, QStringLiteral("Проект коллекции агента"));
                    item->setIcon(0, style()->standardIcon(QStyle::SP_DriveNetIcon));
                    item->setFlags(item->flags() & ~(Qt::ItemIsDragEnabled | Qt::ItemIsDropEnabled));
                }
                if (libraryView_ == LibraryView::CollectionProject && collectionProject_ == project) {
                    selection = item;
                    if (!isCollectionProject(item)) libraryView_ = LibraryView::Group;
                }
            }
        }
        tree_->expandToDepth(0);
        if (selection && (libraryView_ == LibraryView::Group || libraryView_ == LibraryView::CollectionProject))
            tree_->setCurrentItem(selection);
        else if (libraryView_ == LibraryView::Group || libraryView_ == LibraryView::CollectionProject)
            libraryView_ = LibraryView::All;
    }
    // Порядок важен: refillTagBar сначала обрезает «мёртвые» выбранные теги (после
    // переименования/удаления выбранного чипа) и лишь затем refillList рисует список —
    // иначе список фильтруется по устаревшему набору уже несуществующим тегом.
    displayedEntry_ = {}; // Explicit model refresh may change metadata of the same UUID.
    refillTagBar(); updateLibraryCounts(); refillViews(); updateCollectionContext();
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
        const auto today = QDate::currentDate();
        auto ids = libraryView_ == LibraryView::CollectionProject ? QList<model::Id>{}
            : !search_->text().isEmpty() ? app_.model()->search(search_->text())
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
        // Срок истекает после окончания указанной локальной даты.
        const int expiryMode = expiryFilter_->currentIndex();
        if (expiryMode > 0)
            ids.erase(std::remove_if(ids.begin(), ids.end(), [this, expiryMode, today](model::Id id) {
                const auto* entry = app_.model()->entry(id);
                if (!entry || !entry->expiresAt.isValid()) return true; // без срока не попадают ни в одну ветку
                const qint64 days = today.daysTo(entry->expiresAt.date());
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
            const auto expiryDate = entry->expiresAt.date();
            const bool expired = expiryDate.isValid() && expiryDate < today;
            if (expiryDate.isValid()) {
                const QString dateText = expiryDate.toString(QStringLiteral("dd.MM.yyyy"));
                if (expired) title += QStringLiteral("  ⏰ просрочен %1").arg(dateText);
                else if (today.daysTo(expiryDate) <= model::VaultModel::ExpiryWarningDays)
                    title += QStringLiteral("  ⏳ до %1").arg(dateText);
            }
            auto* item = new QListWidgetItem(title, list_); item->setData(Qt::UserRole, id);
            if (id == selected) list_->setCurrentItem(item);
            if (expired)
                item->setForeground(palette().brush(QPalette::Disabled, QPalette::Text)); // просроченные приглушены
        }
        if (!list_->currentItem() && list_->count()) list_->setCurrentRow(0);
        if (list_->currentItem()) registry_->selectEntry(list_->currentItem()->data(Qt::UserRole).value<model::Id>());
        if (collectionActive_) list_->clearSelection();
    }
    searchScope_->setText(libraryView_ == LibraryView::CollectionProject ? QStringLiteral("Проект коллекции агента")
        : !search_->text().isEmpty() ? QStringLiteral("Поиск по всему хранилищу")
        : libraryView_ == LibraryView::Group ? groupPath(selectedNode()) : QStringLiteral("Библиотека"));
    searchScope_->setVisible(!search_->text().isEmpty());
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
    QHash<QString, int> uses;
    const auto tags = app_.gate() == Application::Gate::Ready ? app_.model()->allTags(&uses) : QStringList();
    tagsScroll_->setVisible(!tags.isEmpty());
    // Обрезка «мёртвых» выбранных тегов: после переименования/удаления или блокировки
    // тега может уже не быть — выбор не должен фильтровать список несуществующим тегом.
    QStringList alive;
    for (const QString& selected : selectedTags_)
        if (uses.contains(selected.toCaseFolded()))
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
        auto* chip = new QToolButton(tagBar_);
        chip->setText(tag); chip->setCheckable(true); chip->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chip->setToolTip(QStringLiteral("Записей: %1").arg(uses.value(tag.toCaseFolded())));
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
    if (collectionActive_ && app_.gate() == Application::Gate::Ready) {
        right_->setCurrentWidget(collectionScroll_);
#ifdef Q_OS_WIN
        if (envExportAction_) envExportAction_->setEnabled(false);
#endif
        return;
    }
    if (const int selected = int(selectedEntries().size()); selected > 1 && app_.gate() == Application::Gate::Ready) {
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
void MainWindow::report(const QString& error) {
    error_->setText(error);
    error_->setVisible(!error.isEmpty());
    if (!error.isEmpty()) statusBar()->showMessage(QStringLiteral("Операция не выполнена."));
}
bool MainWindow::ensureSchemaUpgradeApproved(QWidget* parent) {
    if(!ready()) return false;
    if(!app_.schemaUpgradeRequired() || app_.schemaUpgradeApproved()) return true;
    const auto generation=app_.generation(); const auto before=*app_.model();
#ifdef Q_OS_WIN
    const auto suspended=hotkey_->suspended(); hotkey_->setSuspended(true); const auto resume=qScopeGuard([&] {hotkey_->setSuspended(suspended);});
#endif
    QMessageBox box(QMessageBox::Warning,QStringLiteral("Обновление формата"),QStringLiteral("Данные будут сохранены в формате v%1. Прежняя версия программы не откроет новый формат. Перед записью будет создана уникальная проверенная зашифрованная копия прежнего файла в каталоге хранилища. Продолжить?").arg(model::VaultModel::CurrentVersion),QMessageBox::Yes|QMessageBox::No,parent?parent:this);
    box.setObjectName("schemaUpgradeConfirmation"); box.setTextFormat(Qt::PlainText); box.setDefaultButton(QMessageBox::No); box.setEscapeButton(QMessageBox::No);
    if(box.exec()!=QMessageBox::Yes) {report({}); return false;}
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
    report({}); refreshAll(); statusBar()->showMessage(QStringLiteral("Сохранено"), 3000); return true;
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
    report({}); refreshAll(); statusBar()->showMessage(QStringLiteral("Сохранено"), 3000); return true;
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
    if (!ready() || editing_ || collectionActive_) return; const auto parent = selectedNode(); if (parent.isNull()) { report(QStringLiteral("Сначала выберите проект или группу.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation(); bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Новая группа"), QStringLiteral("Имя группы:"), QLineEdit::Normal, {}, &ok);
    const auto id = QUuid::createUuid();
    if (ok && ready() && !collectionActive_ && app_.generation() == generation && selectedNode() == parent
        && commit([&](auto& model) { return model.addNode(id, parent, name); })) selectNode(id);
}
void MainWindow::addEntry() {
    if (!ready() || editing_ || collectionActive_ || libraryView_ == LibraryView::CollectionProject) return;
    const auto parent = selectedNode();
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
    if (!ready() || editing_ || collectionActive_) return; const auto id = selectedNode(); if (id.isNull()) { report(QStringLiteral("Выберите проект или группу.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation(); bool ok = false;
    const auto name = QInputDialog::getText(this, QStringLiteral("Переименование"), QStringLiteral("Новое имя:"), QLineEdit::Normal, app_.model()->nodes().value(id).name, &ok);
    if (ok && ready() && !collectionActive_ && app_.generation() == generation && selectedNode() == id)
        commit([&](auto& model) { return model.renameNode(id, name); });
}
void MainWindow::moveSelectedNode() {
    if (!ready() || editing_ || collectionActive_) return; const auto id = selectedNode();
    if (id.isNull() || app_.model()->nodes().value(id).parent.isNull()) { report(QStringLiteral("Выберите группу для переноса.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    QDialog dialog(this); dialog.setWindowTitle(QStringLiteral("Перемещение группы")); auto* layout = new QVBoxLayout(&dialog);
    auto* combo = new QComboBox(&dialog); combo->setObjectName(QStringLiteral("moveTarget")); combo->setAccessibleName(QStringLiteral("Новый родитель группы")); populateGroups(*combo, *app_.model()); layout->addWidget(combo);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog); buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Переместить")); buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена")); layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept); connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() == QDialog::Accepted && ready() && !collectionActive_
        && app_.generation() == generation && selectedNode() == id)
        commit([&](auto& model) { return model.moveNode(id, combo->currentData().value<model::Id>()); });
}
void MainWindow::deleteSelected() {
    if (collectionActive_) return;
    if (selectedEntries().size() > 1) { bulkDelete(); return; }
    if (!ready() || editing_) return; const auto id = selectedEntry(); if (id.isNull()) { report(QStringLiteral("Выберите запись для удаления.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (QMessageBox::question(this, QStringLiteral("Удаление записи"), QStringLiteral("Удалить выбранную запись?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes
        && ready() && app_.generation() == generation) commitEntry([&](auto& model) { return model.removeEntry(id); }, id);
}
void MainWindow::deleteSelectedNode() {
    if (!ready() || editing_ || collectionActive_) return; const auto id = selectedNode(); if (id.isNull()) { report(QStringLiteral("Выберите проект или группу.")); return; }
    QScopedValueRollback guard(editing_, true); const auto generation = app_.generation();
    if (QMessageBox::question(this, QStringLiteral("Удаление проекта/группы"), QStringLiteral("Удалить выбранный проект или группу?"), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes
        && ready() && !collectionActive_ && app_.generation() == generation && selectedNode() == id)
        commit([&](auto& model) { return model.removeNode(id); });
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
        QStringLiteral("Файл содержит основное хранилище и записи коллекции агента. "
                       "Для переноса на другой компьютер понадобятся этот файл и отдельный пароль экспорта. "
                       "Для импорта нужна версия TokenBucket с поддержкой переноса коллекции. После импорта выпустите новый токен для доступа агента."),
        QStringLiteral("&Пароль защищённого экспорта (12–1024 байта UTF-8):"));
    if (dialog.exec() != QDialog::Accepted || !ready() || app_.generation() != generation) return;
    SecureBytes password(dialog.password()); dialog.clearSecrets();
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Защищённый экспорт"), {}, QStringLiteral("TokenBucket (*.tb)"));
    if (path.isEmpty() || !ready() || app_.generation() != generation) { password.clear(); return; }
    QString error;
    const QByteArray bytes = app_.exportContainer(password.bytes(), &error);
    password.clear();
    if (!ready() || app_.generation() != generation) return;
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
            QStringLiteral("Импорт заменит основное хранилище. Полный экспорт также заменит все записи коллекции агента, даже если экспортированная коллекция пуста. "
                           "При импорте старого файла без коллекции её текущие записи сохранятся. Действующий токен агента будет отозван. Продолжить?"),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Импорт из файла"), {}, QStringLiteral("TokenBucket (*.tb);;Все файлы (*)"));
    if (path.isEmpty() || !ready() || app_.generation() != generation) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > container::MaxTransferFileBytes) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось открыть файл импорта или файл слишком велик.")); return;
    }
    const QByteArray bytes = file.read(container::MaxTransferFileBytes + 1);
    if (file.error() != QFileDevice::NoError || !file.atEnd() || bytes.size() != file.size()) {
        QMessageBox::critical(this, QStringLiteral("TokenBucket"), QStringLiteral("Не удалось полностью прочитать файл импорта.")); return;
    }
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
    if (!ready() || collectionActive_ || node == newParent || newParent.isNull()) { refreshAll(); return false; }
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
    auto* item = tree_->itemAt(pos);
    if (isCollectionProject(item)) return;
    const auto id = item ? item->data(0, Qt::UserRole).value<model::Id>() : model::Id{};
    if (item) {
        tree_->setCurrentItem(item);
        if (collectionActive_ || id.isNull() || selectedNode() != id) return;
    }
    QMenu menu(this);
    menu.addAction(QStringLiteral("Новый проект…"), this, &MainWindow::addProject);
    if (item) {
        const auto generation = app_.generation();
        const auto bound = [this, id, generation](auto action) {
            return [this, id, generation, action] {
                if (ready() && !collectionActive_ && app_.generation() == generation &&
                    selectedNode() == id && app_.model()->nodes().contains(id)) (this->*action)();
            };
        };
        menu.addAction(QStringLiteral("Новая группа…"), this, bound(&MainWindow::addGroup));
        menu.addAction(QStringLiteral("Переименовать…"), this, bound(&MainWindow::renameSelectedNode));
        if (!app_.model()->nodes().value(id).parent.isNull())
            menu.addAction(QStringLiteral("Переместить группу…"), this, bound(&MainWindow::moveSelectedNode));
        menu.addSeparator();
        menu.addAction(QStringLiteral("Удалить"), this, bound(&MainWindow::deleteSelectedNode));
    }
    menu.exec(tree_->viewport()->mapToGlobal(pos));
}
void MainWindow::listMenu(const QPoint& pos) {
    const auto item = list_->itemAt(pos);
    if (!item || !ready()) return;
    if (collectionActive_) {
        activatePersonal();
        if (collectionActive_) return;
    }
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
    if(entry && selectedEntries().size()==1) {
        const auto* value=paste::fieldValue(*entry,paste::Field::Secret);
        QString label;
        if(value && !value->isEmpty()) {
            if(entry->type==model::EntryType::Credentials) label=QStringLiteral("Добавить пароль в коллекцию агента…");
            else if(entry->type==model::EntryType::ApiToken) label=QStringLiteral("Добавить токен в коллекцию агента…");
            else label=QStringLiteral("Добавить значение в коллекцию агента…");
        } else if(entry->type==model::EntryType::SshKey && !entry->sshPrivate.isEmpty()) {
            label=QStringLiteral("Добавить приватный ключ в коллекцию агента…");
        }
        if(!label.isEmpty()) {
            auto* add=menu.addAction(label,this,[this,id,generation=app_.generation()] {
                copyEntryToCollection(id,generation);
            });
            add->setObjectName("copyEntryToCollection");
        }
    }
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
void MainWindow::copyEntryToCollection(model::Id id,quint64 generation) {
    const QPointer<MainWindow> self(this);
    auto* core=&app_;
    const auto allowed=[&] {
        return self && self->ready() && core->generation()==generation
            && self->selectedEntry()==id && self->selectedEntries().size()==1;
    };
    if(!allowed() || editing_) return;
    const auto* entry=core->model()->entry(id);
    if(!entry) return;
    const auto type=entry->type;
    QString sourceProject;
    auto group = entry->groupId;
    for (int depth = 0; !group.isNull() && depth < model::VaultModel::MaxDepth; ++depth) {
        const auto node = core->model()->nodes().constFind(group);
        if (node == core->model()->nodes().cend()) break;
        if (node->parent.isNull()) sourceProject = node->name;
        group = node->parent;
    }
    QDialog dialog(this);
    dialog.setObjectName("collectionItemName");
    dialog.setWindowTitle(QStringLiteral("Отдельная копия для агента"));
    dialog.resize(460, 200);
    auto* form = new QFormLayout(&dialog);
    auto* hint = new QLabel(QStringLiteral("Агент сможет читать эту копию по токену даже после блокировки. "
                                           "Изменения исходной записи не затронут копию."), &dialog);
    hint->setWordWrap(true);
    form->addRow(hint);
    auto* project = new QLineEdit(sourceProject, &dialog);
    project->setObjectName("collectionCopyProject");
    project->setPlaceholderText(QStringLiteral("Без проекта"));
    form->addRow(QStringLiteral("&Проект коллекции"), project);
    auto* name = new QLineEdit(type==model::EntryType::PlainValue && !entry->parameterName.isEmpty()
        ? entry->parameterName : entry->title, &dialog);
    name->setObjectName("collectionCopyName");
    form->addRow(QStringLiteral("&Имя записи"), name);
    QCheckBox* decode = nullptr;
    if (type == model::EntryType::ApiToken && entry->reverify && entry->note == Application::collectionBinaryNote()) {
        decode = new QCheckBox(QStringLiteral("Декодировать Base64 в исходные байты"), &dialog);
        decode->setObjectName("collectionCopyDecodeBase64");
        decode->setChecked(true);
        form->addRow(decode);
    }
    auto* status = new QLabel(&dialog);
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    form->addRow(status);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("Добавить копию"));
    buttons->button(QDialogButtonBox::Save)->setObjectName("collectionCopySave");
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!allowed() || !authorizeEntry(*core,id,&dialog,true) || !allowed() || !dialog.isVisible()) return;
        const auto* current = core->model()->entry(id);
        if (!current || current->type != type) return;
        const auto* secret = paste::fieldValue(*current,paste::Field::Secret);
        SecureBytes value(secret ? secret->toUtf8() : type==model::EntryType::SshKey
            ? QByteArray(current->sshPrivate.constData(),current->sshPrivate.size()) : QByteArray());
        if (decode && decode->isChecked()) {
            auto result = QByteArray::fromBase64Encoding(value.bytes(), QByteArray::AbortOnBase64DecodingErrors);
            SecureBytes decoded(std::move(result.decoded));
            const SecureBytes canonical(decoded.bytes().toBase64());
            if (!current->reverify || current->note != Application::collectionBinaryNote() ||
                !result || canonical.bytes() != value.bytes()) {
                status->setText(QStringLiteral("Значение не является корректным Base64. Проверьте исходную запись."));
                return;
            }
            value = std::move(decoded);
        }
        const auto error = core->copyToCollection(project->text(), name->text(), value.bytes());
        if (!allowed()) return;
        if (!error.isEmpty()) status->setText(error);
        else dialog.accept();
    });
    name->selectAll();
    name->setFocus();
    if (dialog.exec() == QDialog::Accepted && allowed()) {
        self->statusBar()->showMessage(QStringLiteral("Отдельная копия добавлена в коллекцию агента."),5000);
        if (self->collectionAccess_ == generation) self->collection_->refresh();
    }
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
    if (!ready() || selectedEntries().size() > 1) return false;
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
bool MainWindow::eventFilter(QObject* watched,QEvent* event) {
    if ((event->type() == QEvent::Resize || event->type() == QEvent::LayoutRequest || event->type() == QEvent::FontChange)
        && workspaceLists_ && watched == workspaceLists_->parentWidget())
        updateWorkspaceLayout();
    return QMainWindow::eventFilter(watched,event);
}
void MainWindow::updateWorkspaceLayout() {
    auto* agent = collection_->listWidget();
    auto* entries = agent->findChild<QTreeWidget*>("collectionItems");
    const auto* add = agent->findChild<QPushButton*>("collectionAdd");
    const auto* reload = agent->findChild<QPushButton*>("collectionReload");
    if (entries)
        entries->setMinimumHeight(2 * entries->fontMetrics().lineSpacing() + 2 * entries->frameWidth() + 8);
    list_->setMinimumHeight(2 * list_->fontMetrics().lineSpacing() + 2 * list_->frameWidth() + 8);
    right_->setMinimumHeight(7 * right_->fontMetrics().lineSpacing() + 12);

    const int personalWidth = resultsTabs_->currentWidget() == registry_
        ? resultsTabs_->minimumSizeHint().width() : resultsTabs_->tabBar()->minimumSizeHint().width();
    // The collection action row already wraps, so each button must fit in the column on its own.
    const int agentWidth = add && reload ? qMax(add->sizeHint().width(), reload->sizeHint().width()) : 0;
    const bool stacked = workspaceLists_->parentWidget()->width() < personalWidth + agentWidth + workspaceLists_->spacing();
    const auto direction = stacked ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (workspaceLists_->direction() != direction) workspaceLists_->setDirection(direction);
    const int personalStretch = stacked ? 1 : qMax(1, personalWidth);
    const int agentStretch = stacked ? 1 : qMax(1, agentWidth);
    if (workspaceLists_->stretch(0) != personalStretch) workspaceLists_->setStretch(0, personalStretch);
    if (workspaceLists_->stretch(1) != agentStretch) workspaceLists_->setStretch(1, agentStretch);
}
void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    if (headerLayout_) {
        const int toolbarWidth=search_->minimumWidth()+headerLayout_->itemAt(1)->minimumSize().width()+headerLayout_->spacing();
        headerLayout_->setDirection(centralWidget()->contentsRect().width()-20 < toolbarWidth
            ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    }
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
    if (hotkey_ && !collectionActive_) hotkey_->popupPanel();
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
