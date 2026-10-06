add_executable(PasteOrbitNative WIN32
    app/main.cpp
    app/app_settings.cpp
    app/localization.cpp
    data/history_store.cpp
    data/history_model.cpp
    services/backup_service.cpp
    services/update_service.cpp
    ui/history_delegate.cpp
    ui/main_window.cpp
    ui/settings_dialog.cpp
    app/app.rc
    resources/resources.qrc
)
target_include_directories(PasteOrbitNative PRIVATE app data services ui)
target_link_libraries(PasteOrbitNative PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    Qt6::Network
    qlementine
    crypt32
    user32
    shell32
    advapi32
    bcrypt
    ole32
    oleaut32
    oleacc
)
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
            --no-translations --no-compiler-runtime
            --dir "$<TARGET_FILE_DIR:PasteOrbitNative>" "$<TARGET_FILE:PasteOrbitNative>"
        VERBATIM)
endif()
