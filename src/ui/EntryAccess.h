#pragma once
#include "../model/Types.h"
class QWidget;
namespace tb { class Application; }
namespace tb::ui {
bool authorizeEntry(Application& app,model::Id id,QWidget* parent,bool force=false);
}
