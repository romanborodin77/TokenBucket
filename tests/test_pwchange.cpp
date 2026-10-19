#include <QtTest>
#include <QApplication>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include "../src/app/Application.h"
#include "../src/ui/MainWindow.h"
#include "../src/ui/ExportDialog.h"
using namespace tb;
namespace {
QWidget* visibleModal() {
    for (auto* widget : QApplication::topLevelWidgets())
        if (widget->isVisible() && widget->isWindow() && widget->windowModality() != Qt::NonModal) return widget;
    return nullptr;
}
}
class TestPwChange : public QObject {
    Q_OBJECT
    QTemporaryDir dir_;
private slots:
    void verifyThenSwitchKeepsVaultOpenable() {
        Application app(dir_.path() + "/seq"); QString err;
        QVERIFY(app.createPasswordVault("старый-пароль-12", &err));
        QVERIFY(app.model()->addNode(QUuid::createUuid(), {}, "P").isEmpty());
        QVERIFY(app.save(&err));
        QVERIFY(app.checkPassword("старый-пароль-12", &err));
        QVERIFY(app.switchToPassword("новый-пароль-34", &err));
        app.lock();
        QVERIFY(app.unlock("новый-пароль-34", &err));
        QCOMPARE(app.model()->nodes().size(), 1);
        app.lock();
        QVERIFY(!app.unlock("старый-пароль-12", &err));
    }
    void changeThroughRealDialogs() {
        auto app = std::make_unique<Application>(dir_.path() + "/ui"); QString err;
        QVERIFY(app->createPasswordVault("старый-пароль-12", &err));
        QVERIFY(app->model()->addNode(QUuid::createUuid(), {}, "P").isEmpty());
        QVERIFY(app->save(&err));
        ui::MainWindow window(*app);
        window.show();
        int ticks = 0;
        auto* driver = new QTimer(&window);
        QObject::connect(driver, &QTimer::timeout, &window, [&] {
            if (++ticks > 400) { // 20 с: закрываем зависший диалог, тест падает с диагностикой
                if (auto* modal = visibleModal()) modal->close();
                return;
            }
            auto* modal = visibleModal();
            if (!modal) return;
            if (auto* input = qobject_cast<QInputDialog*>(modal)) {
                input->setTextValue(QStringLiteral("старый-пароль-12"));
                if (auto* box = input->findChild<QDialogButtonBox*>()) box->button(QDialogButtonBox::Ok)->click();
            } else if (auto* exportDialog = qobject_cast<ui::ExportDialog*>(modal)) {
                if (!exportDialog->findChild<QLineEdit*>(QStringLiteral("exportPassword"))) return;
                exportDialog->findChild<QLineEdit*>(QStringLiteral("exportPassword"))->setText(QStringLiteral("новый-пароль-34"));
                exportDialog->findChild<QLineEdit*>(QStringLiteral("confirmExportPassword"))->setText(QStringLiteral("новый-пароль-34"));
                if (auto* box = exportDialog->findChild<QDialogButtonBox*>()) box->button(QDialogButtonBox::Ok)->click();
                driver->stop();
            }
        });
        driver->start(50);
        window.changeMasterPassword();
        driver->stop();
        app->lock();
        QVERIFY2(app->unlock("новый-пароль-34", &err), qPrintable(QStringLiteral("разблокировка новым паролем: %1").arg(err)));
        QCOMPARE(app->model()->nodes().size(), 1);
        app->lock();
        QVERIFY(!app->unlock("старый-пароль-12", &err));
    }
};
QTEST_MAIN(TestPwChange)
#include "test_pwchange.moc"
