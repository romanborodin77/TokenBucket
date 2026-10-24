#include "VaultModel.h"
#include "TextFile.h"
#include <QCborArray>
#include <QCborMap>
#include <QCborValue>
#include <QCborStreamReader>
#include <QSet>
#include <QHostAddress>
#include <QUrl>
#include <QStringDecoder>
#include <functional>
#include <algorithm>
using namespace Qt::StringLiterals;
namespace tb::model {
namespace {
QString iso(const QDateTime& t) { return t.toUTC().toString(Qt::ISODateWithMs); }
QDateTime fromIso(const QCborValue& v) { return QDateTime::fromString(v.toString(), Qt::ISODateWithMs); }
Id idFrom(const QCborValue& v) { return QUuid::fromString(v.toString()); }
QString idStr(const Id& id) { return id.toString(QUuid::WithoutBraces); }
QString utf8(QByteArrayView bytes) {
    return QStringDecoder(QStringConverter::Utf8,QStringConverter::Flag::ConvertInitialBom)(bytes);
}
QString payloadError(const Entry& e) {
    if(int(e.valueFormat)<0 || int(e.valueFormat)>2) return QStringLiteral("Неизвестный формат значения.");
    if(e.type!=EntryType::PlainValue && (!e.parameterName.isEmpty() || !e.plainValue.isEmpty() || e.valueFormat!=ValueFormat::Text))
        return QStringLiteral("У этого типа не должно быть простого значения.");
    if(e.type!=EntryType::TextFile && e.textFile!=TextFileData{}) return QStringLiteral("У этого типа не должно быть тела файла.");
    if(e.type==EntryType::PlainValue || e.type==EntryType::TextFile) {
        if(!e.token.isEmpty() || !e.password.isEmpty() || !e.login.isEmpty() || !e.sshPrivate.isEmpty()
            || !e.sshPublic.isEmpty() || !e.sshPassphrase.isEmpty() || !e.sshComment.isEmpty())
            return QStringLiteral("Новый тип содержит несовместимые прежние полезные поля.");
    }
    if(e.type==EntryType::TextFile) return textfile::validate(e.textFile);
    if(e.type!=EntryType::PlainValue) return {};
    if(e.parameterName.size()>128 || !e.parameterName.isValidUtf16()) return QStringLiteral("Имя параметра: до 128 печатных символов.");
    for(auto ch:e.parameterName.toUcs4()) if(!QChar::isPrint(ch)) return QStringLiteral("Имя параметра содержит управляющий символ.");
    if(!e.plainValue.isValidUtf16() || e.plainValue.contains(QChar(0)) || e.plainValue.contains('\r') || e.plainValue.contains('\n')
        || e.plainValue.contains(QChar(0x2028)) || e.plainValue.contains(QChar(0x2029)))
        return QStringLiteral("Простое значение должно быть одной строкой без NUL.");
    if(!e.plainValue.isEmpty() && e.valueFormat==ValueFormat::Ip && !QHostAddress().setAddress(e.plainValue))
        return QStringLiteral("Укажите корректный IPv4 или IPv6 без порта.");
    if(!e.plainValue.isEmpty() && e.valueFormat==ValueFormat::Url) {
        const QUrl url(e.plainValue,QUrl::StrictMode);
        if(!url.isValid() || url.isRelative()) return QStringLiteral("Укажите корректный абсолютный URL.");
    }
    return {};
}
}
QString VaultModel::addNode(Id id, Id parent, const QString& name) {
    if (id.isNull()) return "Пустой идентификатор.";
    if (name.isEmpty()) return "Имя не может быть пустым.";
    if (nodes_.contains(id) || entries_.contains(id)) return "Такой идентификатор уже есть.";
    for(const auto& e:entries_) if(e.sourceEntryId==id) return QStringLiteral("Идентификатор используется как ссылка на снимок источника.");
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
        || e.sshPassphrase.toUtf8().size() > MaxSecretBytes || e.plainValue.toUtf8().size()>MaxSecretBytes
        || e.textFile.bytes.size()>MaxSecretBytes;
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
    if (int(e.type) < 0 || int(e.type) > 4) return "Неизвестный тип записи.";
    if (!nodes_.contains(e.groupId)) return "Проект или группа не найдены.";
    if (!entries_.contains(e.id) && entries_.size() >= MaxEntries) return "Достигнут предел 10 000 записей.";
    if (secretTooBig(e)) return "Секрет превышает 1 МиБ.";
    if(const auto error=payloadError(e);!error.isEmpty()) return error;
    if(const auto error=sourceError(e);!error.isEmpty()) return error;
    if(const auto* previous=entry(e.id);previous && previous->type==EntryType::TextFile) {
        if(previous->textFile.sourceSnapshot && (e.type!=EntryType::TextFile || !e.textFile.sourceSnapshot || previous->textFile.bytes!=e.textFile.bytes))
            return QStringLiteral("Снимок источника неизменяем. Редактируйте копию с новым идентификатором.");
        if(e.type==EntryType::TextFile && previous->textFile.bytes!=e.textFile.bytes && !e.textFile.visibleLines.isEmpty())
            return QStringLiteral("При изменении тела сначала сбросьте видимость строк.");
    }
    const auto sourceLines=e.type==EntryType::TextFile?textfile::lineRanges(e.textFile.bytes).size():0;
    for(const auto& related:entries_) if(related.id!=e.id && related.sourceEntryId==e.id) {
        if(e.type!=EntryType::TextFile || !e.textFile.sourceSnapshot || related.sourceLine>=sourceLines)
            return QStringLiteral("Запись используется как снимок источника.");
    }
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
QString VaultModel::sourceError(const Entry& e) const {
    if(e.sourceEntryId.isNull()) return e.sourceLine==-1?QString():QStringLiteral("Номер строки без источника.");
    if(e.sourceEntryId==e.id || nodes_.contains(e.sourceEntryId) || e.sourceLine<0 || e.sourceLine>=textfile::MaxLines)
        return QStringLiteral("Недопустимая ссылка на источник.");
    const auto* source=entry(e.sourceEntryId);
    if(source && (source->type!=EntryType::TextFile || !source->textFile.sourceSnapshot
        || e.sourceLine>=textfile::lineRanges(source->textFile.bytes).size())) return QStringLiteral("Источник должен быть текстовым снимком с указанной строкой.");
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
    for (auto& e : entries_) {
        bool visibleMatch=e.type==EntryType::PlainValue && !e.reverify && e.plainValue.contains(needle,Qt::CaseInsensitive);
        if(e.type==EntryType::TextFile && !e.reverify) {
            const auto lines=textfile::lineRanges(e.textFile.bytes);
            for(int line:e.textFile.visibleLines) {
                const auto r=lines[line];
                if(utf8(QByteArrayView(e.textFile.bytes).sliced(r.offset,r.length)).contains(needle,Qt::CaseInsensitive)) {visibleMatch=true; break;}
            }
        }
        if (visibleMatch || e.parameterName.contains(needle,Qt::CaseInsensitive) || e.title.contains(needle, Qt::CaseInsensitive) || e.login.contains(needle, Qt::CaseInsensitive)
            || e.note.contains(needle, Qt::CaseInsensitive)
            // Тег — поисковый атрибут записи; секреты, как и выше, не ищутся.
            || std::any_of(e.tags.cbegin(), e.tags.cend(), [&](const QString& tag) { return tag.contains(needle, Qt::CaseInsensitive); })) {
            seen.insert(e.id);
            r << e.id;
        }
    }
    for (auto& n : nodes_)
        if (n.name.contains(needle, Qt::CaseInsensitive))
            for (auto id : entriesInSubtree(n.id))
                if (!seen.contains(id)) { seen.insert(id); r << id; }
    return r;
}
QByteArray VaultModel::toCbor() const {
    QCborMap root;
    root[u"v"_s] = 4;
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
        m[u"exp"_s] = e.expiresAt.isValid() ? iso(e.expiresAt) : QString(); // пусто — бессрочно
        QCborArray tags;
        for (const QString& tag : e.tags) tags << tag;
        m[u"tags"_s] = tags;
        m[u"key"_s]=e.parameterName; m[u"value"_s]=e.plainValue.toUtf8();
        m[u"format"_s]=e.valueFormat==ValueFormat::Ip?QStringLiteral("ip"):e.valueFormat==ValueFormat::Url?QStringLiteral("url"):QStringLiteral("text");
        m[u"fileName"_s]=e.textFile.fileName; m[u"fileBytes"_s]=e.textFile.bytes; m[u"fileSnapshot"_s]=e.textFile.sourceSnapshot;
        QCborArray visible; for(int line:e.textFile.visibleLines) visible.append(line); m[u"fileVisible"_s]=visible;
        m[u"sourceId"_s]=e.sourceEntryId.isNull()?QString():idStr(e.sourceEntryId); m[u"sourceLine"_s]=e.sourceLine;
        es << m;
    }
    root[u"entries"_s] = es;
    return QCborValue(root).toCbor();
}
// Защищённая сериализация: та же структура и тот же ПОРЯДОК ключей, что у toCbor()
// (QCborMap сохраняет порядок вставки, а не сортирует), поэтому выход побайтово
// совпадает с QCborValue::toCbor() — это фиксирует золотой тест.
void VaultModel::encodeTo(cbor::CborWriter& w) const {
    w.beginMap(3);
    w.text(u"v"); w.uint(4);
    w.text(u"nodes"); {
        w.beginArray(size_t(nodes_.size()));
        for (auto& n : nodes_) {
            w.beginMap(3);
            w.text(u"id"); w.text(idStr(n.id));
            w.text(u"p"); w.text(n.parent.isNull() ? QString() : idStr(n.parent));
            w.text(u"n"); w.text(n.name);
        }
    }
    w.text(u"entries"); {
        w.beginArray(size_t(entries_.size()));
        for (auto& e : entries_) {
            w.beginMap(28);
            w.text(u"id"); w.text(idStr(e.id));
            w.text(u"g"); w.text(idStr(e.groupId));
            w.text(u"type"); w.uint(quint64(int(e.type)));
            w.text(u"title"); w.text(e.title);
            w.text(u"note"); w.text(e.note);
            w.text(u"url"); w.text(e.url);
            w.text(u"cr"); w.text(iso(e.created));
            w.text(u"mod"); w.text(iso(e.modified));
            w.text(u"fav"); w.boolean(e.favorite);
            w.text(u"rv"); w.boolean(e.reverify);
            w.text(u"tok"); w.bytesUtf8(e.token);
            w.text(u"login"); w.text(e.login);
            w.text(u"pw"); w.bytesUtf8(e.password);
            w.text(u"sshpriv"); w.bytes(e.sshPrivate.constData(), size_t(e.sshPrivate.size()));
            w.text(u"sshpub"); w.bytes(e.sshPublic.constData(), size_t(e.sshPublic.size()));
            w.text(u"sshph"); w.text(e.sshPassphrase);
            w.text(u"sshc"); w.text(e.sshComment);
            w.text(u"exp"); w.text(e.expiresAt.isValid() ? iso(e.expiresAt) : QString()); // пусто — бессрочно
            w.text(u"tags"); {
                w.beginArray(size_t(e.tags.size()));
                for (const QString& tag : e.tags) w.text(tag);
            }
            w.text(u"key"); w.text(e.parameterName); w.text(u"value"); w.bytesUtf8(e.plainValue);
            w.text(u"format"); w.text(e.valueFormat==ValueFormat::Ip?u"ip":e.valueFormat==ValueFormat::Url?u"url":u"text");
            w.text(u"fileName"); w.text(e.textFile.fileName);
            w.text(u"fileBytes"); w.bytes(e.textFile.bytes.constData(),size_t(e.textFile.bytes.size()));
            w.text(u"fileSnapshot"); w.boolean(e.textFile.sourceSnapshot);
            w.text(u"fileVisible"); w.beginArray(size_t(e.textFile.visibleLines.size()));
            for(int line:e.textFile.visibleLines) w.uint(quint64(line));
            w.text(u"sourceId"); w.text(e.sourceEntryId.isNull()?QString():idStr(e.sourceEntryId));
            w.text(u"sourceLine"); w.integer(e.sourceLine);
        }
    }
}
std::size_t VaultModel::encodedSize() const {
    cbor::CborWriter counter; encodeTo(counter); return counter.position();
}
cbor::SecureBuffer VaultModel::toCborSecure() const {
    // Проход 1 — сухой подсчёт точного размера; проход 2 — запись в буфер ровно того
    // размера. Оба прохода исполняют одну и ту же encodeTo, поэтому размеры совпадают,
    // а секреты нигде не попадают в QByteArray/обычную кучу.
    cbor::SecureBuffer buffer(encodedSize());
    cbor::CborWriter writer;
    writer.setOutput(buffer.data(), buffer.size());
    encodeTo(writer);
    Q_ASSERT_X(!writer.overflow() && writer.position() == buffer.size(),
               "VaultModel::toCborSecure", "расхождение проходов подсчёта и записи");
    return buffer;
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
    if (version < 1 || version > 4) { if (err) *err = "Неподдерживаемая версия данных."; return ParseStatus::Bad; }
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
            || m.value(u"type"_s).toInteger() > (version<4?2:4)
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
        e.token = utf8(m.value(u"tok"_s).toByteArray()); e.login = m.value(u"login"_s).toString();
        e.password = utf8(m.value(u"pw"_s).toByteArray());
        e.sshPrivate = m.value(u"sshpriv"_s).toByteArray(); e.sshPublic = m.value(u"sshpub"_s).toByteArray();
        e.sshPassphrase = m.value(u"sshph"_s).toString(); e.sshComment = m.value(u"sshc"_s).toString();
        for(const auto key:{u"key"_s,u"format"_s,u"fileName"_s,u"sourceId"_s})
            if(m.contains(key) && !m.value(key).isString()) return bad();
        for(const auto key:{u"value"_s,u"fileBytes"_s}) if(m.contains(key) && !m.value(key).isByteArray()) return bad();
        if(!m.value(u"value"_s).toByteArray().isValidUtf8() || (m.contains(u"fileSnapshot"_s) && !m.value(u"fileSnapshot"_s).isBool())
            || (m.contains(u"fileVisible"_s) && !m.value(u"fileVisible"_s).isArray())
            || (m.contains(u"sourceLine"_s) && !m.value(u"sourceLine"_s).isInteger())) return bad();
        e.parameterName=m.value(u"key"_s).toString(); e.plainValue=utf8(m.value(u"value"_s).toByteArray());
        const auto format=m.contains(u"format"_s)?m.value(u"format"_s).toString():QStringLiteral("text");
        if(format!="text" && format!="ip" && format!="url") return bad();
        e.valueFormat=format=="ip"?ValueFormat::Ip:format=="url"?ValueFormat::Url:ValueFormat::Text;
        e.textFile.fileName=m.value(u"fileName"_s).toString(); e.textFile.bytes=m.value(u"fileBytes"_s).toByteArray();
        e.textFile.sourceSnapshot=m.value(u"fileSnapshot"_s).toBool();
        for(const auto line:m.value(u"fileVisible"_s).toArray()) {
            if(!line.isInteger() || line.toInteger()<0 || line.toInteger()>=textfile::MaxLines) return bad();
            e.textFile.visibleLines.append(int(line.toInteger()));
        }
        const auto source=m.value(u"sourceId"_s).toString(); e.sourceEntryId=QUuid::fromString(source);
        if(!source.isEmpty() && e.sourceEntryId.isNull()) return bad();
        const auto sourceLine=m.contains(u"sourceLine"_s)?m.value(u"sourceLine"_s).toInteger():-1;
        if(sourceLine< -1 || sourceLine>=textfile::MaxLines) return bad(); e.sourceLine=int(sourceLine);
        if(version<4 && (!e.sourceEntryId.isNull() || e.sourceLine!=-1)) return bad();
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
        if (version >= 3) {
            // Срок действия: отсутствие ключа — бессрочно; строка обязана быть датой ISO.
            const QCborValue expiry = m.value(u"exp"_s);
            // Пустая строка и отсутствие ключа — бессрочно (наш писатель пишет "" для записей без срока).
            if (expiry.isUndefined() || expiry.isNull()
                || (expiry.isString() && expiry.toString().isEmpty())) e.expiresAt = QDateTime();
            else if (!expiry.isString()) { if (err) *err = "Хранилище повреждено (срок действия)."; return ParseStatus::Bad; }
            else {
                e.expiresAt = QDateTime::fromString(expiry.toString(), Qt::ISODateWithMs);
                if (!e.expiresAt.isValid()) { if (err) *err = "Хранилище повреждено (срок действия)."; return ParseStatus::Bad; }
                e.expiresAt = e.expiresAt.toUTC();
            }
        }
        if (secretTooBig(e)) { if (err) *err = "Секрет превышает 1 МиБ."; return ParseStatus::TooLarge; }
        if(!payloadError(e).isEmpty()) return bad();
        fresh.entries_.insert(e.id, e);
    }
    for(const auto& e:fresh.entries_) if(!fresh.sourceError(e).isEmpty()) return bad();
    fresh.decodedVersion_=int(version);
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
void VaultModel::clear() { nodes_.clear(); entries_.clear(); decodedVersion_=4; }
}
