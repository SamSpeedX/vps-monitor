#include "httplib.h"
#include "json.hpp"

#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>

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

using Clock = std::chrono::steady_clock;

static constexpr auto SESSION_LIFETIME = std::chrono::hours(8);

static std::mutex sessions_mutex;

static std::unordered_map<
    std::string,
    Clock::time_point
> sessions;

static bool secure_cookies() {
    return env_or("COOKIE_SECURE", "true") == "true";
}

static std::string generate_session_token() {
    std::random_device random;
    std::uniform_int_distribution<unsigned int> byte(0, 255);

    std::ostringstream result;
    result << std::hex << std::setfill('0');

    for (int i = 0; i < 32; ++i) {
        result << std::setw(2) << byte(random);
    }

    return result.str();
}


static std::string get_session_cookie(const httplib::Request& req) {
    const std::string cookies = req.get_header_value("Cookie");
    const std::string key = "vps_session=";

    std::istringstream stream(cookies);
    std::string item;

    while (std::getline(stream, item, ';')) {
        const auto first = item.find_first_not_of(" \t");

        if (first == std::string::npos) {
            continue;
        }

        item.erase(0, first);

        if (item.compare(0, key.size(), key) == 0) {
            return item.substr(key.size());
        }
    }

    return {};
}

static bool authenticated(const httplib::Request& req) {
    const std::string token = get_session_cookie(req);

    if (token.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(sessions_mutex);

    const auto it = sessions.find(token);

    if (it == sessions.end()) {
        return false;
    }

    if (Clock::now() >= it->second) {
        sessions.erase(it);
        return false;
    }

    return true;
}

static bool constant_time_equal(
    const std::string& a,
    const std::string& b
) {
    const std::size_t length = std::max(a.size(), b.size());
    unsigned int difference =
        static_cast<unsigned int>(a.size() ^ b.size());

    for (std::size_t i = 0; i < length; ++i) {
        const unsigned char x =
            i < a.size() ? static_cast<unsigned char>(a[i]) : 0;

        const unsigned char y =
            i < b.size() ? static_cast<unsigned char>(b[i]) : 0;

        difference |= x ^ y;
    }

    return difference == 0;
}

static bool require_auth(
    const httplib::Request& req,
    httplib::Response& res
) {
    if (authenticated(req)) {
        return true;
    }

    res.status = 401;
    res.set_header("Cache-Control", "no-store");
    res.set_content(
        json{
            {"success", false},
            {"message", "Authentication required"}
        }.dump(),
        "application/json"
    );

    return false;
}

static void set_session_cookie(
    httplib::Response& res,
    const std::string& token
) {
    std::string cookie =
        "vps_session=" + token +
        "; Path=/; HttpOnly; SameSite=Strict; Max-Age=28800";

    if (secure_cookies()) {
        cookie += "; Secure";
    }

    res.set_header("Set-Cookie", cookie);
    res.set_header("Cache-Control", "no-store");
}

static std::string env_or(
    const char* name,
    const std::string& fallback
) {
    const char* value = std::getenv(name);
    return value && *value ? value : fallback;
}


static std::string auth_db_path() {
    return env_or("AUTH_DB_PATH", "/data/vps-monitor.sqlite");
}

static std::string bytes_to_hex(
    const unsigned char* data,
    std::size_t length
) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(length * 2);

    for (std::size_t i = 0; i < length; ++i) {
        result.push_back(hex[data[i] >> 4]);
        result.push_back(hex[data[i] & 0x0f]);
    }

    return result;
}

static std::string generate_initial_password() {
    std::array<unsigned char, 24> bytes{};

    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        throw std::runtime_error("Secure password generation failed");
    }

    return bytes_to_hex(bytes.data(), bytes.size());
}

static std::string hash_password(
    const std::string& password,
    const std::string& salt
) {
    std::array<unsigned char, 32> hash{};

    if (PKCS5_PBKDF2_HMAC(
            password.c_str(),
            static_cast<int>(password.size()),
            reinterpret_cast<const unsigned char*>(salt.data()),
            static_cast<int>(salt.size()),
            210000,
            EVP_sha256(),
            static_cast<int>(hash.size()),
            hash.data()
        ) != 1) {
        throw std::runtime_error("Password hashing failed");
    }

    return bytes_to_hex(hash.data(), hash.size());
}

