/*
  ==============================================================================

    PluginRegistry.cpp

  ==============================================================================
*/

#include "PluginRegistry.h"
#include "PluginArchitecture.h"
#include "PluginHostLauncher.h"
#include "PluginBlacklist.h"
#include "PluginScanner/VST3Scanner.h"
#include "PluginScanner/VST3PluginDescriptionMapper.h"
#include "../IPC/IpcTransport.h"
#include "../IPC/IpcProtocol.h"
#include "../Settings/AppSettings.h"

#if JUCE_WINDOWS
 #if JUCE_MSVC
  #include <windows.h>
  #include <winternl.h>
 #endif
 #include <excpt.h>
 #include <bcrypt.h>
 #pragma comment(lib, "bcrypt.lib")
#endif

namespace minixer
{

namespace
{

//==============================================================================
/** 扫描空闲超时阈值（秒）。

    当没有文件正在扫描且超过该时间未收到新扫描活动时，认为本次扫描已结束。
    现代 DAW 的插件扫描通常允许单个插件耗时数秒（尤其是 32-bit 桥接子进程），
    因此阈值需足够宽松，避免把慢速文件误判为扫描结束。
*/
constexpr double pluginScanIdleTimeoutSeconds = 5.0;

//==============================================================================
/** 清理插件元数据字符串中的控制字符，避免 PluginListComponent 渲染时触发
    juce_SimpleShapedText.cpp:310 的断言。

    仅保留常规可打印字符（>= 0x20）并移除 DEL（0x7F）。多字节 Unicode
    字符（如中文）在 juce::String 内部以 codepoint 形式处理，不会被误截断。
*/
static juce::String sanitizePluginString (const juce::String& input)
{
    if (input.isEmpty())
        return {};

    juce::String output;
    output.preallocateBytes (static_cast<size_t> (input.getNumBytesAsUTF8()) + 1);

    for (auto c = input.getCharPointer(); ! c.isEmpty(); ++c)
    {
        const auto codepoint = static_cast<juce::juce_wchar> (*c);

        if (codepoint >= 0x20 && codepoint != 0x7F)
            output += codepoint;
    }

    return output.trim();
}

static void sanitizePluginDescription (juce::PluginDescription& desc)
{
    desc.name            = sanitizePluginString (desc.name);
    desc.descriptiveName = sanitizePluginString (desc.descriptiveName);
    desc.manufacturerName= sanitizePluginString (desc.manufacturerName);
    desc.category        = sanitizePluginString (desc.category);
    desc.version         = sanitizePluginString (desc.version);
}

//==============================================================================
// —— 自主 VST3 扫描器（替换 JUCE 自带 VST3PluginFormat 扫描）——
//
// 背景：JUCE 内建 VST3 扫描/加载在 setHostContext 之前解析类索引，外壳插件
// （WaveShell / IKM 等）在设置宿主上下文后重排/追加工厂类表，导致索引失效而
// “点 A 出 B”。本工程改用自主扫描器：先 setHostContext 再按完整 128 位 CID
// 枚举，配合加载侧预加载（PluginWrapper）规避灰名错位。
//
// 扫描结果 → PluginDescription 的转换（含 JUCE 哈希复刻）位于共享头
// VST3PluginDescriptionMapper.h，主程序与 PluginHost（32 位扫描）保持一致。

/** 全局共享的扫描器实例（进程生命周期有效）。
    有意不释放：避免应用退出时对不稳定插件执行 ExitDll/FreeLibrary 导致崩溃；
    已加载模块句柄由操作系统在进程退出时统一回收。 */
static vst3scan::VST3Scanner& getCustomScanner()
{
    static auto* scanner = new vst3scan::VST3Scanner ([] (const juce::String& line)
    {
        juce::Logger::writeToLog ("[VST3Scan] " + line);
    });
    return *scanner;
}

#if JUCE_WINDOWS
namespace
{

//==============================================================================
// SEH 桥：MSVC 禁止 __try 与 C++ 对象位于同一函数（C2712），
// 采用“纯指针桥”模式——__try 内只做一次普通函数调用。
struct CustomScanJob
{
    vst3scan::VST3Scanner*   scanner;
    const juce::File*        file;
    vst3scan::PluginModuleRecord* out;
};

int runCustomScanJob (CustomScanJob* job)
{
    if (job == nullptr || job->scanner == nullptr || job->file == nullptr || job->out == nullptr)
        return 0;

    *job->out = job->scanner->scanModule (*job->file, nullptr);
    return 1;
}

int customScanShim (CustomScanJob* job)
{
    __try
    {
        return runCustomScanJob (job);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}

} // namespace

#endif

/** 自主扫描单个 .vst3 文件并转成 PluginDescription。 */
static void scanFileWithCustomScanner (const juce::File& file,
                                       juce::OwnedArray<juce::PluginDescription>& result,
                                       juce::String& errorMessage)
{
    errorMessage.clear();

    vst3scan::PluginModuleRecord rec;

   #if JUCE_WINDOWS
    CustomScanJob job { &getCustomScanner(), &file, &rec };

    try
    {
        if (customScanShim (&job) == 0)
        {
            result.clear();
            errorMessage = TRANS ("Plugin scan raised a structured exception");
            return;
        }
    }
    catch (...)
    {
        result.clear();
        errorMessage = TRANS ("Plugin scan threw a C++ exception");
        return;
    }
   #else
    try
    {
        rec = getCustomScanner().scanModule (file, nullptr);
    }
    catch (...)
    {
        result.clear();
        errorMessage = TRANS ("Plugin scan threw a C++ exception");
        return;
    }
   #endif

    if (! rec.loaded)
    {
        result.clear();
        errorMessage = rec.loadError.isNotEmpty() ? rec.loadError
                                                  : TRANS ("Failed to load module");
        return;
    }

    vst3scan::addDescriptionsFromModule (rec, result);

    if (result.isEmpty())
        errorMessage = TRANS ("No plugin descriptions found");
}

//==============================================================================
/** 通过 PluginHost 子进程扫描 32-bit 插件，返回描述列表。

    主进程创建命名管道客户端并启动 PluginHost32.exe --mode=scan，
    子进程加载插件后返回 ScanResult 消息。
*/
static bool scanPluginViaHost (const juce::String& fileOrIdentifier,
                               PluginArchitecture arch,
                               juce::OwnedArray<juce::PluginDescription>& result,
                               const std::atomic<bool>* cancelled = nullptr)
{
   #if ! JUCE_WINDOWS
    juce::ignoreUnused (fileOrIdentifier, arch, result);
    return false;
   #else
    auto ipcKey = juce::Uuid().toString();

    PluginHostLauncher launcher;
    PluginHostLaunchOptions options;
    options.pluginId   = ipcKey;
    options.pluginPath = fileOrIdentifier;
    options.ipcKey     = ipcKey;
    options.mode       = "scan";
    options.architecture = arch;

    if (! launcher.launch (options))
    {
        juce::Logger::writeToLog ("Failed to launch PluginHost for scan: " + launcher.getLastError());
        return false;
    }

    auto transport = createDefaultIpcTransport();

    if (transport == nullptr || ! transport->connect (ipcKey))
    {
        launcher.terminateProcess();
        return false;
    }

    juce::MemoryBlock frame;
    bool gotResult = false;

    // 扫描应在 30 秒内完成
    for (int attempt = 0; attempt < 300; ++attempt)
    {
        if (cancelled != nullptr && cancelled->load (std::memory_order_relaxed))
            break; // 用户取消扫描

        if (transport->readMessage (frame, 100))
        {
            gotResult = true;
            break;
        }

        if (! launcher.isRunning())
            break;
    }

    if (! gotResult)
    {
        if (launcher.didCrash())
            PluginBlacklist::getInstance().recordCrash (fileOrIdentifier, launcher.getExitCode());

        launcher.terminateProcess();
        return false;
    }

    if (frame.getSize() < ControlHeader::size)
        return false;

    ControlHeader header;
    std::memcpy (&header, frame.getData(), ControlHeader::size);

    if (! header.isValid())
        return false;

    const auto type = static_cast<ControlMessageType> (header.type);

    if (type == ControlMessageType::ScanError)
    {
        PluginBlacklist::getInstance().recordScanFailure (fileOrIdentifier, "scanFailure");
        return false;
    }

    if (type != ControlMessageType::ScanResult)
    {
        PluginBlacklist::getInstance().recordScanFailure (fileOrIdentifier, "scanFailure");
        return false;
    }

    MessageReader reader (static_cast<const uint8_t*> (frame.getData()) + ControlHeader::size,
                          frame.getSize() - ControlHeader::size);

    juce::String xmlString;
    if (! reader.readString (xmlString))
        return false;

    auto xml = juce::XmlDocument::parse (xmlString);

    if (xml == nullptr)
        return false;

    for (auto* child = xml->getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        auto desc = std::make_unique<juce::PluginDescription>();

        if (desc->loadFromXml (*child))
        {
            sanitizePluginDescription (*desc);
            result.add (std::move (desc));
        }
    }

    launcher.waitForExit (2000);

    if (! result.isEmpty())
        PluginBlacklist::getInstance().clearEntry (fileOrIdentifier);

    return ! result.isEmpty();
   #endif
}

//==============================================================================
#if JUCE_WINDOWS
/** 使用 Windows BCrypt API 计算文件 SHA-256 哈希。

    参考：Microsoft Learn - CNG Cryptographic Primitive Functions
    https://learn.microsoft.com/windows/win32/seccng/cng-cryptographic-primitive-functions
*/
static juce::String computeFileHashSha256WithBCrypt (const juce::File& file)
{
    juce::FileInputStream stream (file);

    if (! stream.openedOk())
        return {};

    BCRYPT_ALG_HANDLE algHandle = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider (&algHandle, BCRYPT_SHA256_ALGORITHM, nullptr, 0);

    if (! NT_SUCCESS (status) || algHandle == nullptr)
        return {};

    DWORD hashObjectSize = 0;
    DWORD hashSize       = 0;
    DWORD resultSize     = 0;

    BCryptGetProperty (algHandle, BCRYPT_OBJECT_LENGTH,
                       reinterpret_cast<PUCHAR> (&hashObjectSize),
                       sizeof (hashObjectSize), &resultSize, 0);
    BCryptGetProperty (algHandle, BCRYPT_HASH_LENGTH,
                       reinterpret_cast<PUCHAR> (&hashSize),
                       sizeof (hashSize), &resultSize, 0);

    if (hashObjectSize == 0 || hashSize == 0)
    {
        BCryptCloseAlgorithmProvider (algHandle, 0);
        return {};
    }

    std::vector<UCHAR> hashObject (hashObjectSize);
    std::vector<UCHAR> hash (hashSize);

    BCRYPT_HASH_HANDLE hashHandle = nullptr;
    status = BCryptCreateHash (algHandle, &hashHandle,
                               hashObject.data(), static_cast<ULONG> (hashObject.size()),
                               nullptr, 0, 0);

    if (! NT_SUCCESS (status) || hashHandle == nullptr)
    {
        BCryptCloseAlgorithmProvider (algHandle, 0);
        return {};
    }

    juce::MemoryBlock buffer (64 * 1024);

    while (! stream.isExhausted())
    {
        const auto bytesRead = stream.read (buffer.getData(), buffer.getSize());

        if (bytesRead <= 0)
            break;

        BCryptHashData (hashHandle, reinterpret_cast<PUCHAR> (buffer.getData()),
                        static_cast<ULONG> (bytesRead), 0);
    }

    status = BCryptFinishHash (hashHandle, hash.data(), static_cast<ULONG> (hash.size()), 0);

    BCryptDestroyHash (hashHandle);
    BCryptCloseAlgorithmProvider (algHandle, 0);

    if (! NT_SUCCESS (status))
        return {};

    // 转换为小写十六进制字符串
    juce::String hex;
    hex.preallocateBytes (hash.size() * 2 + 1);

    for (auto b : hash)
        hex += juce::String::toHexString (static_cast<int> (b)).paddedLeft ('0', 2);

    return hex.toLowerCase();
}
#endif

} // anonymous namespace

//==============================================================================
class PluginRegistry::ScanMetadataStore
{
public:
    struct Entry
    {
        juce::String filePath;
        juce::int64  lastModifiedTimeMs = 0;
        juce::int64  fileSize = 0;
        juce::String fileHash;
        bool         lastScanSuccess = false;
        juce::String lastError;
        juce::Time   lastScanTime;
        juce::StringArray pluginIdentifiers;
    };

