#pragma once
#include <QHash>
#include <QList>
#include "Types.h"
namespace tb::model {
class VaultModel {
public:
    struct Node { Id id, parent; QString name; }; // parent.isNull() => проект
    static constexpr int MaxEntries = 10000;
    static constexpr qsizetype MaxSecretBytes = 1024 * 1024;
    static constexpr qsizetype MaxPlainBytesCBOR = 64LL * 1024 * 1024;
    static constexpr int MaxDepth = 8;

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

    QByteArray toCbor() const;
    enum class ParseStatus { Ok, Bad, TooLarge };
    ParseStatus fromCbor(const QByteArray& cbor, QString* err);
    void clear();
private:
    bool wouldCycle(Id node, Id newParent) const;
    static bool secretTooBig(const Entry& e);
    QHash<Id, Node> nodes_;
    QHash<Id, Entry> entries_;
};
}
