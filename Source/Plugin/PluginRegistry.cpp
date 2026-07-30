/*
  ==============================================================================

    PluginRegistry.cpp

  ==============================================================================
*/

#include "PluginRegistry.h"
#include "PluginArchitecture.h"
#include "PluginHostLauncher.h"
#include "PluginBlacklist.h"
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
#if JUCE_WINDOWS
/** 在 Windows 上捕获插件扫描过程中可能触发的结构化异常（SEH）。 */
static bool safeFindAllTypesForFile (juce::AudioPluginFormat* format,
                                     juce::OwnedArray<juce::PluginDescription>* result,
                                     const juce::String* fileOrIdentifier)
{
    __try
    {
        format->findAllTypesForFile (*result, *fileOrIdentifier);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
#endif

//==============================================================================
/** 通过 PluginHost 子进程扫描 32-bit 插件，返回描述列表。

    主进程创建命名管道客户端并启动 PluginHost32.exe --mode=scan，
    子进程加载插件后返回 ScanResult 消息。
*/
static bool scanPluginViaHost (const juce::String& fileOrIdentifier,
                               PluginArchitecture arch,
                               juce::OwnedArray<juce::PluginDescription>& result)
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

        // PluginListComponent 触发的扫描没有显式的开始回调；当第一个文件进入扫描
        // 且当前没有进行中的报告时，自动开启一次新的扫描会话。
        if (! registry.scanInProgress)
            registry.beginScanReport();

        ++registry.lastReport.totalFiles;

        // 黑名单跳过：除非用户勾选“重新扫描上次出错的插件”，否则跳过黑名单中的插件。
        if (PluginBlacklist::getInstance().isBlacklisted (file.getFullPathName()))
        {
            if (! registry.rescanFailedPlugins)
            {
                juce::Logger::writeToLog ("Skipping blacklisted plugin file: " + fileOrIdentifier);
                registry.recordScanBlacklisted (file);
                registry.lastScanActivityTime = now;
                return true;
            }

            // 用户要求重试：清除该条黑名单记录，本次重新扫描。
            juce::Logger::writeToLog ("Retrying blacklisted plugin file: " + fileOrIdentifier);
            PluginBlacklist::getInstance().clearEntry (file.getFullPathName());
        }

        // 增量扫描：文件未变化且上次扫描成功时直接复用已知描述。
        if (registry.shouldSkipFile (file))
        {
            juce::Logger::writeToLog ("Skipping unchanged plugin file: " + fileOrIdentifier);

            for (const auto& desc : registry.knownList.getTypes())
            {
                if (desc.fileOrIdentifier == fileOrIdentifier)
                    result.add (std::make_unique<juce::PluginDescription> (desc));
            }

            registry.recordScanSuccess (file, result, true);
            registry.lastScanActivityTime = now;
            return true;
        }

        // 先直接让 JUCE 扫描，不预先根据架构过滤。这样即使架构识别出错，
        // 64-bit 插件也能在本进程正确识别；同时 SEH/try-catch 能保护宿主。
        scanWithJuce (format, result, fileOrIdentifier);

        // 如果直接扫描没拿到描述，并且文件是 32-bit，再尝试用 PluginHost32 桥接。
        if (result.isEmpty())
        {
            const auto arch = detectPluginArchitecture (file);

            if (arch == PluginArchitecture::x86)
            {
                juce::Logger::writeToLog ("Direct scan returned empty; trying 32-bit bridge for: " + fileOrIdentifier);

                if (scanPluginViaHost (fileOrIdentifier, arch, result))
                {
                    registry.lastScanActivityTime = now;
                    updateResult (file, result, {});
                    return true;
                }

                result.clear();
            }
        }

        updateResult (file, result, {});
        registry.lastScanActivityTime = now;
        return true;
    }

private:
    void scanWithJuce (juce::AudioPluginFormat& format,
                       juce::OwnedArray<juce::PluginDescription>& result,
                       const juce::String& fileOrIdentifier)
    {
        try
        {
           #if JUCE_WINDOWS
            if (! safeFindAllTypesForFile (&format, &result, &fileOrIdentifier))
            {
                result.clear();
                juce::Logger::writeToLog ("Plugin scan raised a structured exception for: " + fileOrIdentifier);
            }
           #else
            format.findAllTypesForFile (result, fileOrIdentifier);
           #endif

            for (auto* desc : result)
            {
                if (desc != nullptr)
                    sanitizePluginDescription (*desc);
            }
        }
        catch (...)
        {
            result.clear();
            juce::Logger::writeToLog ("Plugin scan threw a C++ exception for: " + fileOrIdentifier);
        }
    }

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
void PluginRegistry::scanForVST3 (bool recursive)
{
    scanForVST3 (getVST3DefaultSearchPath(), recursive);
}

//==============================================================================
void PluginRegistry::scanForVST3 (const juce::FileSearchPath& extraPaths, bool recursive)
{
    scanForVST3Internal (extraPaths, recursive, false);
}

//==============================================================================
void PluginRegistry::rescanForVST3 (bool recursive)
{
    rescanForVST3 (getVST3DefaultSearchPath(), recursive);
}

//==============================================================================
void PluginRegistry::rescanForVST3 (const juce::FileSearchPath& extraPaths, bool recursive)
{
    scanForVST3Internal (extraPaths, recursive, true);
}

//==============================================================================
void PluginRegistry::scanForVST3Internal (const juce::FileSearchPath& extraPaths,
                                          bool recursive,
                                          bool forceRescan)
{
    juce::AudioPluginFormat* vst3Format = nullptr;

    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        auto* format = formatManager.getFormat (i);

        if (format != nullptr && format->getName() == juce::VST3PluginFormat::getFormatName())
        {
            vst3Format = format;
            break;
        }
    }

    if (vst3Format == nullptr)
        return;

    beginScanReport();

    // 合并默认路径与用户指定的额外路径，保持原有顺序追加。
    juce::FileSearchPath pathsToScan (getVST3DefaultSearchPath());

    for (int i = 0; i < extraPaths.getNumPaths(); ++i)
        pathsToScan.add (extraPaths[i]);

    auto files = vst3Format->searchPathsForPlugins (pathsToScan, recursive, false);

    for (auto& file : files)
    {
        if (forceRescan)
        {
            // 强制重新扫描：清除该文件的元数据成功标志，使其不会被跳过。
            if (metadataStore != nullptr)
            {
                ScanMetadataStore::Entry cleared;
                cleared.filePath = file;
                cleared.lastScanSuccess = false;
                metadataStore->updateEntry (cleared);
            }
        }

        juce::OwnedArray<juce::PluginDescription> typesFound;
        knownList.scanAndAddFile (file, true, typesFound, *vst3Format);
    }

    knownList.scanFinished();
    finishScanReport();

    // 本次扫描的重试标志用完即重置，避免影响后续扫描。
    rescanFailedPlugins = false;
}

//==============================================================================
std::unique_ptr<juce::PluginDescription> PluginRegistry::findDescriptionForIdentifier (const juce::String& identifier) const
{
    return knownList.getTypeForIdentifierString (identifier);
}

} // namespace minixer
