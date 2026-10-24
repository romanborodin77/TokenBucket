#pragma once
#include <QDialog>
class QLineEdit;
class QRadioButton;
class QLabel;
class QSettings;
namespace tb::ui {
// Диалог выдачи секрета в переменную окружения (спека 2026-10-20-envexport-design):
// Temporary — запустить выбранную программу с переменной в её окружении (секрет
// нигде не сохраняется); Permanent — записать в HKCU\Environment открытым текстом
// (осознанный риск M-2: предупреждение показано, чекбокс «стирать при выходе»
// синхронизирован с QSettings env-clear-on-exit). Валидация имени — envops::validName.
class EnvExportDialog : public QDialog {
    Q_OBJECT
public:
    enum class Mode { Temporary, Permanent };
    explicit EnvExportDialog(QWidget* parent = nullptr,QSettings* preferences=nullptr);
    QString name() const;
    Mode mode() const;
    QString program() const;
    QString arguments() const;
protected:
    void accept() override;
private:
    QLineEdit *name_{}, *program_{}, *arguments_{};
    QRadioButton *temporary_{}, *permanent_{};
    QLabel* error_{};
};
}
