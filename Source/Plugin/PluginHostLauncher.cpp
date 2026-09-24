/*
  ==============================================================================

    PluginHostLauncher.cpp

  ==============================================================================
*/

#include "PluginHostLauncher.h"

#include <algorithm>
#include <memory>
#include <mutex>
#include <vector>

namespace minixer
{

//==============================================================================
PluginHostLauncher::~PluginHostLauncher()
{
    // 进程句柄析构不会结束子进程，这里显式强杀，避免残留孤儿 PluginHost 进程。
    // 正常情况下（已由收割器等待退出）子进程已不在运行，此处为无操作。
    if (process.isRunning())
        process.kill();
}

//==============================================================================
juce::File PluginHostLauncher::getHostExecutableForArchitecture (PluginArchitecture arch)
{
    auto hostExe = juce::File::getSpecialLocation (juce::File::currentApplicationFile);

    const juce::String baseName = "PluginHost";
    juce::String fileName;

    switch (arch)
    {
        case PluginArchitecture::x86:
            fileName = baseName + "32.exe";
            break;
        case PluginArchitecture::x64:
            fileName = baseName + "64.exe";
            break;
        default:
            fileName = baseName + "64.exe";
            break;
    }

    return hostExe.getParentDirectory().getChildFile (fileName);
}

//==============================================================================
bool PluginHostLauncher::launch (const PluginHostLaunchOptions& options)
{
    auto executable = getHostExecutableForArchitecture (options.architecture);

    if (! executable.existsAsFile())
    {
        lastError = "PluginHost executable not found: " + executable.getFullPathName();
        return false;
    }

    juce::StringArray args;
    args.add (executable.getFullPathName());
    args.add ("--mode=" + options.mode);
    args.add ("--plugin-id=" + options.pluginId);
    args.add ("--plugin-path=" + options.pluginPath);
    args.add ("--ipc-key=" + options.ipcKey);
    args.add ("--max-frames=" + juce::String (static_cast<juce::int64> (options.maxFramesPerBlock)));

    if (options.logPath.isNotEmpty())
        args.add ("--log-path=" + options.logPath);

    if (options.pluginDescriptionXmlB64.isNotEmpty())
        args.add ("--plugin-desc-b64=" + options.pluginDescriptionXmlB64);

    exitCodeKnown = false;
    knownExitCode = 0;

    if (! process.start (args, juce::ChildProcess::wantStdOut | juce::ChildProcess::wantStdErr))
    {
        lastError = "Failed to start PluginHost process";
        return false;
    }

    return true;
}

//==============================================================================
bool PluginHostLauncher::isRunning() const
{
    return process.isRunning();
}

//==============================================================================
bool PluginHostLauncher::waitForExit (int timeoutMs) const
{
    return process.waitForProcessToFinish (timeoutMs);
}

//==============================================================================
void PluginHostLauncher::terminateProcess()
{
    process.kill();
}

//==============================================================================
int PluginHostLauncher::getExitCode() const
{
    if (exitCodeKnown)
        return knownExitCode;

    const uint32_t code = process.getExitCode();

    exitCodeKnown = true;
    knownExitCode = static_cast<int> (code);
    return knownExitCode;
}

//==============================================================================
bool PluginHostLauncher::didCrash() const
{
    if (process.isRunning())
        return false;

    return getExitCode() != 0;
}

//==============================================================================
PluginHostProcessReaper& PluginHostProcessReaper::getInstance()
{
    static PluginHostProcessReaper instance;
    return instance;
}

//==============================================================================
void PluginHostProcessReaper::reapAsync (std::shared_ptr<PluginHostLauncher> launcher,
                                         std::function<void()> onFinished)
{
    if (launcher == nullptr)
    {
        // 没有子进程可等待：直接回调，避免调用方一直停留在“卸载中”
        if (onFinished != nullptr)
            juce::MessageManager::callAsync (std::move (onFinished));

        return;
    }

    {
        std::lock_guard<std::mutex> lock (mutex);
        inFlight.push_back (launcher);
    }

    auto task = [this, launcher, onFinished]() -> juce::ThreadPoolJob::JobStatus
    {
        // 后台线程：等待子进程优雅退出，超时则强杀（绝不在消息线程上等待）
        if (launcher->isRunning())
        {
            if (! launcher->waitForExit (exitGraceMs))
                launcher->terminateProcess();
        }

        {
            std::lock_guard<std::mutex> lock (mutex);
            inFlight.erase (std::remove (inFlight.begin(), inFlight.end(), launcher), inFlight.end());
        }

        if (onFinished != nullptr)
            juce::MessageManager::callAsync (onFinished);

        return juce::ThreadPoolJob::jobHasFinished;
    };

    pool.addJob (std::function<juce::ThreadPoolJob::JobStatus()> (std::move (task)));
}

//==============================================================================
void PluginHostProcessReaper::drain (int timeoutMs)
{
    // 先给在途收割一点时间让子进程优雅退出
    pool.removeAllJobs (true, timeoutMs);

    // 兜底：仍未结束的（含已被移出任务队列的）子进程直接强杀，避免残留孤儿进程
    std::vector<std::shared_ptr<PluginHostLauncher>> remaining;

    {
        std::lock_guard<std::mutex> lock (mutex);
        remaining.swap (inFlight);
    }

    for (auto& launcher : remaining)
    {
        if (launcher != nullptr && launcher->isRunning())
            launcher->terminateProcess();
    }
}

} // namespace minixer
