# Minixer VST3 插件进程级沙盒方案设计文档

> **版本**：v1.0-draft  
> **日期**：2026-07-17  
> **状态**：开发中（32-bit 桥接、PluginHost 子进程、IPC 协议规范已完成）  
> **关联文档**：[MINIXER_Development_Plan.md](../MINIXER_Development_Plan.md)

---

## 1. 文档信息

### 1.1 目的

本文档为 Minixer 插件系统的下一版改进提供详细设计方案：将 VST3 插件的**扫描**与**运行**全部迁移到独立子进程（PluginHost），实现崩溃隔离、挂起超时保护、32-bit 桥接，并为未来 macOS / Linux 移植预留跨平台抽象层。

### 1.2 范围

- **包含**：
  - 64-bit 与 32-bit PluginHost 子进程架构。
  - 主进程 ↔ 子进程的 IPC 协议（音频、控制、状态、编辑器）。
  - 扫描流程、崩溃检测、黑名单与增量扫描。
  - 跨平台抽象策略。
- **不包含**：
  - VST2 / AAX 等无法 AGPLv3 分发的格式。
  - 新格式扩展（CLAP / LV2 等后续阶段处理）。
  - 插件编辑器跨进程内嵌 HWND/Cocoa/X11（第一阶段仍使用独立浮动窗口）。

---

## 2. 约束与假设

### 2.1 许可证约束

- 项目采用 AGPLv3，任何随项目分发的代码、二进制、桥接器必须开源。
- 不可依赖 jBridge 等闭源商业桥接方案。
- VST3 SDK 可通过 GPLv3（与 AGPLv3 兼容）授权使用；VST2 / AAX SDK 不可随项目分发。

### 2.2 架构约束

- 主程序为 64-bit Windows 应用。
- 64-bit 进程无法直接加载 32-bit DLL；32-bit 插件需由独立的 32-bit 子进程加载。
- 未来需支持 macOS 与 Linux；IPC 与进程模型必须可跨平台抽象。

### 2.3 性能假设

- 目标端到端延迟 < 20ms（@48kHz / 512 samples）。
- IPC 音频传输需控制在亚毫秒级，避免额外 memcpy 与线程切换。
- 共享内存 + 无锁/轻锁环形缓冲区为首选方案。

---

## 3. 术语

| 术语 | 说明 |
|---|---|
| **Host** / **主进程** | Minixer 主程序（64-bit）。 |
| **PluginHost** / **子进程** | 独立可执行文件，负责加载并运行单个插件实例。 |
| **PluginHost64** | 64-bit 子进程，运行 64-bit VST3 插件。 |
| **PluginHost32** | 32-bit 子进程，运行 32-bit VST3 插件。 |
| **Slot** | 插件机架上的一个槽位，对应一个插件实例。 |
| **IPC** | 进程间通信（Inter-Process Communication）。 |
| **Control Channel** | 控制通道，用于收发命令与状态。 |
| **Audio Channel** | 音频通道，基于共享内存环形缓冲区。 |

---

## 4. 总体架构

### 4.1 进程模型

```
┌─────────────────────────────────────────────────────────────┐
│                    Minixer Host (64-bit)                     │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │  Mixer GUI  │  │ Plugin Rack │  │  Plugin Manager UI  │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
│         │                │                  │               │
│         ▼                ▼                  ▼               │
│  ┌──────────────────────────────────────────────────────┐  │
│  │              AudioProcessorGraph                      │  │
│  │  ┌────────┐   ┌────────┐   ┌────────┐   ┌────────┐  │  │
│  │  │IO Input│ → │Bridge  │ → │Bridge  │ → │IO Output│  │  │
│  │  │ Node   │   │ Node 1 │   │ Node 2 │   │  Node   │  │  │
│  │  └────────┘   │(IPC)   │   │(IPC)   │   └────────┘  │  │
│  │               └────┬───┘   └────┬───┘               │  │
│  └────────────────────┼────────────┼─────────────────────┘  │
└───────────────────────┼────────────┼────────────────────────┘
                        │ Shared Memory + Control Channel
        ┌───────────────┘            └───────────────┐
        ▼                                              ▼
┌─────────────────────┐                    ┌─────────────────────┐
│  PluginHost64.exe   │                    │  PluginHost64.exe   │
│  ┌───────────────┐  │                    │  ┌───────────────┐  │
│  │ 64-bit VST3   │  │                    │  │ 64-bit VST3   │  │
│  │ Plugin (EQ)   │  │                    │  │ Plugin (Comp) │  │
│  └───────────────┘  │                    │  └───────────────┘  │
└─────────────────────┘                    └─────────────────────┘

（32-bit 插件由 PluginHost32.exe 加载，IPC 协议完全相同）
```

