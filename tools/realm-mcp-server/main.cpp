#include "MCPServer.hpp"
#include "RealmControlClient.hpp"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>

using namespace Realm::MCP;

static bool validRealmName(std::string_view name) {
    return !name.empty() && name.size() <= 128 && std::ranges::none_of(name, [](unsigned char character) { return character < 0x20 || character == 0x7F; });
}

static void printUsage(std::ostream& output) {
    output << "usage: hyprland-realm-mcp-server [--realm NAME] [--socket ABSOLUTE_PATH]\n";
}

int main(int argc, char** argv) {
    std::optional<std::string>           realm;
    std::optional<std::filesystem::path> socketPath;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--help") {
            printUsage(std::cout);
            return 0;
        }
        if ((argument == "--realm" || argument == "--socket") && index + 1 >= argc) {
            printUsage(std::cerr);
            return 2;
        }
        if (argument == "--realm" && !realm) {
            realm = argv[++index];
            continue;
        }
        if (argument == "--socket" && !socketPath) {
            socketPath = argv[++index];
            continue;
        }
        std::cerr << "unknown or duplicate argument\n";
        printUsage(std::cerr);
        return 2;
    }

    if (realm && !validRealmName(*realm)) {
        std::cerr << "when supplied, --realm must be 1 to 128 bytes without control characters\n";
        return 2;
    }
    if (!socketPath) {
        auto discovered = CRealmControlClient::discoverSocketPath();
        if (!discovered) {
            std::cerr << discovered.error() << '\n';
            return 1;
        }
        socketPath = std::move(*discovered);
    }

    CRealmControlClient client{std::move(*socketPath)};
    if (auto connected = client.connect(); !connected) {
        std::cerr << connected.error() << '\n';
        return 1;
    }

    CMCPServer server{client, std::move(realm)};
    return server.run();
}
