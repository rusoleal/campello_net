#include <campello_net/version.hpp>

#include <iostream>
#include <string>

int main() {
    using namespace systems::leal::campello_net;

    std::cout << "campello_net " << version_string() << "\n";
    std::cout << "Phase 0 echo example — build pipeline validated.\n";

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "quit") {
            break;
        }
        std::cout << "Echo: " << line << "\n";
    }

    return 0;
}