    void loadFromXml (const juce::XmlElement& xml)
    {
        entries.clear();

        for (auto* child = xml.getFirstChildElement(); child != nullptr; child = child->getNextElement())
        {
            if (child->getTagName() != "Entry")
                continue;

            Entry entry;
            entry.filePath            = child->getStringAttribute ("filePath");
            entry.lastModifiedTimeMs  = child->getStringAttribute ("lastModifiedTimeMs").getLargeIntValue();
            entry.fileSize            = child->getStringAttribute ("fileSize").getLargeIntValue();
            entry.fileHash            = child->getStringAttribute ("fileHash");
            entry.lastScanSuccess     = child->getBoolAttribute ("lastScanSuccess", false);
            entry.lastError           = child->getStringAttribute ("lastError");
            entry.lastScanTime        = juce::Time (child->getStringAttribute ("lastScanTimeMs").getLargeIntValue());

            auto* idsXml = child->getChildByName ("Identifiers");

            if (idsXml != nullptr)
            {
                for (auto* idXml = idsXml->getFirstChildElement(); idXml != nullptr; idXml = idXml->getNextElement())
                {
                    if (idXml->getTagName() == "Id")
                        entry.pluginIdentifiers.add (idXml->getAllSubText().trim());
                }
            }

            if (entry.filePath.isNotEmpty())
                entries.add (std::move (entry));
        }
    }

    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto root = std::make_unique<juce::XmlElement> ("ScanMetadata");

