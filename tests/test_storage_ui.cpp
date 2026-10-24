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
#include <QFileDialog>
#include <QAction>
#include <QTableView>
#include <QTreeWidget>
#include <QTreeView>
#include <QTableWidget>
#include <QTabWidget>
#include "../src/ui/RegistryView.h"
#include "../src/ui/TextFileDialog.h"
#include "../src/ui/TextImportDialog.h"
#include "../src/ui/MiniPanel.h"
#include "../src/platform/ClipboardGuard.h"
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
    static QString clipboardText() {
#ifdef Q_OS_WIN
        if(!OpenClipboard(nullptr)) return QStringLiteral("<unavailable>");
        const auto close=qScopeGuard([] {CloseClipboard();});
        const auto handle=GetClipboardData(CF_UNICODETEXT);
        const auto* data=handle?static_cast<const wchar_t*>(GlobalLock(handle)):nullptr;
        const QString text=data?QString::fromWCharArray(data):QString(); if(data) GlobalUnlock(handle); return text;
#else
        return QApplication::clipboard()->text();
#endif
    }
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
    void registryRolesAreSafeAndShareMainFilters() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry value; value.id=QUuid::createUuid(); value.groupId=group; value.title="IP"; value.type=model::EntryType::PlainValue; value.parameterName="PUBLIC_IP"; value.plainValue="192.0.2.10"; QVERIFY(app.model()->setEntry(value).isEmpty());
        auto protectedValue=value; protectedValue.id=QUuid::createUuid(); protectedValue.reverify=true; protectedValue.plainValue="synthetic-protected-value"; QVERIFY(app.model()->setEntry(protectedValue).isEmpty());
        model::Entry secret; secret.id=QUuid::createUuid(); secret.groupId=group; secret.title="TOKEN"; secret.token="synthetic-token-value"; QVERIFY(app.model()->setEntry(secret).isEmpty());
        ui::RegistryView registry(app); registry.setGroup(group); auto* view=registry.findChild<QTreeView*>("registryEntries"); QCOMPARE(view->model()->rowCount(),3);
        for(int row=0;row<3;++row) for(int col=0;col<5;++col) for(auto role:{Qt::DisplayRole,Qt::ToolTipRole,Qt::AccessibleTextRole,Qt::UserRole,Qt::EditRole}) {
            const auto text=view->model()->index(row,col).data(role).toString(); QVERIFY(!text.contains(secret.token)); QVERIFY(!text.contains(protectedValue.plainValue));
        }
        QSignalSpy copies(&registry,&ui::RegistryView::copyRequested); view->setCurrentIndex(view->model()->index(0,0)); QTest::keyClick(view,Qt::Key_C,Qt::ControlModifier); QCOMPARE(copies.size(),1);
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        auto* embedded=window.findChild<ui::RegistryView*>()->findChild<QTreeView*>(); QCOMPARE(embedded->model()->rowCount(),3);
        window.search()->setText("192.0.2.10"); QCOMPARE(embedded->model()->rowCount(),1); QCOMPARE(embedded->model()->index(0,0).data(Qt::UserRole).value<model::Id>(),value.id);
        window.search()->setText(protectedValue.plainValue); QCOMPARE(embedded->model()->rowCount(),0);
        app.lock(); QCOMPARE(view->model()->rowCount(),0); QCOMPARE(embedded->model()->rowCount(),0);
    }
    void compareSourceRequiresBothOwnersAndReportsOnlyResult_data() {
        QTest::addColumn<int>("action"); for(int i=0;i<6;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void compareSourceRequiresBothOwnersAndReportsOnlyResult() {
        QFETCH(int,action); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); auto source=document(group,true); source.reverify=true; source.textFile.bytes="TOKEN=synthetic-value\r\n"; source.textFile.visibleLines.clear(); QVERIFY(app.model()->setEntry(source).isEmpty());
        model::Entry e; e.id=QUuid::createUuid(); e.groupId=group; e.title="TOKEN"; e.token=action==1?"synthetic-different":"synthetic-value"; e.reverify=true; e.sourceEntryId=source.id; e.sourceLine=0; QVERIFY(app.model()->setEntry(e).isEmpty());
        if(action==4) QVERIFY(app.model()->removeEntry(source.id).isEmpty());
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        int auths=0; QString result; QTimer driver; driver.setInterval(10); connect(&driver,&QTimer::timeout,&window,[&] {
            if(auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {
                ++auths; if(action==2 && auths==2) {auth->reject(); return;} if(action==3 && auths==2) {app.lock(); return;}
                if(action==5 && auths==2) {auto changed=e; changed.token="changed-without-modified"; QVERIFY(app.model()->setEntry(changed).isEmpty());}
                auth->setTextValue("valid-password"); auth->accept();
            } else if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {result=box->text(); box->accept();}
        });
        bool timedOut=false; QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&window,[&] {timedOut=true; app.lock();});
        driver.start(); watchdog.start(5000); window.compareWithSource(e.id); driver.stop(); watchdog.stop(); QVERIFY(!timedOut);
        QCOMPARE(auths,action==4?1:2); QCOMPARE(result,action==0?QStringLiteral("Совпадает"):action==1?QStringLiteral("Отличается"):QString());
    }
    void registryMigrationIsReviewedAndTransactional_data() {
        QTest::addColumn<int>("action"); for(int i=0;i<7;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void registryMigrationIsReviewedAndTransactional() {
        QFETCH(int,action); QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        model::Entry ip; ip.id=QUuid::createUuid(); ip.groupId=group; ip.title="PUBLIC_IP"; ip.token="192.0.2.10"; QVERIFY(app.model()->setEntry(ip).isEmpty()); QVERIFY(app.save(&error));
        QFile vault(app.storePathForTest()); QVERIFY(vault.open(QIODevice::ReadOnly)); const auto before=vault.readAll(); vault.close();
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;}); window.tree()->setCurrentItem(window.tree()->topLevelItem(0));
        if(action==3) ops.failWriteCall=ops.writeCalls+1;
        if(action==4) ops.beforeWrite=[&](const QString& path) {if(path.endsWith("vault.tb.tmp")) ops.failWrite=true;};
        if(action==5) ops.beforeWrite=[&](const QString& path) {if(path.contains(".before-registry-migration-")) QMetaObject::invokeMethod(&app,&Application::lock,Qt::BlockingQueuedConnection);};
        bool seen=false,timedOut=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&window,[&] {
            auto* modal=QApplication::activeModalWidget();
            if(auto* auth=qobject_cast<QInputDialog*>(modal)) {auth->setTextValue("valid-password"); auth->accept();}
            else if(auto* box=qobject_cast<QMessageBox*>(modal)) {QCOMPARE(box->textFormat(),Qt::PlainText); box->button(action==2?QMessageBox::No:QMessageBox::Yes)->click();}
            else if(auto* preview=qobject_cast<QDialog*>(modal);preview && preview->objectName()=="registryMigrationPreview" && !seen) {
                seen=true; QTimer::singleShot(0,preview,[&,preview] {
                    if(action==1) {preview->reject(); return;}
                    if(action==0 && preview->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->isEnabled()) {
                        auto* table=preview->findChild<QTableWidget*>(); table->setCurrentCell(0,4); QCOMPARE(table->item(0,4)->text(),QStringLiteral("••••••••"));
                        preview->findChild<QPushButton*>("migrationReveal")->click(); QCOMPARE(table->item(0,4)->text(),ip.token);
                        auto* timer=preview->findChild<QTimer*>("migrationHideTimer"); QVERIFY(timer->isActive()); QMetaObject::invokeMethod(timer,"timeout"); QCOMPARE(table->item(0,4)->text(),QStringLiteral("••••••••"));
                    }
                    if(action==6) {auto changed=ip; changed.token="192.0.2.20"; QVERIFY(app.model()->setEntry(changed).isEmpty());}
                    preview->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
                    if(app.gate()==Application::Gate::Ready) {if(action==3 || action==4 || action==6) QVERIFY(!preview->findChild<QLabel*>("registryMigrationError")->text().isEmpty()); preview->reject();}
                });
            }
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&window,[&] {timedOut=true; app.lock();});
        driver.start(); watchdog.start(7000); window.previewRegistryMigration(); driver.stop(); watchdog.stop(); QVERIFY(seen); QVERIFY(!timedOut);
        ops.beforeWrite={}; ops.failWrite=false; ops.failWriteCall=0;
        if(action==0 || action==5) QVERIFY(app.unlock("valid-password",&error));
        QCOMPARE(app.model()->entry(ip.id)->type,action==0?model::EntryType::PlainValue:model::EntryType::ApiToken);
        QVERIFY(vault.open(QIODevice::ReadOnly)); const auto after=vault.readAll(); vault.close(); if(action!=0) QCOMPARE(after,before);
        const auto copies=QDir(dir.path()).entryList({"vault.tb.before-registry-migration-*"},QDir::Files);
        if(action==0) {
            QCOMPARE(copies.size(),1); QFile copy(dir.filePath(copies.first())); QVERIFY(copy.open(QIODevice::ReadOnly)); const auto safety=copy.readAll(); QCOMPARE(safety,before);
            Application restored(dir.filePath("recovered")); QVERIFY(restored.importContainer(safety,"valid-password","recovery-password",&error));
            QCOMPARE(restored.model()->entry(ip.id)->type,model::EntryType::ApiToken); QCOMPARE(restored.model()->entry(ip.id)->token,ip.token);
            window.tree()->setCurrentItem(window.tree()->topLevelItem(0)); seen=false; driver.start(); watchdog.start(7000); window.previewRegistryMigration(); driver.stop(); watchdog.stop(); QVERIFY(seen); QVERIFY(!timedOut);
            QCOMPARE(QDir(dir.path()).entryList({"vault.tb.before-registry-migration-*"},QDir::Files),copies);
            QVERIFY(vault.open(QIODevice::ReadOnly)); QCOMPARE(vault.readAll(),after); vault.close();
        }
        if(action==1 || action==2) QVERIFY(copies.isEmpty());
    }
    void mainWindowHasAResourceRegistry() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        QVERIFY(window.findChild<QWidget*>("resourceRegistry")); QVERIFY(window.findChild<QAction*>("previewRegistryMigration"));
    }
    void textFileImportReadsBoundedSource_data() {
        QTest::addColumn<int>("action"); QTest::newRow("saved")<<0; QTest::newRow("oversize")<<1; QTest::newRow("invalid-utf8")<<2; QTest::newRow("preview-cancel")<<3;
    }
    void textFileImportReadsBoundedSource() {
        QFETCH(int,action); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        QByteArray bytes=QByteArray::fromHex("efbbbf")+"PUBLIC_IP=192.0.2.10\r\nTOKEN=synthetic\r\n# synthetic hidden\r\n";
        if(action==1) bytes=QByteArray(model::VaultModel::MaxSecretBytes+1,'x'); if(action==2) bytes=QByteArray::fromHex("ff");
        const auto path=dir.filePath("source.txt"); QVERIFY(store::realFileOps().writeAll(path,bytes));
        QFile vault(app.storePathForTest()); QVERIFY(vault.open(QIODevice::ReadOnly)); const auto before=vault.readAll(); vault.close();
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        auto* tree=window.tree(); QVERIFY(tree->topLevelItem(0)); tree->setCurrentItem(tree->topLevelItem(0)); QCOMPARE(tree->currentItem()->data(0,Qt::UserRole).value<model::Id>(),group);
        const bool nativeDisabled=QApplication::testAttribute(Qt::AA_DontUseNativeDialogs); QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true); const auto restore=qScopeGuard([&] {QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,nativeDisabled);});
        bool fileSeen=false,authSeen=false,previewSeen=false,timedOut=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&window,[&] {
            auto* modal=QApplication::activeModalWidget();
            if(auto* file=qobject_cast<QFileDialog*>(modal);file && !fileSeen) {fileSeen=true; file->selectFile(path); QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);}
            else if(auto* auth=qobject_cast<QInputDialog*>(modal)) {authSeen=true; auth->setTextValue("valid-password"); auth->accept();}
            else if(auto* box=qobject_cast<QMessageBox*>(modal)) box->button(QMessageBox::Yes)->click();
            else if(auto* preview=qobject_cast<ui::TextImportDialog*>(modal);preview && !previewSeen) {
                previewSeen=true; QTimer::singleShot(0,preview,[preview,action] {if(action==3) preview->reject(); else preview->accept();});
            }
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&window,[&] {timedOut=true; app.lock();});
        driver.start(); watchdog.start(10000); window.importTextFile(); driver.stop(); watchdog.stop(); QVERIFY(!timedOut); QVERIFY(fileSeen); QCOMPARE(authSeen,action==0 || action==3); QCOMPARE(previewSeen,action==0 || action==3);
        QCOMPARE(app.model()->entries().size(),action==0?3:0); QVERIFY(vault.open(QIODevice::ReadOnly)); const auto after=vault.readAll(); vault.close();
        if(action!=0) QCOMPARE(after,before);
        else for(const auto& e:app.model()->entries()) if(e.type==model::EntryType::TextFile) {QVERIFY(e.textFile.sourceSnapshot); QCOMPARE(e.textFile.bytes,bytes); QCOMPARE(e.textFile.visibleLines,QList<int>{0});}
    }
    void initTestCase() {qApp->setQuitOnLastWindowClosed(false);}
    void textPreviewMasksUnknownDataAndCancelIsReadOnly() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        auto p=io::parseTextFile("# synthetic-comment\nsynthetic-sensitive-label=synthetic-password\nPUBLIC_IP=192.0.2.10\n","demo.txt"); p.source.groupId=group;
        const auto before=*app.model(); int called=0; ui::TextImportDialog dialog(app,p,[&](const auto&,const auto&,QString*) {++called; return false;});
        auto* parameters=dialog.findChild<QTableView*>("textImportParameters"); auto* source=dialog.findChild<QTableView*>("textImportSource");
        QCOMPARE(parameters->model()->rowCount(),2); QCOMPARE(source->model()->rowCount(),4);
        for(auto role:{Qt::DisplayRole,Qt::ToolTipRole,Qt::AccessibleTextRole,Qt::UserRole,Qt::EditRole}) {
            QVERIFY(!parameters->model()->index(0,3).data(role).toString().contains("synthetic-password"));
            QVERIFY(!parameters->model()->index(0,2).data(role).toString().contains("sensitive-label"));
            QVERIFY(!source->model()->index(0,1).data(role).toString().contains("synthetic-comment"));
        }
        dialog.reject(); QCOMPARE(called,0); QVERIFY(*app.model()==before); QCOMPARE(parameters->model()->rowCount(),0); QCOMPARE(source->model()->rowCount(),0);
    }
    void textPreviewConfirmsClassificationAndStaysOpenOnError() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); auto p=io::parseTextFile("TOKEN=synthetic\n","demo.txt"); p.source.groupId=group;
        int applied=0; bool classified=false; ui::TextImportDialog dialog(app,p,[&](const io::TextImportPreview& preview,const QList<io::RegistrySelection>& choices,QString* e) {
            ++applied; classified=preview.source.textFile.visibleLines==QList<int>{0} && choices.first().type==model::EntryType::PlainValue; *e="synthetic storage refused"; return false;
        }); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* type=dialog.findChild<QComboBox*>("textImportType"); answer(false); type->setCurrentIndex(1); QCOMPARE(type->currentData().toInt(),int(model::EntryType::ApiToken));
        answer(true); type->setCurrentIndex(1); QCOMPARE(type->currentData().toInt(),int(model::EntryType::PlainValue));
        answer(false); dialog.accept(); QCOMPARE(applied,0); QVERIFY(dialog.isVisible());
        answer(true); dialog.accept(); QCOMPARE(applied,1); QVERIFY(classified); QVERIFY(dialog.isVisible()); QCOMPARE(dialog.findChild<QLabel*>("textImportError")->text(),QString("synthetic storage refused"));
        QVERIFY(app.model()->entries().isEmpty()); dialog.reject();
    }
    void textPreviewLockClosesClassification() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        auto p=io::parseTextFile("PUBLIC_IP=192.0.2.10","demo.txt"); p.source.groupId=group; int applied=0;
        ui::TextImportDialog dialog(app,p,[&](const auto&,const auto&,QString*) {++applied; return true;});
        bool seen=false; QTimer::singleShot(0,[&] {QVERIFY(qobject_cast<QMessageBox*>(QApplication::activeModalWidget())); seen=true; app.lock();});
        dialog.accept(); QVERIFY(seen); QCOMPARE(applied,0); QCOMPARE(dialog.findChild<QTableView*>("textImportParameters")->model()->rowCount(),0); QVERIFY(dialog.findChild<QLineEdit*>("textImportTitle")->text().isEmpty());
    }
    void textPreviewUsesTimedOwnerCheckAndBlocksNativeCopy() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        auto p=io::parseTextFile("PUBLIC_IP=192.0.2.10\nTOKEN=synthetic","demo.txt"); p.source.groupId=group; p.source.reverify=true;
        ui::TextImportDialog dialog(app,p,{}); dialog.setRevealSeconds(5); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* table=dialog.findChild<QTableView*>("textImportParameters"); QVERIFY(!table->model()->index(0,3).data().toString().contains("192.0.2.10"));
        QTimer::singleShot(0,[] {auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(auth); auth->setTextValue("valid-password"); auth->accept();});
        dialog.findChild<QPushButton*>("textImportReveal")->click(); QCOMPARE(table->model()->index(0,3).data().toString(),QString("192.0.2.10"));
        auto* timer=dialog.findChild<QTimer*>("textImportHideTimer"); QVERIFY(timer->isActive()); QCOMPARE(timer->interval(),5000);
        QSignalSpy changed(QApplication::clipboard(),&QClipboard::dataChanged); QTest::keyClick(table,Qt::Key_C,Qt::ControlModifier); QCOMPARE(changed.count(),0);
        QMetaObject::invokeMethod(timer,"timeout"); QVERIFY(!table->model()->index(0,3).data().toString().contains("192.0.2.10"));
        app.lock(); QCOMPARE(table->model()->rowCount(),0); QCOMPARE(dialog.findChild<QTableView*>("textImportSource")->model()->rowCount(),0);
    }
    void structureDocumentAppliesOnceOrKeepsOriginal_data() {
        QTest::addColumn<int>("action"); QTest::newRow("save")<<0; QTest::newRow("save-failed")<<1; QTest::newRow("cancel")<<2;
    }
    void structureDocumentAppliesOnceOrKeepsOriginal() {
        QFETCH(int,action); QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error)); const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        auto e=document(group); e.textFile.bytes="PUBLIC_IP=192.0.2.10\r\nTOKEN=synthetic\r\n"; e.textFile.visibleLines.clear(); QVERIFY(app.model()->setEntry(e).isEmpty()); QVERIFY(app.save(&error));
        QFile file(app.storePathForTest()); QVERIFY(file.open(QIODevice::ReadOnly)); const auto before=file.readAll(); file.close();
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat); ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;});
        ops.failWrite=action==1; bool seen=false,timedOut=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&window,[&] {
            auto* modal=QApplication::activeModalWidget();
            if(auto* auth=qobject_cast<QInputDialog*>(modal)) {auth->setTextValue("valid-password"); auth->accept();}
            else if(auto* box=qobject_cast<QMessageBox*>(modal)) box->button(QMessageBox::Yes)->click();
            else if(auto* preview=qobject_cast<ui::TextImportDialog*>(modal);preview && !seen) {
                seen=true; QTimer::singleShot(0,preview,[&,preview] {
                    if(action==2) {preview->reject(); return;} preview->accept();
                    if(action==1) {QVERIFY(!preview->findChild<QLabel*>("textImportError")->text().isEmpty()); QCOMPARE(app.model()->entries().size(),1); preview->reject();}
                });
            }
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&window,[&] {timedOut=true; app.lock();});
        driver.start(); watchdog.start(6000); window.structureDocument(e.id); driver.stop(); watchdog.stop(); QVERIFY(seen); QVERIFY(!timedOut); QCOMPARE(*app.model()->entry(e.id),e);
        QCOMPARE(app.model()->entries().size(),action==0?4:1); QVERIFY(file.open(QIODevice::ReadOnly)); const auto after=file.readAll(); file.close();
        if(action!=0) QCOMPARE(after,before);
        ops.failWrite=false; app.lock(); QVERIFY(app.unlock("valid-password",&error)); QCOMPARE(app.model()->entries().size(),action==0?4:1);
    }
    void textImportHasAnExplicitPreviewAction() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;}); QVERIFY(window.findChild<QAction*>("importTextFile"));
    }
    void documentLockClosesOutputPrompts_data() {
        QTest::addColumn<int>("prompt"); QTest::newRow("warning")<<0; QTest::newRow("owner-check")<<1; QTest::newRow("file-dialog")<<2;
    }
    void documentLockClosesOutputPrompts() {
        QFETCH(int,prompt); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); const auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty());
        ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip);
        const bool nativeDisabled=QApplication::testAttribute(Qt::AA_DontUseNativeDialogs); QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
        const auto restore=qScopeGuard([&] {QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,nativeDisabled);});
        bool locked=false,timedOut=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&dialog,[&] {
            auto* modal=QApplication::activeModalWidget();
            if((prompt==0 && qobject_cast<QMessageBox*>(modal)) || (prompt==1 && qobject_cast<QInputDialog*>(modal)) || (prompt==2 && qobject_cast<QFileDialog*>(modal))) {
                if(!locked) {locked=true; app.lock();} return;
            }
            if(auto* box=qobject_cast<QMessageBox*>(modal)) box->button(QMessageBox::Yes)->click();
            else if(auto* auth=qobject_cast<QInputDialog*>(modal)) {auth->setTextValue("valid-password"); auth->accept();}
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&dialog,[&] {timedOut=true; if(auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();});
        driver.start(); watchdog.start(2000); dialog.findChild<QPushButton*>(prompt==2?"exportTextFile":"copyTextFile")->click(); driver.stop(); watchdog.stop();
        QVERIFY(locked); QVERIFY(!timedOut); QCOMPARE(dialog.findChild<QTableView*>()->model()->rowCount(),0); QVERIFY(!clip.snapshot().owned);
    }
    void documentVisibilityCommitsOrRollsBack_data() {
        QTest::addColumn<bool>("fail"); QTest::newRow("saved")<<false; QTest::newRow("write-failed")<<true;
    }
    void documentVisibilityCommitsOrRollsBack() {
        QFETCH(bool,fail); QTemporaryDir dir; FaultOps ops;
#ifdef Q_OS_WIN
        Application app(dir.path(),nullptr,platform::hello::request,ops);
#else
        Application app(dir.path(),nullptr,ops);
#endif
        QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); const auto e=document(group);
        QVERIFY(app.model()->setEntry(e).isEmpty()); QVERIFY(app.save(&error));
        QSettings prefs(dir.filePath("ui.ini"),QSettings::IniFormat),run(dir.filePath("run.ini"),QSettings::IniFormat);
        ui::MainWindow window(app,nullptr,&prefs,&run,[] {return true;}); ops.failWrite=fail;
        QTimer driver; driver.setInterval(10); connect(&driver,&QTimer::timeout,&window,[] {
            if(auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {auth->setTextValue("valid-password"); auth->accept();}
            else if(auto* confirm=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) confirm->button(QMessageBox::Yes)->click();
        });
        bool visited=false,timedOut=false; QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&app,[&] {timedOut=true; app.lock();});
        QTimer::singleShot(0,[&] {
            auto* dialog=qobject_cast<ui::TextFileDialog*>(QApplication::activeModalWidget()); QVERIFY(dialog); visited=true;
            auto* table=dialog->findChild<QTableView*>(); table->setCurrentIndex(table->model()->index(1,1));
            dialog->findChild<QPushButton*>("textLineVisibility")->click();
            if(!timedOut) {QVERIFY(app.model()->entry(e.id)); QCOMPARE(app.model()->entry(e.id)->textFile.visibleLines,fail?QList<int>{0}:(QList<int>{0,1}));}
            dialog->reject();
        });
        driver.start(); watchdog.start(5000); window.openDocument(e.id); driver.stop(); watchdog.stop(); QVERIFY(visited); QVERIFY(!timedOut);
        QCOMPARE(app.model()->entry(e.id)->textFile.bytes,e.textFile.bytes);
        ops.failWrite=false; app.lock(); QVERIFY(app.unlock("valid-password",&error));
        QCOMPARE(app.model()->entry(e.id)->textFile.visibleLines,fail?QList<int>{0}:(QList<int>{0,1}));
    }
    void pendingDocumentCopyCannotOutliveLock() {
#ifdef Q_OS_WIN
        QVERIFY(qEnvironmentVariableIsSet("TOKENBUCKET_TEST_USER_CLIPBOARD"));
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); const auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty());
        ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip); auto* table=dialog.findChild<QTableView*>(); table->setCurrentIndex(table->model()->index(0,1));
        QVERIFY(OpenClipboard(nullptr)); bool locked=false;
        QTimer::singleShot(1,[&] {locked=true; app.lock(); CloseClipboard();});
        dialog.findChild<QPushButton*>("copyTextLine")->click(); QVERIFY(locked); QCOMPARE(table->model()->rowCount(),0);
        QVERIFY(!clipboardText().contains("public")); QVERIFY(!clip.snapshot().owned);
