cmake_minimum_required(VERSION 3.19)

project(ReEmby VERSION 0.10.3 LANGUAGES CXX)

# Capture the build commit short hash at configure time so the resulting
# binary self-identifies which commit it was built from. Lets users verify
# their downloaded artifact matches the latest push without checking CI
# manually — displayed in the main window title + About dialog.
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE QEMBY_BUILD_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QEMBY_BUILD_COMMIT)
    set(QEMBY_BUILD_COMMIT "unknown")
endif()
execute_process(
    COMMAND git rev-parse --short=8 --abbrev-ref HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE QEMBY_BUILD_BRANCH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT QEMBY_BUILD_BRANCH)
    set(QEMBY_BUILD_BRANCH "unknown")
endif()
message(STATUS "ReEmby build identifier: ${QEMBY_BUILD_BRANCH} @ ${QEMBY_BUILD_COMMIT}")

# Release 也生成 PDB 调试符号：用户崩溃时 WER 自动产 dump
# （C:/Users/<u>/AppData/Local/CrashDumps），有 PDB 才能把崩溃 offset
# 符号化到函数名/代码行。PDB 随 artifact 一起打包（bin/Release 目录）。
if(MSVC)
    add_compile_options("$<$<CONFIG:Release>:/Zi>")
    add_link_options("$<$<CONFIG:Release>:/DEBUG>")
endif()

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET 13.0)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GNUInstallDirs)
include(ReEmbyPlatformDeps)

set(QEMBY_RUNTIME_LIB_SUBDIR "qemby" CACHE STRING
    "Subdirectory under the install libdir for bundled private runtime libraries")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")


set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# Parallelize the AUTOMOC scan/generation stage (CMake >= 3.27; ignored
# by older versions). CI on a 4-vCPU runner benefits from moc not being
# the serial bottleneck.
set(CMAKE_AUTOMOC_PARALLEL 4)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/MP)
endif()

# ==========================================
# 允许通过缓存变量或环境变量提供 Qt 安装路径
# Windows 未显式指定时仍会尝试兼容现有本地默认路径
# ==========================================
set(QEMBY_QT_ROOT "" CACHE PATH
    "Optional Qt installation root or CMake prefix path")
qemby_append_default_qt_prefixes()

# 寻找 Qt6 (不再允许降级)
find_package(Qt6 REQUIRED COMPONENTS Widgets Core Network Concurrent OpenGLWidgets LinguistTools WebSockets WebEngineWidgets)
if(UNIX AND NOT APPLE)
    find_package(Qt6 QUIET COMPONENTS DBus)
endif()

# zlib: used by bilibilidanmakuprovider to inflate B站 raw-deflate danmaku
# payloads (comment.bilibili.com serves Content-Encoding: deflate but the
# body is *raw* deflate — no zlib header, no trailing adler32 — so Qt's
# qUncompress wrapper cannot decode it). We need inflateInit2(stream,
# -15) which only the public zlib API exposes. Pull the upstream
# tarball via FetchContent so the CI runners do not need a separate
# zlib-dev package install step. Build is ~10s, fully covered by
# sccache on incremental builds.
include(FetchContent)
set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIB_BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
set(ZLIB_INSTALL OFF CACHE BOOL "" FORCE)
set(SKIP_BUILD_EXAMPLES ON CACHE BOOL "" FORCE)
set(SKIP_SHARED_LIB_COMPILE_FLAG ON CACHE BOOL "" FORCE)
set(SKIP_STATIC_LIB_COMPILE_FLAG ON CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
    # GitHub tag tarball instead of zlib.net: the zlib.net fossil tarball
    # carries a gzip header with the FNAME flag (1f8b 0808), which the
    # CMake 4.4 bundled libarchive on the Windows CI runner refuses to
    # open ("Unrecognized archive format"). GitHub's tag tarball uses a
    # plain gzip header (1f8b 0800) and extracts fine. Same zlib-1.3.1
    # source tree either way (zlib-1.3.1/ containing CMakeLists.txt).
    URL https://github.com/madler/zlib/archive/refs/tags/v1.3.1.tar.gz
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(zlib)

# ==========================================
# 为第三方子模块 (qwindowkit/qmsetup) 注入所需的 Qt 版本环境变量
# ==========================================
set(QT_VERSION_MAJOR ${Qt6_VERSION_MAJOR})
set(QT_VERSION_MINOR ${Qt6_VERSION_MINOR})
set(QT_VERSION ${Qt6_VERSION})

# ==========================================
# 引入 QCoro (C++20 Coroutines for Qt)
# ==========================================
include(FetchContent)

function(qemby_ensure_noop_install_script build_dir)
    if(NOT build_dir)
        return()
    endif()

    file(MAKE_DIRECTORY "${build_dir}")
    file(WRITE "${build_dir}/cmake_install.cmake"
        "# ReEmby placeholder install script for third-party dependency\n")
endfunction()

set(_qemby_saved_skip_install_rules ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)
FetchContent_Declare(
    QCoro
    GIT_REPOSITORY https://github.com/danvratil/qcoro.git
    GIT_TAG v0.10.0
)

# 强制 QCoro 使用 Qt6 进行编译，并关闭不必要的测试与示例构建以加快编译速度
set(QCORO_QT_MAJOR_VERSION 6 CACHE STRING "Use Qt6 for QCoro" FORCE)
set(QCORO_BUILD_EXAMPLES OFF CACHE BOOL "Disable QCoro examples" FORCE)
set(QCORO_WITH_QML OFF CACHE BOOL "Disable QCoro QML module" FORCE)
set(QCORO_WITH_QTQUICK OFF CACHE BOOL "Disable QCoro QtQuick module" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable QCoro tests" FORCE)

FetchContent_MakeAvailable(QCoro)
FetchContent_GetProperties(QCoro)
qemby_ensure_noop_install_script("${qcoro_BINARY_DIR}")

# ==========================================
# 引入 spdlog (高性能线程安全日志库, MIT License)
# ==========================================
FetchContent_Declare(
    spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG v1.15.3
)
set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "Disable spdlog examples" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "Disable spdlog tests" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "Disable spdlog install rules" FORCE)
FetchContent_MakeAvailable(spdlog)
FetchContent_GetProperties(spdlog)
qemby_ensure_noop_install_script("${spdlog_BINARY_DIR}")
set(CMAKE_SKIP_INSTALL_RULES ${_qemby_saved_skip_install_rules})
unset(_qemby_saved_skip_install_rules)
# ==========================================

set(QWINDOWKIT_INSTALL OFF CACHE BOOL
    "Disable QWindowKit development install rules inside ReEmby" FORCE)

add_subdirectory(libs/qwindowkit)
add_subdirectory(libs/qwindowkit/examples/shared)
add_subdirectory(src/qEmbyCore)
add_subdirectory(src/qEmbyApp)

include(ReEmbyPackaging)
