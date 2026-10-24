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
#include <QTemporaryDir>
#include <QTimer>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/SettingsDialog.h"
#include "../src/ui/Theme.h"
using namespace tb;
class TestSettings : public QObject {
    Q_OBJECT
private slots:
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
        ui::MainWindow window(app,nullptr,&preferences); bool visited=false;
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
        ui::MainWindow window(app,nullptr,&preferences);
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
        ui::theme::apply(qt.arguments().contains("--dark")?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        ui::MainWindow window(vault,nullptr,&preferences);
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
