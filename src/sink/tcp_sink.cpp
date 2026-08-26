#include "sink/tcp_sink.h"

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace std42::sink {

// ── Client ────────────────────────────────────────────────────────────────

TcpJsonlSink::Client::Client(int fd, TcpJsonlSink* owner) : fd_(fd), owner_(owner) {
    const int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
    // macOS: suppress SIGPIPE per socket. Linux uses MSG_NOSIGNAL on send().
    ::setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}

TcpJsonlSink::Client::~Client() { shutdown(); }

void TcpJsonlSink::Client::start() {
    writer_ = std::thread([this] { write_loop(); });
}

void TcpJsonlSink::Client::enqueue(const std::shared_ptr<const std::string>& line) {
    {
        std::lock_guard<std::mutex> lk(gate_);
        if (closed_.load(std::memory_order_acquire)) return;
        if (queue_.size() >= kMaxQueued) {
            queue_.pop_front();                       // drop oldest
            owner_->dropped_.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(line);
    }
    cv_.notify_one();
}

void TcpJsonlSink::Client::write_loop() {
    for (;;) {
        std::shared_ptr<const std::string> line;
        {
            std::unique_lock<std::mutex> lk(gate_);
            cv_.wait(lk, [this] {
                return !queue_.empty() || closed_.load(std::memory_order_acquire);
            });
            if (closed_.load(std::memory_order_acquire) && queue_.empty()) return;
            line = queue_.front();
            queue_.pop_front();
        }

        const std::string payload = *line + "\n";
        size_t off = 0;
        while (off < payload.size()) {
#ifdef MSG_NOSIGNAL
            const ssize_t n = ::send(fd_, payload.data() + off,
                                     payload.size() - off, MSG_NOSIGNAL);
#else
            const ssize_t n = ::send(fd_, payload.data() + off,
                                     payload.size() - off, 0);
#endif
            if (n > 0) {
                off += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            closed_.store(true, std::memory_order_release);   // peer gone
            return;
        }
    }
}

void TcpJsonlSink::Client::shutdown() {
    bool was_closed = closed_.exchange(true, std::memory_order_acq_rel);
    if (!was_closed && fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
    }
    cv_.notify_all();
    if (writer_.joinable()) writer_.join();
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

// ── Sink ──────────────────────────────────────────────────────────────────

TcpJsonlSink::TcpJsonlSink(int port) : port_(port) {
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        return;
    }

    const int one = 1;
    ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // local consumers only
    addr.sin_port = htons(static_cast<uint16_t>(port_));

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
        ::listen(listen_fd_, 8) < 0) {
        error_ = std::strerror(errno);
        status_ = "error: " + error_;
        ::close(listen_fd_);
        listen_fd_ = -1;
        return;
    }

    status_ = "listening on 127.0.0.1:" + std::to_string(port_);
    accepter_ = std::thread([this] { accept_loop(); });
}

TcpJsonlSink::~TcpJsonlSink() { stop(); }

void TcpJsonlSink::accept_loop() {
    for (;;) {
        const int fd = ::accept(listen_fd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            return;                                   // listener closed
        }
        if (stopping_.load(std::memory_order_acquire)) {
            ::close(fd);
            return;
        }

        auto client = std::make_unique<Client>(fd, this);
        client->start();

        std::lock_guard<std::mutex> lk(gate_);
        // Reap clients whose peer has gone away.
        for (auto it = clients_.begin(); it != clients_.end();) {
            if ((*it)->closed()) it = clients_.erase(it);
            else ++it;
        }
        clients_.push_back(std::move(client));
    }
}

void TcpJsonlSink::write_line(const std::string& line) {
    if (line.empty()) return;
    auto shared = std::make_shared<const std::string>(line);

    std::lock_guard<std::mutex> lk(gate_);
    if (clients_.empty()) return;
    for (auto it = clients_.begin(); it != clients_.end();) {
        if ((*it)->closed()) {
            it = clients_.erase(it);
        } else {
            (*it)->enqueue(shared);
            ++it;
        }
    }
    ++records_sent_;
}

std::string TcpJsonlSink::status() const {
    std::lock_guard<std::mutex> lk(gate_);
    return status_;
}

void TcpJsonlSink::stop() {
    stopping_.store(true, std::memory_order_release);

    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
    if (accepter_.joinable()) accepter_.join();

    std::vector<std::unique_ptr<Client>> doomed;
    {
        std::lock_guard<std::mutex> lk(gate_);
        doomed.swap(clients_);
        status_ = "off";
    }
    for (auto& c : doomed) c->shutdown();
}

TcpSinkSnapshot TcpJsonlSink::snapshot() const {
    std::lock_guard<std::mutex> lk(gate_);
    TcpSinkSnapshot s;
    s.port = port_;
    s.client_count = static_cast<int>(clients_.size());
    s.records_sent = records_sent_;
    s.dropped = dropped_.load(std::memory_order_relaxed);
    s.error_message = error_;
    if (!error_.empty())     s.phase = SinkPhase::Error;
    else if (listen_fd_ >= 0) s.phase = SinkPhase::Active;
    else                      s.phase = SinkPhase::Off;
    return s;
}

} // namespace std42::sink
