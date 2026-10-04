#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace swapdex::websocket {

// The browser control channel on Windows is a WebSocket to a loopback port, because
// --remote-debugging-pipe is POSIX only. This is the minimal client plus the server
// side the CDP test double needs: HTTP upgrade, text frames, ping and close.
// Windows requires that sockets are masked from client to server and unmasked back.

// Winsock must be started once per process. Safe to call from anywhere.
bool initialize();

// Loopback TCP helpers. Sockets are reported as integers so no caller ever sees a
// platform handle type.
std::intptr_t invalid_socket();
std::intptr_t listen_loopback(unsigned short& bound_port);
std::intptr_t connect_loopback(unsigned short port);
void close_socket(std::intptr_t socket);
// True when the socket has data or reached end of stream within the timeout.
bool poll_readable(std::intptr_t socket, std::chrono::milliseconds timeout);
bool send_all(std::intptr_t socket, std::string_view data, std::chrono::milliseconds timeout);
// A read bounded by timeout. Null means no data arrived in time; *closed reports
// whether the peer ended the stream or the socket broke, as opposed to a timeout.
std::optional<std::string> receive_some(std::intptr_t socket, std::chrono::milliseconds timeout, bool* closed = nullptr);

class WebSocket {
public:
    // Connects to host:port and performs the client upgrade for path. Null when the
    // peer is not a websocket endpoint.
    static std::optional<WebSocket> connect(const std::string& host, unsigned short port, const std::string& path, std::chrono::milliseconds timeout);
    // Accepts one connection on a listening socket and performs the server upgrade.
    static std::optional<WebSocket> accept(std::intptr_t listener, std::chrono::milliseconds timeout);

    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;
    WebSocket(WebSocket&& other) noexcept;
    WebSocket& operator=(WebSocket&& other) noexcept;
    ~WebSocket();

    bool send_text(std::string_view payload);
    // The next complete text message, reassembled across continuation frames. Ping
    // frames are answered and close frames end the stream, both without surfacing.
    std::optional<std::string> receive(std::chrono::milliseconds timeout);
    void close();
    bool open() const { return socket_ != invalid_socket(); }

private:
    WebSocket(std::intptr_t socket, bool client_side);

    bool send_frame(std::uint8_t opcode, std::string_view payload);
    // Reads raw bytes into the receive buffer, waiting no longer than the deadline.
    bool fill(std::chrono::steady_clock::time_point deadline);
    bool read_exact(std::size_t count, std::chrono::steady_clock::time_point deadline, std::string& out);

    std::intptr_t socket_ = -1;
    bool client_side_ = true;
    std::string incoming_;
    std::size_t incoming_offset_ = 0;
};

}
