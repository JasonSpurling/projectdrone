cmake_minimum_required(VERSION 3.16)
project(data_flight CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-O2")

find_package(PkgConfig REQUIRED)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(LIBZIP REQUIRED libzip) # extracting MASTER.txt/ACFTREF.txt from the FAA registry's .zip distribution

add_executable(data_flight src/main.cpp)

target_link_libraries(data_flight
    PRIVATE
    CURL::libcurl
    SQLite::SQLite3
    nlohmann_json::nlohmann_json
    Threads::Threads
    ${LIBZIP_LIBRARIES}
)
target_include_directories(data_flight PRIVATE ${LIBZIP_INCLUDE_DIRS})
target_link_directories(data_flight PRIVATE ${LIBZIP_LIBRARY_DIRS})

target_include_directories(data_flight PRIVATE src)
