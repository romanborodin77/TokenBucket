#pragma once
#include "../model/VaultModel.h"
namespace tb::io {
struct MigrationProposal {quint64 generation=0; QList<model::Entry> before,after; QString error;};
MigrationProposal proposeRegistryMigration(const model::VaultModel&,const QList<model::Id>&,quint64 generation);
bool buildMigrationCandidate(const model::VaultModel&,const MigrationProposal&,model::VaultModel* out,QString* error);
}
