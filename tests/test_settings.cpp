#include <QtTest>
#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QKeySequenceEdit>
#include <QStackedWidget>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/ui/MiniPanel.h"
#include "../src/ui/Theme.h"
#include "../src/ui/EnvExportDialog.h"
#include "../src/platform/EnvOps.h"
using namespace tb;
class TestSettings : public QObject {
    Q_OBJECT
#ifdef Q_OS_WIN
    static QKeySequence freeHotkey(const QKeySequence& excluded={}) {
        for(int key=Qt::Key_F6;key<=Qt::Key_F11;++key) {
            const QKeySequence sequence(Qt::CTRL|Qt::ALT|Qt::SHIFT|Qt::Key(key));
            if(sequence==excluded) continue;
            const auto parsed=paste::parseHotkey(sequence);
            if(parsed && RegisterHotKey(nullptr,0x4f18,parsed->modifiers,parsed->key)) {
                UnregisterHotKey(nullptr,0x4f18); return sequence;
            }
        }
        return {};
    }
#endif
private slots:
#ifdef Q_OS_WIN
    void orderedFailuresAndRetry_data() {
        QTest::addColumn<int>("fault");
        QTest::newRow("invalid-before-any-write")<<0;
        QTest::newRow("hotkey-conflict-before-other-writes")<<1;
        QTest::newRow("run-failure-after-hotkey")<<2;
        QTest::newRow("preference-failure-after-run")<<3;
        QTest::newRow("unchanged-does-not-write")<<4;
    }
    void orderedFailuresAndRetry() {
        QFETCH(int,fault);
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        const auto initial=freeHotkey(); QVERIFY(!initial.isEmpty());
        const auto replacement=freeHotkey(initial); QVERIFY(!replacement.isEmpty());
        const auto prefsParent=dir.filePath("prefs"),runParent=dir.filePath("run");
        QVERIFY(QDir().mkdir(prefsParent)); QVERIFY(QDir().mkdir(runParent));
        QSettings preferences(prefsParent+"/ui.ini",QSettings::IniFormat),run(runParent+"/run.ini",QSettings::IniFormat);
        preferences.setValue("hotkey",initial.toString(QKeySequence::PortableText));
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        run.sync();
        ui::MainWindow window(app,nullptr,&preferences,&run);
        bool blockedHotkey=false;
        const auto release=qScopeGuard([&] {
            UnregisterHotKey(nullptr,0x4f18);
            if(blockedHotkey) UnregisterHotKey(nullptr,0x4f19);
        });
        if(fault==1) {
            const auto key=paste::parseHotkey(replacement); QVERIFY(key);
            blockedHotkey=RegisterHotKey(nullptr,0x4f19,key->modifiers,key->key); QVERIFY(blockedHotkey);
        }
        const auto block=[&](QSettings& settings,const QString& parent) {
            if(QFile::exists(settings.fileName()) && !QFile::remove(settings.fileName())) return false;
            if(!QDir().rmdir(parent)) return false;
            QFile file(parent); return file.open(QIODevice::WriteOnly);
        };
        if(fault==2 || fault==4) QVERIFY(block(run,runParent));
        if(fault==3 || fault==4) QVERIFY(block(preferences,prefsParent));
        bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            if(fault!=4) {
                dialog->findChild<QKeySequenceEdit*>("hotkey")->setKeySequence(fault==0?QKeySequence("F12"):fault==3?initial:replacement);
                dialog->findChild<QCheckBox*>("autostart")->setChecked(true);
                dialog->findChild<QCheckBox*>("trayNotifications")->setChecked(false);
                dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                QVERIFY(dialog->isVisible());
                QVERIFY(!dialog->findChild<QLabel*>("settingsError")->text().isEmpty());
                QCOMPARE(window.findChild<QTimer*>("secretHideTimer")->interval(),15000);
                QCOMPARE(dialog->draft().revealSeconds,5);
                if(fault<=1) {
                    QCOMPARE(preferences.value("hotkey").toString(),initial.toString(QKeySequence::PortableText));
                    QVERIFY(!run.contains("TokenBucket")); QVERIFY(!preferences.contains("trayNotifications"));
                } else {
                    const auto keyName=fault==2?QStringLiteral("Сочетание клавиш"):QStringLiteral("Запускать вместе с Windows");
                    QVERIFY(dialog->findChild<QLabel*>("settingsError")->text().contains(keyName));
                    if(fault==2) {
                        QCOMPARE(preferences.value("hotkey").toString(),replacement.toString(QKeySequence::PortableText));
                        QVERIFY(QFile::remove(runParent)); QVERIFY(QDir().mkdir(runParent));
                    } else {
                        QCOMPARE(run.value("TokenBucket").toString(),platform::autostart::commandForTest());
                        QVERIFY(QFile::remove(prefsParent)); QVERIFY(QDir().mkdir(prefsParent));
                    }
                    dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
                    QVERIFY(!dialog->isVisible());
                    QCOMPARE(preferences.value("secretRevealSeconds").toInt(),5);
                    QCOMPARE(run.value("TokenBucket").toString(),platform::autostart::commandForTest());
                }
            } else {
                QVERIFY(!dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
                QVERIFY(QMetaObject::invokeMethod(dialog,"saveRequested")); QVERIFY(!dialog->isVisible());
                QVERIFY(!QFile::exists(preferences.fileName())); QVERIFY(!QFile::exists(run.fileName()));
            }
            visited=true; if(dialog->isVisible()) dialog->reject();
        });
        QTimer::singleShot(3000,&window,[] {if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        window.showSettings(); QVERIFY(visited);
        const auto registered=paste::parseHotkey(fault==2?replacement:initial); QVERIFY(registered);
        QVERIFY(!RegisterHotKey(nullptr,0x4f18,registered->modifiers,registered->key));
        if(fault==2) {
            const auto old=paste::parseHotkey(initial); QVERIFY(old);
            QVERIFY(RegisterHotKey(nullptr,0x4f18,old->modifiers,old->key)); UnregisterHotKey(nullptr,0x4f18);
        }
    }
    void environmentOptionUsesInjectedCheckedPreferences() {
        QTemporaryDir dir; const auto parent=dir.filePath("blocked");
        QFile file(parent); QVERIFY(file.open(QIODevice::WriteOnly)); file.close();
        QSettings preferences(parent+"/env.ini",QSettings::IniFormat);
        ui::EnvExportDialog dialog(nullptr,&preferences);
        auto* clear=dialog.findChild<QCheckBox*>("clearOnExit"); QVERIFY(clear);
        clear->setChecked(true); QVERIFY(!clear->isChecked());
        QVERIFY(!dialog.findChild<QLabel*>("validationError")->text().isEmpty());
        QVERIFY(QFile::remove(parent)); QVERIFY(QDir().mkdir(parent));
        clear->setChecked(true); QVERIFY(clear->isChecked());
        QSettings reader(preferences.fileName(),preferences.format());
        QVERIFY(platform::envops::clearOnExitEnabled(&reader));
    }
#endif
    void saveAppliesAndReopens() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run);
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            window.popupQuickPanel();
            for(auto* widget:QApplication::topLevelWidgets())
                if(auto* panel=qobject_cast<ui::MiniPanel*>(widget)) QVERIFY(!panel->isVisible());
            dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
            dialog->findChild<QCheckBox*>("trayNotifications")->setChecked(false);
            dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            if(dialog->isVisible()) dialog->reject();
        });
        window.showSettings();
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),5);
        QCOMPARE(preferences.value("trayNotifications").toBool(),false);
        QCOMPARE(window.findChild<QTimer*>("secretHideTimer")->interval(),5000);
