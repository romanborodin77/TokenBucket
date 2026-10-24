#pragma once
#include "../model/VaultModel.h"
namespace tb::io {
enum class ConflictAction {Skip,CreateVersion,Replace};
struct ProposedParameter {
    int line=0; QString name,value,section;
    model::EntryType suggestedType=model::EntryType::ApiToken;
    bool operator==(const ProposedParameter&) const = default;
};
struct TextImportPreview {model::Entry source; QList<ProposedParameter> parameters; QString error;};
struct RegistrySelection {
    int parameter=0; model::EntryType type=model::EntryType::ApiToken;
    QString title,parameterName; QStringList groupPath;
    ConflictAction conflict=ConflictAction::CreateVersion; model::Id replaceId;
    int matchingEntries=0; // UI summary only; replacement is checked again when building.
    bool exactMatch=false;
};
model::EntryType suggestStorageType(const QString& name,const QString& value);
TextImportPreview parseTextFile(const QByteArray&,const QString& fileName);
QList<RegistrySelection> defaultRegistrySelections(const model::VaultModel&,const TextImportPreview&);
QList<model::Id> registryMatches(const model::VaultModel&,const TextImportPreview&,const RegistrySelection&);
bool buildRegistryCandidate(const model::VaultModel&,const TextImportPreview&,const QList<RegistrySelection>&,model::VaultModel* out,QString* error);
}
