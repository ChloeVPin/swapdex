#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "service.hpp"
#include "shell.hpp"
#include "service_control.hpp"
#include "util.hpp"

namespace {

void print_usage() {
    std::cout << "Usage:\n"
              << "  swapdex install [--no-start]\n"
              << "  swapdex start\n"
              << "  swapdex stop\n"
              << "  swapdex status\n"
              << "  swapdex uninstall [--purge-data]\n"
              << "  swapdex launch\n"
              << "  swapdex list\n"
              << "  swapdex add [account name]\n"
              << "  swapdex accounts\n"
              << "  swapdex version\n"
              << "  swapdex help\n";
}

}

int main(int argc, char** argv) {
    try {
        const std::vector<std::string> arguments(argv + 1, argv + argc);
        if (arguments.empty()) {
            print_usage();
            return 64;
        }
        const std::string command = arguments.front();
        if (command == "help" || command == "--help" || command == "-h") {
            if (arguments.size() != 1) {
                print_usage();
                return 64;
            }
            print_usage();
            return 0;
        }
        if (command == "install" && (arguments.size() == 1 || (arguments.size() == 2 && arguments[1] == "--no-start"))) {
            swapdex::ServiceControl control;
            return control.install(arguments.size() == 1);
        }
        if (command == "start" && arguments.size() == 1) {
            swapdex::ServiceControl control;
            return control.start();
        }
        if (command == "stop" && arguments.size() == 1) {
            swapdex::ServiceControl control;
            return control.stop();
        }
        if (command == "status" && arguments.size() == 1) {
            swapdex::ServiceControl control;
            return control.status();
        }
        if (command == "uninstall" && (arguments.size() == 1 || (arguments.size() == 2 && arguments[1] == "--purge-data"))) {
            swapdex::ServiceControl control;
            return control.uninstall(arguments.size() == 2);
        }
        swapdex::ServiceOptions options;
        if (command == "launch" && arguments.size() == 1) {
            swapdex::Service service(options);
            return service.run();
        }
        if (command == "list" && arguments.size() == 1) {
            swapdex::Service service(options);
            return service.list();
        }
        if (command == "add") {
            std::string label;
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                if (!label.empty()) {
                    label.push_back(' ');
                }
                label.append(arguments[index]);
            }
            swapdex::Service service(options);
            return service.add(std::move(label));
        }
        if (command == "accounts" && arguments.size() == 1) {
            return swapdex::accounts_command();
        }
        if (command == "version" && arguments.size() == 1) {
            std::cout << SWAPDEX_VERSION << "\n";
            return 0;
        }
        print_usage();
        return 64;
    } catch (const swapdex::Error& error) {
        std::cerr << "swapdex: " << error.what() << "\n";
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "swapdex: unexpected failure\n";
        static_cast<void>(error);
        return 1;
    }
}