### 4.2 设计原则

1. **一个插件实例 = 一个子进程**：崩溃只影响该插件，主进程与其他插件不受影响。
2. **统一 IPC 协议**：64-bit 与 32-bit 子进程使用完全相同的控制与音频协议。
3. **扫描与运行复用 PluginHost**：子进程支持 `scan` 模式与 `runtime` 模式两种启动方式。
4. **跨平台抽象**：IPC 底层按平台实现（Windows / macOS / Linux），上层接口一致。
5. **无闭源依赖**：所有桥接代码随项目 AGPLv3 开源。

---

## 5. PluginHost 子进程（已完成）

### 5.1 可执行文件

| 文件 | 架构 | 用途 |
|---|---|---|
| `PluginHost64.exe` | x86_64 | 加载 64-bit VST3 插件。 |
| `PluginHost32.exe` | x86 | 加载 32-bit VST3 插件。 |

两者共用同一套源码，通过编译目标区分位数。安装包需同时包含两个可执行文件。

### 5.2 启动参数

```
PluginHost64.exe --mode=runtime|scan
                 --plugin-id=<uuid>
                 --plugin-path=<absolute-path-to-vst3>
                 --ipc-key=<unique-shared-memory-key>
                 [--log-path=<path>]
```

- `--mode=scan`：仅加载插件并枚举 `PluginDescription`，然后退出。
- `--mode=runtime`：加载插件，进入音频处理循环，等待主进程命令。
- `--plugin-id`：主进程分配的唯一实例 ID，用于日志与调试。
- `--ipc-key`：共享内存与控制通道的命名前缀。

### 5.3 子进程内部结构

```
┌─────────────────────────────────────┐
│         PluginHost Process          │
│  ┌───────────────────────────────┐  │
│  │      IPC Manager              │  │
│  │  (Control Channel + Audio SHM)│  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │      Plugin Wrapper           │  │
│  │  (VST3 format via JUCE)       │  │
│  └───────────────┬───────────────┘  │
│                  │                  │
│  ┌───────────────▼───────────────┐  │
│  │      VST3 Plugin Instance     │  │
│  └───────────────────────────────┘  │
└─────────────────────────────────────┘
```

- **IPC Manager**：解析控制命令、读写共享内存、同步音频帧。
- **Plugin Wrapper**：封装 JUCE 的 `AudioPluginInstance`，处理 `prepareToPlay`、`processBlock`、`getStateInformation`、`setStateInformation` 等调用。
- **日志**：子进程将日志写入指定文件，便于定位崩溃插件。

### 5.4 扫描模式

扫描模式下子进程执行以下流程：

1. 解析命令行，加载 VST3 文件（`InitDll` + `GetPluginFactory`）。
2. 先对工厂调用 `IPluginFactory3::setHostContext`，再枚举全部类；插件类一律以完整 128 位 CID 标识，不使用索引或 32 位哈希，以规避外壳插件（WaveShell / IKM 等）在设置宿主上下文后的类索引移位。
3. 只收录 `Audio Module Class` 类别的类，转换为 `PluginDescription`（名称、厂商、类别、版本等元数据）。自主扫描器不实例化插件，因此描述中不包含输入输出通道数。
4. 对描述做基础校验（插件名称非空）。
5. 通过 Control Channel 将 `PluginDescription` 列表序列化后返回。
6. 正常退出，返回码 `0`。