        for (const auto& entry : entries)
        {
            auto* child = root->createNewChildElement ("Entry");
            child->setAttribute ("filePath", entry.filePath);
            child->setAttribute ("lastModifiedTimeMs", juce::String (entry.lastModifiedTimeMs));
            child->setAttribute ("fileSize", juce::String (entry.fileSize));
            child->setAttribute ("fileHash", entry.fileHash);
            child->setAttribute ("lastScanSuccess", entry.lastScanSuccess);
            child->setAttribute ("lastError", entry.lastError);
            child->setAttribute ("lastScanTimeMs", juce::String (entry.lastScanTime.toMilliseconds()));

            auto* idsXml = child->createNewChildElement ("Identifiers");

            for (const auto& id : entry.pluginIdentifiers)
            {
                auto* idXml = idsXml->createNewChildElement ("Id");
                idXml->addTextElement (id);
            }
        }

        return root;
    }

    Entry* findEntry (const juce::String& filePath)
    {
        for (auto& entry : entries)
            if (entry.filePath == filePath)
                return &entry;

        return nullptr;
    }

    const Entry* findEntry (const juce::String& filePath) const
    {
        return const_cast<ScanMetadataStore*> (this)->findEntry (filePath);
    }

    void updateEntry (const Entry& entry)
    {
        if (auto* existing = findEntry (entry.filePath))
        {
            *existing = entry;
        }
        else
        {
            entries.add (entry);
        }
    }

private:
    juce::Array<Entry> entries;
};

//==============================================================================
class PluginRegistry::ArchFilterScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit ArchFilterScanner (PluginRegistry& owner)
        : registry (owner)
    {
    }

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override
    {
        // 扫描已完全交给自主扫描器，不再依赖 JUCE 的格式实现。
        juce::ignoreUnused (format);
        // 维护当前正在扫描的文件计数。即使在同步消息线程扫描模式下，JUCE 的进度
        // 对话框仍可能在单个文件扫描期间泵送消息循环；该计数器可防止在此时把
        // 扫描误判为空闲并提前结束报告。
        struct ActiveScanGuard
        {
            explicit ActiveScanGuard (int& counter) : c (counter) { ++c; }
            ~ActiveScanGuard() { --c; }
            int& c;
        };

        // 记录当前正在扫描的文件路径，供 UI 状态标签显示。
        struct CurrentFileGuard
        {
            CurrentFileGuard (juce::String& target, const juce::String& value)
                : target (target) { target = value; }
            ~CurrentFileGuard() { target.clear(); }
            juce::String& target;
        };

        const ActiveScanGuard guard (registry.activeScanCount);
        const CurrentFileGuard fileGuard (registry.currentScanningFile, fileOrIdentifier);
        const auto file = juce::File (fileOrIdentifier);
        const auto now  = juce::Time::getCurrentTime();

        // OS/解压残留文件（__MACOSX / ._* / .DS_Store / .Trashes 等）直接跳过：
        // 返回 false 令其进入 JUCE 黑名单——PluginDirectoryScanner 只把
        // “未黑名单且零类型”的文件列为 failed，进黑名单后既不会重复扫描，
        // 也不会出现在 “failed to load” 列表里（枚举路径已提前过滤，这里是兜底）。
        if (vst3scan::VST3Scanner::isOsResidueFile (file))
        {
            juce::Logger::writeToLog ("Skipping OS residue file: " + fileOrIdentifier);
            registry.lastScanActivityTime = now;
            return false;
        }

        // PluginListComponent 触发的扫描没有显式的开始回调；当第一个文件进入扫描
        // 且当前没有进行中的报告时，自动开启一次新的扫描会话。
        if (! registry.scanInProgress)
            registry.beginScanReport();

        ++registry.lastReport.totalFiles;

        // bundle 目录（*.vst3 目录）→ 解析到实际 DLL 文件再扫描（普通文件原样返回）。
        // 无法解析（如只含 macOS 二进制的跨平台直达包）则记为失败，原因清晰可见。
        const auto libFile = vst3scan::VST3Scanner::resolveLibraryFile (file);

        if (! libFile.existsAsFile())
        {
            juce::Logger::writeToLog ("No loadable .vst3 library inside: " + fileOrIdentifier);
            updateResult (file, result, TRANS ("No loadable .vst3 library found in bundle"));
            registry.lastScanActivityTime = now;
            return true;
        }

        // 黑名单跳过：除非用户勾选“重新扫描上次出错的插件”，否则跳过黑名单中的插件。
        if (PluginBlacklist::getInstance().isBlacklisted (libFile.getFullPathName()))
        {
            if (! registry.rescanFailedPlugins)
            {
                juce::Logger::writeToLog ("Skipping blacklisted plugin file: " + fileOrIdentifier);
                registry.recordScanBlacklisted (libFile);
                registry.lastScanActivityTime = now;
                return true;
            }

            // 用户要求重试：清除该条黑名单记录，本次重新扫描。
            juce::Logger::writeToLog ("Retrying blacklisted plugin file: " + fileOrIdentifier);
            PluginBlacklist::getInstance().clearEntry (libFile.getFullPathName());
        }

        // 增量扫描：文件未变化且上次扫描成功时直接复用已知描述。
        if (registry.shouldSkipFile (libFile))
        {
            juce::Logger::writeToLog ("Skipping unchanged plugin file: " + fileOrIdentifier);

            for (const auto& desc : registry.knownList.getTypes())
            {
                if (desc.fileOrIdentifier == libFile.getFullPathName())
                    result.add (std::make_unique<juce::PluginDescription> (desc));
            }

            registry.recordScanSuccess (libFile, result, true);
            registry.lastScanActivityTime = now;
            return true;
        }

        // —— 自主 VST3 扫描器（替换 JUCE 自带 VST3PluginFormat 扫描）——
        // 关键差异：先 setHostContext 再枚举，按完整 128 位 CID 记录；
        // 避免外壳插件（WaveShell / IKM 等）类表索引位移导致的“点 A 出 B”。
        // 32-bit 插件在本进程 LoadLibraryW 必然失败，回退到 PluginHost 子进程桥接。
        juce::String customScanError;
        scanFileWithCustomScanner (libFile, result, customScanError);

        if (result.isEmpty() && detectPluginArchitecture (libFile) == PluginArchitecture::x86)
        {
            juce::Logger::writeToLog ("Custom scan returned empty; trying 32-bit bridge for: " + fileOrIdentifier);

            if (scanPluginViaHost (libFile.getFullPathName(), PluginArchitecture::x86, result))
            {
                for (auto* desc : result)
                {
                    if (desc != nullptr)
                        sanitizePluginDescription (*desc);
                }

                registry.lastScanActivityTime = now;
                updateResult (libFile, result, {});
                return true;
            }
        }

        for (auto* desc : result)
        {
            if (desc != nullptr)
                sanitizePluginDescription (*desc);
        }

        updateResult (libFile, result, customScanError);
        registry.lastScanActivityTime = now;
        return true;
    }

private:
    void updateResult (const juce::File& file,
                       const juce::OwnedArray<juce::PluginDescription>& result,
                       const juce::String& errorMessage)
    {
        const bool success = ! result.isEmpty();
        registry.updateScanMetadataForFile (file, success, result, errorMessage);

        if (success)
            registry.recordScanSuccess (file, result, false);
        else
            registry.recordScanFailure (file, errorMessage.isEmpty() ? TRANS ("No plugin descriptions found")
                                                                     : errorMessage);
    }

