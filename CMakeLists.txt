cmake_minimum_required (VERSION 3.10)

project (AirlineTycoon)

set(CMAKE_CXX_STANDARD 17)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
elseif (CMAKE_BUILD_TYPE STREQUAL "Tidy")
    set(CMAKE_BUILD_TYPE Release)
    add_compile_definitions(NDEBUG)
    # enable clang-tidy
    set(CMAKE_CXX_CLANG_TIDY clang-tidy; -header-filter=.;)
endif()

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${PROJECT_SOURCE_DIR}/cmake")
set(GAMEFILES_FOLDER "${PROJECT_SOURCE_DIR}/gamefiles")
set(PATCH_FOLDER "${PROJECT_SOURCE_DIR}/patch")

if (MSVC)
    add_compile_options("/Zi")

    add_link_options("/DEBUG:FULL")
    add_link_options("/OPT:REF")
    add_link_options("/OPT:ICF")
endif()

option(AT_ASAN "Build with AddressSanitizer (MSVC only; for the multiplayer repro harness)" OFF)
if (MSVC AND AT_ASAN)
    add_compile_options("/fsanitize=address")
    # /RTC1 is incompatible with ASan
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG}")
    string(REGEX REPLACE "/RTC[1csu]+" "" CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")
    add_link_options("/INCREMENTAL:NO")
    # ASan + Debug inflates stack frames; the default 1 MB main thread stack overflows
    add_link_options("/STACK:16777216")
    # Directory of cl.exe also holds the ASan runtime DLLs
    get_filename_component(AT_ASAN_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    message(STATUS "AddressSanitizer enabled, runtime DLL dir: ${AT_ASAN_BIN_DIR}")
endif()

if (WIN32)
    # Set and bind SDL folders
    set(SDL2_PATH "${CMAKE_MODULE_PATH}/sdl2")
    set(SDL2_MIXER_PATH "${CMAKE_MODULE_PATH}/sdl2-mixer")
    set(SDL2_IMAGE_PATH "${CMAKE_MODULE_PATH}/sdl2-image")
    set(SDL2_TTF_PATH "${CMAKE_MODULE_PATH}/sdl2-ttf")

    # Download when SDL is not found
    if(NOT EXISTS ${SDL2_TTF_PATH})
        message(STATUS "Downloading SDL2. This is a (longer) one time operation. Please wait.")
        execute_process(
            COMMAND powershell -ExecutionPolicy Bypass -File ${CMAKE_MODULE_PATH}/dl-sdl2.ps1
            WORKING_DIRECTORY ${CMAKE_MODULE_PATH}
        )
    endif()

    # Copy SDL dependencies
    set(DEPENDENCIES_PATH ${DEPENDENCIES_PATH} "${CMAKE_BINARY_DIR}/dependencies")
    file(MAKE_DIRECTORY ${DEPENDENCIES_PATH})
    file(GLOB_RECURSE dll_files ${SDL2_PATH}/*.dll ${SDL2_MIXER_PATH}/*.dll ${SDL2_IMAGE_PATH}/*.dll ${SDL2_TTF_PATH}/*.dll ${PROJECT_SOURCE_DIR}/libs/*.dll ${PROJECT_SOURCE_DIR}/libs/*.exe)
    list(FILTER dll_files INCLUDE REGEX "x64|sentry\\.dll|\\.exe$")
    foreach(file ${dll_files})
        file(COPY ${file} DESTINATION ${DEPENDENCIES_PATH})
    endforeach()
endif()

if (WIN32 OR APPLE)
    # Needed on windows as include is not under repo / project
    include_directories(libs/jansson/include)
endif()

include_directories(libs/libflic/repo)

find_package(SDL2main REQUIRED)
find_package(SDL2_mixer REQUIRED)
find_package(SDL2_image REQUIRED)
find_package(SDL2_ttf REQUIRED)

# Include external libs.
add_subdirectory (libs)

# Include sub-projects.
add_subdirectory (src)

