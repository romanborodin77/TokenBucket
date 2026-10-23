#include <QtTest>
#include <QApplication>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QListWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStackedWidget>
#include <QToolButton>
#include <QTextDocument>
#include <QTimer>
#include <QDir>
#include <QScreen>
#include <QCheckBox>
#include <QDateEdit>
#include <QAccessible>
#include <QSettings>
#include <QSplitter>
#include <QSignalSpy>
#include <QMenu>
#include <QScrollArea>
#include <QScrollBar>
#include "../src/app/Application.h"
#include "../src/app/IdleTimer.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/MiniPanel.h"
#include "../src/platform/ClipboardGuard.h"
#include "../src/ui/EntryEditDialog.h"
#include "../src/ui/EntryCard.h"
#include "../src/ui/AboutDialog.h"
#include "../src/ui/Brand.h"
#include "../src/ui/Theme.h"
using namespace tb;
class TestUi : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
    QSettings uiPreferences_{dir_.filePath("window-test.ini"), QSettings::IniFormat};
    static void answerText(QWidget& owner, const QString& text) {
        QTimer::singleShot(0, &owner, [text] {
            if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { dialog->setTextValue(text); dialog->accept(); }
        });
    }
    static void editDialog(QWidget& owner, std::function<void(EntryEditDialog&)> fill) {
        QTimer::singleShot(0, &owner, [fill] {
            if (auto* dialog = qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget())) {
                fill(*dialog);
                QTimer::singleShot(5000, dialog, &EntryEditDialog::reject);
                dialog->findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            }
        });
    }
    static QLineEdit* field(QObject& dialog, const char* name) { return dialog.findChild<QLineEdit*>(QString::fromLatin1(name)); }
    static void confirm(QWidget& owner) {
        QTimer::singleShot(0, &owner, [] { if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) box->button(QMessageBox::Yes)->click(); });
    }