    PluginRegistry& registry;
};

//==============================================================================
// —— 异步扫描：后台线程 + PluginHost 子进程逐文件扫描 ——
//
// 线程安全模型：
//   - 后台线程（ScanWorkerThread）只做三件事：目录枚举、逐文件通过 PluginHost
//     子进程（或进程内兜底）扫描、把每个文件的结果压入 pendingResults 队列。
//     它不直接触碰 KnownPluginList / 扫描报告。
//   - 消息线程定期调用 pumpScanResults()，从队列取出结果，在消息线程上更新
//     KnownPluginList、增量元数据与扫描报告，从而避免与被扫描插件执行相关的
//     JUCE 消息线程断言，也避免整个软件在扫描期间卡死。
//   - 进度（total / completed / detail）由 worker 写、UI 定时读取，用 progressLock 保护。

//==============================================================================
struct PluginRegistry::ScanFileResult
{
    enum class Type
    {
        Success,       // 扫描成功，descriptions 有效
        Skipped,       // 增量未变化，消息线程从已知列表复用描述
        Failed,        // 扫描失败，errorMessage 有效
        Blacklisted    // 因黑名单跳过（未勾选重试）
    };

    Type type = Type::Failed;
    juce::String                             filePath;
    juce::String                             errorMessage;
    juce::OwnedArray<juce::PluginDescription> descriptions;
};

