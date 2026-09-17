/*
  ==============================================================================

    PluginBlacklist.h
    管理扫描/运行期崩溃或失败的插件黑名单。

    黑名单不是由用户手动增删，而是在扫描页面通过“重新扫描上次出错的插件”
    复选框控制：未勾选时跳过黑名单中的插件；勾选时清除其失败状态并重新扫描。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <mutex>

namespace minixer
{

//==============================================================================
/** 单个黑名单条目。 */
struct BlacklistEntry
{
    juce::String filePath;      /**< 插件文件绝对路径。 */
    juce::String reason;        /**< 失败原因：crash / scanFailure / timeout。 */
    int          crashCount = 0;/**< 崩溃/失败累计次数。 */
    juce::Time   lastCrashTime; /**< 最近一次失败时间。 */
    int          lastExitCode = 0; /**< 子进程上次退出码（运行时崩溃有效）。 */
    bool         permanentlySkipped = false; /**< 连续多次失败后永久跳过。 */

    /** 返回是否仍在“冷静期”内（7 天内不自动重试）。 */
    bool isInCooldownPeriod() const noexcept;

    /** 返回当前是否应被跳过。 */
    bool shouldSkip() const noexcept;

    /** 返回是否允许本次重新扫描。 */
    bool canRetryNow() const noexcept;
};

//==============================================================================
/** 插件黑名单管理器。

    持久化文件：%AppData%/Minixer/PluginBlacklist.json
*/
class PluginBlacklist
{
public:
    //==============================================================================
    static PluginBlacklist& getInstance();

    //==============================================================================
    /** 从磁盘加载黑名单。 */
    void load();

    /** 保存黑名单到磁盘。 */
    void save() const;

    //==============================================================================
    /** 记录一次运行时崩溃。 */
    void recordCrash (const juce::String& filePath, int exitCode);

    /** 记录一次扫描失败。 */
    void recordScanFailure (const juce::String& filePath, const juce::String& reason);

    /** 清除指定插件的黑名单记录。 */
    void clearEntry (const juce::String& filePath);

    /** 清空所有黑名单记录。 */
    void clearAll();

    //==============================================================================
    /** 判断指定文件是否应被跳过（黑名单且未勾选重试）。 */
    bool isBlacklisted (const juce::String& filePath) const;

    /** 判断指定文件是否允许重新扫描。 */
    bool canRetry (const juce::String& filePath) const;

    /** 返回所有黑名单条目。 */
    juce::Array<BlacklistEntry> getEntries() const;

    /** 返回黑名单条目数量。 */
    int getNumEntries() const noexcept;

private:
    //==============================================================================
    PluginBlacklist();
    ~PluginBlacklist() = default;

    BlacklistEntry* findEntry (const juce::String& filePath);
    const BlacklistEntry* findEntry (const juce::String& filePath) const;

    void updateEntry (const BlacklistEntry& entry);

    juce::File getBlacklistFile() const;

    //==============================================================================
    juce::Array<BlacklistEntry> entries;

    /** 黑名单会被后台扫描线程（读 isBlacklisted）与消息线程（写）并发访问。 */
    mutable std::mutex lock;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginBlacklist)
};

} // namespace minixer
