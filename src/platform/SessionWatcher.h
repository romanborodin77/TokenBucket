#pragma once
#include <QObject>
#include <QString>
class QWidget;
namespace tb::platform {
class SessionWatcher : public QObject {
    Q_OBJECT
public:
    explicit SessionWatcher(QObject* parent = nullptr);
    ~SessionWatcher() override;
    QString error() const { return error_; }
signals:
    void lockNow();
private slots:
    void onActiveChanged(bool active) { if (active) emit lockNow(); }
private:
    QString error_;
#ifdef Q_OS_WIN
    QWidget* receiver_ = nullptr;
#endif
};
}
