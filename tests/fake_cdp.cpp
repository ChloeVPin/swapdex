#include <array>
#include <csignal>
#include <string>
#include <string_view>
#if defined(_WIN32)
#include <fstream>
#include <filesystem>
#include "websocket.hpp"
#else
#include <unistd.h>
#endif

#if defined(_WIN32)
// The Windows build drives the app over a WebSocket on a loopback port. This double
// plays the app side: it publishes a DevToolsActivePort file exactly like the real
// browser and then serves the same canned answers over the socket.
namespace {

std::filesystem::path user_data_dir(int argc, char** argv) {
    constexpr std::string_view prefix = "--user-data-dir=";
    for (int i = 1; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument.rfind(std::string(prefix), 0) == 0) {
            return argument.substr(prefix.size());
        }
    }
    return {};
}

void answer(swapdex::websocket::WebSocket& socket, const std::string& line, bool& done) {
    if (line.find("\"id\":1") != std::string::npos) {
        socket.send_text("{\"id\":1,\"result\":{\"value\":1}}");
    } else if (line.find("\"id\":2") != std::string::npos) {
        socket.send_text("{\"id\":2,\"result\":{\"value\":2}}");
        socket.send_text("{\"method\":\"Test.event\",\"payload\":\"ok\"}");
    } else if (line.find("\"id\":3") != std::string::npos) {
        socket.send_text("{\"id\":3,\"result\":{}}");
        done = true;
    }
}

}

int main(int argc, char** argv) {
    const std::filesystem::path directory = user_data_dir(argc, argv);
    if (directory.empty() || !swapdex::websocket::initialize()) {
        return 1;
    }
    unsigned short port = 0;
    const std::intptr_t listener = swapdex::websocket::listen_loopback(port);
    if (listener == swapdex::websocket::invalid_socket()) {
        return 1;
    }
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    {
        std::ofstream marker(directory / "DevToolsActivePort", std::ios::trunc);
        marker << port << "\n/devtools/browser/fake\n";
    }
    bool done = false;
    while (!done) {
        auto socket = swapdex::websocket::WebSocket::accept(listener, std::chrono::seconds(30));
        if (!socket.has_value()) {
            break;
        }
        while (socket->open() && !done) {
            const auto message = socket->receive(std::chrono::seconds(30));
            if (!message.has_value()) {
                break;
            }
            answer(*socket, *message, done);
        }
    }
    swapdex::websocket::close_socket(listener);
    return 0;
}
#else
namespace {

void write_all(int descriptor, std::string_view value) {
    std::size_t offset = 0;
    while (offset < value.size()) {
        const ssize_t count = write(descriptor, value.data() + offset, value.size() - offset);
        if (count < 0) {
            return;
        }
        offset += static_cast<std::size_t>(count);
    }
}

}

int main() {
    std::signal(SIGPIPE, SIG_IGN);
    std::string buffer;
    std::array<char, 4096> chunk {};
    for (;;) {
        const ssize_t count = read(3, chunk.data(), chunk.size());
        if (count <= 0) {
            return 0;
        }
        buffer.append(chunk.data(), static_cast<std::size_t>(count));
        for (;;) {
            const std::size_t delimiter = buffer.find('\0');
            if (delimiter == std::string::npos) {
                break;
            }
            const std::string line = buffer.substr(0, delimiter);
            buffer.erase(0, delimiter + 1U);
            if (line.find("\"id\":1") != std::string::npos) {
                write_all(4, "{\"id\":1,\"result\":{\"value\":1}}");
                write_all(4, std::string_view("\0", 1));
            } else if (line.find("\"id\":2") != std::string::npos) {
                write_all(4, "{\"id\":2,");
                write_all(4, "\"result\":{\"value\":2}}");
                write_all(4, std::string_view("\0", 1));
                write_all(4, "{\"method\":\"Test.event\",\"payload\":\"ok\"}");
                write_all(4, std::string_view("\0", 1));
            } else if (line.find("\"id\":3") != std::string::npos) {
                write_all(4, "{\"id\":3,\"result\":{}}");
                write_all(4, std::string_view("\0", 1));
                return 0;
            }
        }
    }
}
#endif