//==============================================================================
class PluginRegistry::ScanWorkerThread final : public juce::Thread
{
public:
    ScanWorkerThread (PluginRegistry& owner,
                      const juce::FileSearchPath& paths,
                      bool recursive,
                      bool forceRescan)
        : juce::Thread ("PluginScanThread"),
          registry (owner),
          extraPaths (paths),
          recursiveMode (recursive),
          forceRescanMode (forceRescan) {}

    std::atomic<bool> cancelled { false };

    void run() override
    {
        // —— 枚举所有待扫描 .vst3（文件系统遍历，可能较慢，放在后台线程）——
        // extraPaths 已包含“内置默认目录 + 用户自定义目录”（在消息线程解析）。 
        juce::StringArray files;

        for (int i = 0; i < extraPaths.getNumPaths(); ++i)
        {
            const juce::File dir (extraPaths[i]);

            if (! dir.isDirectory())
                continue;

            for (const auto& f : vst3scan::VST3Scanner::findVST3Files (dir, recursiveMode))
            {
                const auto lib = vst3scan::VST3Scanner::resolveLibraryFile (f);

                if (lib.existsAsFile())
                    files.addIfNotAlreadyThere (lib.getFullPathName());
            }
        }

        {
            juce::ScopedLock lock (registry.progressLock);
            registry.progressTotal = files.size();
            registry.progressCompleted = 0;
        }

        for (const auto& file : files)
        {
            if (threadShouldExit() || cancelled.load (std::memory_order_relaxed))
                break;

            scanSingleFile (file);
        }

        registry.scanThreadComplete = true;
        juce::Logger::writeToLog ("Plugin scan worker finished");
    }

private:
    void scanSingleFile (const juce::String& file)
    {
        registry.setScanProgressDetail (juce::File (file).getFileName());

        auto result = std::make_unique<PluginRegistry::ScanFileResult>();
        result->filePath = file;

        auto done = [&]
        {
            registry.pushScanResult (std::move (result));
        };

        const juce::File lib (file);

        // OS / 解压残留（__MACOSX / ._* 等）：直接忽略，不计入报告。
        if (vst3scan::VST3Scanner::isOsResidueFile (lib))
        {
            registry.onScanFileProgressed();
            return;
        }

        // bundle 目录无法解析出可加载 DLL。
        if (! lib.existsAsFile())
        {
            result->type = PluginRegistry::ScanFileResult::Type::Failed;
            result->errorMessage = TRANS ("No loadable .vst3 library found in bundle");
            done();
            return;
        }

        // 黑名单跳过（除非本次为强制全量重扫）。
        if (! forceRescanMode && PluginBlacklist::getInstance().isBlacklisted (lib.getFullPathName()))
        {
            result->type = PluginRegistry::ScanFileResult::Type::Blacklisted;
            done();
            return;
        }

        // 增量跳过：文件未变化且上次成功 → 消息线程从已知列表复用描述。
        if (! forceRescanMode)
        {
            std::lock_guard<std::mutex> metaLock (registry.metadataMutex);

            if (registry.shouldSkipFile (lib))
            {
                result->type = PluginRegistry::ScanFileResult::Type::Skipped;
                done();
                return;
            }
        }

        // 扫描：优先 PluginHost 子进程（进程隔离 + 按完整 CID 枚举，杜绝外壳错位）；
        // 子进程不可用（exe 缺失 / 崩溃 / 非 Windows）时回退进程内自主扫描器。
        const auto arch = detectPluginArchitecture (lib);
        bool ok = false;
        juce::String error;

       #if JUCE_WINDOWS
        if (arch != PluginArchitecture::unknown
            && PluginHostLauncher::getHostExecutableForArchitecture (arch).existsAsFile())
        {
            ok = scanPluginViaHost (file, arch, result->descriptions, &cancelled);

            if (ok && result->descriptions.isEmpty())
                ok = false;
        }
       #else
        juce::ignoreUnused (arch);
       #endif

        if (! ok)
        {
            // 进程内兜底：自定义扫描器（先 setHostContext 再按 CID 枚举）。
            scanFileWithCustomScanner (lib, result->descriptions, error);
            ok = ! result->descriptions.isEmpty();
        }

        if (ok)
        {
            result->type = PluginRegistry::ScanFileResult::Type::Success;
        }
        else
        {
            result->type = PluginRegistry::ScanFileResult::Type::Failed;
            result->errorMessage = error.isNotEmpty() ? error
                                                       : TRANS ("No plugin descriptions found");
        }

        done();
    }

    PluginRegistry&     registry;
    juce::FileSearchPath extraPaths;
    bool                 recursiveMode;
    bool                 forceRescanMode;
};

//==============================================================================
PluginRegistry& PluginRegistry::getInstance()
{
    static PluginRegistry instance;
    return instance;
}

//==============================================================================
PluginRegistry::PluginRegistry()
{
    metadataStore = std::make_unique<ScanMetadataStore>();
    loadScanMetadata();

    formatManager.addDefaultFormats();
    knownList.setCustomScanner (std::make_unique<ArchFilterScanner> (*this));
    loadList();
    loadCustomScanPaths();
}

//==============================================================================
// 定义在 .cpp 中，以便在 ScanWorkerThread 完整定义之后实例化其析构。
PluginRegistry::~PluginRegistry()
{
    if (scanWorker != nullptr)
    {
        scanWorker->signalThreadShouldExit();
        scanWorker->stopThread (2000);
    }
}

//==============================================================================
void PluginRegistry::setScanProgressDetail (const juce::String& detail)
{
    juce::ScopedLock lock (progressLock);
    progressDetail = detail;
    currentScanningFile = juce::File (detail).getFullPathName();
}

