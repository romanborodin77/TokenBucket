#pragma once
#include "AgentPipe.h"
#include <QDeadlineTimer>

namespace tb::agent {
// Owns the pipe and buffers throughout overlapped I/O. Destruction cancels
// unfinished work before freeing any buffer or releasing the pipe handle.
class Exchange {
public:
    Exchange(QByteArray request, const QString& endpointSuffix);
    ~Exchange();
    Exchange(const Exchange&) = delete;
    Exchange& operator=(const Exchange&) = delete;
    bool advance(bool wait = false);
    QByteArrayView reply() const { return QByteArrayView(reply_.constData(), received_); }
    static bool success(QByteArrayView reply) { return !reply.isEmpty() && !reply.startsWith("{\"ok\":false,"); }
private:
    enum class Phase { Writing, Reading, Complete };
    void arm(Phase phase);
    void close();

    Handle pipe_, event_;
    QByteArray request_, reply_;
    OVERLAPPED operation_{};
    QDeadlineTimer deadline_;
    Phase phase_ = Phase::Complete;
    DWORD status_ = ERROR_SUCCESS, bytes_ = 0, received_ = 0;
};
}
