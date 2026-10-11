#include "httplib.h"
#include "json.hpp"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

using json = nlohmann::json;

static const std::string FRONTEND = "/app/frontend";
// static const std::string AGENT = "/app/agent/monitor";
static const std::string AGENT =
    std::getenv("AGENT_PATH")
        ? std::getenv("AGENT_PATH")
        : "/app/agent/monitor";

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
    const std::string command = AGENT;

    FILE* pipe = popen(command.c_str(), "r");

    if (!pipe) {
        throw std::runtime_error("Unable to start system agent");
    }

    std::string output;
    char buffer[4096];

    while (fgets(buffer, sizeof(buffer), pipe)) {
        output += buffer;
    }

    const int status = pclose(pipe);

    if (status != 0) {
        throw std::runtime_error("System agent failed");
    }

    return json::parse(output);
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
                const auto data = run_agent();
                res.set_content(data.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(
                    json{{"detail", "C++ system agent failed"}}.dump(),
                    "application/json"
                );

                std::cerr << "System endpoint: " << e.what() << '\n';
            }
        }
    );

    server.Get("/api/docker",
        [](const httplib::Request&, httplib::Response& res) {
            res.status = 501;
            res.set_content(
                json{
                    {"detail",
                     "Docker monitoring will be implemented in the next step"}
                }.dump(),
                "application/json"
            );
        }
    );

    server.Get("/api/overview",
        [](const httplib::Request&, httplib::Response& res) {
            try {
                auto data = run_agent();

                data["container_count"] = nullptr;
                data["project_count"] = nullptr;
                data["docker_storage"] = json::array();

                res.set_content(data.dump(), "application/json");
            } catch (const std::exception& e) {
                res.status = 500;
                res.set_content(
                    json{{"detail", "Overview unavailable"}}.dump(),
                    "application/json"
                );

                std::cerr << "Overview endpoint: " << e.what() << '\n';
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
