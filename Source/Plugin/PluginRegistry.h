/*
  ==============================================================================

    PluginRegistry.h
    管理已扫描插件列表与格式管理器。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginSlotState.h"

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
    /** 扫描 VST3 默认位置与指定路径。

        注意：该函数必须在 JUCE 消息线程调用，因为 VST3 插件的实例化与总线
        信息查询要求消息线程（参见 juce_VST3PluginFormat.cpp 中的
        JUCE_ASSERT_MESSAGE_THREAD）。它在当前线程同步执行，会阻塞调用者直到
        扫描完成。如需 UI 反馈与取消功能，请使用 PluginManagerComponent。 */
    void scanForVST3 (const juce::FileSearchPath& extraPaths, bool recursive = true);

    /** 扫描 VST3 默认位置。 */
    void scanForVST3 (bool recursive = true);

    /** 强制重新扫描 VST3 默认位置与指定路径（忽略增量扫描缓存）。 */
    void rescanForVST3 (const juce::FileSearchPath& extraPaths, bool recursive = true);

    /** 强制重新扫描 VST3 默认位置。 */
    void rescanForVST3 (bool recursive = true);

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
    ~PluginRegistry() = default;

    //==============================================================================
    void scanForVST3Internal (const juce::FileSearchPath& extraPaths, bool recursive, bool forceRescan);

    //==============================================================================
    /** 自定义扫描器：在真正加载 VST3 之前读取 PE 头，跳过与当前进程架构不匹配的插件，
        避免 JUCE Debug 模式下在 juce_VST3PluginFormat.cpp:1208 触发 jassert 导致崩溃。
        同时支持增量扫描：文件未变化且上次扫描成功时直接复用已知描述。 */
    class ArchFilterScanner;

    //==============================================================================
    /** 增量扫描元数据。 */
    class ScanMetadataStore;

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
