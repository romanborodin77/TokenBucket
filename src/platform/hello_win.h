#pragma once
#include <QString>
namespace tb::platform::hello {
// Windows Hello как подтверждение владельца (реализация — hello_win.cpp, WinRT
// UserConsentVerifier через interop с HWND). availability() — можно ли предлагать
// Hello при создании хранилища (проверка без диалога, вызывается в фоне);
// request() — сам запрос: модальное окно Hello поверх переданного HWND.
// Границы доверия: подтверждает владельца УЧЁТНОЙ ЗАПИСИ, а не процесса (спека §4).
enum class Availability { Available, NotConfigured, Unavailable };
enum class Result { Verified, Failed, Canceled };
Availability availability();
Result request(void* hwnd, const QString& prompt);
}
