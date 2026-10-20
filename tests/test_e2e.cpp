#include <QtTest>
#include <QApplication>
#include <QClipboard>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QListWidget>
#include <QLineEdit>
#include <QMessageBox>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QRadioButton>
#include <QTemporaryDir>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QToolButton>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/SetupDialog.h"
#include "../src/ui/UnlockDialog.h"
#include "../src/ui/EntryEditDialog.h"
#include "../src/ui/EntryCard.h"
#include "../src/ui/TrayController.h"
#include "../src/platform/ClipboardGuard.h"
#include "../src/platform/QuickPaste.h"
#ifdef Q_OS_WIN
#include <windows.h>
#endif
using namespace tb;

// E2E-путешествия: реальные диалоги, реальная модель/файлы/буфер, без моков.
// Системные файловые диалоги (QFileDialog) автотестом не управляемы — они в ручной матрице приёмки.
namespace {
void typeInto(::EntryEditDialog& dialog, int type, const QString& title, std::function<void(::EntryEditDialog&)> extra = {}) {
    dialog.findChild<QComboBox*>(QStringLiteral("entryType"))->setCurrentIndex(type);
    dialog.findChild<QLineEdit*>(QStringLiteral("entryTitle"))->setText(title);
    if (extra) extra(dialog);
    dialog.accept();
}
}

