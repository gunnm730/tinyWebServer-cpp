cmake_minimum_required(VERSION 3.10)
project(tinyWebServer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Threads
find_package(Threads REQUIRED)

# MySQL Connector/C++
find_library(MYSQLCPPCONN_LIB mysqlcppconn REQUIRED)
find_path(MYSQLCPPCONN_INCLUDE_DIR cppconn/connection.h
    PATHS /usr/include /usr/local/include /usr/include/mysql)

# Source files (header-only modules are pulled in via include)
add_executable(server
    src/main.cpp
    src/config/config.cpp
    src/server/webserver.cpp
    src/http/http_connection.cpp
)

target_include_directories(server PRIVATE
    ${MYSQLCPPCONN_INCLUDE_DIR}
    src
)

target_link_libraries(server PRIVATE
    Threads::Threads
    ${MYSQLCPPCONN_LIB}
)
