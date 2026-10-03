#include "websocket.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

namespace swapdex::websocket {
namespace {

constexpr std::string_view websocket_guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::size_t maximum_frame_bytes = 16U * 1024U * 1024U;
constexpr std::size_t maximum_header_bytes = 64U * 1024U;

std::once_flag winsock_once;
bool winsock_ready = false;

std::string base64_encode(const std::string_view data) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2U) / 3U * 4U);
    std::size_t index = 0;
    while (index < data.size()) {
        const std::uint32_t a = static_cast<unsigned char>(data[index]);
        const std::uint32_t b = index + 1U < data.size() ? static_cast<unsigned char>(data[index + 1U]) : 0U;
        const std::uint32_t c = index + 2U < data.size() ? static_cast<unsigned char>(data[index + 2U]) : 0U;
        const std::uint32_t packed = (a << 16U) | (b << 8U) | c;
        out.push_back(alphabet[(packed >> 18U) & 63U]);
        out.push_back(alphabet[(packed >> 12U) & 63U]);
        out.push_back(index + 1U < data.size() ? alphabet[(packed >> 6U) & 63U] : '=');
        out.push_back(index + 2U < data.size() ? alphabet[packed & 63U] : '=');
        index += 3U;
    }
    return out;
}

std::string sha1_base64(const std::string_view data) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0) {
        return {};
    }
    std::array<unsigned char, 20> digest{};
    const LONG status = BCryptHash(algorithm, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())), static_cast<ULONG>(data.size()), digest.data(), static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) {
        return {};
    }
    return base64_encode(std::string_view(reinterpret_cast<const char*>(digest.data()), digest.size()));
}

void random_bytes(unsigned char* data, std::size_t count) {
    if (BCryptGenRandom(nullptr, data, static_cast<ULONG>(count), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        // The mask only has to be unpredictable; a counter still beats a constant.
        static unsigned fallback = 0x9e3779b9U;
        for (std::size_t index = 0; index < count; ++index) {
            fallback = fallback * 1664525U + 1013904223U;
            data[index] = static_cast<unsigned char>((fallback >> 16U) & 0xffU);
        }
    }
}

std::string read_http_headers(std::intptr_t socket, std::chrono::steady_clock::time_point deadline, std::string& leftover) {
    std::string headers;
    std::array<char, 8192> chunk{};
    while (headers.find("\r\n\r\n") == std::string::npos) {
        if (headers.size() > maximum_header_bytes) {
            return {};
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return {};
        }
        const auto piece = receive_some(socket, remaining);
        if (!piece.has_value()) {
            return {};
        }
        headers.append(*piece);
    }
    const std::size_t end = headers.find("\r\n\r\n");
    leftover = headers.substr(end + 4U);
    return headers.substr(0, end + 4U);
}

} // namespace

bool initialize() {
    std::call_once(winsock_once, [] {
        WSADATA data{};
        winsock_ready = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    });
    return winsock_ready;
}

std::intptr_t invalid_socket() {
    return static_cast<std::intptr_t>(-1);
}

std::intptr_t listen_loopback(unsigned short& bound_port) {
    if (!initialize()) {
        return invalid_socket();
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return invalid_socket();
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(bound_port);
    if (::bind(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(socket, 4) != 0) {
        ::closesocket(socket);
        return invalid_socket();
    }
    sockaddr_in bound{};
    int length = sizeof(bound);
    if (bound_port == 0 && ::getsockname(socket, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
        bound_port = ntohs(bound.sin_port);
    }
    return static_cast<std::intptr_t>(socket);
}

std::intptr_t connect_loopback(unsigned short port) {
    if (!initialize()) {
        return invalid_socket();
    }
    const SOCKET socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket == INVALID_SOCKET) {
        return invalid_socket();
    }
    u_long non_blocking = 1;
    if (::ioctlsocket(socket, FIONBIO, &non_blocking) != 0) {
        ::closesocket(socket);
        return invalid_socket();
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (WSAGetLastError() != WSAEWOULDBLOCK) {
            ::closesocket(socket);
            return invalid_socket();
        }
        WSAPOLLFD ready{socket, POLLWRNORM, 0};
        if (WSAPoll(&ready, 1, 5000) <= 0 || (ready.revents & POLLWRNORM) == 0) {
            ::closesocket(socket);
            return invalid_socket();
        }
        int socket_error = 0;
        int length = sizeof(socket_error);
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error), &length) != 0 || socket_error != 0) {
            ::closesocket(socket);
            return invalid_socket();
        }
    }
    return static_cast<std::intptr_t>(socket);
}

