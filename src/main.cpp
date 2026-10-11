#include "httplib.h"
#include "json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <sys/wait.h>

using json = nlohmann::json;

static std::string env_or(
    const char* name,
    const std::string& fallback
) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}

static const std::string FRONTEND =
    env_or("FRONTEND_PATH", "/app/frontend");

static const std::string AGENT =
    env_or("AGENT_PATH", "/app/agent/monitor");

struct CommandResult {
    std::string output;
    int status;
};

CommandResult run_command(const std::string& command) {
    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe) {
        throw std::runtime_error("Unable to execute command");
    }

    std::string output;
    char buffer[4096];

    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    int status = pclose(pipe);

    if (status == -1) {
        throw std::runtime_error("Unable to collect command status");
    }

    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    } else {
        status = 128;
    }

    return {output, status};
}

std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);

    if (!file) {
        return {};
    }

    return std::string(
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()
    );
}

json run_agent() {
    auto result = run_command("'" + AGENT + "' 2>/dev/null");

    if (result.status != 0) {
        throw std::runtime_error("System agent failed");
    }

    return json::parse(result.output);
}

bool docker_available() {
    try {
        return run_command("docker info 2>/dev/null").status == 0;
    } catch (...) {
        return false;
    }
}

std::map<std::string, std::string> parse_labels(
    const std::string& value
) {
    std::map<std::string, std::string> labels;
    std::size_t start = 0;

    while (start < value.size()) {
        const auto end = value.find(',', start);
        const auto part = value.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start
        );

        const auto separator = part.find('=');

        if (separator != std::string::npos) {
            labels[part.substr(0, separator)] =
                part.substr(separator + 1);
        }

        if (end == std::string::npos) {
            break;
        }

        start = end + 1;
    }

    return labels;
}

json docker_data() {
    if (!docker_available()) {
        throw std::runtime_error(
            "Docker unavailable or permission denied"
        );
    }

    const auto raw = run_command(
        "docker ps -a --size --format '{{json .}}' 2>/dev/null"
    );

    if (raw.status != 0) {
        throw std::runtime_error("Unable to list Docker containers");
    }

    json containers = json::array();

    std::size_t start = 0;

    while (start < raw.output.size()) {
        const auto end = raw.output.find('\n', start);
        const auto line = raw.output.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start
        );

        if (!line.empty()) {
            const auto item = json::parse(line);

            const auto labels = parse_labels(
                item.value("Labels", std::string(""))
            );

            const auto project_it =
                labels.find("com.docker.compose.project");

            const std::string project =
                project_it != labels.end()
                    ? project_it->second
                    : "standalone";

            containers.push_back({
                {"id", item.value("ID", json(nullptr))},
                {"name", item.value("Names", json(nullptr))},
                {"image", item.value("Image", json(nullptr))},
                {"status", item.value("Status", json(nullptr))},
                {"size", item.value("Size", json(nullptr))},
                {"project", project},
                {"ports", item.value("Ports", std::string(""))}
            });
        }

        if (end == std::string::npos) {
            break;
        }

        start = end + 1;
    }

    json storage = json::array();

    const auto storage_result = run_command(
        "docker system df --format '{{json .}}' 2>/dev/null"
    );

    if (storage_result.status != 0) {
        throw std::runtime_error("Unable to read Docker storage");
    }

    start = 0;

    while (start < storage_result.output.size()) {
        const auto end = storage_result.output.find('\n', start);
        const auto line = storage_result.output.substr(
            start,
            end == std::string::npos
                ? std::string::npos
                : end - start
        );

        if (!line.empty()) {
            try {
                storage.push_back(json::parse(line));
            } catch (const json::parse_error&) {
                storage.push_back({{"raw", line}});
            }
        }

        if (end == std::string::npos) {
            break;
        }

        start = end + 1;
    }

    std::map<std::string, json> grouped;

    for (const auto& container : containers) {
        const std::string name = container["project"].get<std::string>();

        if (grouped.find(name) == grouped.end()) {
            grouped[name] = json::array();
        }

        grouped[name].push_back(container);
    }

    json projects = json::array();

    for (const auto& [name, items] : grouped) {
        projects.push_back({
            {"name", name},
            {"container_count", items.size()},
            {"containers", items}
        });
    }

    return {
        {"containers", containers},
        {"projects", projects},
        {"storage", storage}
    };
}