class TestE2E : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void initTestCase() {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_FONTDIR", qEnvironmentVariable("WINDIR").toUtf8() + "/Fonts");
#endif
    }

    // Полный день жизни пользователя: создание хранилища через SetupDialog, проекты/группы,
    // все три типа записей через редактор (генератор пароля, теги, SSH из файла), поиск,
    // чип тега + тип-фильтр, избранное, копирование с очисткой буфера, повторная проверка
    // при показе и редактировании, блокировка и разблокировка через UnlockDialog.
    void fullVaultLifecycleJourney() {
        // 1. Экран создания: мастер-пароль дважды.
        SetupDialog setup(false);
        setup.findChild<QLineEdit*>(QStringLiteral("masterPassword"))->setText(QStringLiteral("мастер-пароль-e2e"));
        setup.findChild<QLineEdit*>(QStringLiteral("confirmPassword"))->setText(QStringLiteral("мастер-пароль-e2e"));
        setup.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok)->click();
        const QByteArray masterPassword = setup.masterPassword();
        QVERIFY(Application::validPassword(masterPassword));

        Application app(dir_.path() + "/life");
        QString error;
        QVERIFY(app.createPasswordVault(masterPassword, &error));

        // 2. Структура: проект → группа (модель уровня UI-действий).
        const auto project = QUuid::createUuid(), group = QUuid::createUuid();
        QVERIFY(app.model()->addNode(project, {}, "Прод").isEmpty());
        QVERIFY(app.model()->addNode(group, project, "Сервисы").isEmpty());

        // 3. Три записи через РЕАЛЬНЫЙ редактор: токен с тегами, креды с генератором, SSH из файла.
        ui::MainWindow w(app); w.show();
        {
            ::EntryEditDialog tokenDialog(*app.model(), std::nullopt, &w);
            tokenDialog.setGroup(group);
            typeInto(tokenDialog, 0, QStringLiteral("Токен CI"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("entryToken"))->setText(QStringLiteral("ci-token-abc-123"));
                d.findChild<QLineEdit*>(QStringLiteral("entryTags"))->setText(QStringLiteral(" прод , CI , ci "));
                d.findChild<QCheckBox*>(QStringLiteral("entryReverify"))->setChecked(true);
            });
            QVERIFY(tokenDialog.entry());
            QVERIFY(app.model()->setEntry(*tokenDialog.entry()).isEmpty());
        }
        QString generated;
        {
            ::EntryEditDialog credDialog(*app.model(), std::nullopt, &w);
            credDialog.setGroup(group);
            typeInto(credDialog, 1, QStringLiteral("Логин портала"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("entryLogin"))->setText(QStringLiteral("roman@example.com"));
                d.findChild<QPushButton*>(QStringLiteral("generatePassword"))->click();
                generated = d.findChild<QLineEdit*>(QStringLiteral("entryPassword"))->text();
            });
            QVERIFY(credDialog.entry());
            QVERIFY(app.model()->setEntry(*credDialog.entry()).isEmpty());
        }
        QCOMPARE(generated.size(), 24); // генератор сработал внутри реального редактора
        QString sshPath;
        {
            const QByteArray key = "-----BEGIN OPENSSH PRIVATE KEY-----\nabc\r\n-----END OPENSSH PRIVATE KEY-----\r\n";
            sshPath = dir_.filePath("id_e2e");
            QFile keyFile(sshPath); QVERIFY(keyFile.open(QIODevice::WriteOnly)); keyFile.write(key); keyFile.close();
            ::EntryEditDialog sshDialog(*app.model(), std::nullopt, &w);
            sshDialog.setGroup(group);
            typeInto(sshDialog, 2, QStringLiteral("Ключ сервера"), [&](auto& d) {
                d.findChild<QLineEdit*>(QStringLiteral("sshPrivatePath"))->setText(sshPath);
                d.findChild<QCheckBox*>(QStringLiteral("entryFavorite"))->setChecked(true);
            });
            QVERIFY(sshDialog.entry());
            QVERIFY(app.model()->setEntry(*sshDialog.entry()).isEmpty());
        }
        QVERIFY(app.save(&error));
        w.refreshAll();
        // записи лежат в группе «Сервисы» — выбираем её в дереве (после refreshAll выделен проект).
        for (QTreeWidgetItemIterator it(w.tree()); *it; ++it)
            if ((*it)->text(0) == QStringLiteral("Сервисы")) { w.tree()->setCurrentItem(*it); break; }
        QCOMPARE(w.list()->count(), 3);

        // 4. Поиск находит каждую запись; чип тега «прод» фильтрует вместе с тип-фильтром.
        w.search()->setText(QStringLiteral("портала")); QCOMPARE(w.list()->count(), 1);
        w.search()->setText(QStringLiteral("сервисы"));  QCOMPARE(w.list()->count(), 3); // имя группы находит поддерево
        w.search()->clear();
        auto* tagBar = w.findChild<QWidget*>(QStringLiteral("tagBar"));
        QToolButton* chip = nullptr;
        for (auto* button : tagBar->findChildren<QToolButton*>())
            if (button->text() == QStringLiteral("прод")) chip = button;
        QStringList chipTexts;
        for (auto* button : tagBar->findChildren<QToolButton*>()) chipTexts << button->text();
        const QString firstChip = chipTexts.isEmpty() ? QString() : chipTexts.first();
        QVERIFY(chip);
        chip->click(); QCOMPARE(w.list()->count(), 1); // только токен с тегом
        auto* typeFilter = w.findChild<QComboBox*>(QStringLiteral("typeFilter"));
        typeFilter->setCurrentIndex(1); QCOMPARE(w.list()->count(), 1); // «API-токены» — совпадает
        typeFilter->setCurrentIndex(3); QCOMPARE(w.list()->count(), 0); // «SSH-ключи» — нет
        typeFilter->setCurrentIndex(0); chip->click();

        // 5. Карточка: показ секрета по кнопке (15 с), копирование и очистка буфера.
        const auto tokenId = [app = &app] {
            for (auto id : app->model()->entries().keys())
                if (app->model()->entry(id)->title == QStringLiteral("Токен CI")) return id;
            return model::Id{};
        }();
        w.list()->setCurrentRow(0);
        // выбрать именно токен: через поиск
        w.search()->setText(QStringLiteral("токен ci")); QCOMPARE(w.list()->count(), 1);
        QVERIFY(!w.card()->secretText().contains(QStringLiteral("ci-token-abc-123"))); // до показа скрыт
        {   // показ секрета у reverify-записи просит подтверждение — отвечает реальный драйвер
            QString verifyAnswer = QString::fromUtf8(masterPassword);
            QTimer verifyDriver; verifyDriver.setInterval(20);
            connect(&verifyDriver, &QTimer::timeout, &w, [&] {
                for (auto* top : QApplication::topLevelWidgets())
                    if (auto* input = qobject_cast<QInputDialog*>(top); input && input->isVisible())
                        { input->setTextValue(verifyAnswer); input->accept(); }
            });
            verifyDriver.start();
            w.card()->revealButton()->click();
            verifyDriver.stop();
        }
        QVERIFY(w.card()->secretText().contains(QStringLiteral("ci-token-abc-123")));
        w.card()->hideNowForTest();
        ClipboardGuard guard;
        QVERIFY(guard.copySecret(QStringLiteral("e2e-secret-42"), &error));
        QCOMPARE(QApplication::clipboard()->text(), QStringLiteral("e2e-secret-42"));
        guard.triggerClearNowForTest();
        QVERIFY(QApplication::clipboard()->text().isEmpty()); // свой секрет очищен

        // 6. Reverify-запись: верный пароль в реальном диалоге проверки открывает редактор
        // (отрицательная ветка «неверный пароль — редактор не открывается» покрыта
        // отдельным тестом reverifyAskedBeforeEditorShowsSecrets в test_ui).
        QString answer(QStringLiteral("wrong-password-xx"));
        bool editorSeen = false;
        QTimer driver; driver.setInterval(20);
        connect(&driver, &QTimer::timeout, &w, [&] {
            for (auto* top : QApplication::topLevelWidgets()) {
                if (auto* input = qobject_cast<QInputDialog*>(top); input && input->isVisible())
                    { input->setTextValue(answer); input->accept(); continue; }
                if (auto* editor = qobject_cast<::EntryEditDialog*>(top); editor && editor->isVisible()) {
                    editorSeen = true;
                    if (auto* box = editor->findChild<QDialogButtonBox*>())
                        box->button(QDialogButtonBox::Cancel)->click(); // штатная отмена, как в прочих тестах
                }
            }
        });
        answer = QString::fromUtf8(masterPassword);
        driver.start();
        w.editSelected(); // блокируется в exec() редактора, драйвер фиксирует и закрывает
        driver.stop();
        QVERIFY(editorSeen);
        // 7. Блокировка из UI и разблокировка через РЕАЛЬНЫЙ UnlockDialog.
        QVERIFY(app.save(&error));
        w.close(); // сворачивание в трей не блокирует (окно скрыто, но приложение живёт)
        QVERIFY(w.isHidden());
        app.lock();
        QCOMPARE(app.gate(), Application::Gate::NeedUnlock);
        ::UnlockDialog unlock(::UnlockDialog::Kind::Password);
        unlock.findChild<QLineEdit*>(QStringLiteral("unlockPassword"))->setText(QString::fromUtf8(masterPassword));
        unlock.accept();
        QVERIFY(app.unlock(unlock.password(), &error));
        QCOMPARE(app.gate(), Application::Gate::Ready);
        QCOMPARE(app.model()->entries().size(), 3); // все записи на месте
        const auto* finalToken = app.model()->entry(tokenId);
        QVERIFY2(finalToken, "запись токена не найдена после разблокировки");
        QVERIFY2(finalToken->tags.contains(QStringLiteral("прод")),
                 qPrintable(QStringLiteral("теги после круга: %1").arg(finalToken->tags.join(QLatin1Char(',')))));
    }

    // Миграция «на другую машину»: экспорт в файл, новое приложение в другом каталоге
    // импортирует контейнер с новым мастер-паролем; теги и SSH-байты переживают переезд.
    void exportImportMigrationJourney() {
        Application source(dir_.path() + "/machine-a"); QString error;
        QVERIFY(source.createPasswordVault("пароль-машины-а", &error));
        const auto project = QUuid::createUuid();
        QVERIFY(source.model()->addNode(project, {}, "Проект А").isEmpty());
        model::Entry token; token.id = QUuid::createUuid(); token.groupId = project;
        token.type = model::EntryType::ApiToken; token.title = "Токен переезда";
        token.token = "mig-token"; token.tags = {QStringLiteral("переезд")};
        QVERIFY(source.model()->setEntry(token).isEmpty());
        model::Entry ssh = token; ssh.id = QUuid::createUuid();
        ssh.type = model::EntryType::SshKey; ssh.title = "Ключ переезда"; ssh.token.clear();
        ssh.tags.clear(); ssh.sshPrivate = QByteArray("-----BEGIN OPENSSH PRIVATE KEY-----\r\nxyz\r\n-----END-----\r\n");
        QVERIFY(source.model()->setEntry(ssh).isEmpty());
        QVERIFY(source.save(&error));

        const QByteArray exported = source.exportContainer("пароль-переноса-9", &error);
        QVERIFY(!exported.isEmpty());
        const QString exportPath = dir_.filePath("transfer.tb");
        QFile file(exportPath); QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(exported), qint64(exported.size())); file.close();

        Application target(dir_.path() + "/machine-b");
        QVERIFY(target.importContainer(exported, "пароль-переноса-9", "пароль-машины-б", &error));
        QCOMPARE(target.model()->entries().size(), 2);
        QVERIFY(target.model()->allTags().contains(QStringLiteral("переезд")));
        target.lock();
        QVERIFY(target.unlock("пароль-машины-б", &error));
        const auto* moved = target.model()->entry(ssh.id);
        QVERIFY(moved && moved->sshPrivate.contains("xyz\r\n")); // байты ключа без изменений
    }

