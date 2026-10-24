#include <QtTest>
#include <QComboBox>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTemporaryDir>
#include <QMessageBox>
#include <QInputDialog>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QTextDocument>
#include <QClipboard>
#include <QLabel>
#include <QListWidget>
#include <QSettings>
#include <QScopeGuard>
#include <QAccessible>
#include <QFile>
#include "../src/ui/EntryAccess.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/Theme.h"
#include "FaultFileOps.h"
#include "../src/app/Application.h"
#include "../src/ui/EntryEditDialog.h"
#include "../src/ui/EntryCard.h"
using namespace tb;
class TestStorageUi : public QObject {
    Q_OBJECT
    static void answer(bool yes) {
        QTimer::singleShot(0,[yes] {
            auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(box);
            QCOMPARE(box->textFormat(),Qt::PlainText); QCOMPARE(box->defaultButton(),qobject_cast<QPushButton*>(box->button(QMessageBox::No)));
            box->button(yes?QMessageBox::Yes:QMessageBox::No)->click();
        });
    }
    static model::Entry document(model::Id group,bool snapshot=false) {
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=model::EntryType::TextFile; e.title="Document";
        e.created=QDateTime::currentDateTimeUtc(); e.textFile={"demo.txt",QByteArray::fromHex("efbbbf")+QStringLiteral("public value\r\nsynthetic-password\r\n").toUtf8(),snapshot,{0}};
        return e;
    }
private slots:
    void fiveTypesHaveRealEditors() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        EntryEditDialog dialog(*app.model(),std::nullopt); dialog.setGroup(group);
        auto* types=dialog.findChild<QComboBox*>("entryType"); QVERIFY(types); QCOMPARE(types->count(),5);
        types->setCurrentIndex(types->findData(int(model::EntryType::PlainValue)));
        dialog.findChild<QLineEdit*>("entryTitle")->setText("Public IP");
        auto* name=dialog.findChild<QLineEdit*>("entryParameterName"); auto* value=dialog.findChild<QLineEdit*>("entryPlainValue");
        QVERIFY(name); QVERIFY(value); name->setText("PUBLIC_IP"); value->setText("192.0.2.10");
        dialog.accept(); QVERIFY(dialog.entry()); QCOMPARE(dialog.entry()->plainValue,QString("192.0.2.10"));
        QVERIFY(dialog.entry()->token.isEmpty()); QVERIFY(dialog.entry()->password.isEmpty());
    }
    void plainCardIsVisibleWithoutTimer() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry entry; entry.id=QUuid::createUuid(); entry.groupId=group; entry.type=model::EntryType::PlainValue;
        entry.title="Public IP"; entry.plainValue="192.0.2.10"; QVERIFY(app.model()->setEntry(entry).isEmpty());
        ui::EntryCard card(app); card.showEntry(entry.id); QCOMPARE(card.secretText(),entry.plainValue);
        QCOMPARE(card.findChild<QLabel*>("cardValueLabel")->text(),QStringLiteral("ЗНАЧЕНИЕ"));
        QVERIFY(!card.findChild<QTimer*>("secretHideTimer")->isActive()); card.hideNowForTest(); QCOMPARE(card.secretText(),entry.plainValue);
        app.lock(); QVERIFY(!card.secretText().contains(entry.plainValue));
    }
    void newTextFileHasASeparateBody() {
        model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        EntryEditDialog dialog(model,std::nullopt); dialog.setGroup(group);
        dialog.findChild<QComboBox*>("entryType")->setCurrentIndex(4); dialog.findChild<QLineEdit*>("entryTitle")->setText("Document");
        auto* body=dialog.findChild<QPlainTextEdit*>("entryTextFileBody"); QVERIFY(body); body->setPlainText("one\ntwo\n");
        dialog.findChild<QLineEdit*>("entryTextFileName")->setText("demo.txt"); dialog.accept(); QVERIFY(dialog.entry());
        QCOMPARE(dialog.entry()->textFile.bytes,QByteArray("one\ntwo\n")); QVERIFY(dialog.entry()->token.isEmpty());
        QVERIFY(dialog.entry()->textFile.visibleLines.isEmpty());
    }
    void secretToPlainIsExplicit() {
        model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.title="IP"; e.token="192.0.2.10"; QVERIFY(model.setEntry(e).isEmpty());
        EntryEditDialog dialog(model,e); auto* type=dialog.findChild<QComboBox*>("entryType");
        answer(false); type->setCurrentIndex(3); QCOMPARE(type->currentData().toInt(),0); QVERIFY(!dialog.entry());
        answer(true); type->setCurrentIndex(3); QCOMPARE(dialog.findChild<QLineEdit*>("entryPlainValue")->text(),e.token);
        dialog.accept(); QVERIFY(dialog.entry()); QCOMPARE(dialog.entry()->plainValue,e.token); QVERIFY(dialog.entry()->token.isEmpty());
        QCOMPARE(model.entry(e.id)->type,model::EntryType::ApiToken);
    }
    void metadataPreservesLegacyInactiveFields() {
        model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.title="Legacy"; e.token="synthetic-token"; e.password="unused-password";
        e.sshPrivate="unused-key\r\n"; QVERIFY(model.setEntry(e).isEmpty()); EntryEditDialog dialog(model,e);
        auto* types=dialog.findChild<QComboBox*>("entryType"); types->setCurrentIndex(3); QCOMPARE(types->currentData().toInt(),0);
        dialog.accept(); QVERIFY(dialog.entry()); QCOMPARE(dialog.entry()->password,e.password); QCOMPARE(dialog.entry()->sshPrivate,e.sshPrivate);
    }
    void unchangedDocumentKeepsExactBytes_data() {
        QTest::addColumn<bool>("snapshot"); QTest::newRow("editable")<<false; QTest::newRow("snapshot")<<true;
    }
    void unchangedDocumentKeepsExactBytes() {
        QFETCH(bool,snapshot); model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        const auto e=document(group,snapshot); QVERIFY(model.setEntry(e).isEmpty()); EntryEditDialog dialog(model,e);
        QVERIFY(dialog.findChild<QPlainTextEdit*>("entryTextFileBody")->toPlainText().isEmpty());
        dialog.findChild<QLineEdit*>("entryTitle")->setText("Metadata only"); dialog.accept(); QVERIFY(dialog.entry());
        QCOMPARE(dialog.entry()->textFile,e.textFile); QCOMPARE(dialog.entry()->id,e.id); QCOMPARE(dialog.entry()->created,e.created);
    }
    void editedDocumentResetsVisibilityAndSnapshotCopies_data() {
        QTest::addColumn<bool>("snapshot"); QTest::newRow("editable")<<false; QTest::newRow("snapshot-copy")<<true;
    }
    void editedDocumentResetsVisibilityAndSnapshotCopies() {
        QFETCH(bool,snapshot); model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        const auto e=document(group,snapshot); QVERIFY(model.setEntry(e).isEmpty()); EntryEditDialog dialog(model,e);
        auto* edit=dialog.findChild<QPushButton*>("editTextFileBody"); auto* body=dialog.findChild<QPlainTextEdit*>("entryTextFileBody"); QVERIFY(edit); QVERIFY(body);
        answer(false); edit->click(); QVERIFY(body->toPlainText().isEmpty());
        answer(true); edit->click(); body->moveCursor(QTextCursor::End); body->insertPlainText("new");
        answer(false); dialog.accept(); QVERIFY(!dialog.entry());
        answer(true); dialog.accept(); QVERIFY(dialog.entry());
        QCOMPARE(dialog.entry()->textFile.bytes,QByteArray::fromHex("efbbbf")+QStringLiteral("public value\nsynthetic-password\nnew").toUtf8());
        QVERIFY(dialog.entry()->textFile.visibleLines.isEmpty()); QVERIFY(!dialog.entry()->textFile.sourceSnapshot);
        QCOMPARE(dialog.entry()->id!=e.id,snapshot); QCOMPARE(model.entry(e.id)->textFile.bytes,e.textFile.bytes);
        QVERIFY(model.setEntry(*dialog.entry()).isEmpty());
    }
    void entryAccessRechecksExactRecord_data() {
        QTest::addColumn<int>("action"); for(int i=0;i<5;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void entryAccessRechecksExactRecord() {
        QFETCH(int,action); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=model::EntryType::PlainValue; e.reverify=true; e.plainValue="192.0.2.10";
        QVERIFY(app.model()->setEntry(e).isEmpty()); bool visited=false;
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog); visited=true;
            if(action==1) {dialog->reject(); return;} if(action==2) {app.lock(); return;}
            if(action==3) {auto changed=e; changed.plainValue="192.0.2.20"; QVERIFY(app.model()->setEntry(changed).isEmpty());}
            if(action==4) QVERIFY(app.model()->removeEntry(e.id).isEmpty());
            dialog->setTextValue("valid-password"); dialog->accept();
        });
        QCOMPARE(ui::authorizeEntry(app,e.id,nullptr),action==0); QVERIFY(visited);
    }
    void protectedPlainCardUsesConfiguredTimer() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=model::EntryType::PlainValue; e.reverify=true; e.plainValue="192.0.2.10";
        QVERIFY(app.model()->setEntry(e).isEmpty()); ui::EntryCard card(app); card.setRevealSeconds(5); card.showEntry(e.id);
        QVERIFY(!card.secretText().contains(e.plainValue));
        QTimer::singleShot(0,[] {auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(auth); auth->setTextValue("valid-password"); auth->accept();});
        card.revealButton()->click(); QCOMPARE(card.secretText(),e.plainValue);
        auto* timer=card.findChild<QTimer*>("secretHideTimer"); QVERIFY(timer->isActive()); QCOMPARE(timer->interval(),5000);
        card.hideNowForTest(); QVERIFY(!card.secretText().contains(e.plainValue));
    }
    void fileBodyCannotPublishThroughNativeCopy() {
        model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Demo").isEmpty());
        const auto e=document(group); QVERIFY(model.setEntry(e).isEmpty()); EntryEditDialog dialog(model,e);
        answer(true); dialog.findChild<QPushButton*>("editTextFileBody")->click();
        auto* body=dialog.findChild<QPlainTextEdit*>("entryTextFileBody"); body->selectAll();
        const auto before=body->toPlainText(); QSignalSpy changed(QApplication::clipboard(),&QClipboard::dataChanged);
        const auto cleanup=qScopeGuard([] {auto* clipboard=QApplication::clipboard(); if(clipboard->ownsClipboard()) clipboard->clear();});
        QTest::keyClick(body,Qt::Key_C,Qt::ControlModifier); QCOMPARE(changed.size(),0);
        QTest::keyClick(body,Qt::Key_X,Qt::ControlModifier); QCOMPARE(changed.size(),0); QCOMPARE(body->toPlainText(),before);
    }
    void fileSaveFailureRestoresBodyAndVisibility() {
        QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        const auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty()); QVERIFY(app.save(&error));
        QFile file(app.storePathForTest()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto before=file.readAll(); file.close();
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        auto* list=window.findChild<QListWidget*>("entriesList"); QVERIFY(list); QCOMPARE(list->count(),1); list->setCurrentRow(0);
        ops.failWrite=true; bool visited=false;
        QTimer::singleShot(0,[&] {
            auto* editor=qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget()); QVERIFY(editor); visited=true;
            answer(true); editor->findChild<QPushButton*>("editTextFileBody")->click();
            auto* body=editor->findChild<QPlainTextEdit*>("entryTextFileBody"); body->moveCursor(QTextCursor::End); body->insertPlainText("not saved");
            answer(true); editor->accept();
        });
        window.editSelected(); QVERIFY(visited); QCOMPARE(*app.model()->entry(e.id),e);
        QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),before);
    }
    void editorRequiresOwnerBeforeShowingProtectedValue_data() {
        QTest::addColumn<int>("action"); QTest::newRow("confirmed")<<0; QTest::newRow("cancel")<<1; QTest::newRow("lock")<<2;
    }
    void editorRequiresOwnerBeforeShowingProtectedValue() {
        QFETCH(int,action); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.type=model::EntryType::PlainValue; e.reverify=true; e.title="IP"; e.plainValue="192.0.2.10";
        QVERIFY(app.model()->setEntry(e).isEmpty());
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;}); window.findChild<QListWidget*>("entriesList")->setCurrentRow(0);
        QTimer driver; driver.setInterval(10); bool authSeen=false,editorSeen=false;
        connect(&driver,&QTimer::timeout,&window,[&] {
            if(auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                authSeen=true; if(action==1) {auth->reject(); return;} if(action==2) {app.lock(); return;}
                auth->setTextValue("valid-password"); auth->accept();
            } else if(auto* editor=qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget())) {
                editorSeen=true; auto* field=editor->findChild<QLineEdit*>("entryPlainValue"); QCOMPARE(field->echoMode(),QLineEdit::Password);
                if(auto* accessible=QAccessible::queryAccessibleInterface(field)) QVERIFY(!accessible->text(QAccessible::Value).contains(e.plainValue));
                editor->reject(); QVERIFY(field->text().isEmpty());
            }
        });
        QTimer::singleShot(5000,&window,[] {if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        driver.start(); window.editSelected(); driver.stop(); QVERIFY(authSeen); QCOMPARE(editorSeen,action==0);
    }
    void lockCannotRepopulateFileBodyAfterConfirmation() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        const auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty());
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;}); window.findChild<QListWidget*>("entriesList")->setCurrentRow(0);
        bool visited=false;
        QTimer::singleShot(0,[&] {
            auto* editor=qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget()); QVERIFY(editor);
            QTimer::singleShot(0,[&] {
                auto* confirm=qobject_cast<QMessageBox*>(QApplication::activeModalWidget()); QVERIFY(confirm);
                connect(confirm,&QDialog::finished,&app,&Application::lock,Qt::SingleShotConnection); confirm->button(QMessageBox::Yes)->click();
            });
            editor->findChild<QPushButton*>("editTextFileBody")->click(); visited=true;
            QVERIFY(editor->findChild<QPlainTextEdit*>("entryTextFileBody")->toPlainText().isEmpty()); QVERIFY(!editor->entry());
        });
        window.editSelected(); QVERIFY(visited); QCOMPARE(app.gate(),Application::Gate::NeedUnlock);
    }
    void fiveFormsFit_data() {
        QTest::addColumn<int>("type"); QTest::addColumn<int>("width"); QTest::addColumn<bool>("dark");
        for(int type=0;type<5;++type) for(int width:{420,900}) for(bool dark:{false,true})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(type).arg(width).arg(dark?"dark":"light")))<<type<<width<<dark;
    }
    void fiveFormsFit() {
        QFETCH(int,type); QFETCH(int,width); QFETCH(bool,dark);
        ui::theme::apply(dark?ui::theme::Mode::Dark:ui::theme::Mode::Light);
        model::VaultModel model; const auto group=QUuid::createUuid(); QVERIFY(model.addNode(group,{},"Демо").isEmpty());
        EntryEditDialog dialog(model,std::nullopt); dialog.setGroup(group); dialog.findChild<QComboBox*>("entryType")->setCurrentIndex(type);
        dialog.findChild<QLineEdit*>("entryTitle")->setText("Вымышленные данные");
        dialog.findChild<QLineEdit*>("entryPlainValue")->setText("192.0.2.10"); dialog.findChild<QLineEdit*>("entryParameterName")->setText("PUBLIC_IP");
        dialog.resize(width,480); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        QCOMPARE(dialog.width(),width); auto* cancel=dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Cancel);
        QVERIFY(dialog.rect().contains(QRect(cancel->mapTo(&dialog,QPoint()),cancel->size())));
        cancel->setFocus(); QTest::keyClick(cancel,Qt::Key_Tab); QVERIFY(dialog.focusWidget());
        const auto shots=qEnvironmentVariable("TOKENBUCKET_STORAGE_SCREENSHOTS");
        if(!shots.isEmpty()) {QVERIFY(QDir().mkpath(shots)); QVERIFY(dialog.grab().save(shots+QString("/form-%1-%2-%3.png").arg(type).arg(width).arg(dark?"dark":"light")));}
        dialog.reject(); QVERIFY(!dialog.entry());
    }
};
QTEST_MAIN(TestStorageUi)
#include "test_storage_ui.moc"
