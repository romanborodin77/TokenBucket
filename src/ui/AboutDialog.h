#pragma once
#include <QDialog>
class QLabel;
namespace tb::ui {
// Раздел «О программе» — утверждённый макет «C Акцентный» (2026-10-20):
// тёмная шапка с логотипом «Щит-жетон», версия-бейдж и детальный техстек.
class AboutDialog : public QDialog {
    Q_OBJECT
public:
    explicit AboutDialog(QWidget* parent = nullptr);
private:
    void openLicenses();
    QLabel* versionChip_{};
};
}