超时或崩溃由主进程捕获，不污染宿主。

---

## 6. IPC 协议规范（已完成）

### 6.1 通道划分

| 通道 | 用途 | 底层实现 |
|---|---|---|
| **Control Channel** | 命令、状态、参数、状态块传输 | Windows Named Pipe / macOS & Linux Unix Domain Socket |
| **Audio Shared Memory** | 实时音频采样数据 | Platform Shared Memory + 无锁环形缓冲区 |
| **Editor Channel（可选）** | 编辑器窗口坐标、显示/隐藏事件 | 复用 Control Channel 或单独命名管道 |

### 6.2 控制协议（请求-响应 + 异步通知）

所有控制消息采用二进制帧结构：

```c
struct ControlHeader
{
    uint32_t magic;      // 'MINX' (0x4D494E58)
    uint32_t version;    // 协议版本，当前为 1
    uint32_t type;       // 消息类型
    uint32_t payloadSize;// 后续 payload 字节数
    uint64_t requestId;  // 请求 ID，异步响应原样返回
};
```

#### 6.2.1 主进程 → 子进程命令

| 消息类型 | 说明 | Payload |
|---|---|---|
| `Init` | 初始化插件 | sampleRate, bufferSize, inputChannels, outputChannels |
| `PrepareToPlay` | 准备播放 | sampleRate, bufferSize |
| `ReleaseResources` | 释放资源 | 空 |
| `ProcessBlock` | 触发一帧处理 | 空（音频数据通过 SHM） |
| `SetState` | 恢复插件状态 | 二进制状态块 |
| `GetState` | 获取插件状态 | 空 |
| `SetParameter` | 设置参数 | parameterIndex, value |
| `GetLatency` | 获取延迟样本数 | 空 |
| `ShowEditor` | 显示编辑器窗口 | windowHandle (平台相关) |
| `HideEditor` | 隐藏编辑器窗口 | 空 |
| `Shutdown` | 优雅关闭 | 空 |

#### 6.2.2 子进程 → 主进程响应/通知

| 消息类型 | 说明 | Payload |
|---|---|---|
| `InitResult` | 初始化结果 | success bool, error string |
| `StateData` | 状态数据 | 二进制状态块 |
| `LatencyInfo` | 延迟信息 | latencySamples |
| `EditorClosed` | 编辑器关闭通知 | 空 |
| `LogMessage` | 子进程日志 | severity, message |
| `Error` | 运行时错误 | errorCode, message |

### 6.3 音频共享内存布局

每个插件实例分配一块共享内存，包含控制头 + 输入环缓冲 + 输出环缓冲。

```c
struct AudioSharedMemory
{
    // 控制头（原子访问）
    alignas(64) Atomic<uint32_t> hostWriteSeq;   // 主进程写入序列号
    alignas(64) Atomic<uint32_t> hostReadSeq;    // 主进程读取序列号
    alignas(64) Atomic<uint32_t> pluginWriteSeq; // 子进程写入序列号
    alignas(64) Atomic<uint32_t> pluginReadSeq;  // 子进程读取序列号

    uint32_t maxFramesPerBlock;  // 最大块大小
    uint32_t numInputChannels;
    uint32_t numOutputChannels;

    // 缓冲区实际数据紧跟其后
    // float inputChannels[numInputChannels][maxFramesPerBlock];
    // float outputChannels[numOutputChannels][maxFramesPerBlock];
};
```

#### 6.3.1 同步策略

- 采用**单生产者单消费者**模型：主进程写输入、读输出；子进程读输入、写输出。
- 使用序列号（sequence counter）代替锁，避免音频线程阻塞。
- 若子进程未及时消费，主进程可选择：
  - 重复上一帧（hold）。
  - 输出静音。
  - 标记该插件为挂起并旁通。

### 6.4 状态传输

