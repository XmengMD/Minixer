/*
  ==============================================================================

    PluginRegistry.h
    管理已扫描插件列表与格式管理器。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginSlotState.h"

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>

namespace minixer
{

//==============================================================================
/** 单次扫描报告。 */
struct PluginScanReport
{
    int totalFiles      = 0; /**< 扫描文件总数。 */
    int successCount    = 0; /**< 成功扫描数量。 */
    int newCount        = 0; /**< 新增插件数量。 */
    int updatedCount    = 0; /**< 更新插件数量。 */
    int skippedCount      = 0; /**< 未变化而跳过的数量。 */
    int failedCount       = 0; /**< 失败文件数量。 */
    int blacklistedCount  = 0; /**< 本次扫描因黑名单而跳过的数量。 */

    juce::Time scanStartTime;
    juce::Time scanEndTime;

    struct FailedEntry
    {
        juce::String filePath;
        juce::String reason;
    };
    juce::Array<FailedEntry> failedEntries;

    juce::StringArray blacklistedFilePaths;

    /** 是否有尚未展示给用户的报告（由 UI 展示后清空）。 */
    bool hasUnshownReport = false;
};

//==============================================================================
/** 插件目录单例。

    持有 AudioPluginFormatManager 与 KnownPluginList，负责：
    - 注册 VST3 等格式
    - 扫描系统插件目录
    - 按标识符查找 PluginDescription
    - 持久化/加载已扫描列表
    - 增量扫描元数据与扫描报告
*/
class PluginRegistry
{
public:
    //==============================================================================
    static PluginRegistry& getInstance();

    //==============================================================================
    juce::AudioPluginFormatManager& getFormatManager() noexcept      { return formatManager; }
    juce::KnownPluginList&          getKnownPluginList() noexcept    { return knownList; }

    //==============================================================================
    /** 从磁盘加载已扫描列表。 */
    void loadList();

    /** 将已扫描列表保存到磁盘。 */
    void saveList() const;

    //==============================================================================
    /** 异步扫描 VST3（插件管理器 UI 专用入口）。

        在专用后台线程上进行目录枚举，并对每个 .vst3 文件启动 PluginHost 子进程
        （优先）或进程内扫描兜底，结果进入内部队列；调用方必须定期（如 100ms
        Timer）调用 pumpScanResults() 把结果应用到 KnownPluginList / 增量元数据 /
        扫描报告，并返回 UI 反馈。该入口不阻塞消息线程，避免扫描期间整个软件卡死。

        扫描目录取 getScanSearchPaths()（内置默认目录 + 用户自定义目录）。
    */
    void startAsyncScan (bool recursive, bool forceRescan = false);

    /** 取消正在进行的异步扫描。在结果队列被清空且线程退出后立即结束扫描报告。 */
    void cancelAsyncScan();

    /** 返回后台扫描线程是否仍在运行。 */
    bool isScanThreadRunning() const noexcept;

    /** 消息线程应定期调用（100ms 建议）：把后台扫描结果队列应用到
        KnownPluginList / 元数据 / 报告，并在全部完成后结束扫描报告。 */
    void pumpScanResults();

    //==============================================================================
    /** 扫描进度（线程安全，供 UI 定时器读取）。 */
    int         getScanProgressTotal() const noexcept;
    int         getScanProgressCompleted() const noexcept;
    juce::String getScanProgressDetail() const;   /**< 当前正在扫描的文件名或阶段文本。 */

    //==============================================================================
    /** 设置下次扫描时是否重新扫描黑名单中“上次出错的插件”。
        勾选时会同时清除 JUCE KnownPluginList 黑名单、本应用黑名单以及
        未应用的 dead man's pedal 文件，确保失败插件真正被重新扫描。 */
    void setRescanFailedPlugins (bool shouldRescan);

    bool getRescanFailedPlugins() const noexcept { return rescanFailedPlugins; }

    //==============================================================================
    /** 根据 createIdentifierString() 查找插件描述。 */
    std::unique_ptr<juce::PluginDescription> findDescriptionForIdentifier (const juce::String& identifier) const;

    //==============================================================================
    /** 返回 VST3 格式的默认扫描路径。 */
    juce::FileSearchPath getVST3DefaultSearchPath() const;

    //==============================================================================
    /** 返回全部扫描目录 = 内置默认目录 + 用户自定义目录（去重）。 */
    juce::FileSearchPath getScanSearchPaths() const;

    /** 返回用户自定义的扫描目录（不含内置默认目录）。 */
    juce::FileSearchPath getCustomScanPaths() const;

    /** 新增一个用户自定义扫描目录。 */
    void addScanPath (const juce::File& dir);

    /** 移除一个用户自定义扫描目录。 */
    void removeScanPath (const juce::File& dir);

