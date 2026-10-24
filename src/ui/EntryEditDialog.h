#pragma once
#include <QDialog>
#include <optional>
#include "../model/VaultModel.h"
class QComboBox; class QLineEdit; class QPlainTextEdit; class QCheckBox; class QLabel; class QDateEdit;
class QDialogButtonBox; class QPushButton;
namespace tb::ui { void populateGroups(QComboBox& combo, const model::VaultModel& model); }
class EntryEditDialog : public QDialog {
    Q_OBJECT
public:
    EntryEditDialog(const tb::model::VaultModel& model, std::optional<tb::model::Entry> existing, QWidget* parent = nullptr);
    ~EntryEditDialog() override;
    std::optional<tb::model::Entry> entry() const { return result_; }
    void setGroup(tb::model::Id id);
    void clearSecrets();
public slots:
    void reject() override;
    // accept публичен, как и слот QDialog::accept, который он переопределяет:
    // закрытие по сохранению можно вызывать напрямую (в том числе из тестов).
    void accept() override;
protected:
    bool eventFilter(QObject*,QEvent*) override;
private:
    void fitToContents();
    void showError(const QString& message);
    const tb::model::VaultModel& model_;
    tb::model::Entry original_;
    std::optional<tb::model::Entry> result_;
    QComboBox *type_{}, *group_{};
    QLineEdit *title_{}, *url_{}, *login_{}, *password_{}, *token_{}, *sshPass_{}, *sshPath_{}, *sshComment_{}, *tags_{};
    QPlainTextEdit *note_{}, *sshPublic_{};
    QLineEdit *parameter_{},*plainValue_{},*fileName_{};
    QComboBox* valueFormat_{};
    QPlainTextEdit* fileBody_{};
    QPushButton* editBody_{};
    QString bodyBaseline_;
    tb::model::EntryType shownType_=tb::model::EntryType::ApiToken;
    bool bodyLoaded_=false,editingCopy_=false;
    QCheckBox *favorite_{}, *reverify_{}, *expiry_{};
    QDateEdit* expiryDate_{};
    QLabel* error_{};
    QWidget* fields_{};
    QDialogButtonBox* buttons_{};
    bool canceled_ = false, reading_ = false;
};
