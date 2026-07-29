# Minixer (zh-CN)

轻量级单通道独立音频混音台（Standalone Mixer），基于 JUCE 8 / C++17 / Windows 开发。

Minixer 常驻于系统后台，可将硬件麦克风输入经 VST3 插件链（EQ、Compressor、Gate 等）实时处理后，输出至任意 Windows 音频设备（如 VB-Audio Virtual Cable），供 Discord、Zoom、OBS、游戏等第三方软件作为虚拟麦克风输入。

## 核心功能

- **独立输入 / 输出设备选择**：支持 WASAPI、DirectSound 等 Windows 音频驱动，可独立指定输入/输出设备、采样率与缓冲区大小。
- **VST3 插件机架**：12 个插件插槽，支持 64-bit / 32-bit VST3 插件动态加载、旁通、删除。
- **插件崩溃隔离（进程级沙盒）**：每个插件实例运行在独立的 `PluginHost64.exe` / `PluginHost32.exe` 子进程中，单个插件崩溃自动旁通，不影响主程序。
- **实时电平监测**：输入 / 输出 Peak / RMS 电平表，dBFS 刻度，过载预警。
- **通道条控制**：Input Trim、Pan、Stereo、Output Fader，支持 dB 刻度微调。
- **预设系统**：保存 / 加载整套插件链状态与通道条参数。
- **插件扫描与黑名单**：后台扫描 VST3 插件目录，崩溃/超时插件自动加入黑名单并跳过。
- **系统常驻**：最小化至系统托盘，支持开机自启、全局快捷键、配置持久化。

## 技术栈

- JUCE 8
- C++17
- CMake
- Visual Studio 2022
- Windows 10 / 11

## 构建步骤（CMake）

### 1. 准备 JUCE

