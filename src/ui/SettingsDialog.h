#pragma once
#include <QDialog>
#include "../app/Preferences.h"
#include "../app/Application.h"
class QListWidget; class QComboBox; class QStackedWidget; class QLabel; class QDialogButtonBox;
class QPushButton;
namespace tb::ui {
class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    SettingsDialog(const prefs::Values& initial,const Application::BackupInfo& backup,
                   const QString& registeredHotkey,QWidget* parent=nullptr);
    prefs::Values draft() const;
    void setApplyResult(const prefs::ApplyResult&,const prefs::Values& current);
    void setUnlockMode(Application::Mode mode,bool helloAvailable);
signals:
    void saveRequested();
    void changePasswordRequested();
    void changeUnlockModeRequested();
    void clearEnvironmentRequested();
    void restoreBackupRequested();
protected:
    void resizeEvent(QResizeEvent*) override;
private:
    void updateDraft();
    prefs::Values baseline_;
    QListWidget* sections_{};
    QComboBox* sectionCombo_{};
    QStackedWidget* pages_{};
    QLabel *error_{},*dirtyHint_{},*registered_{};
    QDialogButtonBox* buttons_{};
    QPushButton *password_{},*unlock_{},*clearEnvironment_{};
    bool passwordAllowed_=false,unlockAllowed_=false;
};
}
