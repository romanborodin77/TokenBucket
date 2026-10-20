#include "VaultModel.h"
#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCborStreamReader>
#include <QSet>
#include <functional>
#include <algorithm>
using namespace Qt::StringLiterals;
namespace tb::model {
namespace {
QString iso(const QDateTime& t) { return t.toUTC().toString(Qt::ISODateWithMs); }
QDateTime fromIso(const QCborValue& v) { return QDateTime::fromString(v.toString(), Qt::ISODateWithMs); }
Id idFrom(const QCborValue& v) { return QUuid::fromString(v.toString()); }
QString idStr(const Id& id) { return id.toString(QUuid::WithoutBraces); }
}
QString VaultModel::addNode(Id id, Id parent, const QString& name) {
    if (id.isNull()) return "Пустой идентификатор.";
    if (name.isEmpty()) return "Имя не может быть пустым.";
    if (nodes_.contains(id) || entries_.contains(id)) return "Такой идентификатор уже есть.";
    if (!parent.isNull()) {
        if (!nodes_.contains(parent)) return "Родитель не найден.";
        if (depth(parent) >= MaxDepth) return "Превышена вложенность 8 уровней.";
    }
    nodes_.insert(id, {id, parent, name});
    return {};
}
QString VaultModel::renameNode(Id id, const QString& name) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return "Узел не найден.";
    if (name.isEmpty()) return "Имя не может быть пустым.";
    it->name = name;
    return {};
}
int VaultModel::depth(Id node) const {
    if (!nodes_.contains(node)) return 0;
    int d = 1;
    Id p = nodes_.value(node).parent;
    while (!p.isNull()) { ++d; p = nodes_.value(p).parent; }
    return d;
}
bool VaultModel::wouldCycle(Id node, Id newParent) const {
    Id p = newParent;
    while (!p.isNull()) { if (p == node) return true; p = nodes_.value(p).parent; }
    return false;
}
QString VaultModel::moveNode(Id id, Id newParent) {
    auto it = nodes_.find(id);
    if (it == nodes_.end()) return "Узел не найден.";
    if (it->parent.isNull()) return "Проект нельзя переместить.";
    if (!nodes_.contains(newParent)) return "Новый родитель не найден.";
    if (wouldCycle(id, newParent)) return "Перенос создал бы цикл.";
    if (id == newParent) return "Нельзя переместить в себя.";
    int subtree = 1;
    std::function<int(Id)> h = [&](Id root) { int m = 0; for (auto&n : nodes_) if (n.parent == root) m = std::max(m, 1 + h(n.id)); return m; };
    subtree = 1 + h(id);
    if (depth(newParent) + subtree > MaxDepth) return "Превышена вложенность 8 уровней.";
    it->parent = newParent;
    return {};
}
QString VaultModel::removeNode(Id id) {
    if (!nodes_.contains(id)) return "Узел не найден.";
    for (auto& n : nodes_) if (n.parent == id) return "Сначала удалите вложенные группы.";
    if (!entriesInGroup(id).isEmpty()) return "Сначала удалите записи из группы.";
    nodes_.remove(id);
    return {};
}
QList<Id> VaultModel::childNodes(Id parent) const {
    QList<Id> r;
    for (auto& n : nodes_) if (n.parent == parent) r << n.id;
    std::sort(r.begin(), r.end(), [&](Id a, Id b){ return nodes_[a].name < nodes_[b].name; });
    return r;
}
bool VaultModel::secretTooBig(const Entry& e) {
    return e.token.toUtf8().size() > MaxSecretBytes || e.password.toUtf8().size() > MaxSecretBytes
        || e.sshPrivate.size() > MaxSecretBytes || e.sshPublic.size() > MaxSecretBytes
        || e.sshPassphrase.toUtf8().size() > MaxSecretBytes;
}
QStringList VaultModel::normalizeTags(const QStringList& raw) {
    QStringList result;
    for (const QString& rawTag : raw) {
        const QString tag = rawTag.trimmed();
        if (tag.isEmpty()) continue;
        // Дубликаты отбрасываем без учёта регистра: «Прод» и «прод» — один тег,
        // написание первого вхождения считается каноничным.
        if (std::any_of(result.cbegin(), result.cend(), [&](const QString& existing) {
                return existing.compare(tag, Qt::CaseInsensitive) == 0; }))
            continue;
        result << tag;
    }
    return result;
}
QString VaultModel::setEntry(const Entry& e) {
    if (e.id.isNull() || nodes_.contains(e.id)) return "Недопустимый идентификатор записи.";
    if (int(e.type) < 0 || int(e.type) > 2) return "Неизвестный тип записи.";
    if (!nodes_.contains(e.groupId)) return "Проект или группа не найдены.";
    if (!entries_.contains(e.id) && entries_.size() >= MaxEntries) return "Достигнут предел 10 000 записей.";
    if (secretTooBig(e)) return "Секрет превышает 1 МиБ.";
    // Инвариант: в модели теги всегда нормализованы — редактор и CBOR-слой
    // могут полагаться на отсутствие пустых тегов и регистровых дублей.
    const QStringList tags = normalizeTags(e.tags);
    if (tags.size() > MaxTags) return "Тегов больше 16.";
    if (std::any_of(tags.cbegin(), tags.cend(), [](const QString& tag) { return tag.size() > MaxTagChars; }))
        return "Тег длиннее 32 символов.";
    Entry stored = e; // параметр константен: нормализованные теги кладём в копию
    stored.tags = tags;
    entries_.insert(stored.id, stored);
    return {};
}
QString VaultModel::removeEntry(Id id) { return entries_.remove(id) ? QString() : "Запись не найдена."; }
const Entry* VaultModel::entry(Id id) const {
    auto it = entries_.constFind(id);
    return it == entries_.cend() ? nullptr : &it.value();
}
QList<Id> VaultModel::entriesInGroup(Id groupId) const {
    QList<Id> r;
    for (auto& e : entries_) if (e.groupId == groupId) r << e.id;
    return r;
}
QList<Id> VaultModel::entriesInSubtree(Id groupId) const {
    QList<Id> r = entriesInGroup(groupId);
    for (auto& n : nodes_) if (n.parent == groupId) r << entriesInSubtree(n.id);
    return r;
}
QList<Id> VaultModel::search(const QString& needle) const {
    QList<Id> r;
    if (needle.isEmpty()) return r;
    // Дедупликация через QSet (хеш-проверка за O(1)): содержит-проверка по QList была
    // квадратичной — на 10 000 записей каждый узел поиска тянул бы проход по всему результату.
    // Порядок возврата и так недетерминирован (итерация QHash), поведения это не меняет.
    QSet<Id> seen;
    for (auto& e : entries_)
        if (e.title.contains(needle, Qt::CaseInsensitive) || e.login.contains(needle, Qt::CaseInsensitive)
            || e.note.contains(needle, Qt::CaseInsensitive)
            // Тег — поисковый атрибут записи; секреты, как и выше, не ищутся.
            || std::any_of(e.tags.cbegin(), e.tags.cend(), [&](const QString& tag) { return tag.contains(needle, Qt::CaseInsensitive); })) {
            seen.insert(e.id);
            r << e.id;
        }
    for (auto& n : nodes_)
        if (n.name.contains(needle, Qt::CaseInsensitive))
            for (auto id : entriesInSubtree(n.id))
                if (!seen.contains(id)) { seen.insert(id); r << id; }
    return r;
}
QByteArray VaultModel::toCbor() const {
    QCborMap root;
    root[u"v"_s] = 2; // v2 = v1 + обязательный массив тегов "tags" у каждой записи
    QCborArray ns;
    for (auto& n : nodes_) { QCborMap m; m[u"id"_s] = idStr(n.id); m[u"p"_s] = n.parent.isNull() ? QString() : idStr(n.parent); m[u"n"_s] = n.name; ns << m; }
    root[u"nodes"_s] = ns;
    QCborArray es;
    for (auto& e : entries_) {
        QCborMap m;
        m[u"id"_s] = idStr(e.id); m[u"g"_s] = idStr(e.groupId);
        m[u"type"_s] = int(e.type);
        m[u"title"_s] = e.title; m[u"note"_s] = e.note; m[u"url"_s] = e.url;
        m[u"cr"_s] = iso(e.created); m[u"mod"_s] = iso(e.modified);
        m[u"fav"_s] = e.favorite; m[u"rv"_s] = e.reverify;
        m[u"tok"_s] = e.token.toUtf8(); m[u"login"_s] = e.login; m[u"pw"_s] = e.password.toUtf8();
        m[u"sshpriv"_s] = e.sshPrivate; m[u"sshpub"_s] = e.sshPublic;
        m[u"sshph"_s] = e.sshPassphrase; m[u"sshc"_s] = e.sshComment;
        QCborArray tags;
        for (const QString& tag : e.tags) tags << tag;
        m[u"tags"_s] = tags;
        es << m;
    }
    root[u"entries"_s] = es;
    return QCborValue(root).toCbor();
}
VaultModel::ParseStatus VaultModel::fromCbor(const QByteArray& cbor, QString* err) {
    if (err) err->clear();
    const auto bad = [&] { if (err) *err = "Хранилище повреждено."; return ParseStatus::Bad; };
    if (cbor.size() > MaxPlainBytesCBOR) { if (err) *err = "Содержимое хранилища превышает 64 МиБ."; return ParseStatus::TooLarge; }
    QCborStreamReader reader(cbor);
    QCborValue v = QCborValue::fromCbor(reader);
    if (!v.isMap() || reader.lastError() != QCborError::NoError || reader.currentOffset() != cbor.size()) return bad();
    QCborMap root = v.toMap();
    if (root.size() != 3 || !root.value(u"nodes"_s).isArray() || !root.value(u"entries"_s).isArray()) return bad();
    // v2 = v1 + теги записей; v1 читается как записи без тегов. Неизвестная версия — отказ,
    // чтобы новая программа не молча потеряла данные, а старая — не отбросила новые поля.
    const qint64 version = root.value(u"v"_s).toInteger(-1);
    if (version != 1 && version != 2) { if (err) *err = "Неподдерживаемая версия данных."; return ParseStatus::Bad; }
    VaultModel fresh;
    for (const auto& nv : root.value(u"nodes"_s).toArray()) {
        if (!nv.isMap()) return bad();
        QCborMap m = nv.toMap();
        if (m.size() != 3 || !m.value(u"id"_s).isString() || !m.value(u"p"_s).isString()
            || !m.value(u"n"_s).isString() || m.value(u"n"_s).toString().isEmpty()) return bad();
        Id id = idFrom(m.value(u"id"_s)), parent = idFrom(m.value(u"p"_s));
        if (id.isNull() || fresh.nodes_.contains(id)
            || (parent.isNull() && !m.value(u"p"_s).toString().isEmpty())) return bad();
        fresh.nodes_.insert(id, {id, parent, m.value(u"n"_s).toString()});
    }
    // QHash сериализует узлы в произвольном порядке: сначала читаем, затем проверяем связи.
    for (const auto& n : std::as_const(fresh.nodes_)) {
        Id current = n.id;
        for (int depth = 0; !current.isNull(); ++depth) {
            const auto it = fresh.nodes_.constFind(current);
            if (depth >= MaxDepth || it == fresh.nodes_.cend()) return bad();
            current = it->parent;
        }
    }
    int count = 0;
    for (const auto& ev : root.value(u"entries"_s).toArray()) {
        if (++count > MaxEntries) { if (err) *err = "Превышен предел 10 000 записей."; return ParseStatus::TooLarge; }
        QCborMap m = ev.toMap();
        if (!ev.isMap()) return bad();
        // Поля записи могут отсутствовать (минимальные внешние v1/v2-файлы) — тогда берём
        // значение по умолчанию; но присутствующий ключ обязан иметь корректный тип,
        // иначе файл считается повреждённым.
        for (const auto key : {u"id"_s, u"g"_s, u"title"_s, u"note"_s, u"url"_s, u"cr"_s,
                              u"mod"_s, u"login"_s, u"sshph"_s, u"sshc"_s})
            if (m.contains(key) && !m.value(key).isString()) return bad();
        for (const auto key : {u"tok"_s, u"pw"_s, u"sshpriv"_s, u"sshpub"_s})
            if (m.contains(key) && !m.value(key).isByteArray()) return bad();
        if ((m.contains(u"fav"_s) && !m.value(u"fav"_s).isBool())
            || (m.contains(u"rv"_s) && !m.value(u"rv"_s).isBool())
            || !m.value(u"type"_s).isInteger() || m.value(u"type"_s).toInteger() < 0
            || m.value(u"type"_s).toInteger() > 2
            || !m.value(u"tok"_s).toByteArray().isValidUtf8()
            || !m.value(u"pw"_s).toByteArray().isValidUtf8()) return bad();
        Entry e;
        e.id = idFrom(m.value(u"id"_s)); e.groupId = idFrom(m.value(u"g"_s));
        e.type = EntryType(m.value(u"type"_s).toInteger());
        if (e.id.isNull() || fresh.entries_.contains(e.id) || fresh.nodes_.contains(e.id)
            || !fresh.nodes_.contains(e.groupId)) return bad();
        e.title = m.value(u"title"_s).toString(); e.note = m.value(u"note"_s).toString(); e.url = m.value(u"url"_s).toString();
        e.created = fromIso(m.value(u"cr"_s)); e.modified = fromIso(m.value(u"mod"_s));
        if ((!e.created.isValid() && !m.value(u"cr"_s).toString().isEmpty())
            || (!e.modified.isValid() && !m.value(u"mod"_s).toString().isEmpty())) return bad();
        e.favorite = m.value(u"fav"_s).toBool(); e.reverify = m.value(u"rv"_s).toBool();
        e.token = QString::fromUtf8(m.value(u"tok"_s).toByteArray()); e.login = m.value(u"login"_s).toString();
        e.password = QString::fromUtf8(m.value(u"pw"_s).toByteArray());
        e.sshPrivate = m.value(u"sshpriv"_s).toByteArray(); e.sshPublic = m.value(u"sshpub"_s).toByteArray();
        e.sshPassphrase = m.value(u"sshph"_s).toString(); e.sshComment = m.value(u"sshc"_s).toString();
        // Теги есть только в v2; отсутствие ключа — пустой список (спека формата).
        // Неверный тип элемента или нарушение лимитов означает правку файла извне.
        if (version >= 2 && m.contains(u"tags"_s)) {
            const QCborValue tagsValue = m.value(u"tags"_s);
            if (!tagsValue.isArray()) { if (err) *err = "Хранилище повреждено (теги)."; return ParseStatus::Bad; }
            for (const auto& tagValue : tagsValue.toArray()) {
                if (!tagValue.isString()) { if (err) *err = "Хранилище повреждено (теги)."; return ParseStatus::Bad; }
                e.tags << tagValue.toString();
            }
            // Лимиты те же, что в setEntry, но написание берём из файла без нормализации:
            // повторное сохранение не должно молча менять пользовательские теги.
            if (e.tags.size() > MaxTags
                || std::any_of(e.tags.cbegin(), e.tags.cend(), [](const QString& tag) { return tag.isEmpty() || tag.size() > MaxTagChars; })) {
                if (err) *err = "Хранилище повреждено (теги).";
                return ParseStatus::Bad;
            }
        }
        if (secretTooBig(e)) { if (err) *err = "Секрет превышает 1 МиБ."; return ParseStatus::TooLarge; }
        fresh.entries_.insert(e.id, e);
    }
    *this = std::move(fresh);
    return ParseStatus::Ok;
}
QStringList VaultModel::allTags() const {
    QStringList tags;
    for (const auto& entry : entries_)
        for (const QString& tag : entry.tags)
            if (std::none_of(tags.cbegin(), tags.cend(), [&](const QString& existing) {
                    return existing.compare(tag, Qt::CaseInsensitive) == 0; }))
                tags << tag;
    std::sort(tags.begin(), tags.end(), [](const QString& a, const QString& b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    return tags;
}
QString VaultModel::renameTag(const QString& from, const QString& to) {
    const QStringList fromList = normalizeTags({from});
    const QStringList toList = normalizeTags({to});
    if (fromList.isEmpty()) return "Тег не найден.";
    if (toList.isEmpty()) return "Имя тега пустое.";
    const QString oldTag = fromList.first(), newTag = toList.first();
    if (newTag.size() > MaxTagChars) return "Тег длиннее 32 символов.";
    bool found = false;
    for (auto& entry : entries_) {
        auto match = std::find_if(entry.tags.begin(), entry.tags.end(), [&](const QString& tag) {
            return tag.compare(oldTag, Qt::CaseInsensitive) == 0; });
        if (match == entry.tags.end()) continue;
        found = true;
        // Новый тег встаёт на место старого: порядок тегов внутри записи — часть
        // пользовательских данных. При слиянии (новое имя уже есть без учёта регистра)
        // ничего не добавляем — останется существующее написание.
        const qsizetype pos = match - entry.tags.begin();
        entry.tags.erase(match);
        if (std::none_of(entry.tags.cbegin(), entry.tags.cend(), [&](const QString& tag) {
                return tag.compare(newTag, Qt::CaseInsensitive) == 0; }))
            entry.tags.insert(pos, newTag);
    }
    return found ? QString() : "Тег не найден.";
}
QString VaultModel::removeTag(const QString& tag) {
    bool found = false;
    for (auto& entry : entries_) {
        auto tail = std::remove_if(entry.tags.begin(), entry.tags.end(), [&](const QString& existing) {
            return existing.compare(tag, Qt::CaseInsensitive) == 0; });
        if (tail != entry.tags.end()) { found = true; entry.tags.erase(tail, entry.tags.end()); }
    }
    return found ? QString() : "Тег не найден.";
}
void VaultModel::clear() { nodes_.clear(); entries_.clear(); }
}
