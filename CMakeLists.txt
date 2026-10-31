cmake_minimum_required(VERSION 3.19)
project(rts-tools VERSION 0.2.0 LANGUAGES CXX)

# 发行版的 Qt 插件本身没有 RPATH，Qt 部署脚本里 CMake 内置的 RPATH_SET 写不进去，
# Linux 上要用 patchelf（构建机需安装，必须在 find_package 之前设置）
if(UNIX AND NOT APPLE)
    set(QT_DEPLOY_USE_PATCHELF ON)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Widgets SerialPort Network LinguistTools)

qt_standard_project_setup()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 统一的警告级别；新代码不应引入新警告
function(rts_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endfunction()

# 两个可执行文件共用的代码：本地管道协议、端点清单、数据目录
add_library(rts-common STATIC
    common/jsonstore.cpp
    common/jsonstore.h
    common/plotpaths.h
    common/plotprotocol.cpp
    common/plotprotocol.h
    common/translation.cpp
    common/translation.h
    common/plotendpoints.cpp
    common/plotendpoints.h
)
target_include_directories(rts-common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rts-common PUBLIC Qt::Core Qt::Gui)
rts_enable_warnings(rts-common)

qt_add_executable(rts-tools
    WIN32 MACOSX_BUNDLE
    app/main.cpp
    app/rttsession.cpp
    app/rttsettingsdialog.cpp
    app/rttsettingsdialog.h
    app/rttsession.h
    app/updatechecker.cpp
    app/updatechecker.h
    app/updatedialog.cpp
    app/updatedialog.h
    app/serialpanel.cpp
    app/serialpanel.h
    app/serialpanel.ui
    app/logfile.cpp
    app/logfile.h
    app/receivelogview.cpp
    app/receivelogview.h
    highlight/highlightrule.cpp
    highlight/highlightrule.h
    highlight/highlightdialog.cpp
    highlight/highlightdialog.h
    csv/csvparser.cpp
    csv/csvparser.h
    csv/csvpanel.cpp
    csv/csvpanel.h
    csv/csvrowlist.cpp
    csv/csvrowlist.h
    csv/csvgroupdialog.cpp
    csv/csvgroupdialog.h
    csv/csvicons.h
    plot/server/plotserver.cpp
    plot/server/plotserver.h
    plot/server/plotlauncher.cpp
    plot/server/plotlauncher.h
)

# 曲线查看器：独立进程（每个系列一个实例），崩溃不影响主程序
qt_add_executable(rts-plot
    WIN32 MACOSX_BUNDLE
    plot/viewer/main_plot.cpp
    plot/viewer/plotclient.cpp
    plot/viewer/plotclient.h
    plot/viewer/sampletable.h
    plot/viewer/plotcanvas.cpp
    plot/viewer/plotcanvas.h
    plot/viewer/plotwindow.cpp
    plot/viewer/plotwindow.h
    plot/viewer/ploticons.h
    plot/viewer/endpointdialog.cpp
    plot/viewer/endpointdialog.h
)

# 资源路径保持 :/theme/images/<name>，与 .ui 样式表里的引用一致
set(rts_tools_images resources/images/chevron-down.png resources/images/rts-tools.png)
set(rts_plot_images resources/images/rts-plot.png)
foreach(image ${rts_tools_images} ${rts_plot_images})
    get_filename_component(image_name ${image} NAME)
    set_source_files_properties(${image} PROPERTIES QT_RESOURCE_ALIAS images/${image_name})
endforeach()

qt_add_resources(rts-tools "theme"
    PREFIX "/theme"
    FILES ${rts_tools_images}
)

qt_add_resources(rts-plot "theme"
    PREFIX "/theme"
    FILES ${rts_plot_images}
)

# Windows 上给 exe 加图标（Linux 用窗口图标 + 安装到 hicolor 图标目录）
if(WIN32)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resources/rts-tools.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/rts-tools.rc @ONLY)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/resources/rts-plot.rc.in
                   ${CMAKE_CURRENT_BINARY_DIR}/rts-plot.rc @ONLY)
    # 图标文件换了要重新编译资源，否则 exe 里还是旧图标
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/rts-tools.rc PROPERTY
                 OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/rts-tools.ico)
    set_property(SOURCE ${CMAKE_CURRENT_BINARY_DIR}/rts-plot.rc PROPERTY
                 OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/images/rts-plot.ico)
    target_sources(rts-tools PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rts-tools.rc)
    target_sources(rts-plot PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/rts-plot.rc)
endif()

