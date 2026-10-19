#pragma once
#include <QMainWindow>
#include "../app/Application.h"
class QTreeWidget; class QListWidget; class QLineEdit; class QStackedWidget;
class QPlainTextEdit; class QLabel; class QAction;
namespace tb::ui {
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(Application& app, QWidget* parent = nullptr);
    void refreshAll();
    QTreeWidget* tree() const { return tree_; }
    QListWidget* list() const { return list_; }
    QLineEdit* search() const { return search_; }
signals:
    void closing();
public slots:
    void addProject();
    void addGroup();
    void addEntry();
    void editSelected();
    void deleteSelected();
    void renameSelectedNode();
    void moveSelectedNode();
    void deleteSelectedNode();
protected:
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;
private:
    bool ready() const;
    bool commit(const std::function<QString(model::VaultModel&)>& mutation);
    void report(const QString& error);
    void updateEnabled();
    void refillList(model::Id entry = {});
    void showMetadata();
    void selectNode(model::Id id);
    model::Id selectedNode() const;
    model::Id selectedEntry() const;
    Application& app_;
    QTreeWidget* tree_{};
    QListWidget* list_{};
    QLineEdit* search_{};
    QStackedWidget* right_{};
    QPlainTextEdit* details_{};
    QLabel* error_{};
    QList<QAction*> mutations_;
    bool editing_ = false, narrow_ = false;
};
}
