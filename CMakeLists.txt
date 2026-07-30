cmake_minimum_required(VERSION 3.22)

project(Minixer
    VERSION 1.0.0
    DESCRIPTION "轻量级单通道独立音频混音台"
    LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ==============================================================================
# 1. 定位 JUCE
# ==============================================================================
# 优先级：
#   1. CMake 命令行参数 -D JUCE_DIR=<path>
#   2. 环境变量 JUCE_DIR
#   3. 项目根目录附近的常见位置（../JUCE、../../JUCE 等）
#
# 设置后即可使用本 CMake 配置，无需再打开 Projucer 重新导出 IDE 工程。
if(NOT DEFINED JUCE_DIR)
    if(DEFINED ENV{JUCE_DIR})
        file(TO_CMAKE_PATH "$ENV{JUCE_DIR}" JUCE_DIR)
    endif()
endif()

if(NOT JUCE_DIR)
    set(_juce_candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/../JUCE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../JUCE"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../JUCE"
        "$ENV{USERPROFILE}/JUCE"
        "$ENV{USERPROFILE}/source/JUCE"
        "$ENV{USERPROFILE}/source/repos/JUCE"
        "$ENV{ProgramFiles}/JUCE"
        "$ENV{ProgramFiles(x86)}/JUCE"
        "C:/JUCE"
        "D:/JUCE")

    foreach(_candidate IN LISTS _juce_candidates)
        if(EXISTS "${_candidate}/CMakeLists.txt")
            set(JUCE_DIR "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT JUCE_DIR OR NOT EXISTS "${JUCE_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "找不到 JUCE 根目录。请通过以下任一方式指定：\n"
        "  1. 设置环境变量 JUCE_DIR=<JUCE 根目录>\n"
        "  2. 配置时传入 -D JUCE_DIR=<JUCE 根目录>\n"
        "  3. 将 JUCE 放在项目根目录的 ../JUCE、../../JUCE 等常见位置。")
endif()

message(STATUS "Minixer: 使用 JUCE 目录: ${JUCE_DIR}")
add_subdirectory("${JUCE_DIR}" JUCE)

# ==============================================================================
# 1.5 ASIO 支持
# ==============================================================================
# 本项目使用 Steinberg ASIO SDK 提供 ASIO 音频设备支持。
# 使用 ASIO SDK 即表示你同意 Steinberg 的许可协议：
#   https://www.steinberg.net/en/company/developers.html
# ASIO 是 Steinberg Media Technologies GmbH 的注册商标。
if(NOT DEFINED ASIO_DIR)
    if(DEFINED ENV{ASIO_DIR})
        file(TO_CMAKE_PATH "$ENV{ASIO_DIR}" ASIO_DIR)
    endif()
endif()

if(NOT ASIO_DIR)
    set(_asio_candidates
        "${CMAKE_CURRENT_SOURCE_DIR}/../SDK/asiosdk_2.3.3_2019-06-14"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../SDK/asiosdk_2.3.3_2019-06-14"
        "${CMAKE_CURRENT_SOURCE_DIR}/../../../SDK/asiosdk_2.3.3_2019-06-14"
        "$ENV{USERPROFILE}/SDK/asiosdk_2.3.3_2019-06-14"
        "C:/SDK/asiosdk_2.3.3_2019-06-14"
        "D:/CodePacks/Store/SDK/asiosdk_2.3.3_2019-06-14")

    foreach(_candidate IN LISTS _asio_candidates)
        if(EXISTS "${_candidate}/common/iasiodrv.h")
            set(ASIO_DIR "${_candidate}")
            break()
        endif()
    endforeach()
endif()

if(NOT ASIO_DIR OR NOT EXISTS "${ASIO_DIR}/common/iasiodrv.h")
    message(FATAL_ERROR
        "找不到 Steinberg ASIO SDK。请通过以下任一方式指定：\n"
        "  1. 设置环境变量 ASIO_DIR=<ASIO SDK 根目录>\n"
        "  2. 配置时传入 -D ASIO_DIR=<ASIO SDK 根目录>\n"
        "  3. 将 ASIO SDK 放在常见位置（如 ../SDK/asiosdk_2.3.3_2019-06-14）。")
endif()

message(STATUS "Minixer: 使用 ASIO SDK 目录: ${ASIO_DIR}")
set(_asio_include_dir "${ASIO_DIR}/common")

# ==============================================================================
# 2. 源文件
# ==============================================================================
set(_minixer_sources
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/MainComponent.h
    Source/LookAndFeel/MixerLookAndFeel.cpp
    Source/LookAndFeel/MixerLookAndFeel.h
    Source/Plugin/PluginRegistry.cpp
    Source/Plugin/PluginRegistry.h
    Source/Plugin/PluginManagerComponent.cpp
    Source/Plugin/PluginManagerComponent.h
    Source/Plugin/PluginEditorWindow.cpp
    Source/Plugin/PluginEditorWindow.h
    Source/Plugin/PluginArchitecture.cpp
    Source/Plugin/PluginArchitecture.h
    Source/Plugin/PluginHostLauncher.cpp
    Source/Plugin/PluginHostLauncher.h
    Source/Plugin/PluginHostClient.cpp
    Source/Plugin/PluginHostClient.h
    Source/Plugin/PluginBridgeNode.cpp
    Source/Plugin/PluginBridgeNode.h
    Source/Plugin/PluginBlacklist.cpp
    Source/Plugin/PluginBlacklist.h
    Source/Plugin/PluginSlotState.h
    Source/IPC/IpcProtocol.h
    Source/IPC/IpcTransport.h
    Source/IPC/SharedMemoryRegion.h
    Source/IPC/WindowsIpcTransport.cpp
    Source/IPC/WindowsIpcTransport.h
    Source/IPC/WindowsSharedMemoryRegion.cpp
    Source/IPC/WindowsSharedMemoryRegion.h
    Source/Settings/AppSettings.cpp
    Source/Settings/AppSettings.h
    Source/Settings/KeyboardShortcut.cpp
    Source/Settings/KeyboardShortcut.h
    Source/Settings/ShortcutInputSource.cpp
    Source/Settings/ShortcutInputSource.h
    Source/Settings/MidiShortcutInputManager.cpp
    Source/Settings/MidiShortcutInputManager.h
    Source/Settings/HidShortcutInputManager.cpp
    Source/Settings/HidShortcutInputManager.h
    Source/Components/LevelMeterComponent.cpp
    Source/Components/LevelMeterComponent.h
    Source/Components/RotaryKnobComponent.cpp
    Source/Components/RotaryKnobComponent.h
    Source/Components/FaderComponent.cpp
    Source/Components/FaderComponent.h
    Source/Components/PluginSlotComponent.cpp
    Source/Components/PluginSlotComponent.h
    Source/Components/ChannelStripComponent.cpp
    Source/Components/ChannelStripComponent.h
    Source/Components/SettingsComponent.cpp
    Source/Components/SettingsComponent.h
    Source/Components/PresetBarComponent.cpp
    Source/Components/PresetBarComponent.h
    Source/Components/LevelMeterProcessor.cpp
    Source/Components/LevelMeterProcessor.h
    Source/Components/KeyCaptureButton.cpp
    Source/Components/KeyCaptureButton.h
    Source/Components/ShortcutsSettingsComponent.cpp
    Source/Components/ShortcutsSettingsComponent.h
    Source/Components/AboutComponent.cpp
    Source/Components/AboutComponent.h)

set(_pluginhost_sources
    Source/PluginHost/Main.cpp
    Source/PluginHost/PluginHostServer.cpp
    Source/PluginHost/PluginHostServer.h
    Source/PluginHost/PluginWrapper.cpp
    Source/PluginHost/PluginWrapper.h
    Source/IPC/IpcProtocol.h
    Source/IPC/IpcTransport.h
    Source/IPC/SharedMemoryRegion.h
    Source/IPC/WindowsIpcTransport.cpp
    Source/IPC/WindowsIpcTransport.h
    Source/IPC/WindowsSharedMemoryRegion.cpp
    Source/IPC/WindowsSharedMemoryRegion.h)

# ==============================================================================
# 3. 主程序 Minixer (GUI app)
# ==============================================================================
# Minixer 主程序固定为 64 位。32 位构建仅用于生成 PluginHost32.exe。
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    juce_add_gui_app(Minixer
        VERSION     1.0.0
        COMPANY_NAME ""
        PRODUCT_NAME "Minixer"
        IS_SYNTHESIZER          FALSE
        NEEDS_MIDI_INPUT        FALSE
        NEEDS_MIDI_OUTPUT       FALSE
        IS_MIDI_EFFECT          FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE)

    # 应用图标（Windows .ico）。JUCE 的 ICON_BIG 在 Windows 上会调用 juceaide 转换图片格式，
    # 因此这里直接生成 resources.rc 并链接，避免对输入格式的额外要求。
    if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/minixer_06_warm_orange.ico")
        set(_minixer_rc "${CMAKE_CURRENT_BINARY_DIR}/Minixer_generated_resources.rc")
        file(WRITE "${_minixer_rc}"
            "// Auto-generated by CMake\n"
            "IDI_ICON1 ICON \"${CMAKE_CURRENT_SOURCE_DIR}/assets/minixer_06_warm_orange.ico\"\n")
        target_sources(Minixer PRIVATE "${_minixer_rc}")
    endif()

    target_sources(Minixer PRIVATE ${_minixer_sources})

    target_include_directories(Minixer PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/Source
        ${CMAKE_CURRENT_SOURCE_DIR}/JuceLibraryCode
        ${_asio_include_dir})

    target_compile_definitions(Minixer PRIVATE
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_PLUGINHOST_VST3=1
        JUCE_ASIO=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
        DONT_SET_USING_JUCE_NAMESPACE=1)

    if(MSVC)
        target_compile_definitions(Minixer PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_compile_options(Minixer PRIVATE /utf-8 /W4)
    else()
        target_compile_options(Minixer PRIVATE -Wall -Wextra)
    endif()

    target_link_libraries(Minixer PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_core
        juce::juce_data_structures
        juce::juce_events
        juce::juce_graphics
        juce::juce_gui_basics
        juce::juce_gui_extra)
endif()

# ==============================================================================
# 4. PluginHost 子进程 (console app)
# ==============================================================================
# PluginHost 与主程序共用同一套源码，根据编译目标位数生成：
#   x64  -> PluginHost64.exe
#   Win32-> PluginHost32.exe
#
# 注意：CMake 单次配置只能生成单一架构。若需同时获得 64 位与 32 位 PluginHost，
#       请分别用不同 generator/platform 配置两个构建目录，例如：
#         cmake -B build-x64 -A x64
#         cmake -B build-x86 -A Win32
juce_add_console_app(PluginHost
    VERSION     1.0.0
    COMPANY_NAME ""
    PRODUCT_NAME "Minixer PluginHost")

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set_target_properties(PluginHost PROPERTIES OUTPUT_NAME "PluginHost64")
else()
    set_target_properties(PluginHost PROPERTIES OUTPUT_NAME "PluginHost32")
endif()

target_sources(PluginHost PRIVATE ${_pluginhost_sources})

target_include_directories(PluginHost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Source
    ${CMAKE_CURRENT_SOURCE_DIR}/JuceLibraryCode
    ${_asio_include_dir})

target_compile_definitions(PluginHost PRIVATE
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_ASIO=1
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    DONT_SET_USING_JUCE_NAMESPACE=1)

if(MSVC)
    target_compile_definitions(PluginHost PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(PluginHost PRIVATE /utf-8 /W4 /bigobj)
else()
    target_compile_options(PluginHost PRIVATE -Wall -Wextra)
endif()

target_link_libraries(PluginHost PRIVATE
    juce::juce_audio_basics
    juce::juce_audio_devices
    juce::juce_audio_formats
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_core
    juce::juce_data_structures
    juce::juce_events
    juce::juce_graphics
    juce::juce_gui_basics
    juce::juce_gui_extra)

# Windows 下 PluginHost 需要 dbghelp.lib 写 minidump
if(WIN32)
    target_link_libraries(PluginHost PRIVATE dbghelp)
endif()

# ==============================================================================
# 5. 构建后：将 PluginHost 复制到 Minixer 输出目录
# ==============================================================================
# 运行期要求 PluginHost64.exe / PluginHost32.exe 与 Minixer.exe 位于同一目录。
# x64 构建时复制到同目录 Minixer 旁边；x86 构建时复制到 64 位 Minixer 的固定输出目录。
if(WIN32)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        add_custom_command(TARGET PluginHost POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:Minixer>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:PluginHost>"
                "$<TARGET_FILE_DIR:Minixer>/")
    else()
        set(_minixer_release_dir "${CMAKE_CURRENT_SOURCE_DIR}/build/Minixer_artefacts/Release")
        add_custom_command(TARGET PluginHost POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "${_minixer_release_dir}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:PluginHost>"
                "${_minixer_release_dir}/")
    endif()
endif()

# ==============================================================================
# 6. 安装/打包辅助（可选）
# ==============================================================================
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    install(TARGETS Minixer PluginHost
        RUNTIME DESTINATION bin)
else()
    install(TARGETS PluginHost
        RUNTIME DESTINATION bin)
endif()

if(WIN32)
    install(FILES "$<TARGET_FILE:PluginHost>" DESTINATION bin)
endif()
