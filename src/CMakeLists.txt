add_executable(PasteOrbitNative WIN32
    app/main.cpp
    app/app_settings.cpp
    app/localization.cpp
    data/history_store.cpp
    data/history_model.cpp
    data/clipboard_content.cpp
    data/text_formats.cpp
    services/backup_service.cpp
    services/update_service.cpp
    ui/portable_icons.cpp
    ui/history_delegate.cpp
    ui/main_window.cpp
    ui/main_window_preview.cpp
    ui/settings_dialog.cpp
    resources/resources.qrc
)
target_include_directories(PasteOrbitNative PRIVATE app data services ui platform)
# 平台实现由构建选择，存储层只依赖不含系统类型的接口。
if(WIN32)
    target_sources(PasteOrbitNative PRIVATE
        app/app.rc
        ui/main_window_position.cpp
        platform/windows/data_protection.cpp
        platform/windows/paste_target.cpp
        platform/windows/global_hotkey.cpp)
elseif(APPLE)
    target_sources(PasteOrbitNative PRIVATE
        platform/macos/desktop_integration.mm
        platform/macos/platform_check.cpp
        platform/macos/data_protection.cpp
        platform/macos/global_hotkey.cpp
        platform/macos/paste_target.mm
        platform/macos/main_window_position.cpp)
    target_compile_options(PasteOrbitNative PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
    target_link_libraries(PasteOrbitNative PRIVATE
        "-framework AppKit" "-framework Carbon" "-framework ApplicationServices" "-framework Security")
    set_target_properties(PasteOrbitNative PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.pasteorbit.PasteOrbit"
        MACOSX_BUNDLE_ICON_FILE "PasteOrbit.icns"
        MACOSX_BUNDLE_BUNDLE_NAME "PasteOrbit"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PASTEORBIT_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PASTEORBIT_VERSION}")
    # Finder 和 Dock 使用 icns；图标转换只在 macOS 构建目录内进行。
    set(iconset "${CMAKE_CURRENT_BINARY_DIR}/PasteOrbit.iconset")
    set(icon_commands COMMAND "${CMAKE_COMMAND}" -E make_directory "${iconset}")
    foreach(size 16 32 128 256 512)
        math(EXPR doubled "${size} * 2")
        list(APPEND icon_commands
            COMMAND sips -z ${size} ${size} "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/PasteOrbit.png"
                --out "${iconset}/icon_${size}x${size}.png"
            COMMAND sips -z ${doubled} ${doubled} "${CMAKE_CURRENT_SOURCE_DIR}/resources/icons/PasteOrbit.png"
                --out "${iconset}/icon_${size}x${size}@2x.png")
    endforeach()
    set(mac_icon "${CMAKE_CURRENT_BINARY_DIR}/PasteOrbit.icns")
    add_custom_command(OUTPUT "${mac_icon}" ${icon_commands}
        COMMAND iconutil -c icns "${iconset}" -o "${mac_icon}"
        DEPENDS resources/icons/PasteOrbit.png VERBATIM)
    set_source_files_properties("${mac_icon}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    target_sources(PasteOrbitNative PRIVATE "${mac_icon}")
    add_test(NAME macos-platform COMMAND PasteOrbitNative --platform-check)
    set_tests_properties(macos-platform PROPERTIES
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 60)
endif()
target_link_libraries(PasteOrbitNative PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Network
    qlementine
)
if(WIN32)
    target_link_libraries(PasteOrbitNative PRIVATE crypt32 user32 shell32 advapi32 bcrypt ole32 oleaut32 oleacc)
endif()
target_compile_definitions(PasteOrbitNative PRIVATE PASTEORBIT_VERSION="${PASTEORBIT_VERSION}")
set_target_properties(PasteOrbitNative PROPERTIES OUTPUT_NAME PasteOrbit)
set_target_properties(PasteOrbitNative PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
if(MSVC)
    # 避免 Windows 宏覆盖标准库的 numeric_limits::max 等接口。
    target_compile_definitions(PasteOrbitNative PRIVATE NOMINMAX)
    target_compile_options(PasteOrbitNative PRIVATE /utf-8)
    set_property(DIRECTORY "${PROJECT_SOURCE_DIR}" PROPERTY VS_STARTUP_PROJECT PasteOrbitNative)
    # 构建后部署匹配的 Qt DLL 和插件，保证 VS 直接启动时不依赖系统 PATH。
    get_target_property(qt_qmake Qt6::qmake IMPORTED_LOCATION)
    get_filename_component(qt_bin "${qt_qmake}" DIRECTORY)
    find_program(qt_deploy windeployqt HINTS "${qt_bin}" REQUIRED NO_DEFAULT_PATH)
    add_custom_command(TARGET PasteOrbitNative POST_BUILD
        COMMAND "${qt_deploy}" "$<IF:$<CONFIG:Debug>,--debug,--release>"
            --no-translations --no-compiler-runtime --no-system-dxc-compiler --no-system-d3d-compiler
            --skip-plugin-types generic,styles
            --exclude-plugins qglib,qsqlibase,qsqlmimer,qsqloci,qsqlmysql,qsqlodbc,qsqlpsql
            --dir "$<TARGET_FILE_DIR:PasteOrbitNative>" "$<TARGET_FILE:PasteOrbitNative>"
        VERBATIM)
endif()
