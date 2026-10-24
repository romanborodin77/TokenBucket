#pragma once
#include <QSettings>
#include <functional>
#include "../app/Application.h"
class QWidget;
namespace tb::ui {
int runSession(Application& core,QSettings& preferences,bool startHidden,
               QSettings* runPreferences=nullptr,
               std::function<bool()> trayAvailable={});
bool runVaultOperation(Application& core,const std::function<bool(void*,QString*)>& operation,
                       QString* error,QWidget* parent=nullptr);
}
