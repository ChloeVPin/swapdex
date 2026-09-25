#include <array>
#include <csignal>
#include <string>
#include <string_view>
#include <unistd.h>

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
