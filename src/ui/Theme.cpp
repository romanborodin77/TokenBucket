#include "Theme.h"
#include <QApplication>
#include <QPalette>
#include <QSettings>
#include <QStyle>
#include <QStyleHints>
namespace tb::ui::theme {
Mode load(QSettings* preferences) {
    QSettings settings(QStringLiteral("TokenBucket"), QStringLiteral("TokenBucket"));
    const auto value = (preferences?*preferences:settings).value(QStringLiteral("theme")).toString();
    if (value == QLatin1String("dark")) return Mode::Dark;
    if (value == QLatin1String("light")) return Mode::Light;
    return Mode::System;
}
namespace {
Mode systemMode() {
#ifdef Q_OS_WIN
    // Системная тема приложений живёт в реестре: AppsUseLightTheme == 0 — тёмная.
    // Значение может отсутствовать (старые сборки) — считаем светлой, как Windows по умолчанию.
    QSettings personalization(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalization)",
        QSettings::NativeFormat);
    return personalization.value(QStringLiteral("AppsUseLightTheme"), 1).toInt() == 0 ? Mode::Dark : Mode::Light;
#else
    return Mode::Light;
#endif
}
}
void apply(Mode mode) {
    if (mode == Mode::System) mode = systemMode();
#if defined(Q_OS_WIN) && QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    QApplication::styleHints()->setColorScheme(mode == Mode::Dark ? Qt::ColorScheme::Dark : Qt::ColorScheme::Light);
#endif
    // Fusion + палитры вместо QSS: единый вид контролов в обеих темах и на всех платформах,
    // тема применяется до создания окон и переключается на лету.
    QApplication::setStyle(QStringLiteral("Fusion"));
    if (mode == Mode::Dark) {
        QPalette p;
        const QColor window(0x2b, 0x2b, 0x2b), base(0x1e, 0x1e, 0x1e), text(0xe0, 0xe0, 0xe0), disabled(0x7a, 0x7a, 0x7a);
        p.setColor(QPalette::Window, window);
        p.setColor(QPalette::WindowText, text);
        p.setColor(QPalette::Disabled, QPalette::WindowText, disabled);
        p.setColor(QPalette::Base, base);
        p.setColor(QPalette::AlternateBase, window);
        p.setColor(QPalette::ToolTipBase, text);
        p.setColor(QPalette::ToolTipText, window);
        p.setColor(QPalette::Text, text);
        p.setColor(QPalette::Disabled, QPalette::Text, disabled);
        p.setColor(QPalette::Dark, QColor(0x1a, 0x1a, 0x1a));
        p.setColor(QPalette::Shadow, QColor(0x11, 0x11, 0x11));
        p.setColor(QPalette::Button, window);
        p.setColor(QPalette::ButtonText, text);
        p.setColor(QPalette::Disabled, QPalette::ButtonText, disabled);
        p.setColor(QPalette::BrightText, Qt::red);
        p.setColor(QPalette::Link, QColor(0x6e, 0xa1, 0xe0));
        p.setColor(QPalette::LinkVisited, QColor(0x9a, 0x7a, 0xc8));
        p.setColor(QPalette::Highlight, QColor(0x20, 0x7c, 0x59));
        p.setColor(QPalette::Disabled, QPalette::Highlight, QColor(0x45, 0x45, 0x45));
        p.setColor(QPalette::HighlightedText, Qt::white);
        p.setColor(QPalette::Disabled, QPalette::HighlightedText, disabled);
        // PlaceholderText не наследуется от стандартной палитры стиля: без явной роли
        // подсказки мест ввода в тёмной теме остаются почти невидимыми.
        p.setColor(QPalette::PlaceholderText, QColor(0x8a, 0x8a, 0x8a));
        QApplication::setPalette(p);
    } else {
        auto p = QApplication::style()->standardPalette();
        p.setColor(QPalette::Highlight, QColor(0x20, 0x7c, 0x59));
        p.setColor(QPalette::HighlightedText, Qt::white);
        QApplication::setPalette(p);
    }
}
}