- `SetState` / `GetState` 的 payload 为插件私有状态（`AudioProcessor::getStateInformation` 返回的二进制数据）。
- 大状态块（> 64KB）建议分片传输，避免命名管道缓冲区限制。
- 传输完成后由子进程调用 `setStateInformation`。

### 6.5 编辑器窗口

第一阶段策略：编辑器作为独立浮动窗口运行在 PluginHost 进程中。

- 主进程发送 `ShowEditor` 命令，可携带父窗口句柄（可选，用于未来内嵌）。
- 子进程调用 `createEditorIfNeeded()` 并显示窗口。
- 窗口标题显示插件名称与槽位编号，便于用户识别。
- 子进程崩溃时，编辑器窗口随进程消失；主进程随后提示用户。

---

## 7. 插件生命周期（已完成）

### 7.1 加载流程

1. 主进程根据插件 PE 头判断架构（复用 [PluginRegistry.cpp](../Source/Plugin/PluginRegistry.cpp) 中的 `shouldAttemptToLoadVst3` 逻辑）。
2. 选择对应 PluginHost 可执行文件（64-bit → PluginHost64，32-bit → PluginHost32）。
3. 生成唯一 `ipc-key` 与 `plugin-id`。
4. 创建共享内存与控制通道。
5. 启动子进程，传递命令行参数。
6. 等待子进程 `InitResult` 响应。
7. 若成功，将 Bridge Node 加入 `AudioProcessorGraph`。
8. 若失败，记录错误，该槽位保持空或提示用户。

### 7.2 运行期流程

1. 主进程音频回调触发 Bridge Node 的 `processBlock`。
2. Bridge Node 将输入采样写入共享内存输入缓冲区。
3. 通过 Control Channel 发送 `ProcessBlock` 命令（或采用音频帧隐式触发，减少控制消息）。
4. 子进程读取输入、调用插件 `processBlock`、写入输出。
5. 主进程读取共享内存输出缓冲区，继续后续信号链。

### 7.3 卸载流程

1. 关闭编辑器窗口（若打开）。
2. 发送 `Shutdown` 命令。
3. 等待子进程优雅退出（最多 2 秒）。
4. 若未退出，调用 `TerminateProcess` / `kill`。
5. 关闭共享内存与控制通道。
6. 从 `AudioProcessorGraph` 移除 Bridge Node。

---

## 8. 扫描流程（已完成）

### 8.1 全量扫描

1. 主进程枚举扫描路径下所有 `.vst3` 文件。
2. 对每个文件启动 PluginHost 子进程（`--mode=scan`）。
3. 子进程按 5.4 流程加载并枚举插件描述。
4. 主进程收集描述或错误信息。
5. 超时（建议 30 秒）或崩溃则记录失败原因。
6. 更新 `KnownPluginList` 与扫描报告 UI。
7. 保存扫描结果到 `PluginList.xml`。

### 8.2 增量扫描

为每个已知插件记录元数据：

```xml
<PLUGIN filePath="..."
        lastModifiedTime="..."
        fileSize="..."
        fileHash="sha256"
        ... />
```

扫描时：

1. 检查文件是否存在。
2. 比较修改时间、大小、哈希。
3. 未变化且上次扫描成功的插件跳过重新扫描（复用已保存的描述）。
4. 新增、修改、上次失败的插件重新扫描。

### 8.3 扫描报告

扫描结束后向用户展示（文件数与插件数分开口径）：

- 实际重扫成功的文件数（不含跳过）。
- 新增插件数（按描述条数，仅来自实际重扫的文件）。
- 更新插件数（按描述条数，仅来自实际重扫的文件）。
- 未变化而跳过的文件数；这些文件复用已有描述，不参与新增/更新统计。
- 失败文件列表：路径、失败原因（崩溃 / 超时 / 不兼容 / 缺失依赖 / 其他）。
- 黑名单跳过的文件数：可通过 Options > `Rescan failed plug-ins` 重试。

---

## 9. 崩溃检测与黑名单（已完成）

