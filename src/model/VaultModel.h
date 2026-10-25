#pragma once
#include <QHash>
#include <QList>
#include "Types.h"
#include "../vault/CborWriter.h"
namespace tb::model {
// Модель хранилища: дерево узлов (проекты/группы) и записи. Чистые данные без UI;
// инварианты (глубина ≤ 8, ≤ 10 000 записей, секрет ≤ 1 МиБ, нормализованные теги)
// проверяются на входе — повреждённое состояние в модель не попадает. fromCbor
// транзакционен: строит временную модель и коммитит только при полном успехе.
// Сериализация: toCbor() — эталон Qt (тесты), toCborSecure() — рабочий путь записи
// в защищённый буфер без обычной кучи (CborWriter). QHash-порядок недетерминирован,
// файл между сохранениями может отличаться порядком элементов — это норма формата.
class VaultModel {
public:
    struct Node { Id id, parent; QString name; bool operator==(const Node&) const = default; }; // parent.isNull() => проект
    bool operator==(const VaultModel&) const = default;
    static constexpr int MaxEntries = 10000;
    static constexpr qsizetype MaxSecretBytes = 1024 * 1024;
    static constexpr qsizetype MaxPlainBytesCBOR = 64LL * 1024 * 1024;
    static constexpr int MaxDepth = 8;
    static constexpr int ExpiryWarningDays = 30; // горизонт подсветки «скоро истекает»
    static constexpr int MaxTags = 16;      // тегов на запись: верхняя граница UI-чипов и CBOR
    static constexpr int MaxTagChars = 32;  // длина одного тега в символах QString
    static constexpr int CurrentVersion = 5; // версия писателя; старее — требует согласия на обновление
    static constexpr int MaxViews = 50, MaxViewName = 64, MaxViewSearch = 256;

    QString addNode(Id id, Id parent, const QString& name);
    QString renameNode(Id id, const QString& name);
    QString moveNode(Id id, Id newParent);
    QString removeNode(Id id);
    QList<Id> childNodes(Id parent) const;
    const QHash<Id, Node>& nodes() const { return nodes_; }
    int depth(Id node) const;

    QString setEntry(const Entry& e);
    QString removeEntry(Id id);
    const Entry* entry(Id id) const;
    const QHash<Id, Entry>& entries() const { return entries_; }
    QList<Id> entriesInGroup(Id groupId) const;
    QList<Id> entriesInSubtree(Id groupId) const;
    QList<Id> search(const QString& needle) const;

    // Нормализация тегов: trim, пустые отбрасываются, дедупликация без учёта регистра —
    // каноничным считается первое написание. Инвариант всех тегов, хранящихся в модели.
    static QStringList normalizeTags(const QStringList& raw);
    QStringList allTags() const;                                 // уникальные теги хранилища, отсортированы без учёта регистра
    QString renameTag(const QString& from, const QString& to);   // "" — успех, иначе текст ошибки
    QString removeTag(const QString& tag);                       // "" — успех, иначе текст ошибки
    // Представления: setViews транзакционен — при любой ошибке список прежний.
    const QList<SavedView>& views() const { return views_; }
    QString setViews(const QList<SavedView>& views);
    static QString viewError(const SavedView& view);

    QByteArray toCbor() const;      // эталон Qt: используется тестами для золотого сравнения
    cbor::SecureBuffer toCborSecure() const; // рабочий путь записи: секреты не проходят через обычную кучу
    std::size_t encodedSize() const;
    enum class ParseStatus { Ok, Bad, TooLarge };
    ParseStatus fromCbor(const QByteArray& cbor, QString* err);
    int decodedVersion() const { return decodedVersion_; }
    void clear();
private:
    void encodeTo(cbor::CborWriter& w) const; // единая логика обоих проходов toCborSecure
    bool wouldCycle(Id node, Id newParent) const;
    static bool secretTooBig(const Entry& e);
    QString sourceError(const Entry& e) const;
    int decodedVersion_=CurrentVersion;
    QList<SavedView> views_;
    QHash<Id, Node> nodes_;
    QHash<Id, Entry> entries_;
};
}