# uic 生成的 ui_serialpanel.h 会 #include 自定义控件头文件，需要能找到源码目录
target_include_directories(rts-tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 版本号显示在窗口标题上，只在这一处维护
target_compile_definitions(rts-tools PRIVATE RTS_TOOLS_VERSION="${PROJECT_VERSION}")

qt_add_translations(
    TARGETS rts-tools
    TS_FILES resources/rts-tools_zh_CN.ts
)
qt_add_translations(
    TARGETS rts-plot
    TS_FILES resources/rts-plot_zh_CN.ts
)

target_link_libraries(rts-tools
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::SerialPort
        Qt::Network
        rts-common
)

target_link_libraries(rts-plot
    PRIVATE
        Qt::Core
        Qt::Widgets
        Qt::Network
        rts-common
)

rts_enable_warnings(rts-tools)
rts_enable_warnings(rts-plot)

# 单元测试：ctest 或 cmake --build build --target test
option(RTS_BUILD_TESTS "Build unit tests" ON)
if(RTS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(UNIX AND NOT APPLE)
    # Linux：整体装到 /opt/rts-tools（含打包进来的 Qt），再在 /usr/bin 放符号链接
    # 只在用户没有指定前缀时才改默认值，--prefix / -DCMAKE_INSTALL_PREFIX 仍然生效
    if(CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
        set(CMAKE_INSTALL_PREFIX /opt/rts-tools CACHE PATH "" FORCE)
    endif()
    # 部署脚本是在「安装后」的可执行文件上解析依赖的，那时 $ORIGIN/../lib 还是空的，
    # 会退回系统里同名的 Qt（版本可能不符，CI 上甚至没有）。安装时先指向构建用的 Qt，
    # 部署完再把 RPATH 改回 $ORIGIN/../lib。
    set(qt_lib_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}")
    set_target_properties(rts-tools rts-plot PROPERTIES
        INSTALL_RPATH "${qt_lib_dir}"
        BUILD_WITH_INSTALL_RPATH OFF)
endif()

install(TARGETS rts-tools rts-plot
    BUNDLE  DESTINATION .
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)

# 桌面环境的启动器图标
if(UNIX AND NOT APPLE)
    install(FILES resources/images/rts-tools.png resources/images/rts-plot.png
            DESTINATION /usr/share/icons/hicolor/256x256/apps)
    install(FILES packaging/rts-tools.desktop packaging/rts-plot.desktop
            DESTINATION /usr/share/applications)
    # 随包分发的 Qt 是 LGPLv3，许可证与声明必须一起装进安装目录
    install(FILES LICENSE THIRD-PARTY-NOTICES.md
                  LICENSES/LGPL-3.0.txt LICENSES/GPL-3.0.txt
            DESTINATION share/doc/rts-tools)
    install(CODE "
        file(MAKE_DIRECTORY \"\$ENV{DESTDIR}/usr/bin\")
        foreach(app rts-tools rts-plot)
            file(CREATE_LINK ${CMAKE_INSTALL_PREFIX}/bin/\${app} \"\$ENV{DESTDIR}/usr/bin/\${app}\" SYMBOLIC)
        endforeach()
    ")
endif()

# 把 Qt 库和插件拷进安装目录，目标机器无需另装 Qt
if(UNIX AND NOT APPLE)
    # Qt 标准部署：把 Qt 库和插件拷进安装目录，并把插件的 RPATH 改成指向自带的 lib。
    # 发行版的 Qt 插件本身没有 RPATH，CMake 内置的 RPATH_SET 写不进去，需要构建机装 patchelf。
    # 这些插件桌面用不到，其中 egldeviceintegrations 没有 RPATH 会让脚本报错。
    # 只有 Qt 6.10 起才有 EXCLUDE_PLUGIN_TYPES 参数，更早的版本只能装默认集合
    # （官方 Qt 二进制自带 RPATH，不筛也能正常部署）。
    if(Qt6Core_VERSION VERSION_GREATER_EQUAL 6.10)
        set(plugin_exclude_args "
    EXCLUDE_PLUGIN_TYPES
        egldeviceintegrations designer kf6 sqldrivers printsupport platformthemes generic
        networkinformation wayland-decoration-client wayland-graphics-integration-client
        wayland-graphics-integration-server wayland-shell-integration")
    else()
        set(plugin_exclude_args "")
    endif()
    qt_generate_deploy_script(
        TARGET rts-tools
        OUTPUT_SCRIPT deploy_script
        CONTENT "
qt_deploy_runtime_dependencies(
    EXECUTABLE \"\\\${QT_DEPLOY_PREFIX}/bin/rts-tools\"
    ADDITIONAL_EXECUTABLES \"\\\${QT_DEPLOY_PREFIX}/bin/rts-plot\"
    LIB_DIR lib
    PLUGINS_DIR plugins${plugin_exclude_args}
    POST_INCLUDE_REGEXES \"/(libQt6|libicu|libdouble-conversion|libb2|libpcre2|libmd4c)\"
    POST_EXCLUDE_REGEXES \"^/\"
)
file(WRITE \"\\\${QT_DEPLOY_PREFIX}/bin/qt.conf\" \"[Paths]\\\\nPrefix = ..\\\\nPlugins = plugins\\\\nLibraries = lib\\\\n\")
")
else()
    qt_generate_deploy_app_script(
        TARGET rts-tools
        OUTPUT_SCRIPT deploy_script
        NO_UNSUPPORTED_PLATFORM_ERROR
    )
endif()
install(SCRIPT ${deploy_script})

if(UNIX AND NOT APPLE)
    # 部署完成，把安装版本的 RPATH 从构建用 Qt 改回自带的 lib（$ORIGIN 更短，RPATH_CHANGE 能就地改写）
    install(CODE "
        foreach(app rts-tools rts-plot)
            file(RPATH_CHANGE
                FILE \"\$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/\${app}\"
                OLD_RPATH \"${qt_lib_dir}\"
                NEW_RPATH \"\$ORIGIN/../lib\")
        endforeach()
    ")
endif()

# deb 打包：cmake --build build --target package（需 -DCMAKE_BUILD_TYPE=Release）
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "rts-tools")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT "vaelin.zheng@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "RTS serial tools with curve viewer")
set(CPACK_PACKAGING_INSTALL_PREFIX "/opt/rts-tools")
set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
# Qt 已自带；其余系统库依赖由 dpkg-shlibdeps 自动计算（自带的 lib 目录要告诉它，免得被当成缺失）
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS_PRIVATE_DIRS "${CMAKE_INSTALL_PREFIX}/lib")
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
include(CPack)