int main() {
    httplib::Server server;

    server.Get("/", [](const httplib::Request&, httplib::Response& res) {
        const auto html = read_file(FRONTEND + "/index.html");

        if (html.empty()) {
            res.status = 500;
            res.set_content("Dashboard file not found", "text/plain");
            return;
        }

        res.set_content(html, "text/html; charset=utf-8");
    });

    server.Get("/api/health",
        [](const httplib::Request&, httplib::Response& res) {
            res.set_content(
                R"({"status":"ok","service":"vps-monitor"})",
                "application/json"
            );
        }
    );

    server.Get("/api/system",
        [](const httplib::Request&, httplib::Response& res) {
            try {
                res.set_content(run_agent().dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(
                    json{{"detail", "C++ agent failed"}}.dump(),
                    "application/json"
                );
                std::cerr << "System endpoint: "
                          << e.what() << '\n';
            }
        }
    );

    server.Get("/api/docker",
        [](const httplib::Request&, httplib::Response& res) {
            try {
                res.set_content(docker_data().dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 503;
                res.set_content(
                    json{{"detail",
                          "Docker unavailable or monitoring failed"}}.dump(),
                    "application/json"
                );
                std::cerr << "Docker endpoint: "
                          << e.what() << '\n';
            }
        }
    );

    server.Get("/api/overview",
        [](const httplib::Request&, httplib::Response& res) {
            try {
                auto system = run_agent();
                const auto docker = docker_data();

                system["container_count"] =
                    docker["containers"].size();

                system["project_count"] =
                    docker["projects"].size();

                system["docker_storage"] = docker["storage"];

                res.set_content(system.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 503;
                res.set_content(
                    json{{"detail", "Overview unavailable"}}.dump(),
                    "application/json"
                );
                std::cerr << "Overview endpoint: "
                          << e.what() << '\n';
            }
        }
    );

    server.set_error_handler(
        [](const httplib::Request&, httplib::Response& res) {
            if (res.body.empty()) {
                res.set_content(
                    json{{"detail", "HTTP error"}}.dump(),
                    "application/json"
                );
            }
        }
    );

    const char* port_env = std::getenv("PORT");
    const int port = port_env ? std::stoi(port_env) : 8765;

    std::cout << "VPS Monitor C++ server listening on port "
              << port << std::endl;

    if (!server.listen("0.0.0.0", port)) {
        std::cerr << "Failed to start HTTP server\n";
        return 1;
    }

    return 0;
}










// #include "httplib.h"
// #include "json.hpp"

// #include <cstdlib>
// #include <fstream>
// #include <iostream>
// #include <iterator>
// #include <string>

// using json = nlohmann::json;

// static const std::string FRONTEND = "/app/frontend";
// // static const std::string AGENT = "/app/agent/monitor";
// static const std::string AGENT =
//     std::getenv("AGENT_PATH")
//         ? std::getenv("AGENT_PATH")
//         : "/app/agent/monitor";

// std::string read_file(const std::string& path) {
//     std::ifstream file(path, std::ios::binary);

//     if (!file) {
//         return {};
//     }

//     return std::string(
//         std::istreambuf_iterator<char>(file),
//         std::istreambuf_iterator<char>()
//     );
// }

// json run_agent() {
//     const std::string command = AGENT;

//     FILE* pipe = popen(command.c_str(), "r");

//     if (!pipe) {
//         throw std::runtime_error("Unable to start system agent");
//     }

//     std::string output;
//     char buffer[4096];

//     while (fgets(buffer, sizeof(buffer), pipe)) {
//         output += buffer;
//     }

//     const int status = pclose(pipe);

//     if (status != 0) {
//         throw std::runtime_error("System agent failed");
//     }

//     return json::parse(output);
// }

// int main() {
//     httplib::Server server;

//     server.Get("/", [](const httplib::Request&, httplib::Response& res) {
//         const auto html = read_file(FRONTEND + "/index.html");

//         if (html.empty()) {
//             res.status = 500;
//             res.set_content("Dashboard file not found", "text/plain");
//             return;
//         }

//         res.set_content(html, "text/html; charset=utf-8");
//     });

//     server.Get("/api/health",
//         [](const httplib::Request&, httplib::Response& res) {
//             res.set_content(
//                 R"({"status":"ok","service":"vps-monitor"})",
//                 "application/json"
//             );
//         }
//     );

//     server.Get("/api/system",
//         [](const httplib::Request&, httplib::Response& res) {
//             try {
//                 const auto data = run_agent();
//                 res.set_content(data.dump(), "application/json");
//             } catch (const std::exception& e) {
//                 res.status = 500;
//                 res.set_content(
//                     json{{"detail", "C++ system agent failed"}}.dump(),
//                     "application/json"
//                 );

//                 std::cerr << "System endpoint: " << e.what() << '\n';
//             }
//         }
//     );

//     server.Get("/api/docker",
//         [](const httplib::Request&, httplib::Response& res) {
//             res.status = 501;
//             res.set_content(
//                 json{
//                     {"detail",
//                      "Docker monitoring will be implemented in the next step"}
//                 }.dump(),
//                 "application/json"
//             );
//         }
//     );

//     server.Get("/api/overview",
//         [](const httplib::Request&, httplib::Response& res) {
//             try {
//                 auto data = run_agent();

//                 data["container_count"] = nullptr;
//                 data["project_count"] = nullptr;
//                 data["docker_storage"] = json::array();

//                 res.set_content(data.dump(), "application/json");
//             } catch (const std::exception& e) {
//                 res.status = 500;
//                 res.set_content(
//                     json{{"detail", "Overview unavailable"}}.dump(),
//                     "application/json"
//                 );

//                 std::cerr << "Overview endpoint: " << e.what() << '\n';
//             }
//         }
//     );

//     server.set_error_handler(
//         [](const httplib::Request&, httplib::Response& res) {
//             if (res.body.empty()) {
//                 res.set_content(
//                     json{{"detail", "HTTP error"}}.dump(),
//                     "application/json"
//                 );
//             }
//         }
//     );

//     const char* port_env = std::getenv("PORT");
//     const int port = port_env ? std::stoi(port_env) : 8765;

//     std::cout << "VPS Monitor C++ server listening on port "
//               << port << std::endl;

//     if (!server.listen("0.0.0.0", port)) {
//         std::cerr << "Failed to start HTTP server\n";
//         return 1;
//     }

//     return 0;
// }