//==============================================================================
void PluginRegistry::pushScanResult (std::unique_ptr<ScanFileResult> result)
{
    {
        std::lock_guard<std::mutex> lock (resultQueueMutex);
        pendingResults.emplace_back (std::move (result));
    }

    {
        juce::ScopedLock plock (progressLock);
        ++progressCompleted;
    }
}

//==============================================================================
void PluginRegistry::onScanFileProgressed()
{
    juce::ScopedLock plock (progressLock);
    ++progressCompleted;
}

//==============================================================================
void PluginRegistry::startAsyncScan (bool recursive, bool forceRescan)
{
    if (scanWorker != nullptr && scanWorker->isThreadRunning())
    {
        juce::Logger::writeToLog ("Async scan already running; ignoring start request.");
        return;
    }

    // 消息线程：开启报告，预记录当前已知标识符以区分新增/更新。
    beginScanReport();

    scanThreadComplete = false;

    {
        juce::ScopedLock plock (progressLock);
        progressTotal = 0;
        progressCompleted = 0;
        progressDetail = TRANS ("Enumerating plugin directories");
        currentScanningFile.clear();
    }

    // 在消息线程解析本次要扫描的目录集合（默认目录 + 自定义目录），避免后台线程
    // 与消息线程在读取 customScanPaths 上产生数据竞争。
    const juce::FileSearchPath pathsToScan = getScanSearchPaths();

    scanWorker = std::make_unique<ScanWorkerThread> (*this, pathsToScan, recursive, forceRescan);
    scanWorker->startThread();
    juce::Logger::writeToLog ("Async plugin scan started");
}

//==============================================================================
void PluginRegistry::cancelAsyncScan()
{
    if (scanWorker == nullptr)
        return;

    scanWorker->cancelled.store (true, std::memory_order_relaxed);
    scanWorker->signalThreadShouldExit();
}

//==============================================================================
bool PluginRegistry::isScanThreadRunning() const noexcept
{
    return scanWorker != nullptr && scanWorker->isThreadRunning();
}

//==============================================================================
void PluginRegistry::pumpScanResults()
{
    std::deque<std::unique_ptr<ScanFileResult>> batch;

    {
        std::lock_guard<std::mutex> lock (resultQueueMutex);

        if (pendingResults.empty() && ! scanThreadComplete)
            return; // 仍在扫描且暂无新结果

        batch.swap (pendingResults);
    }

    for (auto& r : batch)
        applyScannedFile (*r);

    // 全部结果已应用且后台线程已结束 → 结束扫描报告。
    if (scanThreadComplete && scanInProgress)
    {
        finishScanReport();

        {
            juce::ScopedLock plock (progressLock);
            progressDetail = TRANS ("Ready");
        }
    }
}

//==============================================================================
void PluginRegistry::applyScannedFile (const ScanFileResult& result)
{
    ++lastReport.totalFiles;

    const juce::File file (result.filePath);

    switch (result.type)
    {
        case ScanFileResult::Type::Skipped:
        {
            // 复用已知列表中的描述，记录为“未变化而跳过”。
            juce::OwnedArray<juce::PluginDescription> existing;

            for (auto& d : knownList.getTypes())
                if (d.fileOrIdentifier == result.filePath)
                    existing.add (std::make_unique<juce::PluginDescription> (d));

            {
                std::lock_guard<std::mutex> metaLock (metadataMutex);
                updateScanMetadataForFile (file, true, existing, {});
            }

            recordScanSuccess (file, existing, true);
            break;
        }

        case ScanFileResult::Type::Blacklisted:
            recordScanBlacklisted (file);
            break;

        case ScanFileResult::Type::Failed:
            recordScanFailure (file, result.errorMessage);
            {
                juce::OwnedArray<juce::PluginDescription> none;
                std::lock_guard<std::mutex> metaLock (metadataMutex);
                updateScanMetadataForFile (file, false, none, result.errorMessage);
            }
            break;

        case ScanFileResult::Type::Success:
        default:
        {
            {
                std::lock_guard<std::mutex> metaLock (metadataMutex);
                updateScanMetadataForFile (file, true, result.descriptions, {});
            }

            recordScanSuccess (file, result.descriptions, false);
            commitDescriptionsForFile (result.filePath, result.descriptions);
            break;
        }
    }
}

//==============================================================================
void PluginRegistry::commitDescriptionsForFile (const juce::String& file,
                                                const juce::OwnedArray<juce::PluginDescription>& descriptions)
{
    // 先移除该文件原有的已知条目（避免重复），再写入最新描述。
    for (auto& d : knownList.getTypes())
    {
        if (d.fileOrIdentifier == file)
            knownList.removeType (d);
    }

    for (auto* d : descriptions)
        if (d != nullptr)
            knownList.addType (*d);

    knownList.sendChangeMessage();
}

//==============================================================================
int PluginRegistry::getScanProgressTotal() const noexcept
{
    juce::ScopedLock plock (progressLock);
    return progressTotal;
}

//==============================================================================
int PluginRegistry::getScanProgressCompleted() const noexcept
{
    juce::ScopedLock plock (progressLock);
    return progressCompleted;
}

//==============================================================================
juce::String PluginRegistry::getScanProgressDetail() const
{
    juce::ScopedLock plock (progressLock);
    return progressDetail;
}

