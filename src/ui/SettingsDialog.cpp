#include "SettingsDialog.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QSpinBox>
#include <QStackedWidget>
#include <QVBoxLayout>
namespace tb::ui {
namespace {
QLabel* text(const QString& value,QWidget* parent=nullptr) {
    auto* label=new QLabel(value,parent); label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label;
}
}
SettingsDialog::SettingsDialog(const prefs::Values& initial,const Application::BackupInfo& backup,
                             const QString& registeredHotkey,QWidget* parent)
    : QDialog(parent),baseline_(initial) {
    setWindowTitle(QStringLiteral("Настройки")); setMinimumSize(320,280);
    auto* root=new QVBoxLayout(this);
    sectionCombo_=new QComboBox(this); sectionCombo_->setObjectName("settingsSectionCombo");
    sectionCombo_->setAccessibleName(QStringLiteral("Раздел настроек")); root->addWidget(sectionCombo_);
    auto* body=new QHBoxLayout; root->addLayout(body,1);
    sections_=new QListWidget(this); sections_->setObjectName("settingsSections");
    sections_->setAccessibleName(QStringLiteral("Разделы настроек")); sections_->setFixedWidth(176);
    body->addWidget(sections_); pages_=new QStackedWidget(this); body->addWidget(pages_,1);
    const auto page=[&](const QString& name) {
        sections_->addItem(name); sectionCombo_->addItem(name);
        auto* scroll=new QScrollArea(pages_); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame);
        auto* content=new QWidget(scroll); auto* form=new QFormLayout(content);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows); form->setVerticalSpacing(14);
        scroll->setWidget(content); pages_->addWidget(scroll); return form;
    };
    const auto choice=[&](QFormLayout* form,const QString& label,const char* name,
                         const QList<QPair<QString,QVariant>>& items,const QVariant& value) {
        auto* box=new QComboBox(this); box->setObjectName(QString::fromLatin1(name)); box->setAccessibleName(label);
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon); box->setMinimumContentsLength(10);
        for(const auto& item:items) box->addItem(item.first,item.second);
        box->setCurrentIndex(box->findData(value)); form->addRow(label,box); return box;
    };
    const auto check=[&](QFormLayout* form,const QString& label,const char* name,bool value) {
        auto* box=new QCheckBox(label,this); box->setObjectName(QString::fromLatin1(name));
        box->setAccessibleName(label); box->setChecked(value); form->addRow(box); return box;
    };
    auto* general=page(QStringLiteral("Общие"));
    choice(general,QStringLiteral("Тема"),"theme",{{"Как в системе","system"},{"Светлая","light"},{"Тёмная","dark"}},initial.theme);
    auto* autostart=check(general,QStringLiteral("Запускать вместе с Windows"),"autostart",initial.autostart.value_or(false));
    autostart->setEnabled(initial.autostart.has_value());
    if(!initial.autostart) {
        autostart->setToolTip(QStringLiteral("Состояние автозапуска недоступно. Оно не будет перезаписано."));
        general->addRow(text(autostart->toolTip()));
    }
    check(general,QStringLiteral("Уведомления в области уведомлений"),"trayNotifications",initial.notifications);
    choice(general,QStringLiteral("При закрытии главного окна"),"closeBehavior",
           {{"Свернуть в трей",int(prefs::CloseBehavior::Hide)},
            {"Заблокировать и свернуть",int(prefs::CloseBehavior::LockHide)},
            {"Выйти",int(prefs::CloseBehavior::Exit)}},int(initial.closeBehavior));
    auto* security=page(QStringLiteral("Безопасность"));
    auto* idle=new QSpinBox(this); idle->setObjectName("idleTimeoutMinutes"); idle->setRange(1,60);
    idle->setValue(initial.idleMinutes); idle->setSuffix(QStringLiteral(" мин"));
    idle->setAccessibleName(QStringLiteral("Автоблокировка")); security->addRow(QStringLiteral("Автоблокировка"),idle);
    choice(security,QStringLiteral("Показ секрета"),"secretRevealSeconds",
           {{"5 секунд",5},{"10 секунд",10},{"15 секунд",15},{"30 секунд",30}},initial.revealSeconds);
    choice(security,QStringLiteral("Очистка буфера"),"clipboardClearSeconds",
           {{"10 секунд",10},{"20 секунд",20},{"30 секунд",30},{"60 секунд",60}},initial.clipboardSeconds);
    security->addRow(text(QStringLiteral("Смена срока очищает только секрет, скопированный TokenBucket. Чужой буфер сохраняется.")));
    auto* mode=text({}); mode->setObjectName("unlockMode"); security->addRow(QStringLiteral("Способ разблокировки"),mode);
    security->addRow(text(QStringLiteral("Windows Hello подтверждает владельца на этом компьютере. DPAPI привязывает ключ к учётной записи Windows; для переноса нужен защищённый экспорт с паролем.")));
    password_=new QPushButton(QStringLiteral("Сменить мастер-пароль…"),this); password_->setObjectName("changePassword"); security->addRow(password_);
    unlock_=new QPushButton(QStringLiteral("Изменить способ разблокировки…"),this); unlock_->setObjectName("changeUnlockMode"); security->addRow(unlock_);
    auto* quick=page(QStringLiteral("Быстрая вставка"));
    auto* hotkey=new QKeySequenceEdit(initial.hotkey,this); hotkey->setObjectName("hotkey");
    hotkey->setAccessibleName(QStringLiteral("Сочетание клавиш")); quick->addRow(QStringLiteral("Сочетание клавиш"),hotkey);
    registered_=text(registeredHotkey.isEmpty()?QStringLiteral("Не зарегистрировано"):registeredHotkey);
    registered_->setObjectName("registeredHotkey"); quick->addRow(QStringLiteral("Действующее сочетание"),registered_);
    quick->addRow(text(QStringLiteral("Один аккорд: Ctrl/Alt/Win и буква, цифра, Space или F1–F11. Проверки окна-получателя всегда включены.")));
    choice(quick,QStringLiteral("Поле по умолчанию"),"quickPasteDefaultField",
           {{"Секрет",int(paste::Field::Secret)},{"Логин",int(paste::Field::Login)}},int(initial.defaultField));
    auto* delivery=page(QStringLiteral("Выдача секретов"));
    delivery->addRow(text(QStringLiteral("Запустить программу с переменной её окружения или записать пользовательскую переменную Windows можно из выбранной записи. Переменная Windows хранится открыто; предупреждение и проверка владельца обязательны.")));
    check(delivery,QStringLiteral("Удалять переменные TokenBucket при выходе"),"env-clear-on-exit",initial.clearEnvironmentOnExit);
    clearEnvironment_=new QPushButton(QStringLiteral("Удалить переменные TokenBucket…"),this);
    clearEnvironment_->setObjectName("clearEnvironment"); delivery->addRow(clearEnvironment_);
    auto* copies=page(QStringLiteral("Резервные копии"));
    const auto state=backup.state==Application::BackupInfo::State::Missing?QStringLiteral("Нет копии")
        :backup.state==Application::BackupInfo::State::Available?QStringLiteral("Доступна, содержимое ещё не проверено")
        :QStringLiteral("Недоступна");
    auto* status=text(state); status->setObjectName("backupStatus"); copies->addRow(QStringLiteral("Состояние"),status);
    copies->addRow(QStringLiteral("Хранилище"),text(backup.mainPath)); copies->addRow(QStringLiteral("Копия"),text(backup.backupPath));
    if(backup.state==Application::BackupInfo::State::Available) {
        copies->addRow(QStringLiteral("Размер"),text(QStringLiteral("%1 байт").arg(backup.size)));
        copies->addRow(QStringLiteral("Изменена"),text(backup.modified.toLocalTime().toString(Qt::ISODate)));
    }
    backupAvailable_=backup.state==Application::BackupInfo::State::Available;
    restore_=new QPushButton(QStringLiteral("Восстановить резервную копию…"),this);
    restore_->setObjectName("restoreBackup"); restore_->setAccessibleName(QStringLiteral("Восстановить резервную копию")); copies->addRow(restore_);
    dirtyHint_=text(QStringLiteral("Чтобы выполнить отдельное действие, сохраните или отмените изменения.")); root->addWidget(dirtyHint_);
    error_=text({}); error_->setObjectName("settingsError"); error_->setAccessibleName(QStringLiteral("Результат операции")); root->addWidget(error_);
    buttons_=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,this); root->addWidget(buttons_);
    buttons_->button(QDialogButtonBox::Save)->setText(QStringLiteral("Сохранить")); buttons_->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("Отмена"));
    connect(buttons_,&QDialogButtonBox::accepted,this,&SettingsDialog::saveRequested);
    connect(buttons_,&QDialogButtonBox::rejected,this,&QDialog::reject);
    connect(password_,&QPushButton::clicked,this,&SettingsDialog::changePasswordRequested);
    connect(unlock_,&QPushButton::clicked,this,&SettingsDialog::changeUnlockModeRequested);
    connect(clearEnvironment_,&QPushButton::clicked,this,&SettingsDialog::clearEnvironmentRequested);
    connect(restore_,&QPushButton::clicked,this,&SettingsDialog::restoreBackupRequested);
    connect(sections_,&QListWidget::currentRowChanged,sectionCombo_,&QComboBox::setCurrentIndex);
    connect(sectionCombo_,&QComboBox::currentIndexChanged,sections_,qOverload<int>(&QListWidget::setCurrentRow));
    connect(sections_,&QListWidget::currentRowChanged,pages_,&QStackedWidget::setCurrentIndex);
    sections_->setCurrentRow(0);
    for(auto* box:findChildren<QComboBox*>()) if(box!=sectionCombo_) connect(box,&QComboBox::currentIndexChanged,this,&SettingsDialog::updateDraft);
    for(auto* box:findChildren<QCheckBox*>()) connect(box,&QCheckBox::toggled,this,&SettingsDialog::updateDraft);
    connect(idle,&QSpinBox::valueChanged,this,&SettingsDialog::updateDraft);
    connect(hotkey,&QKeySequenceEdit::keySequenceChanged,this,&SettingsDialog::updateDraft);
