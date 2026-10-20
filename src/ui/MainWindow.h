#pragma once
#include <QMainWindow>
#include <optional>
#include "../app/Application.h"
#include "../platform/ClipboardGuard.h"
#include <memory>
namespace tb::platform { class QuickPaste; }
class QTreeWidget; class QListWidget; class QLineEdit; class QStackedWidget; class QComboBox;
class QScrollArea; class QLabel; class QAction;
namespace tb::ui {
class EntryCard;
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Application& app, QWidget* parent = nullptr);
    ~MainWindow() override;
    void refreshAll();
    bool moveNodeTo(model::Id node, model::Id newParent);
    // Точечный откат ОДНОЙ записи вместо полного снимка модели: секреты не размножаются
    // по куче при каждом редактировании (audit M-3). Откат невозможен после lock.
    bool commitEntry(const std::function<QString(model::VaultModel&)>& mutation, model::Id entryId);
    void requestExit();
    void applyIdleTimeout(int minutes);
    void popupQuickPanel();
    QTreeWidget* tree() const { return tree_; }
    QListWidget* list() const { return list_; }
    QLineEdit* search() const { return search_; }
    EntryCard* card() const { return card_; }
signals:
    void closing();
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
    void configureIdleTimeout();
    void showAbout();
#ifdef Q_OS_WIN
    void changeHotkey();
    void exportToEnvironment();
    void removeExportedVariables();
#endif
protected:
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
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
