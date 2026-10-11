cmake_minimum_required(VERSION 3.20)

project(vps_monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)

add_executable(vps-monitor
    src/main.cpp
)

target_include_directories(vps-monitor PRIVATE include)

target_link_libraries(vps-monitor PRIVATE
    Threads::Threads
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
)

target_compile_definitions(vps-monitor PRIVATE CPPHTTPLIB_OPENSSL_SUPPORT)
