// Утилита генерации assets/icon.ico из логотипа «Щит-жетон» (src/ui/Brand.cpp).
// Запуск: tools\make-icon.cmd (собирает PNG-кадры и упаковывает их в ICO).
#include <QGuiApplication>
#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QIODevice>
#include "ui/Brand.h"
#include <cstdio>
int main(int argc, char** argv) {
    // QPixmap требует GUI-приложение; offscreen-платформа задаётся сценарием make-icon.cmd.
    QGuiApplication app(argc, argv);
    const QString outDir = app.arguments().value(1, QDir::current().absoluteFilePath("assets"));
    QDir().mkpath(outDir);
    // Кадры ICO: 256 обязательный (PNG-сжатие допускается с Vista), остальные для проводника.
    const QList<int> sizes = {256, 64, 48, 32, 16};
    QList<QByteArray> pngs;
    for (int size : sizes) {
        QBuffer buffer;
        tb::ui::brand::icon(size).toImage().save(&buffer, "PNG");
        pngs << buffer.data();
    }
    quint32 offset = 6 + quint32(16 * sizes.size());
    QByteArray ico;
    ico.append('\x00'); ico.append('\x00'); ico.append('\x01'); ico.append('\x00'); // ICONDIR: тип ICON
    ico.append(char(sizes.size())); ico.append('\x00');
    for (int i = 0; i < sizes.size(); ++i) {
        const int size = sizes[i];
        ico.append(char(size == 256 ? 0 : size));   // ширина (0 = 256)
        ico.append(char(size == 256 ? 0 : size));   // высота
        ico.append('\x00'); ico.append('\x00');      // палитра/зарезервировано
        ico.append('\x01'); ico.append('\x00');      // плоскости
        ico.append('\x20'); ico.append('\x00');      // 32 бита
        const quint32 bytes = quint32(pngs[i].size());
        for (int shift = 0; shift <= 24; shift += 8) ico.append(char((bytes >> shift) & 0xff));
        for (int shift = 0; shift <= 24; shift += 8) ico.append(char((offset >> shift) & 0xff));
        offset += bytes;
    }
    for (const auto& png : pngs) ico.append(png);
    QFile file(outDir + "/icon.ico");
    if (!file.open(QIODevice::WriteOnly)) { std::printf("cannot write icon.ico\n"); return 1; }
    file.write(ico); file.close();
    std::printf("assets/icon.ico written (%zu bytes)\n", size_t(ico.size()));
    return 0;
}