#ifdef Q_OS_WIN
        window.popupQuickPanel();
        bool popupVisible=false;
        for(auto* widget:QApplication::topLevelWidgets())
            if(auto* panel=qobject_cast<ui::MiniPanel*>(widget)) popupVisible|=panel->isVisible();
        QVERIFY(popupVisible);
#endif
        bool reopened=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            QCOMPARE(dialog->draft().revealSeconds,5); QCOMPARE(dialog->draft().notifications,false);
            reopened=true; dialog->reject();
        });
        window.showSettings(); QVERIFY(reopened);
    }
    void draftNavigationAndCancel_data() {
        QTest::addColumn<int>("dismiss");
        QTest::newRow("cancel")<<0; QTest::newRow("escape")<<1; QTest::newRow("cross")<<2;
    }
    void draftNavigationAndCancel() {
        QFETCH(int,dismiss);
        prefs::Values initial; initial.autostart=std::nullopt;
        ui::SettingsDialog dialog(initial,{},QStringLiteral("Ctrl+Alt+F8"));
        dialog.setUnlockMode(Application::Mode::Password,true);
        auto* autostart=dialog.findChild<QCheckBox*>("autostart");
        QVERIFY(!autostart->isEnabled()); QVERIFY(!dialog.draft().autostart);
        const auto originalPalette=qApp->palette();
        auto* theme=dialog.findChild<QComboBox*>("theme"); theme->setCurrentIndex(theme->findData("dark"));
        auto* reveal=dialog.findChild<QComboBox*>("secretRevealSeconds"); reveal->setCurrentIndex(0);
        auto* sections=dialog.findChild<QListWidget*>("settingsSections");
        sections->setCurrentRow(4); sections->setCurrentRow(1);
        QCOMPARE(dialog.draft().revealSeconds,5); QCOMPARE(dialog.draft().theme,QStringLiteral("dark"));
        QCOMPARE(qApp->palette(),originalPalette);
        QVERIFY(!dialog.findChild<QPushButton*>("changePassword")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton*>("changeUnlockMode")->isEnabled());
        QVERIFY(!dialog.findChild<QPushButton*>("clearEnvironment")->isEnabled());
        for(const auto* label:dialog.findChildren<QLabel*>()) QCOMPARE(label->textFormat(),Qt::PlainText);
        for(const char* name:{"theme","autostart","hotkey","secretRevealSeconds","clipboardClearSeconds"})
            QVERIFY(!dialog.findChild<QWidget*>(name)->accessibleName().isEmpty());
        QSignalSpy saved(&dialog,&ui::SettingsDialog::saveRequested);
        dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        if(dismiss==0) dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel)->click();
        else if(dismiss==1) QTest::keyClick(&dialog,Qt::Key_Escape);
        else dialog.close();
        QCOMPARE(saved.size(),0); QCOMPARE(dialog.result(),int(QDialog::Rejected)); QVERIFY(!dialog.isVisible());
    }
    void partialApplyKeepsRemainingDraft() {
        prefs::Values initial; initial.autostart=false;
        ui::SettingsDialog dialog(initial,{},QStringLiteral("Ctrl+Alt+F8"));
        auto* theme=dialog.findChild<QComboBox*>("theme"); theme->setCurrentIndex(theme->findData("dark"));
        dialog.findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0);
        auto current=initial; current.theme="dark";
        dialog.setApplyResult({{"theme"},"secretRevealSeconds","<b>Ошибка записи</b>"},current);
        QCOMPARE(dialog.draft().theme,QStringLiteral("dark")); QCOMPARE(dialog.draft().revealSeconds,5);
        auto* error=dialog.findChild<QLabel*>("settingsError");
        QVERIFY(error->text().contains(QStringLiteral("Тема"))); QCOMPARE(error->textFormat(),Qt::PlainText);
        QVERIFY(error->text().contains("<b>"));
        QVERIFY(dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->isEnabled());
        QSignalSpy requested(&dialog,&ui::SettingsDialog::saveRequested);
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(requested.size(),1); QCOMPARE(dialog.result(),int(QDialog::Rejected));
        dialog.setApplyResult({},dialog.draft()); QCOMPARE(dialog.result(),int(QDialog::Accepted));
    }
    void navigationAndFooterFit_data() {
        QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        QTest::newRow("wide-light")<<900<<false; QTest::newRow("wide-dark")<<900<<true;
        QTest::newRow("narrow-light")<<420<<false; QTest::newRow("narrow-dark")<<420<<true;
    }
    void navigationAndFooterFit() {
        QFETCH(int,width); QFETCH(bool,dark);
        ui::theme::apply(dark?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        ui::SettingsDialog dialog({}, {},QStringLiteral("Ctrl+Alt+F8"));
        dialog.resize(width,480); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.width(),width);
        auto* sections=dialog.findChild<QListWidget*>("settingsSections");
        auto* combo=dialog.findChild<QComboBox*>("settingsSectionCombo");
        QCOMPARE(sections->isVisible(),width>=720); QCOMPARE(combo->isVisible(),width<720);
        combo->setCurrentIndex(2); QCOMPARE(sections->currentRow(),2);
        QCOMPARE(dialog.findChild<QStackedWidget*>()->currentIndex(),2);
        auto* cancel=dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel);
        QVERIFY(dialog.rect().contains(QRect(cancel->mapTo(&dialog,QPoint()),cancel->size())));
        const auto shots=qEnvironmentVariable("TOKENBUCKET_SETTINGS_SCREENSHOTS");
        if(!shots.isEmpty()) {
            QVERIFY(QDir().mkpath(shots));
            QVERIFY(dialog.grab().save(shots+QStringLiteral("/settings-%1-%2.png").arg(width).arg(dark?"dark":"light")));
        }
    }
    void backupMetadataHasNoAuthenticationSideEffects() {
        QTemporaryDir dir; Application app(dir.path());
        const auto generation=app.generation(); const auto gate=app.gate();
        const auto missing=app.backupInfo(); QCOMPARE(missing.state,Application::BackupInfo::State::Missing);
        QFile file(missing.backupPath); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("invalid container"),qint64(17)); file.close();
        const auto available=app.backupInfo(); QCOMPARE(available.state,Application::BackupInfo::State::Available);
        QCOMPARE(available.size,qint64(17)); QVERIFY(available.modified.isValid());
        QVERIFY(file.remove()); QVERIFY(QDir().mkdir(missing.backupPath));
        QCOMPARE(app.backupInfo().state,Application::BackupInfo::State::Unavailable);
        QCOMPARE(app.generation(),generation); QCOMPARE(app.gate(),gate); QVERIFY(!app.busy());
    }
    void lockClosesWithoutApplyingDraft() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run); bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<ui::SettingsDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            dialog->findChild<QComboBox*>("secretRevealSeconds")->setCurrentIndex(0); visited=true;
            app.lock();
        });
        window.showSettings(); QVERIFY(visited);
        QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),15);
    }
    void menuCancelKeepsPreferences() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123",&error));
        QSettings preferences(dir.filePath("ui.ini"),QSettings::IniFormat);
        preferences.setValue("secretRevealSeconds",15); preferences.sync();
        QSettings run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&preferences,&run);
        auto* action=window.findChild<QAction*>("openSettings"); QVERIFY(action);
        QFile file(preferences.fileName()); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto before=file.readAll(); file.close();
        bool visited=false;
        QTimer::singleShot(0,&window,[&] {
            auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog);
            auto* reveal=dialog->findChild<QComboBox*>("secretRevealSeconds"); QVERIFY(reveal);
            reveal->setCurrentIndex(0); visited=true; dialog->reject();
        });
        action->trigger(); QVERIFY(visited);
        preferences.sync(); QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(),before);
        QCOMPARE(preferences.value("secretRevealSeconds").toInt(),15);
    }
};
int main(int argc,char** argv) {
    QApplication qt(argc,argv);
    if(qt.arguments().contains("--settings-demo")) {
        QTemporaryDir dir; Application vault(dir.path()); QString error;
        if(!vault.createPasswordVault("test-password-123",&error)) return 2;
        QSettings preferences(dir.filePath("demo.ini"),QSettings::IniFormat);
        QSettings run(dir.filePath("demo-run.ini"),QSettings::IniFormat);
        ui::theme::apply(qt.arguments().contains("--dark")?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        ui::MainWindow window(vault,nullptr,&preferences,&run);
        QTimer::singleShot(0,&window,[&] {
            if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                dialog->setWindowTitle(QStringLiteral("Настройки — вымышленные данные"));
                if(qt.arguments().contains("--compact")) dialog->resize(420,480);
            }
        });
        window.showSettings(); return 0;
    }
    TestSettings test; return QTest::qExec(&test,argc,argv);
}
#include "test_settings.moc"