    /** 用一组新目录替换全部用户自定义扫描目录。 */
    void setCustomScanPaths (const juce::FileSearchPath& paths);

    //==============================================================================
    /** 返回最近一次扫描报告。 */
    PluginScanReport getLastScanReport() const;

    /** 将最近一次扫描报告标记为已展示。 */
    void markLastScanReportAsShown();

    /** 检查扫描是否已空闲超过阈值，若是则结束本次扫描报告。 */
    void checkAndFinishIdleScan();

    /** 返回是否正在扫描中。 */
    bool isScanInProgress() const noexcept { return scanInProgress; }

    /** 返回当前正在扫描的文件路径（仅在单个文件扫描期间有效）。 */
    juce::String getCurrentScanningFile() const noexcept;

private:
    //==============================================================================
    PluginRegistry();
    ~PluginRegistry();          // 定义在 .cpp（scanWorker 为不完整类型，需在完整定义后实例化）

    //==============================================================================
    /** 自定义扫描器：在真正加载 VST3 之前读取 PE 头，跳过与当前进程架构不匹配的插件，
        避免 JUCE Debug 模式下在 juce_VST3PluginFormat.cpp:1208 触发 jassert 导致崩溃。
        同时支持增量扫描：文件未变化且上次扫描成功时直接复用已知描述。 */
    class ArchFilterScanner;

    //==============================================================================
    /** 增量扫描元数据。 */
    class ScanMetadataStore;

    //==============================================================================
    /** 一个文件的异步扫描结果（后台线程产生 → 消息线程应用）。 */
    struct ScanFileResult;
    /** 后台扫描线程（PIMPL，定义在 .cpp）。 */
    class  ScanWorkerThread;

    //==============================================================================
    // —— 异步扫描：进度（worker 写 / UI 定时读，由 progressLock 保护）——
    mutable juce::CriticalSection progressLock;
    int                            progressTotal = 0;
    int                            progressCompleted = 0;
    juce::String                   progressDetail = "Ready";

    // —— 异步扫描：结果队列（worker 产生、消息线程 pump 消费）——
    std::mutex                                     resultQueueMutex;
    std::deque<std::unique_ptr<ScanFileResult>>    pendingResults;
    std::unique_ptr<ScanWorkerThread>              scanWorker;
    std::atomic<bool>                              scanThreadComplete { false };

    // —— 增量元数据在后台线程（读取 skip 判定）与消息线程（写入结果）间共享 ——
    std::mutex metadataMutex;

    // —— 用户自定义扫描目录（持久化于 PropertiesFile，仅消息线程访问）——
    juce::FileSearchPath customScanPaths;
    void loadCustomScanPaths();
    void saveCustomScanPaths() const;

    //==============================================================================
    void pushScanResult (std::unique_ptr<ScanFileResult> result);
    void onScanFileProgressed();                     /**< 不产生结果时仍推进进度（残留文件）。 */
    void applyScannedFile (const ScanFileResult& result);
    void commitDescriptionsForFile (const juce::String& file,
                                    const juce::OwnedArray<juce::PluginDescription>& descriptions);
    void setScanProgressDetail (const juce::String& detail);

    //==============================================================================
    juce::File getScanMetadataFile() const;
    void loadScanMetadata();
    void saveScanMetadata() const;

    juce::String computeFileHashSha256 (const juce::File& file) const;
    bool shouldSkipFile (const juce::File& file, juce::String* reasonIfFailed = nullptr) const;
    void updateScanMetadataForFile (const juce::File& file,
                                    bool success,
                                    const juce::OwnedArray<juce::PluginDescription>& descriptions,
                                    const juce::String& errorMessage);

    void beginScanReport();
    void finishScanReport();
    void recordScanSuccess (const juce::File& file,
                            const juce::OwnedArray<juce::PluginDescription>& descriptions,
                            bool wasSkipped);
    void recordScanFailure (const juce::File& file, const juce::String& reason);
    void recordScanBlacklisted (const juce::File& file);

    //==============================================================================
    juce::AudioPluginFormatManager formatManager;
    juce::KnownPluginList          knownList;

    std::unique_ptr<ScanMetadataStore> metadataStore;
    PluginScanReport                   lastReport;
    bool                               scanInProgress = false;
    bool                               rescanFailedPlugins = false;

    /** 本次扫描前已知的插件标识符集合，用于区分新增与更新。 */
    juce::StringArray                  preScanIdentifiers;

    /** 最近一次扫描活动的时间，用于在消息线程扫描中自动识别扫描开始/结束。 */
    juce::Time                         lastScanActivityTime;

    /** 当前正在扫描的文件路径（仅在消息线程访问）。 */
    juce::String                       currentScanningFile;

    /** 当前正在执行的 findPluginTypesFor 调用数；>0 表示有扫描尚未返回。 */
    int                                activeScanCount = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginRegistry)
};

} // namespace minixer
