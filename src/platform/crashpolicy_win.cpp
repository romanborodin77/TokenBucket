#include "CrashPolicy.h"
#include <QtGlobal>
#ifdef Q_OS_WIN
#include <windows.h>
// werapi.h объявляет WerSetFlags и флаги запрета снимков; функцию берём динамически,
// чтобы сборка не зависела от наличия wer.lib на машине разработчика.
#include <werapi.h>
namespace tb::platform::crash {
void disableDumps() {
    // 1) Свои диалоги об ошибках вместо системных: без окна GPF WER не запускает
    //    интерактивный отчёт от имени процесса.
    const UINT previous = SetErrorMode(0);
    SetErrorMode(previous | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    // 2) Прямой запрет WER-снимков (куча + регистры) для этого процесса при аварии
    //    и зависании. Успех не критичен: на машинах без WER-политик снимков и так нет.
    if (HMODULE wer = LoadLibraryW(L"wer.dll")) {
        using WerSetFlags_t = HRESULT(WINAPI*)(DWORD);
        if (auto set = reinterpret_cast<WerSetFlags_t>(GetProcAddress(wer, "WerSetFlags"))) {
            set(WER_FAULT_REPORTING_DISABLE_SNAPSHOT_CRASH | WER_FAULT_REPORTING_DISABLE_SNAPSHOT_HANG);
        }
        // Библиотеку не освобождаем: флаги действуют на весь срок жизни процесса.
    }
}
}
#endif
