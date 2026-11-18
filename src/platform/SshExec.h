#pragma once
#include "SshExecRequest.h"
#include "model/Types.h"
#include <QByteArray>
#include <atomic>
#include <thread>

namespace tb::agent {
struct SshResult {
    const char* error = nullptr;
    int exitCode = 0;
    QByteArray out, err;
};

class SshExecution {
public:
    SshExecution(const model::SshTarget& target, const SshCommand& command, QByteArrayView privateKey,
                 const QString& passphrase, const QString& testProgram = {}, const QString& testKnownHosts = {});
    void cancel() { worker_.request_stop(); }
    const SshResult* result() const { return done_.load(std::memory_order_acquire) ? &result_ : nullptr; }

private:
    SshResult result_;
    std::atomic<bool> done_ = false;
    std::jthread worker_;
};

bool isSshAskpass();
int runSshAskpass();
void cleanAbandonedSshStages();
}