### 9.1 崩溃检测机制

主进程在启动子进程后保留其进程句柄：

- Windows：`WaitForSingleObject` / `RegisterWaitForSingleObject`。
- macOS / Linux：`waitpid` / `kqueue` / `signalfd`。

检测到子进程退出后：

1. 读取退出码。
2. 非零退出码或异常终止视为崩溃。
3. 生成 minidump（Windows：`MiniDumpWriteDump`）。
4. 在主进程中将该槽位自动旁通，UI 显示崩溃提示。
5. 记录崩溃事件：时间、退出码、插件路径、槽位索引。

### 9.2 挂起检测

除崩溃外，还需检测子进程挂起：

- 为每个 `ProcessBlock` 设置最大允许耗时（如 100ms）。
- 若子进程连续多帧未更新输出序列号，判定为挂起。
- 主进程杀进程、旁通槽位、记录挂起事件。

### 9.3 黑名单持久化

黑名单文件：`%AppData%/Minixer/PluginBlacklist.json`

```json
{
  "entries": [
    {
      "filePath": "C:/.../BadPlugin.vst3",
      "reason": "crash",
      "crashCount": 3,
      "lastCrashTime": "2026-07-17T22:00:00Z",
      "permanentlySkipped": true
    }
  ]
}
```

规则：

- 首次崩溃/扫描失败：加入黑名单，下次扫描默认跳过。
- 连续 3 次失败：`permanentlySkipped = true`，仅在用户显式选择重试时才会再次扫描。
- 崩溃后 7 天内不自动重试。
- 重试方式：Plugin Manager 的 Options 菜单提供三个互不重叠的入口：
  - `Scan`：增量扫描，跳过黑名单。
  - `Rescan failed plug-ins`：只重新扫描黑名单中的插件（含 `permanentlySkipped`），已成功/已存在插件不进入本次扫描；不预先清空黑名单，某文件成功加载后移除其记录，失败则按 `crashCount = 1` 重新计数。
  - `Fully rescan`：忽略增量缓存重扫全部文件，并包含黑名单插件重试。

### 9.4 用户界面

- Plugin Manager 的 Options 菜单提供上述三个扫描入口，不再使用复选框。
- 扫描报告中显示本次跳过的黑名单插件数量，并提示用户通过 Options > `Rescan failed plug-ins` 重试。
- 运行时若桥接插件崩溃，自动旁通对应槽位，并在主界面状态栏显示崩溃提示。

---

## 10. 32-bit 桥接

### 10.1 架构判断（已完成）

复用并扩展现有 PE 头读取逻辑（[PluginRegistry.cpp](../Source/Plugin/PluginRegistry.cpp)）：

- `IMAGE_FILE_MACHINE_I386` (0x014c) → 32-bit → 使用 `PluginHost32.exe`。
- `IMAGE_FILE_MACHINE_AMD64` (0x8664) → 64-bit → 使用 `PluginHost64.exe`。
- `IMAGE_FILE_MACHINE_ARM64` (0xaa64) → 暂不支持。

### 10.2 32-bit PluginHost（已完成）

`PluginHost32.exe` 与 64-bit 版本共用源码，编译目标为 Win32。

注意事项：

- 指针宽度差异不影响二进制协议（使用固定宽度类型 `uint32_t` / `uint64_t`）。
- 共享内存句柄在 64-bit 主进程与 32-bit 子进程间可跨进程传递（Windows 命名对象）。
- 编辑器窗口由 32-bit 子进程自己创建，主进程无需处理 HWND 跨位数问题。

### 10.3 分发方式（已完成）

- 安装包同时包含 `PluginHost64.exe` 与 `PluginHost32.exe`。
- 若用户未安装 32-bit 插件，32-bit 子进程不会被启动。

---

## 11. 跨平台策略

### 11.1 抽象层

定义 `IpcTransport` 接口，按平台实现：

