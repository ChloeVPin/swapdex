#pragma once

#include <functional>
#include <stdexcept>
#include <string>

namespace swapdex::test {

class Failure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void check(bool condition, const std::string& message) {
    if (!condition) {
        throw Failure(message);
    }
}

template <typename Left, typename Right>
void check_equal(const Left& left, const Right& right, const std::string& message) {
    if (!(left == right)) {
        throw Failure(message);
    }
}

using TestFunction = std::function<void()>;

}
