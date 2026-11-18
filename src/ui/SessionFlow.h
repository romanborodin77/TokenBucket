#pragma once
#include <QSettings>
#include <functional>
#include "../app/Application.h"
class QWidget;
namespace tb::agent { class Access; }
namespace tb::ui {
int runSession(Application& core,QSettings& preferences,bool startHidden,
               QSettings* runPreferences=nullptr,
               std::function<bool()> trayAvailable={}, agent::Access* agentAccess=nullptr);
bool runVaultOperation(Application& core,const std::function<bool(void*,QString*)>& operation,
                       QString* error,QWidget* parent=nullptr);
}
