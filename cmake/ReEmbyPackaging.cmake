# =============================================================================
# ReEmbyPackaging.cmake — CPack 打包配置（最小重建版）
#
# ⚠️ 注意：本文件为本地重建版本，上游未提交原始文件。上游 Release 页面提供
#   NSIS 安装包 / ZIP 绿色版 (Windows)、AppImage / deb×4 (Linux)、DMG (macOS)，
#   原始打包脚本（含 AppImage 工具链与各发行版 deb 变体）无法从仓库反推，
#   此处提供最小可用的 CPack 配置：
#     - Windows : NSIS + ZIP
#     - Linux   : DEB（含 QEMBY_RUNTIME_LIB_SUBDIR 私有库目录布局）
#     - macOS   : DragNDrop (DMG)
#   如需与官方 Release 完全一致的包，请等待上游补齐该文件。
# =============================================================================

set(CPACK_PACKAGE_NAME "ReEmby")
set(CPACK_PACKAGE_VENDOR "ReEmby Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A modern desktop client for Emby & Jellyfin media servers")
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ReEmby")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_CREATE_DESKTOP_LINKS qEmbyApp)

# Linux: 私有运行时库目录（lib/<arch>/qemby），配合 INSTALL_RPATH=$ORIGIN/../.. 解析
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR DEB)
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ReEmby Project")
    set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
    set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
    set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

    # 目标已在 add_subdirectory 之后定义，此处补设 RPATH 使其找到私有库目录
    if(TARGET qEmbyApp)
        set_target_properties(qEmbyApp PROPERTIES
            INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}/qemby")
    endif()
    if(TARGET qEmbyCore)
        set_target_properties(qEmbyCore PROPERTIES
            INSTALL_RPATH "$ORIGIN")
    endif()
elseif(APPLE)
    set(CPACK_GENERATOR DragNDrop)
    set(CPACK_DMG_VOLUME_NAME "ReEmby")
else()
    set(CPACK_GENERATOR NSIS ZIP)
    set(CPACK_NSIS_EXECUTABLES_DIRECTORY ".")
    set(CPACK_NSIS_MUI_FINISHPAGE_RUN "qEmbyApp")
    set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
endif()

include(CPack)
