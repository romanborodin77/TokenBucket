#pragma once
#include <QMap>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <functional>
#include <optional>
#include "../model/Types.h"
#include "../vault/SecureBytes.h"

class QComboBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QTreeWidget;
class QPushButton;
class QTimer;
namespace tb { class Application; class ClipboardGuard; }

namespace tb::ui {
class CollectionPane : public QWidget {
    Q_OBJECT
public:
    CollectionPane(Application& app, std::function<bool(QWidget*)> approveUpgrade,
                   int revealSeconds, ClipboardGuard& clipboard, QWidget* parent);
    QWidget* listWidget() const { return listPanel_; }
    QWidget* detailWidget() { return this; }
    QStringList projects() const { return projects_.keys(); }
    void setContext(std::optional<QString> project, model::Id destination, QString filter, bool showEntries);
    void authorize(quint64 generation);
    void clearSession();
    void refresh();
    void setRevealSeconds(int seconds);
    void focusList();
    void deactivate();
    void confirmNavigation(std::function<void()> action);

signals:
    void accessRequested();
    void selectionActivated();
    void projectsChanged();
    void vaultChanged();

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    struct Item {
        QString project, name;
        bool operator==(const Item&) const = default;
    };
    using Action = std::function<void(const Item&, const SecureBytes&)>;
    bool currentSession() const;
    bool available();
    bool dirty() const;
    bool sourceStill(const Item& item, const SecureBytes& value) const;
    void beginAction(const QString& message, const QString& button, Action action, bool transfer = false);
    void discardThen(std::function<void()> action);
    void cancelAction();
    void confirmAction();
    void refreshAction();
    void refreshButtons();
    void showStatus(const QString& message);
    void hideValue();
    void invalidateCopy();
    void toggleReveal();
    void copyCurrentValue();
    void replaceProjects(QMap<QString,QStringList> projects);
    void refillList();
    bool matches(const Item& item) const;
    void selectInTree(const std::optional<Item>& item);
    void editItem(const Item& item, SecureBytes value, bool enabled = true);
    void selectItem(const Item& item);
    void reload();
    void save();
    void remove();
    void forgetSavedItem();
    void transfer(bool move);
    void changeFormat(bool base64);
    bool readDraft(SecureBytes& value);

    Application& app_;
    std::optional<quint64> generation_;
    std::optional<QString> scope_;
    QString filter_;
    model::Id vaultDestination_;
    const std::function<bool(QWidget*)> approveUpgrade_;
    QPointer<ClipboardGuard> clipboard_;
    quint64 contentVersion_ = 0;
    QWidget* listPanel_{};
    QTreeWidget* items_{};
    QWidget* editor_{};
    QLabel *heading_{}, *listStatus_{};
    QLineEdit *name_{}, *value_{};
    QComboBox *project_{}, *format_{};
    QLabel* status_{};
    QPlainTextEdit* revealedValue_{};
    QTimer* revealTimer_{};
    QWidget *actionPanel_{}, *transferFields_{};
    QFormLayout* transferForm_{};
    QComboBox* destination_{};
    QLineEdit* newProject_{};
    QLabel* actionMessage_{};
    QPushButton *confirm_{}, *cancel_{};
    QPushButton *access_{}, *add_{}, *reload_{}, *save_{}, *remove_{}, *copyToVault_{}, *moveToVault_{};
    QPushButton *reveal_{}, *copy_{};
    QMap<QString,QStringList> projects_;
    std::optional<Item> selected_;
    Item saved_;
    SecureBytes savedValue_;
    Item pendingSource_;
    SecureBytes pendingValue_;
    Action pending_;
    bool editing_ = false;
    bool valueEdited_ = false;
    bool base64_ = false;
    bool contextAvailable_ = true;
};
}