#endif
    }
    void documentRowsGuardEveryRoleAndCopy() {
        QVERIFY(qEnvironmentVariableIsSet("TOKENBUCKET_TEST_USER_CLIPBOARD"));
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty()); ClipboardGuard clip;
        ui::TextFileDialog dialog(app,e.id,clip); dialog.setRevealSeconds(5); dialog.show(); QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto* table=dialog.findChild<QTableView*>("textFileLines"); QVERIFY(table); QCOMPARE(table->model()->rowCount(),3);
        for(auto role:{Qt::DisplayRole,Qt::ToolTipRole,Qt::AccessibleTextRole,Qt::UserRole,Qt::EditRole})
            QVERIFY(!table->model()->index(1,1).data(role).toString().contains("synthetic-password"));
        table->setCurrentIndex(table->model()->index(0,1)); table->setFocus(); QTest::keySequence(table,QKeySequence::Copy);
        QCOMPARE(QApplication::clipboard()->text(),QStringLiteral("public value")); QVERIFY(clip.snapshot().owned);
        table->setCurrentIndex(table->model()->index(1,1));
        QTimer::singleShot(0,[] {auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(auth); auth->reject();});
        dialog.findChild<QPushButton*>("copyTextLine")->click(); QCOMPARE(QApplication::clipboard()->text(),QStringLiteral("public value"));
        const auto verify=[] {QTimer::singleShot(0,[] {auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(auth); auth->setTextValue("valid-password"); auth->accept();});};
        verify(); dialog.findChild<QPushButton*>("revealTextLine")->click(); QCOMPARE(table->model()->index(1,1).data().toString(),QString("synthetic-password"));
        auto* timer=dialog.findChild<QTimer*>("textLineHideTimer"); QVERIFY(timer->isActive()); QCOMPARE(timer->interval(),5000);
        QMetaObject::invokeMethod(timer,"timeout"); QVERIFY(!table->model()->index(1,1).data().toString().contains("synthetic-password"));
        verify(); dialog.findChild<QPushButton*>("revealTextLine")->click(); table->setCurrentIndex(table->model()->index(0,1));
        QVERIFY(!table->model()->index(1,1).data().toString().contains("synthetic-password"));
        table->setCurrentIndex(table->model()->index(1,1)); verify(); dialog.findChild<QPushButton*>("copyTextLine")->click(); QCOMPARE(QApplication::clipboard()->text(),QString("synthetic-password"));
        app.lock(); QCOMPARE(table->model()->rowCount(),0); QVERIFY(!clip.snapshot().owned); QVERIFY(!dialog.isVisible());
    }
    void documentRevealRejectsChangedRecord_data() {
        QTest::addColumn<int>("action"); for(int i=0;i<4;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void documentRevealRejectsChangedRecord() {
        QFETCH(int,action); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); auto e=document(group); e.reverify=true;
        QVERIFY(app.model()->setEntry(e).isEmpty()); ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip);
        auto* table=dialog.findChild<QTableView*>(); table->setCurrentIndex(table->model()->index(0,1));
        QVERIFY(!table->model()->index(0,1).data().toString().contains("public"));
        QTimer::singleShot(0,[&] {
            auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget()); QVERIFY(auth);
            if(action==0) {auth->reject(); return;} if(action==1) {app.lock(); return;}
            if(action==2) {auto changed=e; changed.title="Changed without modified"; QVERIFY(app.model()->setEntry(changed).isEmpty());}
            else QVERIFY(app.model()->removeEntry(e.id).isEmpty());
            auth->setTextValue("valid-password"); auth->accept();
        });
        dialog.findChild<QPushButton*>("revealTextLine")->click(); QVERIFY(!table->model()->index(0,1).data().toString().contains("public"));
        QVERIFY(!dialog.findChild<QTimer*>("textLineHideTimer")->isActive());
    }
    void wholeDocumentCopyIsExplicitAndExact() {
        QVERIFY(qEnvironmentVariableIsSet("TOKENBUCKET_TEST_USER_CLIPBOARD"));
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); const auto e=document(group);
        QVERIFY(app.model()->setEntry(e).isEmpty()); ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip);
        QVERIFY(clip.copySecret("synthetic-marker",&error)); answer(false); dialog.findChild<QPushButton*>("copyTextFile")->click(); QCOMPARE(QApplication::clipboard()->text(),QString("synthetic-marker"));
        QTimer driver; driver.setInterval(10); connect(&driver,&QTimer::timeout,&dialog,[] {
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->button(QMessageBox::Yes)->click();
            else if(auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {auth->setTextValue("valid-password"); auth->accept();}
        }); driver.start(); dialog.findChild<QPushButton*>("copyTextFile")->click(); driver.stop();
        QCOMPARE(clipboardText().toUtf8(),e.textFile.bytes);
        QApplication::clipboard()->setText("synthetic-foreign"); clip.clearIfOurs(); QCOMPARE(QApplication::clipboard()->text(),QString("synthetic-foreign"));
        QApplication::clipboard()->clear();
    }
    void exportDocumentKeepsBytes_data() {
        QTest::addColumn<int>("existing"); QTest::newRow("new")<<0; QTest::newRow("existing-cancel")<<1; QTest::newRow("existing-confirm")<<2;
    }
    void exportDocumentKeepsBytes() {
        QFETCH(int,existing); QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); const auto e=document(group);
        QVERIFY(app.model()->setEntry(e).isEmpty()); ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip);
        const auto path=dir.filePath("export.txt"); if(existing) QVERIFY(store::realFileOps().writeAll(path,"previous-synthetic"));
        const bool nativeDisabled=QApplication::testAttribute(Qt::AA_DontUseNativeDialogs); QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,true);
        const auto restore=qScopeGuard([&] {QApplication::setAttribute(Qt::AA_DontUseNativeDialogs,nativeDisabled);});
        bool fileSeen=false,replaceSeen=false,timedOut=false; QTimer driver; driver.setInterval(10);
        connect(&driver,&QTimer::timeout,&dialog,[&] {
            if(auto* box=qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                const bool replace=box->text().contains(QStringLiteral("Файл существует")); replaceSeen|=replace;
                QCOMPARE(box->textFormat(),Qt::PlainText); box->button(replace && existing==1?QMessageBox::No:QMessageBox::Yes)->click();
            } else if(auto* auth=qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) {auth->setTextValue("valid-password"); auth->accept();}
            else if(auto* file=qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
                if(!fileSeen) {fileSeen=true; file->selectFile(path); QMetaObject::invokeMethod(file,"accept",Qt::QueuedConnection);}
            }
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog,&QTimer::timeout,&dialog,[&] {timedOut=true; if(auto* modal=qobject_cast<QDialog*>(QApplication::activeModalWidget())) modal->reject();});
        driver.start(); watchdog.start(5000); dialog.findChild<QPushButton*>("exportTextFile")->click(); driver.stop(); watchdog.stop();
        QVERIFY(!timedOut); QVERIFY(fileSeen); QCOMPARE(replaceSeen,existing!=0);
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(),existing==1?QByteArray("previous-synthetic"):e.textFile.bytes);
    }
    void documentViewIsVirtualAndPlainPanelHasNoLoginFallback() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty()); auto e=document(group); e.textFile.bytes=QByteArray(49999,'\n'); e.textFile.visibleLines.clear();
        QVERIFY(app.model()->setEntry(e).isEmpty()); ClipboardGuard clip; ui::TextFileDialog dialog(app,e.id,clip);
        QCOMPARE(dialog.findChild<QTableView*>()->model()->rowCount(),50000); QVERIFY(dialog.findChildren<QWidget*>().size()<100);
        model::Entry value; value.id=QUuid::createUuid(); value.groupId=group; value.type=model::EntryType::PlainValue; value.title="IP"; value.plainValue="192.0.2.10";
        QVERIFY(app.model()->setEntry(value).isEmpty()); ui::MiniPanel panel(app,clip); panel.setDefaultField(paste::Field::Login); panel.popup();
        panel.findChild<QLineEdit*>("miniSearch")->setText("IP");
        auto* entries=panel.findChild<QListWidget*>("miniEntries"); QCOMPARE(entries->count(),1);
        auto* field=panel.findChild<QComboBox*>("miniField"); QCOMPARE(field->count(),1); QCOMPARE(field->currentText(),QStringLiteral("Значение")); QCOMPARE(field->currentData().toInt(),int(paste::Field::Secret)); panel.close();
    }
    void documentCardOffersLineView() {
        QTemporaryDir dir; Application app(dir.path()); QString error; QVERIFY(app.createPasswordVault("valid-password",&error));
        const auto group=QUuid::createUuid(); QVERIFY(app.model()->addNode(group,{},"Demo").isEmpty());
        const auto e=document(group); QVERIFY(app.model()->setEntry(e).isEmpty()); ui::EntryCard card(app); card.showEntry(e.id);
        QVERIFY(card.findChild<QPushButton*>("textFileOpen"));
    }
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
