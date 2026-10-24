#include "TextImport.h"
#include "../model/TextFile.h"
#include <QHostAddress>
#include <QRegularExpression>
#include <QSet>
#include <QStringDecoder>
#include <QUrl>
namespace tb::io {
namespace {
QString key(const QString& name) {auto result=name.trimmed().toUpper(); result.replace(' ','_'); result.replace('-','_'); return result;}
QString sectionName(const QString& name) {
    const auto s=key(name);
    if(s=="VM" || s=="VIRTUAL_MACHINE" || s==QStringLiteral("ВИРТУАЛЬНАЯ_МАШИНА")) return QStringLiteral("Виртуальная машина");
    if(s=="POSTGRES" || s=="POSTGRESQL") return QStringLiteral("PostgreSQL");
    if(s=="PROJECT" || s=="SERVICES" || s==QStringLiteral("ПРОЕКТ_И_СЕРВИСЫ")) return QStringLiteral("Проект и сервисы");
    return {};
}
model::Id existingGroup(const model::VaultModel& model,model::Id group,const QStringList& path) {
    for(const auto& name:path) {
        model::Id found;
        for(const auto& node:model.nodes()) if(node.parent==group && node.name==name) {if(!found.isNull()) return {}; found=node.id;}
        if(found.isNull()) return {}; group=found;
    }
    return group;
}
QList<model::Id> matching(const model::VaultModel& model,const TextImportPreview& preview,const RegistrySelection& choice) {
    const auto group=existingGroup(model,preview.source.groupId,choice.groupPath); QList<model::Id> ids; if(group.isNull()) return ids;
    for(const auto& e:model.entries()) if(e.groupId==group && e.type==choice.type
        && (choice.type==model::EntryType::PlainValue?e.parameterName==choice.parameterName:e.title==choice.title)) ids.append(e.id);
    return ids;
}
bool fail(QString* error,const QString& text) {if(error) *error=text; return false;}
bool applyChoice(model::VaultModel& model,const TextImportPreview& preview,const RegistrySelection& choice,QString* error) {
    if(choice.conflict==ConflictAction::Skip) return true;
    const auto& p=preview.parameters[choice.parameter]; model::Id group=preview.source.groupId;
    for(const auto& name:choice.groupPath) {
        if(name.trimmed().isEmpty() || name.size()>128 || !name.isValidUtf16()) return fail(error,QStringLiteral("Недопустимое имя группы."));
        for(auto ch:name.toUcs4()) if(!QChar::isPrint(ch)) return fail(error,QStringLiteral("Недопустимое имя группы."));
        model::Id found;
        for(const auto& node:model.nodes()) if(node.parent==group && node.name==name) {
            if(!found.isNull()) return fail(error,QStringLiteral("Группа неоднозначна. Выберите другой путь.")); found=node.id;
        }
        if(found.isNull()) {found=QUuid::createUuid(); if(const auto e=model.addNode(found,group,name);!e.isEmpty()) return fail(error,e);}
        group=found;
    }
    model::Entry e;
    if(choice.conflict==ConflictAction::Replace) {
        const auto* previous=model.entry(choice.replaceId); const auto ids=matching(model,preview,choice);
        if(!previous || ids.size()!=1 || ids.first()!=choice.replaceId || previous->id==preview.source.id || previous->type!=choice.type)
            return fail(error,QStringLiteral("Цель замены отсутствует, неоднозначна или имеет другой тип."));
        e=*previous;
    } else {
        e.id=QUuid::createUuid(); e.groupId=group; e.type=choice.type; e.title=choice.title;
        e.parameterName=choice.type==model::EntryType::PlainValue?choice.parameterName:QString();
        e.created=QDateTime::currentDateTimeUtc();
        int version=1; const auto title=e.title;
        const auto occupied=[&] {for(const auto& other:model.entries()) if(other.groupId==group && other.title==e.title) return true; return false;};
        while(occupied()) e.title=title+QStringLiteral(" — версия %1").arg(++version);
    }
    e.modified=QDateTime::currentDateTimeUtc(); e.reverify=e.reverify || preview.source.reverify;
    e.sourceEntryId=preview.source.id; e.sourceLine=p.line;
    if(e.type==model::EntryType::PlainValue) {e.plainValue=p.value; if(choice.conflict!=ConflictAction::Replace) e.valueFormat=model::ValueFormat::Text;}
    else e.token=p.value;
    if(const auto message=model.setEntry(e);!message.isEmpty()) return fail(error,message);
    return true;
}
}
model::EntryType suggestStorageType(const QString& name,const QString& value) {
    using model::EntryType; const auto n=key(name);
    static const QRegularExpression secret("PASS|PWD|TOKEN|SECRET|KEY|CREDENTIAL|CONNECTION|DSN");
    if(secret.match(n).hasMatch() || value.contains('$') || value.contains('`') || value.contains('\\') || value.contains("-----BEGIN")) return EntryType::ApiToken;
    static const QSet<QString> ips={"IP","IP_ADDRESS","PUBLIC_IP","PRIVATE_IP","HOST","HOST_IP","IP_АДРЕС"};
    static const QSet<QString> urls={"URL","APP_URL","PUBLIC_URL","ENDPOINT","API_URL","HOST_URL"};
    static const QSet<QString> identifiers={"ID","PROJECT_ID","TENANT_ID","VM_ID","INSTANCE_ID","VERSION","DB_NAME","DATABASE_NAME","USERNAME","USER","LOGIN","REGION","ZONE","PORT"};
    if(ips.contains(n) && QHostAddress().setAddress(value)) return EntryType::PlainValue;
    if(urls.contains(n)) {
        const QUrl url(value,QUrl::StrictMode);
        if(url.isValid() && !url.host().isEmpty() && (url.scheme()=="https" || url.scheme()=="http")
            && url.userInfo().isEmpty() && !url.hasQuery() && !url.hasFragment()) return EntryType::PlainValue;
    }
    static const QRegularExpression simple("^[\\p{L}\\p{N}_][\\p{L}\\p{N}_.-]{0,127}$");
    if(identifiers.contains(n) && simple.match(value).hasMatch()) return EntryType::PlainValue;
    return EntryType::ApiToken;
}
TextImportPreview parseTextFile(const QByteArray& bytes,const QString& fileName) {
    TextImportPreview result; auto& source=result.source;
    source.id=QUuid::createUuid(); source.type=model::EntryType::TextFile; source.title=fileName;
    source.created=source.modified=QDateTime::currentDateTimeUtc(); source.textFile={fileName,bytes,true,{}};
    result.error=model::textfile::validate(source.textFile); if(!result.error.isEmpty()) return result;
    static const QRegularExpression parameter("^[ \\t]*(?:export[ \\t]+)?([^=:\\r\\n]+?)[ \\t]*(?:=|:[ \\t]+)(.*)$");
    const auto ranges=model::textfile::lineRanges(bytes); QString section;
    for(int line=0;line<ranges.size();++line) {
        const auto r=ranges[line]; const QString raw=QStringDecoder(QStringConverter::Utf8,QStringConverter::Flag::ConvertInitialBom)(QByteArrayView(bytes).sliced(r.offset,r.length));
        const auto trimmed=raw.trimmed(); if(trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith(';') || trimmed.startsWith("//")) continue;
        if(trimmed.startsWith('[') && trimmed.endsWith(']')) {section=sectionName(trimmed.mid(1,trimmed.size()-2)); continue;}
        const auto heading=sectionName(trimmed.endsWith(':')?trimmed.chopped(1):trimmed);
        if(!heading.isEmpty()) {section=heading; continue;}
        const auto match=parameter.match(raw); if(!match.hasMatch()) {section.clear(); continue;}
        const auto name=match.captured(1).trimmed(); bool valid=!name.isEmpty() && name.size()<=128;
        for(auto ch:name.toUcs4()) if(!QChar::isPrint(ch)) valid=false;
        if(!valid) continue;
        auto value=match.captured(2);
        if(value.size()>=2 && ((value.front()=='"' && value.back()=='"') || (value.front()=='\'' && value.back()=='\''))) value=value.mid(1,value.size()-2);
        result.parameters.append({line,name,value,section,suggestStorageType(name,value)});
    }
    return result;
}
QList<RegistrySelection> defaultRegistrySelections(const model::VaultModel& base,const TextImportPreview& preview) {
    QList<RegistrySelection> result; QMultiHash<QString,const model::Entry*> index; QHash<QString,model::Id> groups;
    static const QSet<QString> labels={"TOKEN","PASSWORD","SECRET","API_TOKEN","API_KEY","ACCESS_KEY","SECRET_KEY","ACCESS_KEY_ID","SECRET_ACCESS_KEY",
        "PRIVATE_KEY","SSH_PRIVATE_KEY","SSH_PASSPHRASE","POSTGRES_PASSWORD","DB_PASSWORD","DATABASE_PASSWORD","CLOUDRU_ACCESS_KEY","CLOUDRU_SECRET_KEY","CLOUDRU_KEY_ID"};
    const auto identity=[](model::Id group,model::EntryType type,const QString& name) {return group.toString()+QString::number(int(type))+"/"+name;};
    for(const auto& e:base.entries()) if(e.type==model::EntryType::ApiToken || e.type==model::EntryType::PlainValue)
        index.insert(identity(e.groupId,e.type,e.type==model::EntryType::PlainValue?e.parameterName:e.title),&e);
    for(int i=0;i<preview.parameters.size();++i) {
        const auto& p=preview.parameters[i]; RegistrySelection choice; choice.parameter=i; choice.type=p.suggestedType;
        choice.title=p.suggestedType==model::EntryType::PlainValue?p.name:QStringLiteral("%1 · строка %2").arg(preview.source.textFile.fileName).arg(p.line+1);
        if(choice.type==model::EntryType::ApiToken && labels.contains(key(p.name))) choice.title=key(p.name);
        if(choice.type==model::EntryType::PlainValue) choice.parameterName=p.name;
        if(!p.section.isEmpty()) choice.groupPath.append(p.section);
        const auto path=choice.groupPath.join('/');
        if(!groups.contains(path)) groups.insert(path,existingGroup(base,preview.source.groupId,choice.groupPath));
        const auto range=index.equal_range(identity(groups.value(path),choice.type,choice.type==model::EntryType::PlainValue?choice.parameterName:choice.title));
        for(auto it=range.first;it!=range.second;++it) ++choice.matchingEntries;
        for(auto it=range.first;it!=range.second;++it) {const auto* e=*it; const auto& value=e->type==model::EntryType::PlainValue?e->plainValue:e->token;
            if(value==p.value) {choice.exactMatch=true; choice.conflict=ConflictAction::Skip; break;}}
        result.append(choice);
    }
    return result;
}
QList<model::Id> registryMatches(const model::VaultModel& base,const TextImportPreview& preview,const RegistrySelection& choice) {return matching(base,preview,choice);}
bool buildRegistryCandidate(const model::VaultModel& base,const TextImportPreview& preview,const QList<RegistrySelection>& choices,model::VaultModel* out,QString* error) {
    if(error) error->clear();
    if(!out || !preview.error.isEmpty() || preview.source.type!=model::EntryType::TextFile || !preview.source.textFile.sourceSnapshot)
        return fail(error,QStringLiteral("Нужен проверенный снимок текстового файла."));
    const auto parsed=parseTextFile(preview.source.textFile.bytes,preview.source.textFile.fileName);
    if(!parsed.error.isEmpty() || parsed.parameters!=preview.parameters) return fail(error,QStringLiteral("Предпросмотр не соответствует исходным строкам."));
    QSet<int> selected,publicLines; QSet<model::Id> replacements;
    for(const auto& c:choices) {
        if(c.parameter<0 || c.parameter>=preview.parameters.size() || selected.contains(c.parameter)
            || (c.type!=model::EntryType::ApiToken && c.type!=model::EntryType::PlainValue)
            || int(c.conflict)<0 || int(c.conflict)>2 || c.groupPath.size()>model::VaultModel::MaxDepth)
            return fail(error,QStringLiteral("Недопустимый выбор параметров."));
        selected.insert(c.parameter);
        if(c.conflict==ConflictAction::Replace && (c.replaceId.isNull() || replacements.contains(c.replaceId))) return fail(error,QStringLiteral("Цель замены выбрана несколько раз или отсутствует."));
        if(c.conflict==ConflictAction::Replace) replacements.insert(c.replaceId);
        if(c.type==model::EntryType::PlainValue) publicLines.insert(preview.parameters[c.parameter].line);
    }
    const auto* previousSource=base.entry(preview.source.id);
    if(previousSource) {
        auto expected=*previousSource; expected.textFile.visibleLines=preview.source.textFile.visibleLines;
        if(previousSource->type!=model::EntryType::TextFile || !previousSource->textFile.sourceSnapshot || expected!=preview.source)
            return fail(error,QStringLiteral("Источник изменился или его идентификатор уже занят."));
    } else for(const auto& e:base.entries()) if(e.sourceEntryId==preview.source.id)
        return fail(error,QStringLiteral("Идентификатор источника уже используется ссылками."));
    for(int line:preview.source.textFile.visibleLines) if(!publicLines.contains(line)
        && (!previousSource || !model::textfile::isVisible(previousSource->textFile,line))) return fail(error,QStringLiteral("Видимость неизвестной строки не согласована."));
    auto candidate=base;
    if(const auto e=candidate.setEntry(preview.source);!e.isEmpty()) return fail(error,e);
    for(const auto& c:choices) if(!applyChoice(candidate,preview,c,error)) return false;
    if(candidate.encodedSize()>model::VaultModel::MaxPlainBytesCBOR) return fail(error,QStringLiteral("Кандидат превышает 64 МиБ."));
    *out=std::move(candidate); return true;
}
}