#ifdef Q_OS_WIN
    // Трей: индикатор и подсказка следят за состоянием хранилища; уведомление не падает.
    void trayStatesJourney() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) QSKIP("нативная платформа");
        Application app(dir_.path() + "/tray"); QString error;
        QVERIFY(app.createPasswordVault("tray-password-1", &error));
        ui::TrayController tray(app);
        QVERIFY(!tray.toolTip().isEmpty());
        QVERIFY(tray.toolTip().contains(QStringLiteral("разблокировано")));
        app.lock();
        QVERIFY(tray.toolTip().contains(QStringLiteral("заблокировано")));
        tray.notify(QStringLiteral("проверочное уведомление")); // не выбрасывает секреты — только текст
    }

    // Настоящая вставка: секрет копируется в реальный буфер и Ctrl+V отправляется в живое окно.
    void pasteIntoRealWindowJourney() {
        if (QGuiApplication::platformName() != QLatin1String("windows")) QSKIP("нативная платформа");
        ClipboardGuard guard;
        QVERIFY(guard.copySecret(QStringLiteral("live-paste-777"), nullptr));
        QLineEdit target; target.resize(240, 32); target.show(); target.raise(); target.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&target));
        QTest::qWait(150);
        const bool focused = GetForegroundWindow() == reinterpret_cast<HWND>(target.winId());
        if (!focused) QSKIP("окно не получило фокус в этой среде (посторонний topmost)");
        paste::sendCtrlV();
        QTRY_COMPARE(target.text(), QStringLiteral("live-paste-777")); // SendInput дошёл до поля
        guard.triggerClearNowForTest();
    }
#endif
};
QTEST_MAIN(TestE2E)
#include "test_e2e.moc"
