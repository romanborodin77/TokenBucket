#include "EntryAccess.h"
#include "../app/Application.h"
#include "VerifyDialog.h"
namespace tb::ui {
bool authorizeEntry(Application& app,model::Id id,QWidget* parent,bool force) {
    if(app.gate()!=Application::Gate::Ready || app.busy()) return false;
    const auto* entry=app.model()->entry(id); if(!entry) return false;
    const auto before=*entry; const auto generation=app.generation();
    if((before.reverify || force) && !VerifyDialog::verify(app,parent,QStringLiteral("доступ к записи"))) return false;
    const auto* current=app.model()->entry(id);
    return generation==app.generation() && app.gate()==Application::Gate::Ready && !app.busy() && current && *current==before;
}
}