void close_socket(std::intptr_t socket) {
    if (socket != invalid_socket()) {
        ::closesocket(static_cast<SOCKET>(socket));
    }
}

bool poll_readable(std::intptr_t socket, std::chrono::milliseconds timeout) {
    WSAPOLLFD entry{static_cast<SOCKET>(socket), POLLRDNORM, 0};
    const int ready = WSAPoll(&entry, 1, static_cast<int>(std::max<std::int64_t>(timeout.count(), 0)));
    return ready > 0 && (entry.revents & (POLLRDNORM | POLLHUP | POLLERR)) != 0;
}

bool send_all(std::intptr_t socket, std::string_view data, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const int count = ::send(static_cast<SOCKET>(socket), data.data() + offset, static_cast<int>(std::min<std::size_t>(data.size() - offset, 65536U)), 0);
        if (count > 0) {
            offset += static_cast<std::size_t>(count);
            continue;
        }
        const int error = WSAGetLastError();
        if (count < 0 && error == WSAEWOULDBLOCK) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return false;
            }
            WSAPOLLFD ready{static_cast<SOCKET>(socket), POLLWRNORM, 0};
            if (WSAPoll(&ready, 1, static_cast<int>(remaining.count())) <= 0) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::string> receive_some(std::intptr_t socket, std::chrono::milliseconds timeout, bool* closed) {
    if (closed != nullptr) {
        *closed = false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        std::array<char, 16384> chunk{};
        const int count = ::recv(static_cast<SOCKET>(socket), chunk.data(), static_cast<int>(chunk.size()), 0);
        if (count > 0) {
            return std::string(chunk.data(), static_cast<std::size_t>(count));
        }
        if (count == 0) {
            if (closed != nullptr) {
                *closed = true;
            }
            return std::nullopt;
        }
        const int error = WSAGetLastError();
        if (error == WSAEWOULDBLOCK) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (remaining.count() <= 0) {
                return std::nullopt;
            }
            WSAPOLLFD entry{static_cast<SOCKET>(socket), POLLRDNORM, 0};
            const int ready = WSAPoll(&entry, 1, static_cast<int>(remaining.count()));
            if (ready == 0) {
                // A real timeout: the socket is fine, it just has nothing.
                return std::nullopt;
            }
            if (ready < 0 || (entry.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                if (closed != nullptr) {
                    *closed = true;
                }
                return std::nullopt;
            }
            continue;
        }
        if (closed != nullptr) {
            *closed = true;
        }
        return std::nullopt;
    }
}

std::optional<WebSocket> WebSocket::connect(const std::string& host, unsigned short port, const std::string& path, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const std::intptr_t socket = connect_loopback(port);
    if (socket == invalid_socket()) {
        return std::nullopt;
    }
    unsigned char key_bytes[16] = {0};
    random_bytes(key_bytes, sizeof(key_bytes));
    const std::string key = base64_encode(std::string_view(reinterpret_cast<const char*>(key_bytes), sizeof(key_bytes)));
    std::string request;
    request.reserve(256U + path.size());
    request += "GET " + path + " HTTP/1.1\r\n";
    request += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + key + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    // Chromium's DevTools endpoint silently drops upgrade requests that carry no
    // extra headers beyond the protocol set; a User-Agent is enough to pass.
    // No Origin on purpose: an Origin the browser does not recognize is rejected.
    request += "User-Agent: swapdex\r\n\r\n";
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0 || !send_all(socket, request, remaining)) {
        close_socket(socket);
        return std::nullopt;
    }
    std::string leftover;
    const std::string headers = read_http_headers(socket, deadline, leftover);
    if (headers.empty() || headers.find(" 101") == std::string::npos) {
        close_socket(socket);
        return std::nullopt;
    }
    const std::string expected = sha1_base64(key + std::string(websocket_guid));
    if (headers.find("Sec-WebSocket-Accept: " + expected) == std::string::npos) {
        close_socket(socket);
        return std::nullopt;
    }
    WebSocket ws(socket, true);
    ws.incoming_ = std::move(leftover);
    return ws;
}

