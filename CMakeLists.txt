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

# JSON for Modern C++ (header-only); if not installed, downloads the latest
# release (or COMET_JSON_VERSION)
set(COMET_JSON_VERSION "latest" CACHE STRING
    "nlohmann_json version to download (`latest` for the latest release)")
find_package(nlohmann_json 3.11 QUIET)
if(nlohmann_json_FOUND)
    message(STATUS "nlohmann_json: ${nlohmann_json_VERSION}")
else()
    # This version's checksum is known; it's the fallback for `latest`.
    set(_json_pinned 3.12.0)
    set(_json_pinned_sha256 42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa)
    set(_json_version ${COMET_JSON_VERSION})
    if(_json_version STREQUAL "latest")
        if(COMET_JSON_LATEST)
            set(_json_version ${COMET_JSON_LATEST})
        else()
            set(_reply "${CMAKE_CURRENT_BINARY_DIR}/json-latest.json")
            file(DOWNLOAD "https://api.github.com/repos/nlohmann/json/releases/latest"
                "${_reply}" STATUS _status TIMEOUT 30)
            list(GET _status 0 _status_code)
            if(_status_code EQUAL 0)
                file(READ "${_reply}" _reply)
                string(JSON _tag ERROR_VARIABLE _reply_error GET "${_reply}" tag_name)
                if(NOT _reply_error AND _tag MATCHES "^v([0-9]+[.][0-9]+[.][0-9]+)$")
                    set(_json_version ${CMAKE_MATCH_1})
                    set(COMET_JSON_LATEST ${_json_version} CACHE INTERNAL
                        "latest nlohmann_json release, as of the first configure")
                endif()
            endif()
            if(_json_version STREQUAL "latest")
                message(WARNING "Couldn't find the latest nlohmann_json release; "
                    "using nlohmann_json ${_json_pinned}")
                set(_json_version ${_json_pinned})
            endif()
        endif()
    endif()
    if(_json_version STREQUAL _json_pinned)
        set(_json_hash URL_HASH SHA256=${_json_pinned_sha256})
    else()
        set(_json_hash)
    endif()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL "https://github.com/nlohmann/json/releases/download/v${_json_version}/json.tar.xz"
        ${_json_hash})
    FetchContent_MakeAvailable(nlohmann_json)
    message(STATUS "nlohmann_json: downloaded ${_json_version}")
endif()

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
