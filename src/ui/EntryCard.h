#pragma once
#include <QFrame>
#include <QTimer>
#include "../app/Application.h"
class QLabel; class QPushButton; class QPlainTextEdit;
namespace tb::ui {
// Карточка выбранной записи: показывает несекретные поля сразу, секрет — только
// по кнопке «Показать» (и не дольше 15 с — hideTimer_ прячет значение, если
// пользователь отвлёкся). Записи с reverify требуют VerifyDialog до показа/копирования
// секрета и до редактора (authorize()). Все операции идут через Application::commitEntry
// — точечный откат при отказе сохранения. isSafeTarget защищает сохранение SSH-ключа
// в файл: отказ для каталогов вне профиля пользователя и путей с подстановкой.
class EntryCard : public QFrame {
    Q_OBJECT
public:
    explicit EntryCard(Application& app, QWidget* parent = nullptr);
    void showEntry(model::Id id);
    void clear();
    QString secretText() const;                       // для тестов
    QPushButton* revealButton() const { return reveal_; }
    void hideNowForTest();
    void setRevealSeconds(int seconds);
    void copySecret() { copyField(0); }
    static bool isSafeTarget(const QString& path, QString* err);
signals:
    void copyRequested(const QString& text);          // буфером владеет MainWindow (единый ClipboardGuard)
    void editRequested();
    void moreRequested();
    void documentRequested(model::Id);
protected:
    bool eventFilter(QObject*, QEvent*) override;     // клик мимо карточки мгновенно прячет секрет
    void changeEvent(QEvent*) override;
private:
    bool authorize();
    void copyField(int field);
    void saveSshToFile();
    void updateEnabled();
    void applyAccent();
    Application& app_;
    model::Id id_;
    quint64 selection_ = 0; // generation при показе: модель могла очиститься в QtConcurrent — ссылки на Entry не хранятся
    QLabel *title_{}, *identity_{}, *flags_{}, *metadata_{}, *error_{},*valueLabel_{};
    QPlainTextEdit* value_{};
    QPushButton *reveal_{}, *copySecret_{}, *copyLogin_{}, *copyPhrase_{}, *sshToFile_{};
    QPushButton *edit_{}, *more_{}, *document_{};
    QTimer hideTimer_;
};
}