std::optional<WebSocket> WebSocket::accept(std::intptr_t listener, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    if (!poll_readable(listener, timeout)) {
        return std::nullopt;
    }
    const SOCKET socket = ::accept(static_cast<SOCKET>(listener), nullptr, nullptr);
    if (socket == INVALID_SOCKET) {
        return std::nullopt;
    }
    u_long non_blocking = 1;
    ::ioctlsocket(socket, FIONBIO, &non_blocking);
    std::string leftover;
    const std::string headers = read_http_headers(static_cast<std::intptr_t>(socket), deadline, leftover);
    if (headers.empty() || headers.find("Upgrade:") == std::string::npos || headers.find("Sec-WebSocket-Key:") == std::string::npos) {
        ::closesocket(socket);
        return std::nullopt;
    }
    const std::string needle = "Sec-WebSocket-Key:";
    std::size_t at = headers.find(needle);
    at += needle.size();
    while (at < headers.size() && headers[at] == ' ') {
        ++at;
    }
    const std::size_t end = headers.find("\r\n", at);
    if (end == std::string::npos) {
        ::closesocket(socket);
        return std::nullopt;
    }
    const std::string key = headers.substr(at, end - at);
    std::string response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + sha1_base64(key + std::string(websocket_guid)) + "\r\n\r\n";
    if (!send_all(static_cast<std::intptr_t>(socket), response, std::chrono::seconds(5))) {
        ::closesocket(socket);
        return std::nullopt;
    }
    WebSocket ws(static_cast<std::intptr_t>(socket), false);
    ws.incoming_ = std::move(leftover);
    return ws;
}

WebSocket::WebSocket(std::intptr_t socket, bool client_side)
    : socket_(socket), client_side_(client_side) {}

WebSocket::WebSocket(WebSocket&& other) noexcept
    : socket_(other.socket_), client_side_(other.client_side_), incoming_(std::move(other.incoming_)), incoming_offset_(other.incoming_offset_) {
    other.socket_ = invalid_socket();
}

WebSocket& WebSocket::operator=(WebSocket&& other) noexcept {
    if (this != &other) {
        close();
        socket_ = other.socket_;
        client_side_ = other.client_side_;
        incoming_ = std::move(other.incoming_);
        incoming_offset_ = other.incoming_offset_;
        other.socket_ = invalid_socket();
    }
    return *this;
}

WebSocket::~WebSocket() {
    close();
}

void WebSocket::close() {
    if (socket_ != invalid_socket()) {
        close_socket(socket_);
        socket_ = invalid_socket();
    }
}

bool WebSocket::fill(std::chrono::steady_clock::time_point deadline) {
    if (incoming_offset_ > 0) {
        incoming_.erase(0, incoming_offset_);
        incoming_offset_ = 0;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
        return false;
    }
    bool closed = false;
    const auto piece = receive_some(socket_, remaining, &closed);
    if (!piece.has_value()) {
        if (closed) {
            close();
        }
        return false;
    }
    incoming_.append(*piece);
    return true;
}

bool WebSocket::read_exact(std::size_t count, std::chrono::steady_clock::time_point deadline, std::string& out) {
    while (incoming_.size() - incoming_offset_ < count) {
        if (!fill(deadline)) {
            return false;
        }
    }
    out.assign(incoming_, incoming_offset_, count);
    incoming_offset_ += count;
    return true;
}

bool WebSocket::send_frame(std::uint8_t opcode, std::string_view payload) {
    std::string frame;
    frame.reserve(payload.size() + 14U);
    frame.push_back(static_cast<char>(0x80U | opcode));
    const std::size_t length = payload.size();
    const std::uint8_t mask_bit = client_side_ ? 0x80U : 0x00U;
    if (length < 126U) {
        frame.push_back(static_cast<char>(mask_bit | static_cast<std::uint8_t>(length)));
    } else if (length <= 0xffffU) {
        frame.push_back(static_cast<char>(mask_bit | 126U));
        frame.push_back(static_cast<char>((length >> 8U) & 0xffU));
        frame.push_back(static_cast<char>(length & 0xffU));
    } else {
        frame.push_back(static_cast<char>(mask_bit | 127U));
        for (int shift = 56; shift >= 0; shift -= 8) {
            frame.push_back(static_cast<char>((static_cast<std::uint64_t>(length) >> shift) & 0xffU));
        }
    }
    if (client_side_) {
        unsigned char mask[4] = {0};
        random_bytes(mask, sizeof(mask));
        frame.append(reinterpret_cast<const char*>(mask), sizeof(mask));
        std::string masked = std::string(payload);
        for (std::size_t index = 0; index < masked.size(); ++index) {
            masked[index] = static_cast<char>(masked[index] ^ mask[index % 4U]);
        }
        frame.append(masked);
    } else {
        frame.append(payload);
    }
    return send_all(socket_, frame, std::chrono::seconds(30));
}

bool WebSocket::send_text(std::string_view payload) {
    return send_frame(0x1U, payload);
}