static sqlite3* open_auth_db() {
    sqlite3* db = nullptr;

    if (sqlite3_open(auth_db_path().c_str(), &db) != SQLITE_OK) {
        std::string error = db ? sqlite3_errmsg(db) : "SQLite open failed";

        if (db) {
            sqlite3_close(db);
        }

        throw std::runtime_error(error);
    }

    sqlite3_busy_timeout(db, 5000);

    return db;
}

static void initialize_auth_db() {
    sqlite3* db = open_auth_db();

    try {
        const char* schema = R"SQL(
            CREATE TABLE IF NOT EXISTS users (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                username TEXT NOT NULL UNIQUE,
                password_hash TEXT NOT NULL,
                password_salt TEXT NOT NULL,
                created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
            );
        )SQL";

        char* error = nullptr;

        if (sqlite3_exec(db, schema, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : "Schema creation failed";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }

        sqlite3_stmt* statement = nullptr;

        if (sqlite3_prepare_v2(
                db,
                "SELECT COUNT(*) FROM users",
                -1,
                &statement,
                nullptr
            ) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }

        int result = sqlite3_step(statement);
        int user_count = result == SQLITE_ROW
            ? sqlite3_column_int(statement, 0)
            : -1;

        sqlite3_finalize(statement);

        if (user_count < 0) {
            throw std::runtime_error("Unable to count users");
        }

        if (user_count == 0) {
            const std::string password = generate_initial_password();

            std::array<unsigned char, 16> salt_bytes{};

            if (RAND_bytes(
                    salt_bytes.data(),
                    static_cast<int>(salt_bytes.size())
                ) != 1) {
                throw std::runtime_error("Secure salt generation failed");
            }

            const std::string salt =
                bytes_to_hex(salt_bytes.data(), salt_bytes.size());

            const std::string password_hash =
                hash_password(password, salt);

            if (sqlite3_prepare_v2(
                    db,
                    "INSERT INTO users "
                    "(username, password_hash, password_salt) "
                    "VALUES (?, ?, ?)",
                    -1,
                    &statement,
                    nullptr
                ) != SQLITE_OK) {
                throw std::runtime_error(sqlite3_errmsg(db));
            }

            sqlite3_bind_text(
                statement, 1, "sam", -1, SQLITE_STATIC
            );
            sqlite3_bind_text(
                statement, 2, password_hash.c_str(), -1, SQLITE_TRANSIENT
            );
            sqlite3_bind_text(
                statement, 3, salt.c_str(), -1, SQLITE_TRANSIENT
            );

            if (sqlite3_step(statement) != SQLITE_DONE) {
                sqlite3_finalize(statement);
                throw std::runtime_error(sqlite3_errmsg(db));
            }

            sqlite3_finalize(statement);

            // This should happen only when the initial account is created.
            std::cout
                << "\n========== VPS MONITOR INITIAL LOGIN ==========\n"
                << "Username: sam\n"
                << "Password: " << password << "\n"
                << "Save this password now. It will not be shown again.\n"
                << "===============================================\n"
                << std::endl;
        }

        sqlite3_close(db);
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
}

static bool verify_user_password(
    const std::string& username,
    const std::string& password
) {
    sqlite3* db = open_auth_db();

    try {
        sqlite3_stmt* statement = nullptr;

        const char* query =
            "SELECT password_hash, password_salt "
            "FROM users WHERE username = ? LIMIT 1";

        if (sqlite3_prepare_v2(
                db, query, -1, &statement, nullptr
            ) != SQLITE_OK) {
            throw std::runtime_error(sqlite3_errmsg(db));
        }

        sqlite3_bind_text(
            statement, 1, username.c_str(), -1, SQLITE_TRANSIENT
        );

        bool valid = false;

        if (sqlite3_step(statement) == SQLITE_ROW) {
            const auto* stored_hash = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 0)
            );

            const auto* stored_salt = reinterpret_cast<const char*>(
                sqlite3_column_text(statement, 1)
            );

            if (stored_hash && stored_salt) {
                const std::string calculated =
                    hash_password(password, stored_salt);

                valid = constant_time_equal(calculated, stored_hash);
            }
        }

        sqlite3_finalize(statement);
        sqlite3_close(db);

        return valid;
    } catch (...) {
        sqlite3_close(db);
        throw;
    }
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
    try {
        initialize_auth_db();
    } catch (const std::exception& e) {
        std::cerr << "Authentication database initialization failed: "
                  << e.what() << '\n';
        return 1;
    }
    
    httplib::Server server;

    server.Get("/login",
    [](const httplib::Request&, httplib::Response& res) {
        const auto html = read_file(FRONTEND + "/login.html");

        if (html.empty()) {
            res.status = 500;
            res.set_content(
                "Login page not found",
                "text/plain"
            );
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(html, "text/html; charset=utf-8");
    }
);


server.Post("/api/auth/login",
    [](const httplib::Request& req, httplib::Response& res) {
        json body;

        try {
            body = json::parse(req.body);
        } catch (...) {
            res.status = 400;
            res.set_content(
                json{
                    {"success", false},
                    {"message", "Invalid JSON body"}
                }.dump(),
                "application/json"
            );
            return;
        }

        if (!body.is_object() ||
            !body.contains("username") ||
            !body.contains("password") ||
            !body["username"].is_string() ||
            !body["password"].is_string()) {
            res.status = 400;
            res.set_content(
                json{
                    {"success", false},
                    {"message", "Username and password are required"}
                }.dump(),
                "application/json"
            );
            return;
        }

        const std::string username =
            body["username"].get<std::string>();

        const std::string password =
            body["password"].get<std::string>();

        try {
            if (!verify_user_password(username, password)) {
                res.status = 401;
                res.set_header("Cache-Control", "no-store");
                res.set_content(
                    json{
                        {"success", false},
                        {"message", "Invalid username or password"}
                    }.dump(),
                    "application/json"
                );
                return;
            }
        } catch (const std::exception& e) {
            std::cerr << "Login database error: "
                      << e.what() << '\n';

            res.status = 500;
            res.set_content(
                json{
                    {"success", false},
                    {"message", "Authentication service unavailable"}
                }.dump(),
                "application/json"
            );
            return;
        }

        const std::string token = generate_session_token();

        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            sessions[token] = Clock::now() + SESSION_LIFETIME;
        }

        set_session_cookie(res, token);

        res.set_content(
            json{{"success", true}}.dump(),
            "application/json"
        );
    }
);

