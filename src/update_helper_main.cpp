#include "ikv/updates.hpp"

#include <iostream>
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>

int main(int argc, char** argv) {
    try {
        if (argc < 2) throw std::runtime_error("usage: ikv-updater install|verify|rollback [--manifest FILE --signature FILE --package FILE | --version VERSION] --trust PUBLIC.pem --root DIRECTORY");
        const std::string command(argv[1]);
        std::set<std::string> expected{"--trust", "--root"};
        if (command == "install") expected.insert({"--manifest", "--signature", "--package"});
        else if (command == "verify") expected.insert("--version");
        else if (command != "rollback") throw std::runtime_error("unknown updater command");
        std::map<std::string, std::string> options;
        for (int i = 2; i < argc; i += 2) {
            const std::string flag(argv[i]);
            if (!expected.contains(flag) || i + 1 >= argc || !options.emplace(flag, argv[i + 1]).second || options.at(flag).empty())
                throw std::runtime_error("unknown, duplicate, empty, or incomplete updater option: " + flag);
        }
        if (options.size() != expected.size()) throw std::runtime_error("missing required updater option");
        ikv::updates::Manifest manifest;
        if (command == "install") manifest = ikv::updates::install(options.at("--manifest"), options.at("--signature"), options.at("--package"), options.at("--trust"), options.at("--root"));
        else if (command == "verify") manifest = ikv::updates::verify_release(options.at("--root"), options.at("--version"), options.at("--trust"));
        else manifest = ikv::updates::rollback(options.at("--root"), options.at("--trust"));
        auto files = nlohmann::json::array();
        for (const auto& file : manifest.files)
            files.push_back({{"path", file.path}, {"size", file.size}, {"sha256", file.sha256}, {"executable", file.executable}});
        std::cout << nlohmann::json{{"schema", 2}, {"sequence", manifest.sequence}, {"version", manifest.version}, {"channel", manifest.channel},
                                  {"package", manifest.package}, {"package_size", manifest.package_size},
                                  {"package_sha256", manifest.package_sha256}, {"files", std::move(files)}}.dump() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
