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

    /** 析构时若子进程仍在运行则强制结束，避免残留孤儿 PluginHost 进程。 */
    ~PluginHostLauncher();

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

    JUCE_DECLARE_NON_COPYABLE (PluginHostLauncher)
};

//==============================================================================
/** PluginHost 子进程的后台收割器。

    插件节点析构时若仍持有子进程，不能在该线程上同步等待其退出：析构可能发生在
    AudioProcessorGraph 回收渲染序列（RenderSequence 持有节点的强引用）时的消息
    线程上，而大型插件的子进程退出（卸载 DLL、释放采样库）可达数秒，会直接冻结
    界面。因此把「等待退出 → 超时强杀」交给本收割器的后台线程完成。
*/
class PluginHostProcessReaper
{
public:
    //==============================================================================
    static PluginHostProcessReaper& getInstance();

    //==============================================================================
    /** 在后台等待子进程退出（超时则强杀）并销毁 launcher，不阻塞调用线程。

        @param onFinished  子进程结束后在消息线程回调（可为空）。用于槽位在插件
                           卸载完成后解除“卸载中”状态。
    */
    void reapAsync (std::shared_ptr<PluginHostLauncher> launcher,
                    std::function<void()> onFinished = {});

    /** 等待所有在途收割任务结束（最多 timeoutMs），并强杀仍未退出的子进程。
        程序退出时调用，避免残留孤儿进程。
    */
    void drain (int timeoutMs);

private:
    //==============================================================================
    PluginHostProcessReaper() = default;

    /** 等待子进程优雅退出的宽限时间；超时后强杀。 */
    static constexpr int exitGraceMs = 3000;

    // 注意成员声明顺序：pool 必须最后声明（最先析构），确保线程池在等待并结束
    // 所有在途任务后才轮到 mutex / inFlight 被析构。
    std::mutex mutex;
    std::vector<std::shared_ptr<PluginHostLauncher>> inFlight;
    juce::ThreadPool pool { 4 };

    JUCE_DECLARE_NON_COPYABLE (PluginHostProcessReaper)
};

} // namespace minixer
