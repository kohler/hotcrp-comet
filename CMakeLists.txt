cmake_minimum_required(VERSION 3.20)
project(hotcrp-comet CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Cotamer, normally the `cotamer` submodule. hotcrp-comet needs its llhttp
# integration and nothing else.
set(COTAMER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cotamer" CACHE PATH
    "Cotamer source directory")
set(COTAMER_LLHTTP "ON" CACHE STRING "")
set(COTAMER_CURL "OFF" CACHE STRING "")
set(COTAMER_NLOHMANN_JSON "OFF" CACHE STRING "")
set(COTAMER_ZLIB "OFF" CACHE STRING "")
if(NOT EXISTS "${COTAMER_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Cotamer not found in ${COTAMER_DIR}; "
        "run `git submodule update --init` or set COTAMER_DIR")
endif()
add_subdirectory("${COTAMER_DIR}" cotamer)

# JSON for Modern C++ (header-only)
find_package(nlohmann_json 3.11 REQUIRED)

add_executable(hotcrp-comet
    hotcrp-comet.cc
    clp.c
)
set_source_files_properties(clp.c PROPERTIES LANGUAGE CXX)
target_compile_options(hotcrp-comet PRIVATE -W -Wall)
target_link_libraries(hotcrp-comet PRIVATE
    Cotamer
    CotamerHttp
    nlohmann_json::nlohmann_json
)

install(TARGETS hotcrp-comet DESTINATION bin)
