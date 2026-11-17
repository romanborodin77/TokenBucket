#include "AgentClient.h"

namespace tb::agent {
Exchange::Exchange(QByteArray request, const QString& endpointSuffix)
    : pipe_(openClient(Endpoint::current(), endpointSuffix)), request_(std::move(request)) {
    if (pipe_.get() == INVALID_HANDLE_VALUE) return;
    event_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (event_) arm(Phase::Writing);
}

Exchange::~Exchange() {
    close();
    if (!reply_.isEmpty()) SecureZeroMemory(reply_.data(), static_cast<size_t>(reply_.size()));
}

void Exchange::arm(Phase phase) {
    phase_ = phase;
    bytes_ = 0;
    operation_ = {};
    operation_.hEvent = event_.get();
    ResetEvent(event_.get());
    BOOL done;
    if (phase == Phase::Writing) {
        deadline_ = QDeadlineTimer(RequestTimeoutMs, Qt::PreciseTimer);
        done = WriteFile(pipe_.get(), request_.constData(), static_cast<DWORD>(request_.size()), &bytes_, &operation_);
    } else {
        deadline_ = QDeadlineTimer(ApprovalTimeoutMs + RequestTimeoutMs + ReplyTimeoutMs, Qt::PreciseTimer);
        done = ReadFile(pipe_.get(), reply_.data(), MaxReply, &bytes_, &operation_);
    }
    status_ = done ? ERROR_SUCCESS : GetLastError();
}

void Exchange::close() {
    if (status_ == ERROR_IO_PENDING) {
        CancelIoEx(pipe_.get(), &operation_);
        DWORD ignored = 0;
        GetOverlappedResult(pipe_.get(), &operation_, &ignored, TRUE);
    }
    status_ = ERROR_SUCCESS;
    pipe_.reset();
    phase_ = Phase::Complete;
}

bool Exchange::advance(bool wait) {
    while (phase_ != Phase::Complete) {
        if (deadline_.hasExpired()) { close(); break; }
        if (status_ == ERROR_IO_PENDING) {
            const DWORD timeout = wait ? static_cast<DWORD>(deadline_.remainingTime()) : 0;
            const auto ready = WaitForSingleObject(event_.get(), timeout);
            if (ready == WAIT_TIMEOUT && !wait) return false;
            if (ready != WAIT_OBJECT_0) { close(); break; }
            status_ = GetOverlappedResult(pipe_.get(), &operation_, &bytes_, FALSE) ? ERROR_SUCCESS : GetLastError();
        }
        if (status_ != ERROR_SUCCESS) { close(); break; }
        if (phase_ == Phase::Writing && bytes_ == request_.size()) {
            reply_.resizeForOverwrite(MaxReply);
            arm(Phase::Reading);
        } else {
            if (phase_ == Phase::Reading) {
                const QByteArrayView response(reply_.constData(), bytes_);
                if (response.startsWith('{') && response.endsWith("}\n")) received_ = bytes_;
            }
            close();
        }
    }
    return true;
}
}