下载并安装 [JUCE 8](https://juce.com/) 到本机任意目录。

> 本项目不包含 JUCE 源码，请自行准备。

### 2. 配置 JUCE 路径

以下任一方式均可：

- 设置环境变量：`JUCE_DIR=<JUCE 根目录>`
- 配置时传入：`-D JUCE_DIR=<JUCE 根目录>`
- 将 JUCE 放在项目根目录附近的常见位置，如 `../JUCE`、`../../JUCE`

### 3. 生成并编译

```powershell
# x64（主程序 + 64 位 PluginHost）
cmake -B build -S . -A x64 -T host=x64
cmake --build build --config Debug

# 32 位 PluginHost（用于加载 32-bit VST3 插件）
cmake -B build-x86 -S . -A Win32 -T host=x64
cmake --build build-x86 --target PluginHost --config Debug
```

构建完成后：

- `build/Minixer_artefacts/Debug/Minixer.exe`
- `build/Minixer_artefacts/Debug/PluginHost64.exe`
- `build-x86/PluginHost_artefacts/Debug/PluginHost32.exe`

CMake 构建脚本会在 PluginHost 编译完成后自动将其复制到 Minixer 输出目录。

### 4. 运行

确保 `PluginHost64.exe` 与 `PluginHost32.exe` 与 `Minixer.exe` 位于同一目录，然后运行 `Minixer.exe`。

> 如需继续使用 Projucer，保留的 `Minixer.jucer` 仍可打开和导出，但 CMake 是推荐的本地开发方式。

## 目录结构

```
Minixer/
├── Source/                 # 项目源码
│   ├── Components/         # UI 组件
│   ├── IPC/                # 进程间通信抽象与 Windows 实现
│   ├── LookAndFeel/        # 自定义外观
│   ├── Plugin/             # 插件宿主、机架、桥接节点
│   ├── PluginHost/         # PluginHost 子进程源码
│   └── Settings/           # 应用设置与快捷键
├── JuceLibraryCode/        # Projucer 生成的 JUCE 包含文件
├── assets/                 # 图标资源
├── Builds/                 # 导出的 IDE 工程（编译产物已忽略）
├── docs/                   # 设计文档
├── Minixer.jucer           # JUCE 工程文件（Projucer）
├── CMakeLists.txt          # CMake 构建配置
└── README.md
```

## 注意事项

- 本项目**不包含 ASIO SDK**，默认使用 WASAPI / DirectSound 音频驱动。如需 ASIO 支持，请自行下载 Steinberg ASIO SDK 并配置项目。
- 推荐使用 CMake 构建。Projucer 工作流仍可通过 `Minixer.jucer` 使用，但其导出的 Visual Studio 工程可能包含本机绝对路径，换机或上传前需重新配置Minixer.jucer中的JUCE模块包含路径。
- 项目仅支持 VST3 插件，不支持 VST2 / AAX。
- 运行时需要 `PluginHost64.exe` 与 `PluginHost32.exe` 与主程序位于同一目录，用于加载 64-bit / 32-bit 插件的沙盒子进程。

## 开源协议

GNU Affero General Public License v3.0 (AGPLv3)

详见项目根目录 [LICENSE](LICENSE) 文件。

---

# Minixer (English)

A lightweight single-channel standalone audio mixer, built with JUCE 8 / C++17 / Windows.

Minixer runs in the background, taking hardware microphone input through a VST3 plugin chain (EQ, Compressor, Gate, etc.) and routing the processed signal to any Windows audio device (e.g. VB-Audio Virtual Cable). Third-party applications such as Discord, Zoom, OBS, and games can then use it as a virtual microphone input.

## Features

- **Independent input / output device selection**: supports WASAPI and DirectSound drivers; input and output devices, sample rate, and buffer size can be configured independently.
- **VST3 plugin rack**: 12 plugin slots with dynamic load, bypass, and removal for both 64-bit and 32-bit VST3 plugins.
- **Plugin crash isolation (process-level sandbox)**: each plugin instance runs in a separate `PluginHost64.exe` or `PluginHost32.exe` child process. Crashes are isolated and the slot is automatically bypassed without affecting the main mixer.
- **Real-time level metering**: input / output Peak / RMS meters with dBFS scale and clip warning.
- **Channel strip controls**: Input Trim, Pan, Stereo, and Output Fader with fine dB-scale adjustment.
- **Preset system**: save and load the entire plugin chain state together with channel strip parameters.
- **Plugin scanning and blacklist**: scan VST3 plugin directories in the background; plugins that crash or time out are automatically added to a blacklist and skipped.
- **System resident**: minimize to the Windows system tray, with startup launch, global shortcuts, and persistent configuration.

## Tech Stack

- JUCE 8
- C++17
- CMake
- Visual Studio 2022
- Windows 10 / 11

## Build Instructions (CMake)

### 1. Prepare JUCE

Download and install [JUCE 8](https://juce.com/) to any location on your machine.

> This project does not include the JUCE source code. Please prepare it separately.

### 2. Configure the JUCE path

Use any of the following methods:

- Set environment variable: `JUCE_DIR=<JUCE root directory>`
- Pass during configuration: `-D JUCE_DIR=<JUCE root directory>`
- Place JUCE in a common location near the project root, such as `../JUCE` or `../../JUCE`

### 3. Generate and build

```powershell
# x64 (main app + 64-bit PluginHost)
cmake -B build -S . -A x64 -T host=x64
cmake --build build --config Debug

# 32-bit PluginHost (For loading 32-bit VST3 plugins)
cmake -B build-x86 -S . -A Win32 -T host=x64
cmake --build build-x86 --target PluginHost --config Debug
```

After building:

- `build/Minixer_artefacts/Debug/Minixer.exe`
- `build/Minixer_artefacts/Debug/PluginHost64.exe`
- `build-x86/PluginHost_artefacts/Debug/PluginHost32.exe`

The CMake build script automatically copies PluginHost to the Minixer output directory after compilation.

### 4. Run

Make sure `PluginHost64.exe` and `PluginHost32.exe` are in the same directory as `Minixer.exe`, then run `Minixer.exe`.

> To continue using the Projucer workflow, the retained `Minixer.jucer` can still be opened and exported, but CMake is the recommended local development approach.

## Directory Structure

```
Minixer/
├── Source/                 # Project source code
│   ├── Components/         # UI components
│   ├── IPC/                # IPC abstractions and Windows implementations
│   ├── LookAndFeel/        # Custom look-and-feel
│   ├── Plugin/             # Plugin hosting, rack, and bridge nodes
│   ├── PluginHost/         # PluginHost child-process source
│   └── Settings/           # Application settings and shortcuts
├── JuceLibraryCode/        # Auto-generated JUCE include files
├── assets/                 # Icon resources
├── Builds/                 # Exported IDE projects (build artifacts ignored)
├── docs/                   # Design documents
├── Minixer.jucer           # JUCE project file (Projucer)
├── CMakeLists.txt          # CMake build configuration
└── README.md
```

## Notes

- This project **does not include the ASIO SDK** and uses WASAPI / DirectSound by default. Add the Steinberg ASIO SDK manually if ASIO support is required.
- CMake is the recommended build system. The Projucer workflow is still available via `Minixer.jucer`, but may contain machine-specific absolute JUCE module include paths that need to be reconfigured before use on another machine.
- Only VST3 plugins are supported; VST2 / AAX are not supported.
- At runtime, `PluginHost64.exe` and `PluginHost32.exe` must be in the same directory as the main executable, as they are used to sandbox 64-bit and 32-bit plugins.

## License

GNU Affero General Public License v3.0 (AGPLv3)

See the [LICENSE](LICENSE) file in the project root for the full text.
