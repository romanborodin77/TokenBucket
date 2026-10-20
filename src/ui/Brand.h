#pragma once
#include <QPixmap>
class QColor;
namespace tb::ui::brand {
// Логотип «Щит-жетон» (выбор пользователя 2026-10-20). Координаты нормированы на
// базовую сетку 240×240, как в утверждённом мокапе, и масштабируются под любой размер.
enum class Tone { Normal, Locked }; // Locked — обесцвеченный вариант для заблокированного состояния
QPixmap icon(int size, Tone tone = Tone::Normal);
QPixmap logo(int height); // иконка + ничего более; текст приложения рисуют потребители
QColor accent();          // фирменный акцент (#2f9e6f) для элементов интерфейса
}
