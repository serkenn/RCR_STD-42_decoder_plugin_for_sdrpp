#pragma once
// JSONL-over-TCP sink.
//
// Listens on a port and broadcasts every record to all connected clients. Each
// client owns a bounded queue drained by its own writer thread, so one stalled
// consumer costs that client its oldest records and never blocks either the
// DSP thread or the other clients.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sink/sink.h"

namespace std42::sink {

struct TcpSinkSnapshot {
    SinkPhase phase = SinkPhase::Off;
    int port = 0;
    int client_count = 0;
    long long records_sent = 0;
    long long dropped = 0;
    std::string error_message;
};

class TcpJsonlSink : public IJsonlSink {
public:
    explicit TcpJsonlSink(int port);
    ~TcpJsonlSink() override;

    void write_line(const std::string& line) override;
    std::string status() const override;
    void stop() override;

    int port() const { return port_; }
    TcpSinkSnapshot snapshot() const;

private:
    // Records queued per client before the oldest is dropped.
    static constexpr size_t kMaxQueued = 2048;

    class Client {
    public:
        Client(int fd, TcpJsonlSink* owner);
        ~Client();

        void start();
        void enqueue(const std::shared_ptr<const std::string>& line);
        void shutdown();
        bool closed() const { return closed_.load(std::memory_order_acquire); }

    private:
        void write_loop();

        int fd_;
        TcpJsonlSink* owner_;
        std::mutex gate_;
        std::condition_variable cv_;
        std::deque<std::shared_ptr<const std::string>> queue_;
        std::atomic<bool> closed_{false};
        std::thread writer_;
    };

    void accept_loop();

    mutable std::mutex gate_;
    int port_;
    int listen_fd_ = -1;
    std::thread accepter_;
    std::vector<std::unique_ptr<Client>> clients_;
    std::string status_;
    std::string error_;
    long long records_sent_ = 0;
    std::atomic<long long> dropped_{0};
    std::atomic<bool> stopping_{false};
};

} // namespace std42::sink