```cpp
class IpcTransport
{
public:
    virtual ~IpcTransport() = default;
    virtual bool connect(const String& key) = 0;
    virtual bool accept(const String& key) = 0;
    virtual bool sendMessage(const MemoryBlock& data) = 0;
    virtual bool readMessage(MemoryBlock& data, int timeoutMs) = 0;
    virtual void close() = 0;
};
```

类似地定义 `SharedMemoryRegion`、`ProcessLauncher` 抽象。

### 11.2 平台映射

| 功能 | Windows | macOS | Linux |
|---|---|---|---|
| 共享内存 | `CreateFileMapping` / `MapViewOfFile` | `shm_open` + `mmap` | `shm_open` + `mmap` |
| 控制通道 | Named Pipe | Unix Domain Socket | Unix Domain Socket |
| 进程启动 | `CreateProcess` | `posix_spawn` / `fork+exec` | `posix_spawn` / `fork+exec` |
| 崩溃检测 | `WaitForSingleObject` | `waitpid` | `waitpid` |
| 崩溃转储 | `MiniDumpWriteDump` | `exc_server` / 信号处理 | 信号处理 + `backtrace` |
| 编辑器窗口 | HWND (floating) | NSWindow (floating) | X11/Wayland (floating) |

### 11.3 第一阶段实现

本阶段仅实现 Windows 版本抽象；接口设计保留 macOS / Linux 扩展点。

---

## 12. 安全与性能

### 12.1 安全

- 子进程不访问网络、不读写用户文件（除日志与插件文件）。
- 控制消息必须校验 `magic` 与 `version`。
- 共享内存大小在创建时固定，防止越界写入。
- `payloadSize` 有上限（如 16MB），避免畸形消息导致内存耗尽。

### 12.2 性能

- 音频缓冲区使用非交错（non-interleaved）float 布局，匹配 JUCE 内部格式。
- 序列号使用 `std::atomic` 或平台原子操作，避免锁竞争。
- 控制通道消息在音频线程之外处理，避免阻塞。
- 大块状态传输分片，避免单次大内存拷贝。

### 12.3 延迟补偿

- 子进程通过 `LatencyInfo` 消息报告 `latencySamples`。
- 主进程在 Bridge Node 中记录每个插件延迟。
- 后续阶段可在信号链末端添加补偿延迟线（PDC）。

---

## 13. 任务清单与阶段

### Phase 1：Windows 基础沙盒（必须在本阶段完成）

| # | 任务 | 说明 | 优先级 |
|---|---|---|---|
| 1.1 | 创建 `PluginHost` 工程 | 在 JUCE 中新增 console app 目标，链接 VST3 format。 | 高 |
| 1.2 | 定义 IPC 抽象接口 | `IpcTransport`、`SharedMemoryRegion`、`ProcessLauncher`。 | 高 |
| 1.3 | 实现 Windows IPC | Named Pipe + Shared Memory + 事件同步。 | 高 |
| 1.4 | 实现扫描模式 | 子进程枚举插件描述，主进程收集结果。 | 高 |
| 1.5 | 实现运行期模式 | 音频处理循环、参数/状态传输、编辑器浮动窗口。 | 高 |
| 1.6 | 创建 Bridge Node | `AudioProcessorGraph` 中的 IPC 桥接节点。 | 高 |
| 1.7 | 崩溃检测与黑名单 | 进程监控、minidump、黑名单持久化。 | 高 |
| 1.8 | 32-bit PluginHost（已完成） | 编译 Win32 版本，打通 64-bit 主进程 ↔ 32-bit 子进程。 | 高 |
| 1.9 | 扫描报告 UI | 在 Plugin Manager 中展示成功/失败/黑名单。 | 中 |
| 1.10 | 增量扫描 | 文件哈希/修改时间比对，只扫描变更项。 | 中 |

### Phase 2：稳定性与体验优化

| # | 任务 | 说明 |
|---|---|---|
| 2.1 | 挂起检测 | 音频帧超时保护。 |
| 2.2 | 崩溃日志聚合 | 统一日志格式，便于用户反馈。 |
| 2.3 | 插件重启恢复 | 崩溃后一键重启并恢复状态。 |
| 2.4 | 延迟补偿 | 在 Graph 中实现链式 PDC。 |