private slots:
    void init() { uiPreferences_.clear(); }
    void entryEditorFitsVisibleFields() {
        model::VaultModel model; const auto group = QUuid::createUuid();
        QVERIFY(model.addNode(group, {}, "Project").isEmpty());
        EntryEditDialog dialog(model, std::nullopt); dialog.setGroup(group); dialog.show();
        QCoreApplication::processEvents();
        const auto capture = [&](const QString& name) {
            QCoreApplication::processEvents();
            const auto dir = qEnvironmentVariable("TOKENBUCKET_UI_CAPTURE_DIR");
            if (!dir.isEmpty()) QVERIFY(dialog.grab().save(QDir(dir).filePath(name + ".png")));
        };
        auto* type = dialog.findChild<QComboBox*>("entryType"); QVERIFY(type);
        auto* note = dialog.findChild<QPlainTextEdit*>("entryNote"); QVERIFY(note);
        note->setPlainText(QString(1000, QChar('x')));
        field(dialog, "entryToken")->setText("synthetic-token");
        const auto compact = dialog.height(); capture("api-token");
        type->setCurrentIndex(2);
        const bool spacious = dialog.screen()->availableGeometry().height() >= 800 && dialog.width() >= 580;
        if (spacious) QTRY_VERIFY(dialog.height() > compact + 40);
        const auto expanded = dialog.height(); capture("ssh-key");
        auto* path = field(dialog, "sshPrivatePath");
        auto* accessible = QAccessible::queryAccessibleInterface(path); QVERIFY(accessible);
        QVERIFY(accessible->text(QAccessible::Name).contains(QStringLiteral("приватного")));
        auto* browse = dialog.findChild<QPushButton*>("browseSshKey"); QVERIFY(browse);
        QVERIFY(qAbs(path->mapTo(&dialog, path->rect().center()).y() - browse->mapTo(&dialog, browse->rect().center()).y()) <= 2);
        auto* scroll = dialog.findChild<QScrollArea*>();
        if (spacious) QTRY_COMPARE(scroll->verticalScrollBar()->maximum(), 0);
        type->setCurrentIndex(1); if (spacious) QTRY_VERIFY(dialog.height() < expanded); capture("credentials");
        type->setCurrentIndex(0); QTRY_COMPARE(dialog.height(), compact);
        QCOMPARE(field(dialog, "entryToken")->text(), QStringLiteral("synthetic-token"));
        QCOMPARE(note->toPlainText(), QString(1000, QChar('x')));
        auto* favorite = dialog.findChild<QCheckBox*>("entryFavorite");
        auto* reverify = dialog.findChild<QCheckBox*>("entryReverify");
        if (dialog.width() >= 580) QVERIFY(qAbs(favorite->mapTo(&dialog, favorite->rect().center()).y() - reverify->mapTo(&dialog, reverify->rect().center()).y()) <= 2);
        QVERIFY(note->height() <= note->fontMetrics().lineSpacing() * 4 + 12);
        dialog.accept();
        QVERIFY(dialog.findChild<QLabel*>("entryError")->isVisible());
        if (spacious) QTRY_VERIFY(dialog.height() > compact); capture("validation-error");
    }
    void entryEditorFitsAvailableScreen() {
        model::VaultModel model; EntryEditDialog dialog(model, std::nullopt); dialog.show();
        const auto area = dialog.screen()->availableGeometry();
        for (int type = 0; type < 3; ++type) {
            dialog.findChild<QComboBox*>("entryType")->setCurrentIndex(type); QCoreApplication::processEvents();
            QVERIFY(dialog.frameGeometry().width() <= area.width());
            QVERIFY(dialog.frameGeometry().height() <= area.height());
            QTRY_COMPARE(dialog.findChild<QScrollArea*>()->horizontalScrollBar()->maximum(), 0);
            const auto* save = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
            QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().center())));
            const auto dir = qEnvironmentVariable("TOKENBUCKET_UI_CAPTURE_DIR");
            if (!dir.isEmpty()) QVERIFY(dialog.grab().save(QDir(dir).filePath(QStringLiteral("screen-%1.png").arg(type))));
        }
        if (area.height() <= 600) QVERIFY(dialog.findChild<QScrollArea*>()->verticalScrollBar()->maximum() > 0);
    }
    void libraryRowsDoNotExposeSecrets() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
        const QString marker = QStringLiteral("synthetic-secret-marker");
        for (int i = 0; i < model::VaultModel::MaxEntries; ++i) {
            model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group;
            entry.title = QStringLiteral("Record %1").arg(i); entry.type = model::EntryType(i % 3);
            entry.token = marker; entry.login = marker; entry.password = marker;
            entry.sshPrivate = marker.toUtf8(); entry.sshPassphrase = marker;
            QVERIFY(app.model()->setEntry(entry).isEmpty());
        }
        ui::MainWindow w(app, nullptr, &uiPreferences_);
        QCOMPARE(w.list()->count(), model::VaultModel::MaxEntries);
        for (int i = 0; i < w.list()->count(); ++i) {
            auto* item = w.list()->item(i);
            QVERIFY(!item->data(Qt::UserRole).value<QUuid>().isNull());
            QVERIFY(!w.list()->itemWidget(item));
            for (const int role : {Qt::DisplayRole, Qt::ToolTipRole, Qt::StatusTipRole,
                                  Qt::WhatsThisRole, Qt::AccessibleTextRole, Qt::AccessibleDescriptionRole})
                QVERIFY(!item->data(role).toString().contains(marker));
        }
    }
    void statusTickKeepsReveal() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group;
        entry.title = "<b>Plain title</b>"; entry.token = "synthetic-status-secret"; entry.note = "<img src='https://example.test/'>";
        QVERIFY(app.model()->setEntry(entry).isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); QCoreApplication::processEvents();
        auto* copy = w.card()->findChild<QPushButton*>("copySecretButton"); QVERIFY(copy);
        QCOMPARE(copy->palette().color(QPalette::Button), qApp->palette().color(QPalette::Highlight));
        QVERIFY(w.findChild<QLabel*>("idleStatus"));
        QVERIFY(w.findChild<QLabel*>("idleStatus")->text().contains(QStringLiteral("нет данных")));
        const auto selected = w.list()->currentItem()->data(Qt::UserRole);
        w.card()->revealButton()->click(); QCOMPARE(w.card()->secretText(), entry.token);
        QVERIFY(QMetaObject::invokeMethod(&w, "refreshStatus", Qt::DirectConnection));
        QCOMPARE(w.card()->secretText(), entry.token); QCOMPARE(w.list()->currentItem()->data(Qt::UserRole), selected);
        QTest::qWait(1100); QCOMPARE(w.card()->secretText(), entry.token);
        auto* timer = w.card()->findChild<QTimer*>("secretHideTimer"); QVERIFY(timer); QCOMPARE(timer->interval(), 15000);
        QTRY_COMPARE_WITH_TIMEOUT(w.card()->secretText(), QStringLiteral("••••••••"), 16000);
        auto* idle = new IdleTimer(5000); w.setIdleTimer(idle);
        delete idle; QVERIFY(QMetaObject::invokeMethod(&w, "refreshStatus", Qt::DirectConnection));
        QVERIFY(w.findChild<QLabel*>("idleStatus")->text().contains(QStringLiteral("нет данных")));
        auto* title = w.card()->findChild<QLabel*>("cardTitle"); QVERIFY(title);
        QCOMPARE(title->textFormat(), Qt::PlainText); QCOMPARE(title->text(), entry.title);
        auto* identity = w.card()->findChild<QLabel*>("cardIdentity"); QVERIFY(identity); QVERIFY(identity->text().contains("Project"));
    }
    void statusTickDoesNotExtendHiddenSession() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); QCoreApplication::processEvents();
        IdleTimer idle(200); w.setIdleTimer(&idle);
        connect(&idle, &IdleTimer::idleTimeout, &app, &Application::lock);
        w.hide();
        for (int i = 0; i < 5; ++i) {
            QVERIFY(QMetaObject::invokeMethod(&w, "refreshStatus", Qt::DirectConnection));
            QTest::qWait(20);
        }
        QTRY_COMPARE(app.gate(), Application::Gate::NeedUnlock);
        QVERIFY(w.findChild<QLabel*>("vaultState")->text().contains(QStringLiteral("Заблокировано")));
    }
    void mainGeometryFallbackAndWriteFailure() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        QSettings prefs(dir.filePath("geometry.ini"), QSettings::IniFormat);
        prefs.setValue("legacy", "unchanged");
        prefs.setValue("ui/mainGeometry", QByteArray("broken"));
        prefs.setValue("ui/mainSplitter", QByteArray("broken"));
        {
            ui::MainWindow w(app, nullptr, &prefs); w.show();
            QVERIFY(w.findChild<QSplitter*>("mainSplitter"));
            QVERIFY(!w.findChild<QSplitter*>("mainSplitter")->childrenCollapsible());
            QVERIFY(w.screen()->availableGeometry().intersects(w.frameGeometry()));
            w.move(30000, 30000); w.close();
        }
        {
            ui::MainWindow restored(app, nullptr, &prefs); restored.show();
            QVERIFY(restored.screen()->availableGeometry().contains(restored.frameGeometry().center()));
            restored.resize(650, 550); QCoreApplication::processEvents();
            QVERIFY(restored.findChild<QStackedWidget*>("entryPanel")->isHidden());
            restored.close();
        }
        prefs.sync(); QCOMPARE(prefs.status(), QSettings::NoError);
        QCOMPARE(prefs.value("legacy").toString(), QStringLiteral("unchanged"));
        QCOMPARE(prefs.allKeys(), QStringList({"legacy", "ui/mainGeometry", "ui/mainSplitter"}));
        QSettings denied(dir.path(), QSettings::IniFormat);
        const auto original = app.model()->toCbor();
        ui::MainWindow w(app, nullptr, &denied); w.show(); w.close();
        QVERIFY(w.findChild<QLabel*>("operationError")->text().contains(QStringLiteral("не сохранено")));
        QCOMPARE(app.model()->toCbor(), original);
    }
    void mainGeometryKeepsSecondScreen() {
        if (QGuiApplication::screens().size() < 2) QSKIP("Run with the two-screen offscreen configuration");
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto area = QGuiApplication::screens().at(1)->availableGeometry();
        QWidget previous; previous.resize(800, 600); previous.move(area.topLeft() + QPoint(60, 60)); previous.show();
        QCoreApplication::processEvents();
        QVERIFY(area.contains(previous.frameGeometry().center()));
        uiPreferences_.setValue("ui/mainGeometry", previous.saveGeometry());
        ui::MainWindow restored(app, nullptr, &uiPreferences_); restored.show();
        QCoreApplication::processEvents();
        QVERIFY(area.contains(restored.frameGeometry().center()));
    }
    void mainStatusAndMenuStayCompact() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group; entry.title = "Demo"; entry.token = "synthetic";
        QVERIFY(app.model()->setEntry(entry).isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); QCoreApplication::processEvents();
        QVERIFY(w.findChild<QLabel*>("idleStatus")->parentWidget()->height() <= w.fontMetrics().lineSpacing() * 3 + 16);
        auto* more = w.card()->findChild<QPushButton*>("cardMore"); QVERIFY(more);
        const auto anchor = more->mapToGlobal(QPoint(0, more->height()));
        bool opened = false;
        QTimer::singleShot(0, &w, [&] {
            auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
            if (menu) { opened = true; const auto position = menu->pos(); menu->close(); QCOMPARE(position, anchor); }
        });
        more->click(); QVERIFY(opened);
    }
    void libraryRequiresDestination() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
        EntryEditDialog editor(*app.model(), std::nullopt);
        editor.setGroup({});
        QCOMPARE(editor.findChild<QComboBox*>("entryGroup")->currentIndex(), -1);
        field(editor, "entryTitle")->setText("Test");
        field(editor, "entryToken")->setText("synthetic");
        editor.accept(); QVERIFY(!editor.entry());
        editor.setGroup(group); editor.accept(); QVERIFY(editor.entry());
        QCOMPARE(editor.entry()->groupId, group);
        ui::MainWindow w(app, nullptr, &uiPreferences_);
        QVERIFY(!w.tree()->currentItem());
        auto* all = w.findChild<QAbstractButton*>("libraryAll");
        QVERIFY(all); QVERIFY(all->isChecked());
        w.renameSelectedNode();
        QVERIFY(w.findChild<QLabel*>("operationError")->text().contains(QStringLiteral("Выберите")));
        w.findChild<QLabel*>("operationError")->clear();
        w.deleteSelectedNode();
        QVERIFY(w.findChild<QLabel*>("operationError")->text().contains(QStringLiteral("Выберите")));
        bool asked = false;
        QTimer::singleShot(0, &w, [&] {
            if (auto* dialog = qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget())) {
                asked = true;
                QCOMPARE(dialog->findChild<QComboBox*>("entryGroup")->currentIndex(), -1);
                dialog->reject();
            }
        });
        w.addEntry(); QVERIFY(asked); QVERIFY(app.model()->entries().isEmpty());
    }
    void libraryFiltersAndCounters() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto first = QUuid::createUuid(), second = QUuid::createUuid();
        QVERIFY(app.model()->addNode(first, {}, "Production").isEmpty());
        QVERIFY(app.model()->addNode(second, {}, "Staging").isEmpty());
        for (int i = 0; i < 5; ++i) {
            model::Entry e; e.id = QUuid::createUuid(); e.groupId = i % 2 ? first : second;
            e.title = "Same title"; e.token = "synthetic-secret-marker";
            e.favorite = i % 2; e.reverify = i == 1;
            e.tags = i % 2 ? QStringList{"one", "two"} : QStringList{"one"};
            if (i > 0) e.expiresAt = QDateTime::currentDateTimeUtc().addDays(i == 1 ? -1 : i == 2 ? 0 : i == 3 ? 30 : 31);
            QVERIFY(app.model()->setEntry(e).isEmpty());
        }
        ui::MainWindow w(app, nullptr, &uiPreferences_);
        QCOMPARE(w.list()->count(), 5); QVERIFY(!w.tree()->currentItem());
        auto* favorites = w.findChild<QAbstractButton*>("libraryFavorites");
        auto* expiring = w.findChild<QAbstractButton*>("libraryExpiring");
        auto* all = w.findChild<QAbstractButton*>("libraryAll");
        QVERIFY(favorites && expiring && all);
        QVERIFY(favorites->text().contains("2"));
        favorites->click(); QCOMPARE(w.list()->count(), 2);
        w.search()->setText("nothing"); QCOMPARE(w.list()->count(), 0);
        QVERIFY(favorites->text().contains("2"));
        w.search()->clear(); expiring->click(); QCOMPARE(w.list()->count(), 2);
        auto* expiry = w.findChild<QComboBox*>("expiryFilter");
        QCOMPARE(expiry->currentIndex(), 1);
        expiry->setCurrentIndex(2); QVERIFY(all->isChecked()); QCOMPARE(w.list()->count(), 1);
        favorites->click(); QCOMPARE(expiry->currentIndex(), 0); QCOMPARE(w.list()->count(), 2);
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        QVERIFY(!favorites->isChecked());
        w.search()->setText("Same title"); QCOMPARE(w.list()->count(), 5);
        QVERIFY(w.findChild<QLabel*>("searchScope")->text().contains("всему"));
        for (int i = 0; i < w.list()->count(); ++i) {
            QVERIFY(!w.list()->item(i)->text().contains("synthetic-secret-marker"));
            QVERIFY(w.list()->item(i)->text().contains("API"));
            QVERIFY(w.list()->item(i)->text().contains("Production") || w.list()->item(i)->text().contains("Staging"));
        }
    }
    void librarySelectionFollowsUuid() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        const auto group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(group, {}, "Project").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = group; e.title = "Same"; e.token = "first";
        QVERIFY(app.model()->setEntry(e).isEmpty());
        auto other = e; other.id = QUuid::createUuid(); other.token = "second";
        QVERIFY(app.model()->setEntry(other).isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.list()->setCurrentRow(1);
        const auto selected = w.list()->currentItem()->data(Qt::UserRole).value<model::Id>();
        w.refreshAll(); QCOMPARE(w.list()->currentItem()->data(Qt::UserRole).value<model::Id>(), selected);
        w.card()->revealButton()->click(); QVERIFY(w.card()->secretText() != QStringLiteral("••••••••"));
        QVERIFY(app.model()->removeEntry(selected).isEmpty()); w.refreshAll();
        QVERIFY(w.list()->currentItem()->data(Qt::UserRole).value<model::Id>() != selected);
        QCOMPARE(w.card()->secretText(), QStringLiteral("••••••••"));
        w.search()->setText("missing"); QVERIFY(!w.list()->currentItem());
        QCOMPARE(w.card()->secretText(), QStringLiteral("••••••••"));
    }
    void quickPanelDisambiguatesEntries() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        for (const auto* name : {"Production", "Staging"}) {
            const auto group = QUuid::createUuid();
            QVERIFY(app.model()->addNode(group, {}, name).isEmpty());
            model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
            e.title = "Same title"; e.token = "synthetic";
            QVERIFY(app.model()->setEntry(e).isEmpty());
        }
        ClipboardGuard clipboard; ui::MiniPanel panel(app, clipboard); panel.popup();
        panel.findChild<QLineEdit*>("miniSearch")->setText("Same title");
        auto* list = panel.findChild<QListWidget*>("miniEntries"); QCOMPARE(list->count(), 2);
        QVERIFY(list->item(0)->text() != list->item(1)->text());
        for (int i = 0; i < 2; ++i) QVERIFY(list->item(i)->text().contains(QStringLiteral("Токен")));
        panel.findChild<QLineEdit*>("miniSearch")->setText("missing");
        QVERIFY(!panel.findChild<QLabel*>("miniStatus")->text().isEmpty());
    }
    void quickPanelClosesOnLock() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        ClipboardGuard clipboard; ui::MiniPanel panel(app, clipboard);
        panel.popup(); QVERIFY(panel.isVisible());
        app.lock(); QVERIFY(!panel.isVisible());
        auto* list = panel.findChild<QListWidget*>(); QVERIFY(list);
        QCOMPARE(list->count(), 0);
    }
    void quickPanelKeyboardManualMode() {
        QTemporaryDir dir; Application app(dir.path()); QString error;
        QVERIFY(app.createPasswordVault("test-password-123", &error));
        ClipboardGuard clipboard; ui::MiniPanel panel(app, clipboard);
        panel.popup();
        auto* search = panel.findChild<QLineEdit*>("miniSearch"); QVERIFY(search);
        auto* paste = panel.findChild<QPushButton*>("miniPaste"); QVERIFY(paste);
        QVERIFY(!paste->isEnabled());
        QTest::keyClick(search, Qt::Key_Escape); QVERIFY(!panel.isVisible());
        panel.popup(); QVERIFY(panel.isVisible());
        panel.popup(); QVERIFY(!panel.isVisible());
    }
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("WINDIR").toUtf8() + "/Fonts");
#endif
    }
    void crudThroughModelAndRefresh() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_);
        w.show();
        model::Id p{QUuid::createUuid()};
        QVERIFY(app.model()->addNode(p, model::Id(), "Проект UI").isEmpty());
        w.refreshAll();
        QCOMPARE(w.tree()->topLevelItemCount(), 1);
        QCOMPARE(w.tree()->topLevelItem(0)->text(0), QStringLiteral("Проект UI"));
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = p;
        e.type = model::EntryType::Credentials; e.title = "Логин"; e.login = "u"; e.password = "p";
        QVERIFY(app.model()->setEntry(e).isEmpty());
        w.refreshAll();
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        QCOMPARE(w.list()->count(), 1);
        QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Логин"));
        // поиск
        w.search()->setText("логин");
        QCOMPARE(w.list()->count(), 1);
        w.search()->setText("нет-такой");
        QCOMPARE(w.list()->count(), 0);
        w.search()->clear();
    }
    void allTypesThroughDialogsAndReopen() {
        const auto path = dir_.path() + "/dialog-flow";
        Application app(path); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        answerText(w, QStringLiteral("Проект UI")); w.addProject(); QCOMPARE(w.tree()->topLevelItemCount(), 1);
        answerText(w, QStringLiteral("Группа")); w.addGroup(); QCOMPARE(w.tree()->topLevelItem(0)->childCount(), 1);
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0)->child(0));
        const auto group = w.tree()->currentItem()->data(0, Qt::UserRole).value<model::Id>();
        const QByteArray key("-----BEGIN OPENSSH PRIVATE KEY-----\r\nYWJj\r\n-----END OPENSSH PRIVATE KEY-----\r\n");
        const auto keyPath = dir_.path() + "/test-key";
        QFile keyFile(keyPath); QVERIFY(keyFile.open(QIODevice::WriteOnly)); QCOMPARE(keyFile.write(key), key.size()); keyFile.close();
        for (int type = 0; type < 3; ++type) {
            editDialog(w, [&](EntryEditDialog& dialog) {
                dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(type);
                field(dialog, "entryTitle")->setText(QStringLiteral("Запись %1").arg(type));
                if (type == 0) {
                    field(dialog, "entryToken")->setText(QStringLiteral("TEST_TOKEN_123456\nline2"));
                    dialog.findChild<QCheckBox*>(QStringLiteral("entryFavorite"))->setChecked(true);
                    dialog.findChild<QCheckBox*>(QStringLiteral("entryReverify"))->setChecked(true);
                }
                if (type == 1) { field(dialog, "entryLogin")->setText(QStringLiteral("user@example.test")); field(dialog, "entryPassword")->setText(QStringLiteral("  TEST_PASSWORD_123456  ")); }
                if (type == 2) {
                    field(dialog, "sshPrivatePath")->setText(keyPath); field(dialog, "sshComment")->setText(QStringLiteral("Описание, а не путь"));
                    auto* save = dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save);
                    QVERIFY(dialog.rect().contains(save->mapTo(&dialog, save->rect().bottomRight())));
                    QVERIFY(dialog.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/entry-editor-preview.png")));
                }
            });
            w.addEntry(); QCOMPARE(app.model()->entries().size(), type + 1);
        }
        QCOMPARE(w.list()->count(), 3);
        model::Id ssh;
        for (const auto& entry : app.model()->entries()) {
            QCOMPARE(entry.groupId, group);
            if (entry.type == model::EntryType::ApiToken) QCOMPARE(entry.token, QStringLiteral("TEST_TOKEN_123456\nline2"));
            if (entry.type == model::EntryType::Credentials) QCOMPARE(entry.password, QStringLiteral("  TEST_PASSWORD_123456  "));
            if (entry.type == model::EntryType::SshKey) { ssh = entry.id; QCOMPARE(entry.sshPrivate, key); }
        }
        w.search()->setText(QStringLiteral("Запись 2")); QCOMPARE(w.list()->count(), 1);
        bool emptyPath = false;
        editDialog(w, [&](EntryEditDialog& dialog) {
            emptyPath = field(dialog, "sshPrivatePath")->text().isEmpty(); field(dialog, "entryTitle")->setText(QStringLiteral("Переименованный SSH"));
        });
        w.editSelected(); QVERIFY(emptyPath); QCOMPARE(app.model()->entry(ssh)->sshPrivate, key);
        QCOMPARE(app.model()->entry(ssh)->sshComment, QStringLiteral("Описание, а не путь"));
        w.search()->setText(QStringLiteral("Проект UI")); QCOMPARE(w.list()->count(), 3);
        const auto details = w.findChild<QPlainTextEdit*>(QStringLiteral("entryDetails"))->toPlainText();
        QVERIFY(!details.contains(QStringLiteral("TEST_PASSWORD_123456"))); QVERIFY(!details.contains(QStringLiteral("TEST_TOKEN_123456"))); QVERIFY(!details.contains(QStringLiteral("YWJj")));
        QVERIFY(w.grab().save(QCoreApplication::applicationDirPath() + QStringLiteral("/mainwindow-preview.png")));
        w.resize(650, qMin(600, w.screen()->availableGeometry().height() - 80)); QCoreApplication::processEvents(); QVERIFY(w.findChild<QStackedWidget*>(QStringLiteral("entryPanel"))->isHidden());
        answerText(w, QStringLiteral("Другой проект")); w.addProject();
        const auto other = w.tree()->currentItem()->data(0,Qt::UserRole).value<model::Id>();
        w.search()->setText(QStringLiteral("Запись 1")); QCOMPARE(w.list()->count(), 1);
        const auto credential = w.list()->currentItem()->data(Qt::UserRole).value<model::Id>();
        editDialog(w, [&](EntryEditDialog& dialog) { dialog.setGroup(other); }); w.editSelected();
        QCOMPARE(app.model()->entry(credential)->groupId, other);
        w.requestExit();
        Application reopened(path); QVERIFY(reopened.unlock("пароль-теста-123", &err));
        QCOMPARE(reopened.model()->entries().size(), 3); QCOMPARE(reopened.model()->entry(ssh)->sshPrivate, key);
        QCOMPARE(reopened.model()->entry(credential)->groupId, other);
        ui::MainWindow again(reopened, nullptr, &uiPreferences_); again.show(); again.search()->setText(QStringLiteral("Запись 0"));
        QCOMPARE(again.list()->count(), 1); confirm(again); again.deleteSelected(); QCOMPARE(reopened.model()->entries().size(), 2);
        again.requestExit(); QVERIFY(reopened.unlock("пароль-теста-123", &err)); QCOMPARE(reopened.model()->entries().size(), 2);
    }
    void renameMoveAndDeleteNodes() {
        Application app(dir_.path() + "/nodes"); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        answerText(w, QStringLiteral("А")); w.addProject(); answerText(w, QStringLiteral("Б")); w.addProject();
        QCOMPARE(w.tree()->topLevelItemCount(), 2);
        const auto destination = w.tree()->topLevelItem(1)->data(0,Qt::UserRole).value<model::Id>();
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0)); answerText(w, QStringLiteral("Группа")); w.addGroup();
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0)->child(0)); const auto group = w.tree()->currentItem()->data(0,Qt::UserRole).value<model::Id>();
        answerText(w, QStringLiteral("Новое имя")); w.renameSelectedNode(); QCOMPARE(app.model()->nodes().value(group).name, QStringLiteral("Новое имя"));
        QTimer::singleShot(0, &w, [&] {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) {
                auto* target = dialog->findChild<QComboBox*>(QStringLiteral("moveTarget"));
                if (target) { target->setCurrentIndex(target->findData(destination)); dialog->accept(); } else dialog->reject();
            }
        });
        w.moveSelectedNode(); QCOMPARE(app.model()->nodes().value(group).parent, destination);
        QCOMPARE(w.tree()->currentItem()->data(0,Qt::UserRole).value<model::Id>(), group);
        w.tree()->setCurrentItem(w.tree()->topLevelItem(1)); confirm(w); w.deleteSelectedNode();
        QVERIFY(app.model()->nodes().contains(destination)); QVERIFY(!w.findChild<QLabel*>(QStringLiteral("operationError"))->text().isEmpty());
        w.tree()->setCurrentItem(w.tree()->topLevelItem(1)->child(0)); confirm(w); w.deleteSelectedNode(); QVERIFY(!app.model()->nodes().contains(group));
        w.tree()->setCurrentItem(w.tree()->topLevelItem(0)); confirm(w); w.deleteSelectedNode(); QCOMPARE(w.tree()->topLevelItemCount(), 1);
    }
    void failedSaveRollsBackUiAndModel() {
        Application app(dir_.path() + "/failed-save"); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        QFile main(app.storePathForTest()); QVERIFY(main.open(QIODevice::ReadOnly)); const auto original = main.readAll(); main.close();
        const auto blockedTmp = app.storePathForTest() + ".tmp"; QVERIFY(QDir().mkdir(blockedTmp));
        answerText(w, QStringLiteral("Не сохранится")); w.addProject();
        QVERIFY(app.model()->nodes().isEmpty()); QCOMPARE(w.tree()->topLevelItemCount(), 0);
        QVERIFY(!w.findChild<QLabel*>(QStringLiteral("operationError"))->text().isEmpty());
        QVERIFY(main.open(QIODevice::ReadOnly)); QCOMPARE(main.readAll(), original); main.close();
        QVERIFY(QDir().rmdir(blockedTmp));
        answerText(w, QStringLiteral("Сохранится")); w.addProject(); QCOMPARE(w.tree()->topLevelItemCount(), 1);
        QVERIFY(w.findChild<QLabel*>(QStringLiteral("operationError"))->text().isEmpty());
    }
    void lockDuringSaveDoesNotRestoreSnapshot() {
        Application app(dir_.path() + "/save-lock"); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); bool disabled = false;
        auto connection = connect(&app, &Application::busyChanged, &w, [&](bool busy) {
            if (busy) { disabled = !w.centralWidget()->isEnabled(); app.lock(); }
        });
        answerText(w, QStringLiteral("Отменённый проект")); w.addProject(); disconnect(connection);
        QVERIFY(disabled); QCOMPARE(app.gate(), Application::Gate::NeedUnlock); QVERIFY(app.model()->nodes().isEmpty()); QCOMPARE(w.tree()->topLevelItemCount(), 0);
        QVERIFY(app.unlock("пароль-теста-123", &err)); QVERIFY(app.model()->nodes().isEmpty());
    }
    void lockClosesAndClearsEditor() {
        Application app(dir_.path() + "/editor-lock"); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "Проект").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group; entry.type = model::EntryType::Credentials; entry.title = "Учётная запись"; entry.password = "TEST_PASSWORD_123456";
        QVERIFY(app.model()->setEntry(entry).isEmpty()); QVERIFY(app.save(&err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); bool cleared = false;
        QTimer::singleShot(0, &w, [&] {
            auto* dialog = qobject_cast<EntryEditDialog*>(QApplication::activeModalWidget());
            if (dialog) { auto* password = field(*dialog, "entryPassword"); app.lock(); cleared = password->text().isEmpty() && !dialog->entry(); }
        });
        w.editSelected(); QVERIFY(cleared); QVERIFY(app.model()->entries().isEmpty()); QVERIFY(w.list()->count() == 0);
        QVERIFY(app.unlock("пароль-теста-123", &err)); QCOMPARE(app.model()->entry(entry.id)->password, entry.password);
    }
    void editingKeepsLongMultilineSecrets() {
        model::VaultModel model; const auto group = QUuid::createUuid(); QVERIFY(model.addNode(group, {}, "Проект").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group; entry.title = "Длинный токен";
        entry.token = QStringLiteral("prefix\n") + QString(40000, QChar('x')) + QStringLiteral("\n");
        QVERIFY(model.setEntry(entry).isEmpty());
        EntryEditDialog dialog(model, entry);
        auto* accessible = QAccessible::queryAccessibleInterface(field(dialog, "entryTitle"));
        QVERIFY(accessible); QVERIFY(accessible->text(QAccessible::Name).contains(QStringLiteral("Название")));
        dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
        QVERIFY(dialog.entry()); QCOMPARE(dialog.entry()->token, entry.token);
    }
    void entryTagsPersistThroughEditor() {
        tb::model::VaultModel model;
        auto group = QUuid::createUuid(); QVERIFY(model.addNode(group, {}, "P").isEmpty());
        EntryEditDialog dialog(model, std::nullopt);
        auto* tagsField = dialog.findChild<QLineEdit*>(QStringLiteral("entryTags"));
        QVERIFY(tagsField);
        tagsField->setText(QStringLiteral(" прод , Web , web "));
        field(dialog, "entryTitle")->setText(QStringLiteral("С тегами"));
        dialog.accept();
        const auto saved = dialog.entry(); QVERIFY(saved);
        QCOMPARE(saved->tags, (QStringList{QStringLiteral("прод"), QStringLiteral("Web")}));
        EntryEditDialog reopen(model, *saved);
        auto* reopenedTags = reopen.findChild<QLineEdit*>(QStringLiteral("entryTags"));
        QVERIFY(reopenedTags);
        QCOMPARE(reopenedTags->text(), QStringLiteral("прод, Web"));
    }
    void entryTagsLimitShowsErrorInEditor() {
        tb::model::VaultModel model;
        auto group = QUuid::createUuid(); QVERIFY(model.addNode(group, {}, "P").isEmpty());
        EntryEditDialog dialog(model, std::nullopt);
        QStringList tagList;
        for (int i = 0; i < 17; ++i) tagList << QStringLiteral("тег%1").arg(i);
        dialog.findChild<QLineEdit*>(QStringLiteral("entryTags"))->setText(tagList.join(QLatin1Char(',')));
        field(dialog, "entryTitle")->setText(QStringLiteral("Слишком много тегов"));
        dialog.accept();
        QVERIFY(!dialog.entry());
        QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("entryError"))->text().contains(QStringLiteral("Тегов больше 16.")));
    }
    void sshRejectsUnsupportedOrOversizedFiles() {
        model::VaultModel model; const auto group = QUuid::createUuid(); QVERIFY(model.addNode(group, {}, "Проект").isEmpty());
        const QList<QByteArray> invalid{{}, "PuTTY-User-Key-File-3: ssh-ed25519", "-----BEGIN PUBLIC KEY-----\nabc\n-----END PUBLIC KEY-----", "-----BEGIN PRIVATE KEY-----\nabc\n-----END RSA PRIVATE KEY-----", QByteArray(int(model::VaultModel::MaxSecretBytes) + 1, 'x')};
        for (const auto& bytes : invalid) {
            const auto path = dir_.path() + "/bad-key"; QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
            EntryEditDialog dialog(model, std::nullopt); dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(2);
            field(dialog, "entryTitle")->setText(QStringLiteral("SSH")); field(dialog, "sshPrivatePath")->setText(path);
            dialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Save)->click();
            QVERIFY(!dialog.entry()); QVERIFY(!dialog.findChild<QLabel*>(QStringLiteral("entryError"))->text().isEmpty());
        }
    }
    void reverifyAskedBeforeEditorShowsSecrets() {
        Application app(dir_.path() + "/reverify-edit"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
        e.type = model::EntryType::ApiToken; e.title = "Защищённая"; e.token = "very-secret"; e.reverify = true;
        QVERIFY(app.model()->setEntry(e).isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        QVERIFY(w.list()->count() == 1);
        // Неверный пароль в повторной проверке — редактор не открывается вовсе.
        QString lastAnswer(QStringLiteral("wrong-password-xx"));
        bool editorOpened = false;
        auto* driver = new QTimer(&w);
        QObject::connect(driver, &QTimer::timeout, &w, [&] {
            for (auto* top : QApplication::topLevelWidgets()) {
                if (auto* box = qobject_cast<QMessageBox*>(top); box && box->isVisible()) { box->accept(); continue; } // предупреждение о неверном пароле
                if (auto* input = qobject_cast<QInputDialog*>(top); input && input->isVisible())
                    { input->setTextValue(lastAnswer); input->accept(); continue; }
                if (auto* editor = qobject_cast<EntryEditDialog*>(top); editor && editor->isVisible())
                    { editorOpened = true; editor->reject(); } // fact открытия фиксируем и закрываем exec()
            }
        });
        driver->start(30);
        w.editSelected(); driver->stop();
        QVERIFY(!editorOpened);
        // Верный пароль — редактор открыт.
        lastAnswer = QStringLiteral("valid-password");
        editorOpened = false;
        driver->start(30);
        w.editSelected(); driver->stop();
        QVERIFY(editorOpened);
    }
    void quickPanelShowsWhileMainWindowHidden() {
        // Панель быстрой вставки обязана открываться (Ctrl+Alt+Space/трей), даже когда
        // главное окно скрыто в трей — раньше Qt::Tool прятался вместе с родителем.
        Application app(dir_.path() + "/qp-hidden"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); // окно не показываем вовсе
        w.popupQuickPanel();
        QWidget* panel = nullptr;
        for (auto* top : QApplication::topLevelWidgets())
            if (top->objectName() == QStringLiteral("miniPanel")) panel = top;
        QVERIFY(panel);
        QTRY_VERIFY(panel->isVisible());
        QVERIFY(panel->findChild<QLineEdit*>()); // фокус не проверяем: offscreen не отдаёт его
    }
    void expiryInEditorListAndFilter() {
        Application app(dir_.path() + "/expiry"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        auto make = [&app, group](const QString& title, const QDate& date) {
            model::Entry e; e.id = QUuid::createUuid(); e.groupId = group;
            e.type = model::EntryType::ApiToken; e.title = title; e.token = "t";
            e.expiresAt = QDateTime(date, QTime(0, 0), Qt::UTC);
            QVERIFY(app.model()->setEntry(e).isEmpty());
        };
        make(QStringLiteral("Свежая"), QDate::currentDate().addDays(400));
        make(QStringLiteral("Истекающая"), QDate::currentDate().addDays(10));
        make(QStringLiteral("Просроченная"), QDate::currentDate().addDays(-3));
        QVERIFY(app.save(&error));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        QCOMPARE(w.list()->count(), 3);
        // Редактор: чекбокс и календарная дата переживают круг через запись.
        ::EntryEditDialog dialog(*app.model(), std::nullopt);
        dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(0);
        dialog.findChild<QLineEdit*>(QStringLiteral("entryTitle"))->setText(QStringLiteral("Новая со сроком"));
        auto* expiryBox = dialog.findChild<QCheckBox*>(QStringLiteral("entryExpiry"));
        auto* expiryDate = dialog.findChild<QDateEdit*>(QStringLiteral("entryExpiryDate"));
        QVERIFY(expiryBox && expiryDate);
        QVERIFY(!expiryDate->isEnabled());           // до включения чекбокса дата неактивна
        expiryBox->setChecked(true);
        QVERIFY(expiryDate->isEnabled());
        expiryDate->setDate(QDate(2030, 1, 2));
        dialog.accept();
        QVERIFY(dialog.entry() && dialog.entry()->expiresAt == QDateTime(QDate(2030, 1, 2), QTime(0, 0), Qt::UTC));
        // Пометки в списке и фильтр.
        QStringList titles;
        for (int i = 0; i < w.list()->count(); ++i) titles << w.list()->item(i)->text();
        QVERIFY(titles.join(QChar(u' ')).contains(QStringLiteral("⏳")));
        QVERIFY(titles.join(QChar(u' ')).contains(QStringLiteral("⏰")));
        auto* expiryFilter = w.findChild<QComboBox*>(QStringLiteral("expiryFilter"));
        QVERIFY(expiryFilter);
        expiryFilter->setCurrentIndex(2);           // «Просрочены»
        QCOMPARE(w.list()->count(), 1);
        QVERIFY(w.list()->item(0)->text().contains(QStringLiteral("Просроченная")));
        expiryFilter->setCurrentIndex(1);           // «Истекают (30 дн.)»
        QCOMPARE(w.list()->count(), 1);
        QVERIFY(w.list()->item(0)->text().contains(QStringLiteral("Истекающая")));
        expiryFilter->setCurrentIndex(0);
        QCOMPARE(w.list()->count(), 3);
    }
    void aboutDialogShowsBrandAndStack() {
        ui::AboutDialog dialog;
        auto* logo = dialog.findChild<QLabel*>(QStringLiteral("aboutLogo"));
        auto* version = dialog.findChild<QLabel*>(QStringLiteral("aboutVersion"));
        const QPixmap logoPixmap = logo->pixmap();
        QVERIFY(logo && !logoPixmap.isNull());
        QVERIFY(version && version->text().contains(QStringLiteral("Версия")));
        QStringList parts;
        for (auto* label : dialog.findChildren<QLabel*>()) parts << label->text();
        const QString text = parts.join(QLatin1Char('\n'));
        QVERIFY(text.contains(QStringLiteral("TokenBucket")));
        QVERIFY(text.contains(QStringLiteral("XChaCha20-Poly1305")));
        QVERIFY(text.contains(QStringLiteral("Argon2id")));
        QVERIFY(text.contains(QLatin1String(qVersion())));          // runtime-версия Qt
        QVERIFY(text.contains(QStringLiteral("Роман Бородин")));
        QVERIFY(text.contains(QStringLiteral("workplace-it@rambler.ru")));
        QVERIFY(!tb::ui::brand::icon(16).isNull());                  // логотип в обоих тонах
        QCOMPARE(tb::ui::brand::icon(48, tb::ui::brand::Tone::Locked).width(), 48);
    }
    void passwordGeneratorProducesLongDistinctPasswords() {
        tb::model::VaultModel model; EntryEditDialog dialog(model, std::nullopt);
        dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(1);
        auto* generate = dialog.findChild<QPushButton*>(QStringLiteral("generatePassword"));
        auto* password = dialog.findChild<QLineEdit*>(QStringLiteral("entryPassword"));
        QVERIFY(generate); QVERIFY(password);
        generate->click(); const auto first = password->text();
        generate->click(); const auto second = password->text();
        QCOMPARE(first.size(), 24); QCOMPARE(second.size(), 24); QVERIFY(first != second);
        const QString alphabet = QStringLiteral("ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz23456789#!%+=-");
        for (const QChar ch : first + second)
            QVERIFY2(alphabet.contains(ch), qPrintable(QStringLiteral("недопустимый знак: %1").arg(ch)));
    }
    void entryMutationRollsBackSingleEntry() {
        Application app(dir_.path() + "/rollback-entry"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry victim; victim.id = QUuid::createUuid(); victim.groupId = group;
        victim.type = model::EntryType::ApiToken; victim.title = "Жертва"; victim.token = "tok-original";
        model::Entry bystander; bystander.id = QUuid::createUuid(); bystander.groupId = group;
        bystander.type = model::EntryType::ApiToken; bystander.title = "Свидетель"; bystander.token = "tok-neighbor";
        QVERIFY(app.model()->setEntry(victim).isEmpty()); QVERIFY(app.model()->setEntry(bystander).isEmpty());
        QVERIFY(app.save(&error));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        // Ошибка мутации: точечный откат возвращает запись, сосед не тронут.
        QVERIFY(!w.commitEntry([&](auto& model) {
            auto broken = *model.entry(victim.id);
            broken.token = QString(model::VaultModel::MaxSecretBytes + 1, QChar('x')); // секрет > 1 МиБ
            return model.setEntry(broken);
        }, victim.id));
        QCOMPARE(app.model()->entry(victim.id)->token, QStringLiteral("tok-original"));
        QCOMPARE(app.model()->entry(bystander.id)->token, QStringLiteral("tok-neighbor"));
        // Успешная мутация одной записи сохраняется и обновляет UI.
        QVERIFY(w.commitEntry([&](auto& model) {
            auto changed = *model.entry(victim.id);
            changed.token = QStringLiteral("tok-updated");
            return model.setEntry(changed);
        }, victim.id));
        QCOMPARE(app.model()->entry(victim.id)->token, QStringLiteral("tok-updated"));
        app.lock(); QVERIFY(app.unlock("valid-password", &error));
        QCOMPARE(app.model()->entry(victim.id)->token, QStringLiteral("tok-updated")); // пережила круг
        QCOMPARE(app.model()->entries().size(), 2);
    }
    void typeFilterAndSortModes() {
        Application app(dir_.path() + "/filter"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry older; older.id = QUuid::createUuid(); older.groupId = group;
        older.type = model::EntryType::Credentials; older.title = "Альфа"; older.login = "a"; older.password = "pw1";
        older.created = older.modified = QDateTime::currentDateTimeUtc().addSecs(-100);
        model::Entry newer; newer.id = QUuid::createUuid(); newer.groupId = group;
        newer.type = model::EntryType::ApiToken; newer.title = "Бета"; newer.token = "tok";
        newer.created = newer.modified = QDateTime::currentDateTimeUtc();
        QVERIFY(app.model()->setEntry(older).isEmpty()); QVERIFY(app.model()->setEntry(newer).isEmpty());
        QVERIFY(app.save(&error));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        auto* filter = w.findChild<QComboBox*>(QStringLiteral("typeFilter"));
        auto* sort = w.findChild<QComboBox*>(QStringLiteral("sortMode"));
        QVERIFY(filter); QVERIFY(sort);
        QCOMPARE(w.list()->count(), 2);
        QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Альфа"));          // по названию
        sort->setCurrentIndex(1);                                              // по дате: свежий первым
        QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Бета"));
        filter->setCurrentIndex(2);                                            // только учётные данные
        QCOMPARE(w.list()->count(), 1); QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Альфа"));
        filter->setCurrentIndex(0); sort->setCurrentIndex(0);
        QCOMPARE(w.list()->count(), 2); QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Альфа"));
    }
    void tagBarChipsFilterList() {
        Application app(dir_.path() + "/tags"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry alpha; alpha.id = QUuid::createUuid(); alpha.groupId = group;
        alpha.type = model::EntryType::ApiToken; alpha.title = "Альфа"; alpha.token = "t1"; alpha.tags = {QStringLiteral("Прод")};
        model::Entry beta = alpha; beta.id = QUuid::createUuid(); beta.title = "Бета"; beta.tags = {QStringLiteral("web")};
        model::Entry gamma = alpha; gamma.id = QUuid::createUuid(); gamma.title = "Гамма"; gamma.tags = {QStringLiteral("Прод"), QStringLiteral("web")};
        QVERIFY(app.model()->setEntry(alpha).isEmpty()); QVERIFY(app.model()->setEntry(beta).isEmpty());
        QVERIFY(app.model()->setEntry(gamma).isEmpty());
        QVERIFY(app.save(&error));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show(); w.tree()->setCurrentItem(w.tree()->topLevelItem(0));
        auto* tagBar = w.findChild<QWidget*>(QStringLiteral("tagBar")); QVERIFY(tagBar);
        QCOMPARE(w.list()->count(), 3);
        QToolButton *prod = nullptr, *web = nullptr;
        for (auto* chip : tagBar->findChildren<QToolButton*>()) {
            if (chip->text() == QStringLiteral("Прод")) prod = chip;
            if (chip->text() == QStringLiteral("web")) web = chip;
        }
        QVERIFY(prod); QVERIFY(web);
        prod->click();                                   // один чип: записи с тегом «Прод» (по названию)
        QCOMPARE(w.list()->count(), 2); QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Альфа"));
        web->click();                                    // второй чип: И — только запись с обоими тегами
        QCOMPARE(w.list()->count(), 1); QCOMPARE(w.list()->item(0)->text().section(QLatin1Char('\n'), 0, 0), QStringLiteral("Гамма"));
        auto* filter = w.findChild<QComboBox*>(QStringLiteral("typeFilter"));
        filter->setCurrentIndex(2);                      // «Учётные данные»: все три — API-токены, отсекаются — фильтры работают вместе
        QCOMPARE(w.list()->count(), 0);
        filter->setCurrentIndex(0);
        web->click();                                    // снятие одного чипа из И
        QCOMPARE(w.list()->count(), 2);
        prod->click();                                   // снятие последнего чипа
        QCOMPARE(w.list()->count(), 3);
    }
    void moveNodeToValidatesModelRules() {
        Application app(dir_.path() + "/dnd"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto project = QUuid::createUuid(), first = QUuid::createUuid(), second = QUuid::createUuid();
        QVERIFY(app.model()->addNode(project, {}, "P").isEmpty());
        QVERIFY(app.model()->addNode(first, project, "G1").isEmpty());
        QVERIFY(app.model()->addNode(second, first, "G2").isEmpty());
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        QVERIFY(w.moveNodeTo(first, project));                       // вверх — допустимо
        QCOMPARE(app.model()->nodes().value(first).parent, project);
        QVERIFY(!w.moveNodeTo(project, first));                      // проект нельзя
        QVERIFY(!w.moveNodeTo(first, second));                       // цикл: G2 внутри G1
        QCOMPARE(app.model()->nodes().value(first).parent, project);
        QVERIFY(!w.moveNodeTo(first, {}));                           // пустая цель — тихий откат
        QVERIFY(w.moveNodeTo(second, project));                      // перенос между ветками
        QCOMPARE(app.model()->nodes().value(second).parent, project);
    }
    void cardRequiresExplicitRevealAndHides() {
        Application app(dir_.path() + "/card"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry entry; entry.id = QUuid::createUuid(); entry.groupId = group; entry.title = "Токен"; entry.token = "test-secret";
        QVERIFY(app.model()->setEntry(entry).isEmpty()); QVERIFY(app.save(&error));
        ui::EntryCard card(app); card.showEntry(entry.id); QVERIFY(!card.secretText().contains("test-secret"));
        card.revealButton()->click(); QVERIFY(card.secretText().contains("test-secret"));
        auto* timer = card.findChild<QTimer*>(QStringLiteral("secretHideTimer")); QVERIFY(timer); QCOMPARE(timer->interval(),15000);
        timer->setInterval(1); QTRY_VERIFY(!card.secretText().contains("test-secret"));
        card.revealButton()->click(); QVERIFY(card.secretText().contains("test-secret"));
        app.lock(); QVERIFY(!card.secretText().contains("test-secret")); QVERIFY(!card.revealButton()->isEnabled());
    }
    void cardReverificationIsBoundToSelectionAndGeneration() {
        Application app(dir_.path() + "/card-verify"); QString error; QVERIFY(app.createPasswordVault("valid-password", &error));
        const auto group = QUuid::createUuid(); QVERIFY(app.model()->addNode(group, {}, "P").isEmpty());
        model::Entry first; first.id = QUuid::createUuid(); first.groupId = group; first.title = "Первый"; first.token = "first-secret"; first.reverify = true;
        auto second = first; second.id = QUuid::createUuid(); second.token = "second-secret"; second.reverify = false;
        QVERIFY(app.model()->setEntry(first).isEmpty()); QVERIFY(app.model()->setEntry(second).isEmpty()); QVERIFY(app.save(&error));
        ui::EntryCard card(app); card.showEntry(first.id);
        QTimer::singleShot(0, &card, [] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) dialog->reject(); });
        card.revealButton()->click(); QVERIFY(!card.secretText().contains("first-secret"));
        QTimer::singleShot(0, &card, [] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { dialog->setTextValue(QStringLiteral("valid-password")); dialog->accept(); } });
        card.revealButton()->click(); QVERIFY(card.secretText().contains("first-secret")); QVERIFY(app.save(&error));
        card.hideNowForTest();
        QSignalSpy copies(&card, &ui::EntryCard::copyRequested);
        QTimer::singleShot(0, &card, [&] { if (auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget())) { card.showEntry(second.id); dialog->setTextValue(QStringLiteral("valid-password")); dialog->accept(); } });
        card.findChild<QPushButton*>(QStringLiteral("copySecretButton"))->click(); QCOMPARE(copies.size(), 0);
        card.showEntry(first.id);
        QTimer::singleShot(0, &card, [&] { app.lock(); }); card.revealButton()->click();
        QVERIFY(!card.secretText().contains("first-secret")); QVERIFY(!card.secretText().contains("second-secret"));
    }
    void closeToTrayAndExit() {
        Application app(dir_.path() + "/close-tray"); QString err; QVERIFY(app.createPasswordVault("valid-password", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        w.close();                                                       // крестик: скрыть в трей, без блокировки
        QVERIFY(w.isHidden()); QCOMPARE(app.gate(), Application::Gate::Ready);
        QSignalSpy closing(&w, &ui::MainWindow::closing);
        w.requestExit();                                                 // настоящий выход: блокирует и закрывает
        QCOMPARE(closing.size(), 1); QCOMPARE(app.gate(), Application::Gate::NeedUnlock); QVERIFY(!w.isVisible());
    }
    void idleTimeoutClampAndPersist() {
        Application app(dir_.path() + "/idle"); QString err; QVERIFY(app.createPasswordVault("valid-password", &err));
        ui::MainWindow w(app, nullptr, &uiPreferences_); w.show();
        QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
        const auto previous = settings.value(QStringLiteral("idleTimeoutMinutes"));
        QSignalSpy changed(&w, &ui::MainWindow::idleTimeoutChanged);
        w.applyIdleTimeout(7);
        QCOMPARE(changed.size(), 1); QCOMPARE(changed.takeFirst().at(0).toInt(), 7);
        QCOMPARE(settings.value(QStringLiteral("idleTimeoutMinutes")).toInt(), 7);
        w.applyIdleTimeout(0);                                           // кламп 1–60
        QCOMPARE(changed.takeFirst().at(0).toInt(), 1);
        w.applyIdleTimeout(999);
        QCOMPARE(changed.takeFirst().at(0).toInt(), 60);
        if (previous.isValid()) settings.setValue(QStringLiteral("idleTimeoutMinutes"), previous);
        else settings.remove(QStringLiteral("idleTimeoutMinutes"));
    }
    void themeModesApplyAndLoad() {
        ui::theme::apply(ui::theme::Mode::Dark);
        QVERIFY(qApp->palette().color(QPalette::Highlight).green() > qApp->palette().color(QPalette::Highlight).blue());
        QVERIFY(qApp->palette().color(QPalette::Base).lightness() < 64);   // тёмный Base из рецепта
        ui::theme::apply(ui::theme::Mode::Light);
        QVERIFY(qApp->palette().color(QPalette::Highlight).green() > qApp->palette().color(QPalette::Highlight).blue());
        QVERIFY(qApp->palette().color(QPalette::Base).lightness() > 192);  // светлее тёмного
        QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
        const auto previous = settings.value(QStringLiteral("theme"));
        settings.setValue(QStringLiteral("theme"), QStringLiteral("dark"));
        QCOMPARE(ui::theme::load(), ui::theme::Mode::Dark);
        settings.setValue(QStringLiteral("theme"), QStringLiteral("light"));
        QCOMPARE(ui::theme::load(), ui::theme::Mode::Light);
        settings.setValue(QStringLiteral("theme"), QStringLiteral("мусор"));
        QCOMPARE(ui::theme::load(), ui::theme::Mode::System);
        if (previous.isValid()) settings.setValue(QStringLiteral("theme"), previous);
        else settings.remove(QStringLiteral("theme"));
    }
};
QTEST_MAIN(TestUi)
#include "test_ui.moc"
