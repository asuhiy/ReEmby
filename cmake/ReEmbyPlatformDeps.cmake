# =============================================================================
# ReEmbyPlatformDeps.cmake — 平台与第三方依赖辅助函数
#
# ⚠️ 注意：本文件为本地重建版本。上游仓库 (AlanHJ/qEmby) 的根 CMakeLists.txt
#   第 11 行 include(ReEmbyPlatformDeps)，但该文件未提交到版本库，导致
#   按 README 流程 clone 后无法 configure。此处按调用方契约重建：
#     - qemby_append_default_qt_prefixes()   (根 CMakeLists.txt:38)
#     - qemby_link_libmpv(<target>)          (src/qEmbyApp/CMakeLists.txt:35)
# =============================================================================

# -----------------------------------------------------------------------------
# 向 CMAKE_PREFIX_PATH 追加常见的 Qt 安装路径（可用 QEMBY_QT_ROOT 缓存变量
# 或 QTDIR 环境变量显式指定；Windows 下自动探测常见安装根目录下的 kit 目录）
# -----------------------------------------------------------------------------
function(qemby_append_default_qt_prefixes)
    set(_hints "")

    if(QEMBY_QT_ROOT)
        list(APPEND _hints "${QEMBY_QT_ROOT}")
    endif()

    if(DEFINED ENV{QTDIR} AND NOT "$ENV{QTDIR}" STREQUAL "")
        list(APPEND _hints "$ENV{QTDIR}")
    endif()

    if(WIN32)
        # Qt 在线安装器默认布局: <root>/<version>/<kit>，如 C:/Qt/6.9.2/msvc2022_64
        foreach(_root "C:/Qt" "D:/Qt" "E:/Qt" "E:/Qt6" "$ENV{USERPROFILE}/Qt")
            if(EXISTS "${_root}")
                file(GLOB _kit_dirs LIST_DIRECTORIES true "${_root}/*/*")
                foreach(_kit ${_kit_dirs})
                    if(IS_DIRECTORY "${_kit}")
                        list(APPEND _hints "${_kit}")
                    endif()
                endforeach()
            endif()
        endforeach()
    elseif(APPLE)
        foreach(_p "/opt/homebrew/opt/qt" "/usr/local/opt/qt" "$ENV{USERPROFILE}/Qt")
            if(EXISTS "${_p}")
                list(APPEND _hints "${_p}")
            endif()
        endforeach()
    endif()

    list(REMOVE_DUPLICATES _hints)

    foreach(_h ${_hints})
        list(APPEND CMAKE_PREFIX_PATH "${_h}")
    endforeach()

    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} PARENT_SCOPE)

    if(_hints)
        message(STATUS "ReEmby: Qt prefix hints -> ${_hints}")
    endif()
endfunction()

# -----------------------------------------------------------------------------
# 为目标链接 libmpv 播放引擎
#   Windows : 使用仓库内 libs/libmpv/（结构见 README，需手动放置）
#   Linux/macOS : 优先 pkg-config，回退 find_library 查找系统 libmpv
# -----------------------------------------------------------------------------
function(qemby_link_libmpv target)
    if(WIN32)
        set(QEMBY_LIBMPV_ROOT "${CMAKE_SOURCE_DIR}/libs/libmpv")

        if(NOT EXISTS "${QEMBY_LIBMPV_ROOT}/include/mpv/client.h")
            message(FATAL_ERROR
                "未找到 libmpv 开发文件: ${QEMBY_LIBMPV_ROOT}/include/mpv/client.h\n"
                "请按 README 指引下载 libmpv SDK 放置到 libs/libmpv/ 目录:\n"
                "  libs/libmpv/bin/libmpv-2.dll\n"
                "  libs/libmpv/include/mpv/*.h\n"
                "  libs/libmpv/lib/libmpv.dll.a")
        endif()

        target_include_directories(${target} PRIVATE "${QEMBY_LIBMPV_ROOT}/include")
        if(EXISTS "${QEMBY_LIBMPV_ROOT}/lib/libmpv.dll.a")
            target_link_libraries(${target} PRIVATE
                "${QEMBY_LIBMPV_ROOT}/lib/libmpv.dll.a")
        elseif(EXISTS "${QEMBY_LIBMPV_ROOT}/lib/mpv.dll.a")
            target_link_libraries(${target} PRIVATE
                "${QEMBY_LIBMPV_ROOT}/lib/mpv.dll.a")
        else()
            message(FATAL_ERROR
                "未找到 libmpv 导入库 (libmpv.dll.a / mpv.dll.a): ${QEMBY_LIBMPV_ROOT}/lib/")
        endif()

        # 编译后自动拷贝 libmpv-2.dll 到输出目录
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${QEMBY_LIBMPV_ROOT}/bin/libmpv-2.dll"
                $<TARGET_FILE_DIR:${target}>
            COMMENT "ReEmby: copying libmpv-2.dll to output directory")
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(MPV IMPORTED_TARGET mpv)
        endif()
        if(TARGET PkgConfig::MPV)
            target_link_libraries(${target} PRIVATE PkgConfig::MPV)
        else()
            find_library(QEMBY_MPV_LIB NAMES mpv REQUIRED)
            target_link_libraries(${target} PRIVATE ${QEMBY_MPV_LIB})
            # 头文件通常随系统包安装在标准路径，这里兜底常见前缀
            find_path(QEMBY_MPV_INCLUDE_DIR mpv/client.h)
            if(QEMBY_MPV_INCLUDE_DIR)
                target_include_directories(${target} PRIVATE "${QEMBY_MPV_INCLUDE_DIR}")
            endif()
        endif()
    endif()
endfunction()
