#include "EnvExportDialog.h"
#include "../platform/EnvOps.h"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace tb::ui {
EnvExportDialog::EnvExportDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(QStringLiteral("TokenBucket — переменная окружения"));
    auto* layout = new QVBoxLayout(this);
    auto* form = new QFormLayout;
    name_ = new QLineEdit(this); name_->setObjectName(QStringLiteral("envVarName"));
    name_->setMaxLength(64); name_->setAccessibleName(QStringLiteral("Имя переменной окружения"));
    name_->setPlaceholderText(QStringLiteral("Например: GITHUB_TOKEN"));
    form->addRow(QStringLiteral("Имя пере&менной:"), name_);
    layout->addLayout(form);
    temporary_ = new QRadioButton(QStringLiteral("Запустить приложение с переменной (временно)"), this);
    temporary_->setObjectName(QStringLiteral("envModeTemporary"));
    permanent_ = new QRadioButton(QStringLiteral("Пользовательская переменная Windows (постоянно)"), this);
    permanent_->setObjectName(QStringLiteral("envModePermanent"));
    temporary_->setChecked(true); // рекомендованный режим: секрет нигде не сохраняется
    layout->addWidget(temporary_);
    layout->addWidget(permanent_);
    auto* pages = new QStackedWidget(this);
    auto* tempPage = new QWidget(pages);
    auto* tempForm = new QFormLayout(tempPage);
    program_ = new QLineEdit(tempPage); program_->setObjectName(QStringLiteral("envProgram"));
    program_->setAccessibleName(QStringLiteral("Программа для запуска"));
    auto* browse = new QPushButton(QStringLiteral("Обзор…"), tempPage);
    auto* programRow = new QWidget(tempPage);
    auto* rowLayout = new QHBoxLayout(programRow); rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->addWidget(program_, 1); rowLayout->addWidget(browse);
    tempForm->addRow(QStringLiteral("&Программа:"), programRow);
    arguments_ = new QLineEdit(tempPage); arguments_->setObjectName(QStringLiteral("envArguments"));
    arguments_->setAccessibleName(QStringLiteral("Аргументы командной строки"));
    arguments_->setPlaceholderText(QStringLiteral("можно оставить пустыми"));
    tempForm->addRow(QStringLiteral("&Аргументы:"), arguments_);
    pages->addWidget(tempPage);
    auto* permPage = new QWidget(pages);
    auto* permLayout = new QVBoxLayout(permPage);
    auto* warning = new QLabel(QStringLiteral("Значение будет храниться в реестре в открытом виде "
        "и будет доступно любому процессу от вашего имени. Новые процессы увидят переменную "
        "после перезапуска оболочки/проводника или выхода из системы."), permPage);
    warning->setWordWrap(true); warning->setTextFormat(Qt::PlainText);
    permLayout->addWidget(warning);
    // Опция synced с QSettings: включённая — переменные TokenBucket стираются из реестра
    // при завершении программы (см. main.cpp, точка выхода).
    auto* clearOnExit = new QCheckBox(QStringLiteral("Стирать переменные TokenBucket при выходе из программы"), permPage);
    clearOnExit->setObjectName(QStringLiteral("clearOnExit"));
    clearOnExit->setChecked(tb::platform::envops::clearOnExitEnabled());
    connect(clearOnExit, &QCheckBox::toggled, permPage, [](bool on) { tb::platform::envops::setClearOnExitEnabled(on); });
    permLayout->addWidget(clearOnExit);
    pages->addWidget(permPage);
    layout->addWidget(pages);
    connect(temporary_, &QRadioButton::toggled, pages, [pages](bool temporary) {
        pages->setCurrentIndex(temporary ? 0 : 1);
    });
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Выбор программы"));
        if (!path.isEmpty()) program_->setText(path);
    });
    error_ = new QLabel(this); error_->setWordWrap(true); error_->setObjectName(QStringLiteral("validationError"));
    layout->addWidget(error_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Продолжить"));
    buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    name_->setFocus();
}
QString EnvExportDialog::name() const { return name_->text().trimmed(); }
EnvExportDialog::Mode EnvExportDialog::mode() const { return temporary_->isChecked() ? Mode::Temporary : Mode::Permanent; }
QString EnvExportDialog::program() const { return program_->text().trimmed(); }
QString EnvExportDialog::arguments() const { return arguments_->text(); }
void EnvExportDialog::accept() {
    if (!platform::envops::validName(name())) {
        error_->setText(QStringLiteral("Имя переменной: латинские буквы, цифры и «_», не начинается с цифры, до 64 символов."));
        name_->setFocus(); return;
    }
    if (mode() == Mode::Temporary && program().isEmpty()) {
        error_->setText(QStringLiteral("Укажите программу для запуска."));
        program_->setFocus(); return;
    }
    QDialog::accept();
}
}
