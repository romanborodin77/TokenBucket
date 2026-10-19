#pragma once
#include <QObject>
#include <QTimer>
namespace tb {
class ClipboardGuard : public QObject {
    Q_OBJECT
public:
    explicit ClipboardGuard(QObject* parent = nullptr);
    ~ClipboardGuard() override;
    bool copySecret(const QString& text, QString* err);
    void clearIfOurs();
    void triggerClearNowForTest() { clearIfOurs(); }
signals:
    void cleared();
private:
    QTimer timer_;
    quint64 epoch_ = 0;
    bool copying_ = false, clearing_ = false, destroying_ = false;
#ifdef Q_OS_WIN
    bool open(quint64 epoch);
    void* owner_ = nullptr;
    unsigned long sequence_ = 0;
#else
    QByteArray token_;
#endif
};
}