//==============================================================================
void PluginRegistry::setRescanFailedPlugins (bool shouldRescan)
{
    rescanFailedPlugins = shouldRescan;

    if (! rescanFailedPlugins)
        return;

    // JUCE KnownPluginList 自己维护一份黑名单；scanAndAddFile 在调用 CustomScanner
    // 之前会先检查它。如果不清理，即使勾选了 rescan，插件仍会被 JUCE 直接跳过。
    if (knownList.getBlacklistedFiles().size() > 0)
    {
        juce::Logger::writeToLog ("Clearing JUCE KnownPluginList blacklist for rescan");
        knownList.clearBlacklistedFiles();
    }

    // 同时清理本应用自己的 PluginBlacklist.json。
    if (PluginBlacklist::getInstance().getNumEntries() > 0)
    {
        juce::Logger::writeToLog ("Clearing PluginBlacklist.json for rescan");
        PluginBlacklist::getInstance().clearAll();
    }

    // 若上次崩溃遗留了未应用的 dead man's pedal，也删除，避免它重新污染黑名单。
    auto deadMansPedal = AppSettings::getInstance().getDeadMansPedalFile();
    if (deadMansPedal.existsAsFile())
    {
        juce::Logger::writeToLog ("Removing stale dead man's pedal file: " + deadMansPedal.getFullPathName());
        deadMansPedal.deleteFile();
    }

    // 立即保存，确保下次启动 / 扫描前黑名单已被清空。
    saveList();
}

//==============================================================================
void PluginRegistry::loadList()
{
    auto file = AppSettings::getInstance().getPluginListFile();

    if (! file.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse (file);

    if (xml != nullptr)
        knownList.recreateFromXml (*xml);
}

//==============================================================================
void PluginRegistry::saveList() const
{
    auto xml = knownList.createXml();

    if (xml != nullptr)
    {
        auto file = AppSettings::getInstance().getPluginListFile();
        xml->writeTo (file);
    }
}

//==============================================================================
juce::File PluginRegistry::getScanMetadataFile() const
{
    return AppSettings::getInstance().getAppDataDirectory().getChildFile ("PluginScanMetadata.xml");
}

//==============================================================================
void PluginRegistry::loadScanMetadata()
{
    if (metadataStore == nullptr)
        return;

    auto file = getScanMetadataFile();

    if (! file.existsAsFile())
        return;

    auto xml = juce::XmlDocument::parse (file);

    if (xml != nullptr)
        metadataStore->loadFromXml (*xml);
}

//==============================================================================
void PluginRegistry::saveScanMetadata() const
{
    if (metadataStore == nullptr)
        return;

    auto xml = metadataStore->toXml();

    if (xml != nullptr)
        getScanMetadataFile().replaceWithText (xml->toString());
}

//==============================================================================
juce::String PluginRegistry::computeFileHashSha256 (const juce::File& file) const
{
   #if JUCE_WINDOWS
    return computeFileHashSha256WithBCrypt (file);
   #else
    juce::ignoreUnused (file);
    return {};
   #endif
}

//==============================================================================
bool PluginRegistry::shouldSkipFile (const juce::File& file, juce::String* reasonIfFailed) const
{
    if (metadataStore == nullptr)
        return false;

    if (! file.existsAsFile())
    {
        if (reasonIfFailed != nullptr)
            *reasonIfFailed = TRANS ("File does not exist");

        return false;
    }

    const auto* entry = metadataStore->findEntry (file.getFullPathName());

    if (entry == nullptr)
    {
        if (reasonIfFailed != nullptr)
            *reasonIfFailed = TRANS ("No previous scan metadata");

        return false;
    }

    if (! entry->lastScanSuccess)
    {
        if (reasonIfFailed != nullptr)
            *reasonIfFailed = TRANS ("Last scan failed") + (entry->lastError.isEmpty() ? juce::String()
                                                                                       : ": " + entry->lastError);

        return false;
    }

    const auto modTime = file.getLastModificationTime();
    const auto size    = file.getSize();

    if (modTime.toMilliseconds() != entry->lastModifiedTimeMs || size != entry->fileSize)
    {
        if (reasonIfFailed != nullptr)
            *reasonIfFailed = TRANS ("File modification time or size changed");

        return false;
    }

    if (! entry->fileHash.isEmpty())
    {
        const auto currentHash = computeFileHashSha256 (file);

        if (currentHash.isEmpty())
        {
            if (reasonIfFailed != nullptr)
                *reasonIfFailed = TRANS ("Unable to compute file hash");

            return false;
        }

        if (currentHash != entry->fileHash)
        {
            if (reasonIfFailed != nullptr)
                *reasonIfFailed = TRANS ("File hash changed");

            return false;
        }
    }

    return true;
}

//==============================================================================
void PluginRegistry::updateScanMetadataForFile (const juce::File& file,
                                                bool success,
                                                const juce::OwnedArray<juce::PluginDescription>& descriptions,
                                                const juce::String& errorMessage)
{
    if (metadataStore == nullptr)
        return;

    ScanMetadataStore::Entry entry;
    entry.filePath           = file.getFullPathName();
    entry.lastModifiedTimeMs = file.getLastModificationTime().toMilliseconds();
    entry.fileSize           = file.getSize();
    entry.fileHash           = computeFileHashSha256 (file);
    entry.lastScanSuccess    = success;
    entry.lastError          = errorMessage;
    entry.lastScanTime       = juce::Time::getCurrentTime();

    for (const auto* desc : descriptions)
    {
        if (desc != nullptr)
            entry.pluginIdentifiers.add (desc->createIdentifierString());
    }

    metadataStore->updateEntry (entry);
}

//==============================================================================
void PluginRegistry::beginScanReport()
{
    // 防止嵌套扫描导致当前报告被静默覆盖。若调用方确实需要开启新会话，
    // 应先结束旧会话或等待旧报告展示完毕。
    if (scanInProgress)
    {
        juce::Logger::writeToLog ("Warning: beginning a new scan report while another is in progress; finishing the previous one.");
        finishScanReport();
    }

    lastReport = PluginScanReport();
    lastReport.scanStartTime = juce::Time::getCurrentTime();
    lastReport.hasUnshownReport = true;
    scanInProgress = true;
    lastScanActivityTime = juce::Time::getCurrentTime();

    preScanIdentifiers.clear();

    for (const auto& desc : knownList.getTypes())
        preScanIdentifiers.add (desc.createIdentifierString());
}

//==============================================================================
void PluginRegistry::finishScanReport()
{
    if (! scanInProgress)
        return;

    lastReport.scanEndTime = juce::Time::getCurrentTime();
    scanInProgress = false;

    saveScanMetadata();
    saveList();
}

//==============================================================================
void PluginRegistry::checkAndFinishIdleScan()
{
    if (! scanInProgress)
        return;

    // 只要有文件仍在扫描中，就不应结束报告；这能避免把慢速文件误判为扫描结束。
    if (activeScanCount > 0)
        return;

    if ((juce::Time::getCurrentTime() - lastScanActivityTime) > juce::RelativeTime::seconds (pluginScanIdleTimeoutSeconds))
        finishScanReport();
}

//==============================================================================
juce::String PluginRegistry::getCurrentScanningFile() const noexcept
{
    return currentScanningFile;
}

//==============================================================================
void PluginRegistry::recordScanSuccess (const juce::File& file,
                                        const juce::OwnedArray<juce::PluginDescription>& descriptions,
                                        bool wasSkipped)
{
    if (wasSkipped)
        ++lastReport.skippedCount;
    else
        ++lastReport.successCount;

    for (const auto* desc : descriptions)
    {
        if (desc == nullptr)
            continue;

        const auto id = desc->createIdentifierString();

        if (preScanIdentifiers.contains (id))
            ++lastReport.updatedCount;
        else
            ++lastReport.newCount;
    }

    juce::ignoreUnused (file);
}

//==============================================================================
void PluginRegistry::recordScanFailure (const juce::File& file, const juce::String& reason)
{
    ++lastReport.failedCount;

    PluginScanReport::FailedEntry entry;
    entry.filePath = file.getFullPathName();
    entry.reason   = reason.isEmpty() ? TRANS ("Unknown error") : reason;
    lastReport.failedEntries.add (std::move (entry));

    PluginBlacklist::getInstance().recordScanFailure (file.getFullPathName(), reason);
}

//==============================================================================
void PluginRegistry::recordScanBlacklisted (const juce::File& file)
{
    ++lastReport.blacklistedCount;
    lastReport.blacklistedFilePaths.add (file.getFullPathName());
}

//==============================================================================
PluginScanReport PluginRegistry::getLastScanReport() const
{
    return lastReport;
}

//==============================================================================
void PluginRegistry::markLastScanReportAsShown()
{
    lastReport.hasUnshownReport = false;
}

//==============================================================================
juce::FileSearchPath PluginRegistry::getVST3DefaultSearchPath() const
{
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        auto* format = formatManager.getFormat (i);

        if (format != nullptr && format->getName() == juce::VST3PluginFormat::getFormatName())
            return format->getDefaultLocationsToSearch();
    }

    return {};
}

