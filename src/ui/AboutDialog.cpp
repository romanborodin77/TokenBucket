#include "AboutDialog.h"
#include "Brand.h"
#include "Version.h"
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSysInfo>
#include <QUrl>
#include <QVBoxLayout>
#include <sodium.h>
namespace {
QString shortVersion() {
    // Счётчик версий живёт в CMake (PROJECT_VERSION) и приходит через Version.h;
    // нулевой патч не показываем: 1.0.0 → «1.0», 1.0.2 → «1.0.2».
    QStringList parts = QString::fromLatin1(TOKENBUCKET_VERSION).split(QLatin1Char('.'));
    if (parts.size() == 3 && parts.last() == QLatin1String("0")) parts.removeLast();
    return parts.join(QLatin1Char('.'));
}
QString compilerLine() {
#ifdef _MSC_VER
    return QStringLiteral("MSVC %1.%2").arg(_MSC_VER / 100).arg(_MSC_VER % 100);
#elif defined(__GNUC__) && defined(__clang__)
    return QStringLiteral("Clang %1").arg(__clang_major__);
#else
    return QStringLiteral("GCC");
#endif
}
QLabel* keyValue(const QString& key, const QString& value, QWidget* parent, bool linkEmail = false) {
    auto* row = new QLabel(parent);
    row->setTextFormat(linkEmail ? Qt::RichText : Qt::PlainText);
    if (linkEmail)
        row->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;<a href=\"mailto:%2\" style=\"color:palette(link);text-decoration:none\">%2</a>")
                         .arg(key.toHtmlEscaped(), value.toHtmlEscaped()));
    else
        row->setText(QStringLiteral("<b>%1</b>&nbsp;&nbsp;%2").arg(key.toHtmlEscaped(), value.toHtmlEscaped()));
    row->setOpenExternalLinks(linkEmail);
    return row;
}
}
namespace tb::ui {
AboutDialog::AboutDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("О программе — TokenBucket"));
    setObjectName(QStringLiteral("aboutDialog"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    // Шапка макета «C Акцентный»: тёмный градиент, лого «Щит-жетон», версия-бейдж.
    auto* band = new QFrame(this);
    band->setObjectName(QStringLiteral("aboutBand"));
    band->setStyleSheet(QStringLiteral(
        "QFrame#aboutBand{background:qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #182437,"
        "stop:0.55 #12326b,stop:1 #0e5a46);} QLabel{color:#eaf2ee;}"));
    auto* bandLayout = new QVBoxLayout(band);
    bandLayout->setContentsMargins(28, 26, 28, 22);
    bandLayout->setSpacing(4);
    auto* logo = new QLabel(band);
    logo->setObjectName(QStringLiteral("aboutLogo"));
    logo->setPixmap(brand::logo(96));
    logo->setAlignment(Qt::AlignHCenter);
    auto* name = new QLabel(QStringLiteral("TokenBucket"), band);
    name->setAlignment(Qt::AlignHCenter);
    name->setStyleSheet(QStringLiteral("font-size:24px;font-weight:600;"));
    auto* slogan = new QLabel(QStringLiteral("защищённое хранилище токенов и ключей"), band);
    slogan->setAlignment(Qt::AlignHCenter);
    slogan->setStyleSheet(QStringLiteral("font-size:12px;color:#b9c8d8;"));
    versionChip_ = new QLabel(QStringLiteral("Версия %1").arg(shortVersion()), band);
    versionChip_->setObjectName(QStringLiteral("aboutVersion"));
    versionChip_->setAlignment(Qt::AlignHCenter);
    versionChip_->setStyleSheet(QStringLiteral(
        "font-size:11px;font-weight:600;color:#eafbf2;background:rgba(255,255,255,0.16);"
        "border-radius:10px;padding:3px 14px;"));
    bandLayout->addWidget(logo);
    bandLayout->addWidget(name);
    bandLayout->addWidget(slogan);
    bandLayout->addWidget(versionChip_, 0, Qt::AlignHCenter);
    layout->addWidget(band);
    // Детальный техстек:runtime-версии Qt/libsodium честно берём из библиотек,
    // компилятор и платформу — из сборки и системы.
    auto* body = new QWidget(this);
    auto* bodyLayout = new QVBoxLayout(body);
    bodyLayout->setContentsMargins(28, 18, 28, 10);
    auto* intro = new QLabel(tr("Локальное хранилище секретов: всё шифрование выполняется "
                                "на вашем компьютере, сеть не используется."), body);
    intro->setWordWrap(true);
    bodyLayout->addWidget(intro);
    const QString qtLicense = QStringLiteral("Qt %1 (LGPL-3.0)").arg(QLatin1String(qVersion()));
    const QString sodium = QStringLiteral("libsodium %1 (ISC)").arg(QLatin1String(sodium_version_string()));
    bodyLayout->addSpacing(10);
    bodyLayout->addWidget(keyValue(tr("Контейнер"), tr("TBVK v1 · мастер-пароль / Windows Hello (DPAPI)"), body));
    bodyLayout->addWidget(keyValue(tr("Шифрование"), tr("XChaCha20-Poly1305 · ключ Argon2id (64 МиБ, opslimit 3)"), body));
    bodyLayout->addWidget(keyValue(tr("Формат данных"), tr("CBOR v2 (теги) · до 10 000 записей · секрет до 1 МиБ"), body));
    bodyLayout->addWidget(keyValue(tr("Интерфейс"), qtLicense, body));
    bodyLayout->addWidget(keyValue(tr("Криптография"), sodium, body));
    bodyLayout->addWidget(keyValue(tr("Сборка"), QStringLiteral("%1 · %2")
        .arg(compilerLine(), QSysInfo::prettyProductName()), body));
    bodyLayout->addWidget(keyValue(tr("Автор"), QStringLiteral("Роман Бородин · workplace-it@rambler.ru"), body, true));
    layout->addWidget(body, 1);
    auto* actions = new QDialogButtonBox(QDialogButtonBox::Close | QDialogButtonBox::Help, this);
    actions->button(QDialogButtonBox::Close)->setText(tr("Закрыть"));
    actions->button(QDialogButtonBox::Help)->setText(tr("Открыть лицензии…"));
    auto* actionsRow = new QWidget(this);
    auto* actionsLayout = new QHBoxLayout(actionsRow);
    actionsLayout->setContentsMargins(28, 4, 28, 16);
    actionsLayout->addStretch(1);
    actionsLayout->addWidget(actions);
    layout->addWidget(actionsRow);
    connect(actions, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(actions->button(QDialogButtonBox::Close), &QPushButton::clicked, this, &QDialog::reject);
    connect(actions->button(QDialogButtonBox::Help), &QPushButton::clicked, this, &AboutDialog::openLicenses);
    setFixedWidth(520);
}
void AboutDialog::openLicenses() {
    // В установленном пакете лицензии лежат рядом с программой; в дереве разработки — в docs.
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString& candidate : {appDir + QStringLiteral("/licenses"),
                                     appDir + QStringLiteral("/../../docs/licenses")}) {
        const QString path = QFileInfo(candidate).absoluteFilePath();
        if (QFileInfo::exists(path)) { QDesktopServices::openUrl(QUrl::fromLocalFile(path)); return; }
    }
    QMessageBox::information(this, tr("TokenBucket"),
        tr("Каталог лицензий найдётся в поставке пакета (licenses) или в docs/licenses дерева исходников."));
}
}
