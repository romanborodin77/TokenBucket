#pragma once
#include <QFrame>
#include <QTimer>
#include "../app/Application.h"
class QLabel; class QPushButton; class QPlainTextEdit;
namespace tb::ui {
class EntryCard : public QFrame {
    Q_OBJECT
public:
    explicit EntryCard(Application& app, QWidget* parent = nullptr);
    void showEntry(model::Id id);
    void clear();
    QString secretText() const;
    QPushButton* revealButton() const { return reveal_; }
    void hideNowForTest();
    static bool isSafeTarget(const QString& path, QString* err);
signals:
    void copyRequested(const QString& text);
protected:
    bool eventFilter(QObject*, QEvent*) override;
private:
    bool authorize();
    void copyField(int field);
    void saveSshToFile();
    void updateEnabled();
    Application& app_;
    model::Id id_;
    quint64 selection_ = 0;
    QLabel *title_{}, *metadata_{}, *error_{};
    QPlainTextEdit* value_{};
    QPushButton *reveal_{}, *copySecret_{}, *copyLogin_{}, *copyPhrase_{}, *sshToFile_{};
    QTimer hideTimer_;
};
}
