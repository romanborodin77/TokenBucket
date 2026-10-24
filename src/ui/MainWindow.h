#pragma once
#include <QMainWindow>
#include <QSettings>
#include <QSystemTrayIcon>
#include <functional>
#include <QPointer>
#include <QDate>
#include <optional>
#include "../app/Application.h"
#include "../app/Preferences.h"
#include "../platform/ClipboardGuard.h"
#include <memory>
namespace tb::platform { class QuickPaste; }
namespace tb { class IdleTimer; }
class QTreeWidget; class QListWidget; class QLineEdit; class QStackedWidget; class QComboBox;
class QScrollArea; class QLabel; class QAction;
class QPushButton;
class QSplitter; class QBoxLayout;
namespace tb::ui {
class EntryCard;
// Главное окно: дерево проектов/групп — список записей с фильтрами/сортировкой —
// карточка записи. Все изменения модели идут через commit()/commitEntry()
// (сохранение + откат при отказе). Крестик следует сохранённой политике закрытия;
// настоящий выход — requestExit() («Выход» из трея/меню) всегда блокирует. Живёт один
// цикл Ready (пересоздаётся после разблокировки) — поэтому трей и таймеры
// принадлежат main, а не окну.
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Application& app, QWidget* parent = nullptr, QSettings* uiPreferences = nullptr,
                        QSettings* runPreferences = nullptr,
                        std::function<bool()> trayAvailable=QSystemTrayIcon::isSystemTrayAvailable);
    ~MainWindow() override;
    void refreshAll();
    bool moveNodeTo(model::Id node, model::Id newParent);
    // Точечный откат ОДНОЙ записи вместо полного снимка модели: секреты не размножаются
    // по куче при каждом редактировании (audit M-3). Откат невозможен после lock.
    bool commitEntry(const std::function<QString(model::VaultModel&)>& mutation, model::Id entryId);
    void requestExit();
    void applyIdleTimeout(int minutes);
    void popupQuickPanel();
    void setIdleTimer(IdleTimer* timer);
    QTreeWidget* tree() const { return tree_; }
    QListWidget* list() const { return list_; }
    QLineEdit* search() const { return search_; }
    EntryCard* card() const { return card_; }
signals:
    void closing();
    void lockedHiddenRequested();
    // Короткие события для области уведомлений (без секретов): main связывает с треем.
    void notifyRequested(const QString& text);
    void idleTimeoutChanged(int minutes);
public slots:
    void addProject();
    void addGroup();
    void addEntry();
    void editSelected();
    void deleteSelected();
    void renameSelectedNode();
    void moveSelectedNode();
    void deleteSelectedNode();
    void exportVault();
    void importFromCsvKeePass(); // выбор файла через диалог; применение — applyImport
    void importVault();
    void switchUnlockMode();
    void changeMasterPassword();
    void showAbout();
    void showSettings();
#ifdef Q_OS_WIN
    void exportToEnvironment();
    void removeExportedVariables();
#endif
protected:
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;
private slots:
    void refreshStatus();
private:
    enum class LibraryView { All, Favorites, Expiring, Group };
    void setLibraryView(LibraryView view);
    void updateLibraryCounts();
    QString groupPath(model::Id id) const;
    LibraryView libraryView_ = LibraryView::All;
    QPushButton *libraryAll_{}, *libraryFavorites_{}, *libraryExpiring_{};
    QLabel *searchScope_{}, *listSummary_{};
    model::Id displayedEntry_;
    void restoreUiState();
    void saveUiState();
    QSettings uiSettings_{QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket")};
    QSettings* preferences_{};
    prefs::Values appliedPreferences_;
    std::function<bool()> trayAvailable_;
#ifdef Q_OS_WIN
    QSettings runSettings_{QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)"),QSettings::NativeFormat};
    QSettings* runPreferences_{};
#endif
    prefs::ApplyResult applyPreferences(const prefs::Values&);
    QSplitter* splitter_{};
    QBoxLayout *headerLayout_{}, *filterLayout_{};
    QLabel* emptyHint_{};
    bool geometryErrorReported_ = false;
    QPointer<IdleTimer> idleTimer_;
    QLabel *idleStatus_{}, *clipboardStatus_{}, *hotkeyStatus_{};
    QDate statusDate_ = QDateTime::currentDateTimeUtc().date();
    bool ready() const;
    bool commit(const std::function<QString(model::VaultModel&)>& mutation);
    void rollbackEntry(model::Id entryId, const std::optional<model::Entry>& backup);
    void report(const QString& error);
    void updateEnabled();
    void refillList(model::Id entry = {});
    void refillTagBar();
    void renameTag(const QString& tag);
    void removeTag(const QString& tag);
    void showMetadata();
    void openCardDialog();
    void selectNode(model::Id id);
    void treeMenu(const QPoint& pos);
    void listMenu(const QPoint& pos);
    void showEntryMenu(const QPoint& globalPos);
    void toggleFavorite();
    void updateSwitchActionText();
    int applyImport(const QByteArray& bytes, bool keepassXml, QString* err); // возвращает число добавленных
    model::Id selectedNode() const;
    model::Id selectedEntry() const;
#ifdef Q_OS_WIN
    bool canExportEnv() const;
#endif
    Application& app_;
    QTreeWidget* tree_{};
    QListWidget* list_{};
    QLineEdit* search_{};
    QComboBox* typeFilter_{};
    QComboBox* sortMode_{};
    QComboBox* expiryFilter_{};
    QWidget* tagBar_{};
    QStringList selectedTags_{};
    QStackedWidget* right_{};
    EntryCard* card_{};
    QScrollArea* cardScroll_{};
    ClipboardGuard clipboard_;
    QLabel* error_{};
    QLabel* vaultInfo_{};
    QList<QAction*> mutations_;
    QAction* switchAction_{};
    QAction* changePwAction_{};
    bool helloAvailable_ = false;
#ifdef Q_OS_WIN
    std::unique_ptr<platform::QuickPaste> hotkey_;
    QAction* envExportAction_{};
#endif
    bool editing_ = false, narrow_ = false, cardOpen_ = false;
    bool exiting_ = false;
};
}
