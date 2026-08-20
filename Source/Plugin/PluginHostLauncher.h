/*
  ==============================================================================

    PluginHostLauncher.h
    根据插件架构选择并启动对应的 PluginHost 子进程。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginArchitecture.h"

namespace minixer
{

//==============================================================================
/** PluginHost 子进程启动参数。 */
struct PluginHostLaunchOptions
{
    juce::String pluginId;
    juce::String pluginPath;
    juce::String ipcKey;
    juce::String mode;          // "scan" 或 "runtime"
    juce::String logPath;
    juce::String pluginDescriptionXmlB64; // 运行期模式需要，用于 shell 插件选择正确的子插件
    uint32_t     maxFramesPerBlock = 4096;
    PluginArchitecture architecture = PluginArchitecture::x64;
};

//==============================================================================
/** 根据插件架构选择 PluginHost64.exe 或 PluginHost32.exe，
    启动子进程并返回进程句柄封装。
*/
class PluginHostLauncher
{
public:
    PluginHostLauncher() = default;

    /** 返回指定架构对应的 PluginHost 可执行文件路径。
        路径与宿主可执行文件位于同一目录。
    */
    static juce::File getHostExecutableForArchitecture (PluginArchitecture arch);

    /** 启动子进程。返回是否成功；成功时可通过 getProcess() 获取进程对象。 */
    bool launch (const PluginHostLaunchOptions& options);

    /** 检查子进程是否仍在运行。 */
    bool isRunning() const;

    /** 等待子进程退出，最多等待 timeoutMs 毫秒。返回是否在规定时间内退出。 */
    bool waitForExit (int timeoutMs) const;

    /** 强制终止子进程。 */
    void terminateProcess();

    /** 返回最近一次错误信息。 */
    juce::String getLastError() const { return lastError; }

    /** 返回子进程退出码；若仍在运行则返回 0。 */
    int getExitCode() const;

    /** 返回子进程是否异常终止（非零退出码或非正常退出）。 */
    bool didCrash() const;

private:
    juce::ChildProcess process;
    juce::String lastError;
    mutable bool exitCodeKnown = false;
    mutable int knownExitCode = 0;
};

} // namespace minixer