//==============================================================================
juce::FileSearchPath PluginRegistry::getScanSearchPaths() const
{
    juce::FileSearchPath paths (getVST3DefaultSearchPath());

    for (int i = 0; i < customScanPaths.getNumPaths(); ++i)
    {
        const juce::File dir = customScanPaths[i];
        const juce::String p = dir.getFullPathName();

        bool already = false;

        for (int j = 0; j < paths.getNumPaths(); ++j)
            if (paths[j].getFullPathName() == p) { already = true; break; }

        if (! already)
            paths.add (dir);
    }

    return paths;
}

//==============================================================================
juce::FileSearchPath PluginRegistry::getCustomScanPaths() const
{
    return customScanPaths;
}

//==============================================================================
void PluginRegistry::addScanPath (const juce::File& dir)
{
    if (! dir.isDirectory())
        return;

    const juce::String path = dir.getFullPathName();

    for (int i = 0; i < customScanPaths.getNumPaths(); ++i)
        if (customScanPaths[i].getFullPathName() == path)
            return; // 已存在，避免重复

    customScanPaths.add (dir);
    saveCustomScanPaths();
}

//==============================================================================
void PluginRegistry::removeScanPath (const juce::File& dir)
{
    const juce::String path = dir.getFullPathName();

    for (int i = 0; i < customScanPaths.getNumPaths(); ++i)
    {
        if (customScanPaths[i].getFullPathName() == path)
        {
            customScanPaths.remove (i);
            break;
        }
    }

    saveCustomScanPaths();
}

//==============================================================================
void PluginRegistry::setCustomScanPaths (const juce::FileSearchPath& paths)
{
    customScanPaths = juce::FileSearchPath();

    for (int i = 0; i < paths.getNumPaths(); ++i)
    {
        const juce::File dir = paths[i];

        if (dir.isDirectory())
            customScanPaths.add (dir);
    }

    saveCustomScanPaths();
}

//==============================================================================
void PluginRegistry::loadCustomScanPaths()
{
    customScanPaths = juce::FileSearchPath();

    if (auto* props = AppSettings::getInstance().getPropertiesFile())
        customScanPaths = juce::FileSearchPath (props->getValue ("customVst3ScanPaths"));
}

//==============================================================================
void PluginRegistry::saveCustomScanPaths() const
{
    if (auto* props = AppSettings::getInstance().getPropertiesFile())
        props->setValue ("customVst3ScanPaths", customScanPaths.toString());
}

//==============================================================================
std::unique_ptr<juce::PluginDescription> PluginRegistry::findDescriptionForIdentifier (const juce::String& identifier) const
{
    return knownList.getTypeForIdentifierString (identifier);
}

} // namespace minixer