#ifndef Q_OS_WIN
    for(QWidget* widget:QList<QWidget*>{autostart,hotkey,clearEnvironment_,
        findChild<QComboBox*>("quickPasteDefaultField"),findChild<QCheckBox*>("env-clear-on-exit")}) {
        widget->setEnabled(false); widget->setToolTip(QStringLiteral("Доступно только в Windows."));
    }
#endif
    for(auto* label:findChildren<QLabel*>()) label->setTextFormat(Qt::PlainText);
    updateDraft();
    const auto available=screen()->availableGeometry().size(); resize(qMin(900,available.width()-40),qMin(660,available.height()-80));
}
prefs::Values SettingsDialog::draft() const {
    auto v=baseline_;
    const auto value=[&](const char* name){return findChild<QComboBox*>(QString::fromLatin1(name))->currentData();};
    v.theme=value("theme").toString(); v.closeBehavior=prefs::CloseBehavior(value("closeBehavior").toInt());
    v.notifications=findChild<QCheckBox*>("trayNotifications")->isChecked();
    v.idleMinutes=findChild<QSpinBox*>("idleTimeoutMinutes")->value();
    v.revealSeconds=value("secretRevealSeconds").toInt(); v.clipboardSeconds=value("clipboardClearSeconds").toInt();
#ifdef Q_OS_WIN
    if(v.autostart) v.autostart=findChild<QCheckBox*>("autostart")->isChecked();
    v.hotkey=findChild<QKeySequenceEdit*>("hotkey")->keySequence();
    v.defaultField=paste::Field(value("quickPasteDefaultField").toInt());
    v.clearEnvironmentOnExit=findChild<QCheckBox*>("env-clear-on-exit")->isChecked();
#endif
    return v;
}
void SettingsDialog::updateDraft() {
    const bool dirty=draft()!=baseline_;
    dirtyHint_->setVisible(dirty); buttons_->button(QDialogButtonBox::Save)->setEnabled(dirty);
    password_->setEnabled(!dirty && passwordAllowed_); unlock_->setEnabled(!dirty && unlockAllowed_);
    restore_->setEnabled(!dirty && backupAvailable_);
    restore_->setToolTip(dirty?dirtyHint_->text():!backupAvailable_?QStringLiteral("Доступной резервной копии нет."):QString());
#ifdef Q_OS_WIN
    clearEnvironment_->setEnabled(!dirty);
#endif
}
void SettingsDialog::setUnlockMode(Application::Mode mode,bool helloAvailable) {
    findChild<QLabel*>("unlockMode")->setText(mode==Application::Mode::Hello?QStringLiteral("Windows Hello / DPAPI"):QStringLiteral("Мастер-пароль"));
    passwordAllowed_=mode==Application::Mode::Password;
#ifdef Q_OS_WIN
    unlockAllowed_=mode==Application::Mode::Hello || helloAvailable;
#else
    Q_UNUSED(helloAvailable);
#endif
    updateDraft();
}
void SettingsDialog::setOperationError(const QString& error) {error_->setText(error);}
void SettingsDialog::setApplyResult(const prefs::ApplyResult& result,const prefs::Values& current) {
    baseline_=current;
    if(result.error.isEmpty() && result.failedKey.isEmpty()) {accept(); return;}
    const auto label=[&](const QString& key) {
        auto* widget=findChild<QWidget*>(key); return widget && !widget->accessibleName().isEmpty()?widget->accessibleName():key;
    };
    QStringList applied; for(const auto& key:result.appliedKeys) applied<<label(key);
    error_->setText((applied.isEmpty()?QString():QStringLiteral("Применено: %1. ").arg(applied.join(", ")))
        +QStringLiteral("Не применено: %1. %2").arg(label(result.failedKey),result.error));
    if(result.appliedKeys.contains("hotkey")) registered_->setText(current.hotkey.toString(QKeySequence::NativeText));
    updateDraft();
}
void SettingsDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event); sections_->setVisible(width()>=720); sectionCombo_->setVisible(width()<720);
}
}
