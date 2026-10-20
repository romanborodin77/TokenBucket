#pragma once
namespace tb::ui::theme {
enum class Mode { System, Light, Dark };
Mode load();
void apply(Mode mode);
}
