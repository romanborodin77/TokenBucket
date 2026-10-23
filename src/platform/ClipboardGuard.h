#pragma once
#include <QObject>
#include <QTimer>
namespace tb {
// Выдача секрета в буфер обмена с гарантированной очисткой: своя публикация
// запоминается «эпохой» (epoch_) и снимается через 20 с, ЧУЖОЕ содержимое,
// появившееся после нас, не трогается (пользователь успел скопировать своё).
// На Windows публикация помечает данные исключёнными из истории буфера и
// облачной синхронизации (CanIncludeInClipboardHistory и др.). Флаг destroying_
// в деструкторе делает до 5 попыток очистки: буфер может быть занят другим
// процессом, сдаваться сразу — оставить секрет в системе.
class ClipboardGuard : public QObject {
    Q_OBJECT
public:
    explicit ClipboardGuard(QObject* parent = nullptr);
    ~ClipboardGuard() override;
    bool copySecret(const QString& text, QString* err);
    struct Snapshot { bool owned = false; bool clearPending = false; int remainingMs = -1; };
    Snapshot snapshot() const;
    void cancelPendingCopy() { if (copying_) ++epoch_; }
    void clearIfOurs();
    void triggerClearNowForTest() { clearIfOurs(); }
signals:
    void cleared(); // для уведомления трея «Буфер обмена очищен»
private:
    QTimer timer_;
    quint64 epoch_ = 0;   // счётчик наших публикаций: очистка применяется только к последней
    bool copying_ = false, clearing_ = false, destroying_ = false;
    bool clearPending_ = false;
#ifdef Q_OS_WIN
    bool open(quint64 epoch);
    void* owner_ = nullptr;          // наше окно-владелец публикации (формат ExcludeClipboardContentFromMonitorProcessing требует HWND)
    unsigned long sequence_ = 0;     // sequence number публикации: чужая запись заменила нашу — очистка не нужна
#else
    QByteArray token_;
#endif
};
}