std::optional<std::string> WebSocket::receive(std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string message;
    for (;;) {
        std::string header;
        if (!read_exact(2U, deadline, header)) {
            return std::nullopt;
        }
        const bool fin = (static_cast<unsigned char>(header[0]) & 0x80U) != 0;
        const std::uint8_t opcode = static_cast<std::uint8_t>(header[0]) & 0x0fU;
        const bool masked = (static_cast<unsigned char>(header[1]) & 0x80U) != 0;
        std::uint64_t length = static_cast<unsigned char>(header[1]) & 0x7fU;
        if (length == 126U) {
            std::string extended;
            if (!read_exact(2U, deadline, extended)) {
                return std::nullopt;
            }
            length = (static_cast<std::uint64_t>(static_cast<unsigned char>(extended[0])) << 8U) | static_cast<unsigned char>(extended[1]);
        } else if (length == 127U) {
            std::string extended;
            if (!read_exact(8U, deadline, extended)) {
                return std::nullopt;
            }
            length = 0;
            for (int index = 0; index < 8; ++index) {
                length = (length << 8U) | static_cast<unsigned char>(extended[static_cast<std::size_t>(index)]);
            }
        }
        if (length > maximum_frame_bytes || message.size() + length > maximum_frame_bytes) {
            // A corrupt stream is not a timeout: report it as dead.
            close();
            return std::nullopt;
        }
        std::string mask_key;
        if (masked && !read_exact(4U, deadline, mask_key)) {
            return std::nullopt;
        }
        std::string payload;
        if (length > 0 && !read_exact(static_cast<std::size_t>(length), deadline, payload)) {
            return std::nullopt;
        }
        if (masked) {
            for (std::size_t index = 0; index < payload.size(); ++index) {
                payload[index] = static_cast<char>(payload[index] ^ mask_key[index % 4U]);
            }
        }
        switch (opcode) {
        case 0x8U:
            // Close: acknowledge and end the stream.
            send_frame(0x8U, {});
            close();
            return std::nullopt;
        case 0x9U:
            send_frame(0xaU, payload);
            continue;
        case 0xaU:
            continue;
        case 0x0U:
        case 0x1U:
        case 0x2U:
            message.append(payload);
            if (fin) {
                return message;
            }
            continue;
        default:
            close();
            return std::nullopt;
        }
    }
}

} // namespace swapdex::websocket

#else

namespace swapdex::websocket {

bool initialize() {
    return false;
}

std::intptr_t invalid_socket() {
    return -1;
}

std::intptr_t listen_loopback(unsigned short& bound_port) {
    bound_port = 0;
    return -1;
}

std::intptr_t connect_loopback(unsigned short) {
    return -1;
}

void close_socket(std::intptr_t) {}

bool poll_readable(std::intptr_t, std::chrono::milliseconds) {
    return false;
}

bool send_all(std::intptr_t, std::string_view, std::chrono::milliseconds) {
    return false;
}

std::optional<std::string> receive_some(std::intptr_t, std::chrono::milliseconds) {
    return std::nullopt;
}

std::optional<WebSocket> WebSocket::connect(const std::string&, unsigned short, const std::string&, std::chrono::milliseconds) {
    return std::nullopt;
}

std::optional<WebSocket> WebSocket::accept(std::intptr_t, std::chrono::milliseconds) {
    return std::nullopt;
}

WebSocket::WebSocket(std::intptr_t socket, bool client_side)
    : socket_(socket), client_side_(client_side) {}

WebSocket::WebSocket(WebSocket&& other) noexcept
    : socket_(other.socket_), client_side_(other.client_side_), incoming_(std::move(other.incoming_)), incoming_offset_(other.incoming_offset_) {
    other.socket_ = -1;
}

WebSocket& WebSocket::operator=(WebSocket&& other) noexcept {
    if (this != &other) {
        socket_ = other.socket_;
        client_side_ = other.client_side_;
        incoming_ = std::move(other.incoming_);
        incoming_offset_ = other.incoming_offset_;
        other.socket_ = -1;
    }
    return *this;
}

WebSocket::~WebSocket() = default;

void WebSocket::close() {
    socket_ = -1;
}

bool WebSocket::fill(std::chrono::steady_clock::time_point) {
    return false;
}

bool WebSocket::read_exact(std::size_t, std::chrono::steady_clock::time_point, std::string&) {
    return false;
}

bool WebSocket::send_frame(std::uint8_t, std::string_view) {
    return false;
}

bool WebSocket::send_text(std::string_view) {
    return false;
}

std::optional<std::string> WebSocket::receive(std::chrono::milliseconds) {
    return std::nullopt;
}

} // namespace swapdex::websocket

#endif
