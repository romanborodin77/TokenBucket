#pragma once
class QSettings;
namespace tb::ui::theme {
// Тема приложения. System читает реестр Windows (AppsUseLightTheme) на каждый
// apply — смена темы системой подхватывается перезапуском приложения; Light/Dark —
// палитры поверх Fusion. apply() обязан вызываться до создания первого окна:
// палитру наследуют все последующие виджеты и диалоги.
enum class Mode { System, Light, Dark };
Mode load(QSettings* preferences=nullptr); // выбор пользователя из QSettings
void apply(Mode mode);
}
