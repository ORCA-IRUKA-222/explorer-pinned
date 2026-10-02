cmake_minimum_required(VERSION 3.21)
project(ExplorerPinned LANGUAGES CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the program runs on a clean Windows install without redistributables.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(EP_SOURCES
    src/agent.cpp
    src/comutil.cpp
    src/console.cpp
    src/displaycheck.cpp
    src/main.cpp
    src/pinstore.cpp
    src/setup.cpp
    src/util.cpp
    src/ExplorerPinned.rc
    src/app.manifest
)

add_executable(ExplorerPinned WIN32 ${EP_SOURCES})
target_compile_definitions(ExplorerPinned PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
target_link_libraries(ExplorerPinned PRIVATE ole32 oleaut32 shell32 shlwapi propsys uuid comctl32 user32 advapi32)
if(MSVC)
    target_compile_options(ExplorerPinned PRIVATE /W4 /permissive- /utf-8 /EHsc)
endif()

option(EP_BUILD_TESTS "Build the end-to-end test program" ON)
if(EP_BUILD_TESTS)
    add_executable(ExplorerPinnedE2E tests/e2e.cpp src/util.cpp)
    target_include_directories(ExplorerPinnedE2E PRIVATE src)
    target_compile_definitions(ExplorerPinnedE2E PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
    target_link_libraries(ExplorerPinnedE2E PRIVATE ole32 oleaut32 shell32 shlwapi propsys uuid comctl32 user32 advapi32)
    if(MSVC)
        target_compile_options(ExplorerPinnedE2E PRIVATE /W4 /permissive- /utf-8 /EHsc)
    endif()
endif()
