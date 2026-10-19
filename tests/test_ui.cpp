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
#include <QTextDocument>
#include <QTimer>
#include <QDir>
#include <QScreen>
#include <QCheckBox>
#include <QAccessible>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/EntryEditDialog.h"
using namespace tb;
class TestUi : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
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
    void initTestCase() {
        qApp->setQuitOnLastWindowClosed(false);
#ifdef Q_OS_WIN
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("WINDIR").toUtf8() + "/Fonts");
#endif
    }
    void crudThroughModelAndRefresh() {
        Application app(dir_.path());
        QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app);
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
        QCOMPARE(w.list()->item(0)->text(), QStringLiteral("Логин"));
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
        ui::MainWindow w(app); w.show();
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
        w.close();
        Application reopened(path); QVERIFY(reopened.unlock("пароль-теста-123", &err));
        QCOMPARE(reopened.model()->entries().size(), 3); QCOMPARE(reopened.model()->entry(ssh)->sshPrivate, key);
        QCOMPARE(reopened.model()->entry(credential)->groupId, other);
        ui::MainWindow again(reopened); again.show(); again.search()->setText(QStringLiteral("Запись 0"));
        QCOMPARE(again.list()->count(), 1); confirm(again); again.deleteSelected(); QCOMPARE(reopened.model()->entries().size(), 2);
        again.close(); QVERIFY(reopened.unlock("пароль-теста-123", &err)); QCOMPARE(reopened.model()->entries().size(), 2);
    }
    void renameMoveAndDeleteNodes() {
        Application app(dir_.path() + "/nodes"); QString err; QVERIFY(app.createPasswordVault("пароль-теста-123", &err));
        ui::MainWindow w(app); w.show();
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
        ui::MainWindow w(app); w.show();
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
        ui::MainWindow w(app); w.show(); bool disabled = false;
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
        ui::MainWindow w(app); w.show(); bool cleared = false;
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
};
QTEST_MAIN(TestUi)
#include "test_ui.moc"