### Phase 3：跨平台与格式扩展

| # | 任务 | 说明 |
|---|---|---|
| 3.1 | macOS IPC 实现 | `shm_open` + Unix Domain Socket。 |
| 3.2 | Linux IPC 实现 | 同 macOS 方案。 |
| 3.3 | CLAP 支持 | 添加 CLAP format 到 PluginHost。 |
| 3.4 | LV2 支持 | 评估 JUCE LV2 支持状态。 |

---

## 14. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| IPC 延迟过高 | 实时性受损 | 使用共享内存 + 无锁环形缓冲区；控制通道与音频分离；必要时增大缓冲区。 |
| 插件 UI 跨进程复杂 | 开发周期延长 | 第一阶段使用独立浮动窗口；内嵌延后。 |
| 32-bit 桥接工作量大 | 延迟交付 | 32-bit 与 64-bit 共用源码与协议；优先保证 64-bit 稳定后再验证 32-bit。 |
| 子进程异常退出未恢复 | 用户音频中断 | 主进程自动旁通并提示；提供一键重启。 |
| 共享内存句柄泄露 | 资源耗尽 | 使用 RAII 封装，确保子进程退出时主进程清理。 |
| 某些插件依赖全局状态 | 子进程内仍可能崩溃 | 崩溃已被隔离；加入黑名单并记录。 |

---

## 15. 验收标准

### 15.1 功能验收

- [ ] 64-bit VST3 插件可在独立子进程中加载并正常运行。
- [ ] 32-bit VST3 插件可通过 `PluginHost32.exe` 加载并正常运行。
- [ ] 子进程崩溃不影响主进程与其他插件。
- [ ] 崩溃插件自动进入黑名单，下次扫描跳过。
- [ ] 扫描阶段崩溃/挂起不拖垮主进程。
- [ ] 增量扫描只重新扫描变更插件。
- [ ] 扫描报告 UI 正确显示成功/失败/黑名单。

### 15.2 性能验收

- [ ] 端到端延迟 < 20ms（@48kHz / 512 samples，单插件）。
- [ ] CPU 占用相比进程内模式增加 < 5%（典型场景）。
- [ ] 子进程崩溃检测时间 < 500ms。

### 15.3 质量验收

- [ ] 所有新增代码符合 AGPLv3 要求，无闭源依赖。
- [ ] Windows 10/11 测试通过。
- [ ] 内存/句柄无泄漏（通过长时间运行测试）。

---

## 16. 附录

### 16.1 文件清单建议

```
Source/
├── Plugin/
│   ├── PluginRegistry.*          （现有，扩展架构判断）
│   ├── PluginHostLauncher.*      （新增：启动子进程）
│   ├── PluginHostClient.*        （新增：主进程 IPC 客户端）
│   ├── PluginBridgeNode.*        （新增：Graph 桥接节点）
│   └── PluginBlacklist.*         （新增：黑名单管理）
├── IPC/
│   ├── IpcTransport.h            （新增：抽象接口）
│   ├── WindowsIpcTransport.*     （新增：Windows 实现）
│   ├── SharedMemory.h            （新增：抽象接口）
│   └── WindowsSharedMemory.*     （新增：Windows 实现）
└── PluginHost/                   （新增：子进程工程）
    ├── Main.cpp
    ├── PluginHostServer.*        （子进程 IPC 服务端）
    └── PluginWrapper.*           （VST3 插件封装）
```

### 16.2 参考

- JUCE 8 `AudioPluginFormat`、`AudioPluginInstance`、`AudioProcessorGraph` 文档。
- Steinberg VST3 SDK 授权说明（GPLv3，与 AGPLv3 兼容）。
- Windows IPC：Named Pipes, File Mapping, Events。
- POSIX IPC：`shm_open`, `mmap`, `socket(AF_UNIX)`.
