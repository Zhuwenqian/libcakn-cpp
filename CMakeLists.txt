# cakn-cpp: C++ re-implementation of CKAN (comba library, extracted from
# Hello KSP Launcher). Builds libckan as a shared library + a QtTest suite.
cmake_minimum_required(VERSION 3.16)
project(cakn-cpp VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# 库内 Downloader/RepoIndex 等含 Q_OBJECT（信号/槽），需 AUTOMOC。
set(CMAKE_AUTOMOC ON)

# 统一输出到 dist，与 exe/测试同目录，可直接运行。
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/dist)
# Windows DLL 走 RUNTIME，Unix .so/.dylib 走 LIBRARY，统一落 dist。
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_RELEASE ${CMAKE_SOURCE_DIR}/dist)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_DEBUG ${CMAKE_SOURCE_DIR}/dist)

find_package(Qt6 REQUIRED COMPONENTS Core Network Concurrent Test)

# ---------------------------------------------------------------------------
# libckan：C++ 重写的 CKAN（共享库，编译为 dll/dylib/so）
# 全类导出（CKAN_API），对外只暴露门面 CKan + CKanConfig。
# ---------------------------------------------------------------------------
set(CKAN_SOURCES
    src/ckan/version.cpp
    src/ckan/relationship.cpp
    src/ckan/ckanmodule.cpp
    src/ckan/moduleinstalldescriptor.cpp
    src/ckan/installedmodule.cpp
    src/ckan/registry.cpp
    src/ckan/repository.cpp
    src/ckan/gameinstance.cpp
    src/ckan/downloader.cpp
    src/ckan/repoindex.cpp
    src/ckan/relationshipresolver.cpp
    src/ckan/moduleinstaller.cpp
    src/ckan/txfilemanager.cpp
    src/ckan/ckan.cpp
    thirdparty/miniz.c
)

set(CKAN_HEADERS
    src/ckan/ckan_export.h
    src/ckan/version.h
    src/ckan/relationship.h
    src/ckan/ckanmodule.h
    src/ckan/moduleinstalldescriptor.h
    src/ckan/installedmodule.h
    src/ckan/registry.h
    src/ckan/repository.h
    src/ckan/gameinstance.h
    src/ckan/downloader.h
    src/ckan/repoindex.h
    src/ckan/relationshipresolver.h
    src/ckan/moduleinstaller.h
    src/ckan/txfilemanager.h
    src/ckan/ckanconfig.h
    src/ckan/ckan.h
    thirdparty/miniz.h
)

add_library(libckan SHARED
    ${CKAN_SOURCES}
    ${CKAN_HEADERS}
)
target_compile_definitions(libckan PRIVATE
    CKAN_BUILD_SHARED
    CKAN_BUILDING_LIB
)
# 收窄公开 include：只暴露库自身头文件目录；src/ 与 thirdparty/ 仅内部使用。
target_include_directories(libckan PUBLIC
    ${CMAKE_SOURCE_DIR}/src/ckan
)
target_include_directories(libckan PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/thirdparty
)
target_link_libraries(libckan PUBLIC
    Qt6::Core
    Qt6::Network
    Qt6::Concurrent
)

# ---------------------------------------------------------------------------
# 单元测试（纯库测试，不依赖启动器/UI 源码）
# ---------------------------------------------------------------------------
enable_testing()
add_executable(test_libckan
    tests/test_libckan.cpp
    thirdparty/miniz.c
)
target_compile_definitions(test_libckan PRIVATE CKAN_BUILD_SHARED)
target_include_directories(test_libckan PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/ckan
    ${CMAKE_SOURCE_DIR}/thirdparty
)
target_link_libraries(test_libckan PRIVATE
    Qt6::Core
    Qt6::Network
    Qt6::Test
    libckan
)
add_test(NAME test_libckan COMMAND test_libckan)