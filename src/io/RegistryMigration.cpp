#include "RegistryMigration.h"
#include "TextImport.h"
#include <QSet>
namespace tb::io {
namespace {
model::Entry converted(const model::Entry& before) {
    auto after=before;
    if(before.type!=model::EntryType::ApiToken || !before.login.isEmpty() || !before.password.isEmpty() || !before.sshPrivate.isEmpty()
        || !before.sshPublic.isEmpty() || !before.sshPassphrase.isEmpty() || !before.sshComment.isEmpty()
        || suggestStorageType(before.title,before.token)!=model::EntryType::PlainValue) return after;
    after.type=model::EntryType::PlainValue; after.parameterName=before.title; after.plainValue=before.token; after.token.clear(); return after;
}
bool fail(QString* error,const QString& text) {if(error) *error=text; return false;}
}
MigrationProposal proposeRegistryMigration(const model::VaultModel& base,const QList<model::Id>& ids,quint64 generation) {
    MigrationProposal p; p.generation=generation; QSet<model::Id> seen;
    if(ids.isEmpty()) {p.error=QStringLiteral("Нет выбранных записей."); return p;}
    for(const auto id:ids) {
        const auto* e=base.entry(id); if(!e || seen.contains(id)) {p.error=QStringLiteral("Запись отсутствует или выбрана повторно."); p.before.clear(); p.after.clear(); return p;}
        seen.insert(id); p.before.append(*e); p.after.append(converted(*e));
    }
    return p;
}
bool buildMigrationCandidate(const model::VaultModel& base,const MigrationProposal& p,model::VaultModel* out,QString* error) {
    if(error) error->clear();
    if(!out || !p.error.isEmpty() || p.before.isEmpty() || p.before.size()!=p.after.size()) return fail(error,QStringLiteral("Предложение миграции недоступно."));
    auto candidate=base; QSet<model::Id> seen;
    for(int i=0;i<p.before.size();++i) {
        const auto& before=p.before[i]; const auto* current=base.entry(before.id);
        if(!current || *current!=before || seen.contains(before.id) || p.after[i]!=converted(before)) return fail(error,QStringLiteral("Предложение устарело или изменено. Повторите предпросмотр."));
        seen.insert(before.id);
        if(const auto message=candidate.setEntry(p.after[i]);!message.isEmpty()) return fail(error,message);
    }
    if(candidate.encodedSize()>model::VaultModel::MaxPlainBytesCBOR) return fail(error,QStringLiteral("Модель превышает 64 МиБ."));
    *out=std::move(candidate); return true;
}
}
