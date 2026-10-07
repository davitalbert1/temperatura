cmake_minimum_required(VERSION 3.16)

project(temperatura LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Garante que as ferramentas/DLLs do MinGW (moc, libQt6Core.dll, ...) sejam
# encontradas mesmo quando o cmake roda fora do shell MSYS2 (ex.: PowerShell/VS Code)
get_filename_component(_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
if(WIN32)
    set(ENV{PATH} "${_MINGW_BIN};$ENV{PATH}")
endif()

# Os executáveis são gerados na raiz de build/ (build/clima_viewer.exe, build/download.exe)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# clima_viewer.exe - interface Qt 6 (app/main.cpp)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Sql)

add_executable(clima_viewer WIN32 app/main.cpp)
set_target_properties(clima_viewer PROPERTIES AUTOMOC ON)
target_link_libraries(clima_viewer PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Sql)

# download.exe - download de dados climaticos (download.cpp)
find_package(CURL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

add_executable(download download.cpp)
target_link_libraries(download PRIVATE CURL::libcurl SQLite3::SQLite3 Threads::Threads)

# nlohmann/json (header-only): usa o pacote CMake se existir,
# senão procura o include padrão nlohmann/json.hpp
find_package(nlohmann_json CONFIG QUIET)
if(nlohmann_json_FOUND)
    target_link_libraries(download PRIVATE nlohmann_json::nlohmann_json)
else()
    find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp)
    if(NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "nlohmann/json.hpp nao encontrado. Instale mingw-w64-x86_64-nlohmann-json (MSYS2).")
    endif()
    target_include_directories(download PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