server.Get("/api/auth/me",
    [](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res)) {
            return;
        }

        res.set_header("Cache-Control", "no-store");
        res.set_content(
            json{{"authenticated", true}}.dump(),
            "application/json"
        );
    }
);

server.Post("/api/auth/logout",
    [](const httplib::Request& req, httplib::Response& res) {
        const std::string token = get_session_cookie(req);

        if (!token.empty()) {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            sessions.erase(token);
        }

        std::string cookie =
            "vps_session=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0";

        if (secure_cookies()) {
            cookie += "; Secure";
        }

        res.set_header("Set-Cookie", cookie);
        res.set_header("Cache-Control", "no-store");
        res.set_content(
            json{{"success", true}}.dump(),
            "application/json"
        );
    });

    server.Get("/", [](const httplib::Request& req, httplib::Response& res) {
        if (!authenticated(req)) {
            res.status = 302;
            res.set_header("Location", "/login");
            res.set_header("Cache-Control", "no-store");
            return;
        }
        
        const auto html = read_file(FRONTEND + "/index.html");

        if (html.empty()) {
            res.status = 500;
            res.set_content("Dashboard file not found", "text/plain");
            return;
        }
        
        res.set_header("Cache-Control", "no-store");
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
    [](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res)) {
            return;
        }
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
        [](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res)) {
            return;
        }
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
        [](const httplib::Request& req, httplib::Response& res) {
        if (!require_auth(req, res)) {
            return;
        }
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
