#pragma once
#include <QString>
#include <QStringList>
#include <QVector>
#include "../model/Types.h"
namespace tb::io {
// Результат разбора файла импорта: записи с путём группы из файла и число пропусков.
// Пропуски — нарушения лимитов модели (пустое название, секрет > 1 МиБ, лимит записей
// проверяет уже модель при добавлении); причина последнего пропуска — в lastSkipReason.
struct ImportedEntry {
    model::Entry entry;   // id/created/modified заполняет модель при добавлении
    QStringList groupPath; // путь группы из файла (может быть пуст — корень)
};
struct ImportResult {
    QVector<ImportedEntry> entries;
    int skipped = 0;
    QString lastSkipReason;
};

// Универсальный CSV: первая строка — заголовки; колонки распознаются по имени
// (регистронезависимо, RU/EN): title/nазвание/запись, username/login/логин,
// password/пароль, url/адрес, notes/заметки, group/группа/папка. Разделитель , или ;
// определяется по заголовку; кавычки и запятые внутри кавычек поддержаны; UTF-8 BOM снят.
// Учётные данные без логина становятся API-токенами (тип Credentials при наличии логина).
ImportResult parseCsv(const QByteArray& bytes);

// KeePass 2.x XML (незашифрованный экспорт): рекурсивный обход Group/Entry,
// строки Entry (Key/Value → login/password/url/notes), пароль фраза — sshph при наличии.
ImportResult parseKeePassXml(const QByteArray& bytes);
}
