/*
  ==============================================================================

    Main.cpp (PluginHost)
    PluginHost 子进程入口。

    命令行参数：
      --mode=scan|runtime
      --plugin-id=<uuid>
      --plugin-path=<absolute-path-to-vst3>
      --ipc-key=<unique-shared-memory-key>
      [--log-path=<path>]
      [--plugin-desc-b64=<base64-encoded-plugin-description-xml>]

  ==============================================================================
*/

#include <JuceHeader.h>
#include "PluginHostServer.h"
#include "LookAndFeel/MixerLookAndFeel.h"

#if JUCE_WINDOWS
 #include <windows.h>
 #include <dbghelp.h>
 #pragma comment(lib, "dbghelp.lib")
#endif

#if JUCE_WINDOWS
namespace
{

juce::String g_crashDumpIpcKey;
juce::String g_crashDumpLogPath;

//==============================================================================
juce::File getCrashDumpDirectory()
{
    if (g_crashDumpLogPath.isNotEmpty())
    {
        auto dir = juce::File (g_crashDumpLogPath).getChildFile ("CrashDumps");
        dir.createDirectory();
        return dir;
    }

    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Minixer")
                   .getChildFile ("CrashDumps");

    dir.createDirectory();
    return dir;
}

//==============================================================================
LONG WINAPI writeMiniDumpOnUnhandledException (EXCEPTION_POINTERS* exceptionInfo)
{
    if (exceptionInfo == nullptr)
        return EXCEPTION_EXECUTE_HANDLER;

    auto dumpFile = getCrashDumpDirectory()
                        .getChildFile ("PluginHost_" + g_crashDumpIpcKey
                                       + "_" + juce::Time::getCurrentTime().formatted ("%Y%m%d_%H%M%S")
                                       + ".dmp");

    auto dumpPathW = dumpFile.getFullPathName().toWideCharPointer();
    auto* file = CreateFileW (dumpPathW, GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (file != INVALID_HANDLE_VALUE)
    {
        MINIDUMP_EXCEPTION_INFORMATION miniInfo;
        miniInfo.ThreadId          = GetCurrentThreadId();
        miniInfo.ExceptionPointers = exceptionInfo;
        miniInfo.ClientPointers    = FALSE;

        MiniDumpWriteDump (GetCurrentProcess(),
                           GetCurrentProcessId(),
                           file,
                           MiniDumpNormal,
                           &miniInfo,
                           nullptr,
                           nullptr);

        CloseHandle (file);
    }

    return EXCEPTION_EXECUTE_HANDLER;
}

} // anonymous namespace
#endif

//==============================================================================
static juce::String getCommandLineParameter (const juce::String& name,
                                              const juce::String& defaultValue = {})
{
    auto cmd = juce::JUCEApplicationBase::getCommandLineParameterArray();

    for (const auto& arg : cmd)
    {
        if (arg.startsWith ("--" + name + "="))
            return arg.substring (name.length() + 3);
    }

    return defaultValue;
}

//==============================================================================
/** 运行期控制循环线程。

    IPC 控制循环（加载插件、处理音频/参数/状态消息）一旦进入会是长时间阻塞
    （readMessage 阻塞读管道）。若把它放在消息线程上，本进程的 JUCE 分发循环
    将永远不会运行，插件编辑器窗口就无法被消息泵驱动（不刷新/不及时重绘）。
    因此运行期模式下控制循环放到独立线程，让消息线程进入 runDispatchLoop
    驱动编辑器 UI。
*/
class PluginHostControlThread final : public juce::Thread
{
public:
    explicit PluginHostControlThread (std::shared_ptr<minixer::PluginHostServer> serverIn)
        : juce::Thread ("PluginHost IPC Control Loop"),
          server (std::move (serverIn))
    {}

    void run() override
    {
        const int result = server != nullptr ? server->runRuntimeMode() : 1;

        if (auto* app = juce::JUCEApplicationBase::getInstance())
            app->setApplicationReturnValue (result);

        // 控制循环结束（收到 Shutdown / 管道断开 / 加载失败）→ 结束消息循环，
        // 让进程正常退出并回到 PluginHostApplication::shutdown() 清理。
        if (auto* mm = juce::MessageManager::getInstanceWithoutCreating())
            mm->stopDispatchLoop();
    }

private:
    std::shared_ptr<minixer::PluginHostServer> server;
};

//==============================================================================
class PluginHostApplication  : public juce::JUCEApplicationBase
{
public:
    PluginHostApplication() = default;

    const juce::String getApplicationName() override { return "Minixer PluginHost"; }
    const juce::String getApplicationVersion() override { return ProjectInfo::versionString; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String&) override
    {
        auto mode       = getCommandLineParameter ("mode");
        auto pluginPath = getCommandLineParameter ("plugin-path");
        auto ipcKey     = getCommandLineParameter ("ipc-key");
        auto logPath    = getCommandLineParameter ("log-path");
        auto pluginDescB64 = getCommandLineParameter ("plugin-desc-b64");
        auto maxFramesStr = getCommandLineParameter ("max-frames", "4096");

       #if JUCE_WINDOWS
        g_crashDumpIpcKey  = ipcKey;
        g_crashDumpLogPath = logPath;
        SetUnhandledExceptionFilter (writeMiniDumpOnUnhandledException);
       #endif

        const uint32_t maxFrames = static_cast<uint32_t> (juce::jmax (1, maxFramesStr.getIntValue()));
        const uint32_t numInputs = 2;
        const uint32_t numOutputs = 2;

        if (logPath.isNotEmpty())
        {
            auto* fileLogger = juce::FileLogger::createDateStampedLogger (logPath,
                                                                           "PluginHost_" + ipcKey,
                                                                           ".log",
                                                                           "Minixer PluginHost started");
            juce::Logger::setCurrentLogger (fileLogger);
            logOwner.reset (fileLogger);
        }

        if (mode != "scan" && mode != "runtime")
        {
            juce::Logger::writeToLog ("Missing or invalid --mode");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        if (pluginPath.isEmpty() || ipcKey.isEmpty())
        {
            juce::Logger::writeToLog ("Missing --plugin-path or --ipc-key");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        // 提前触发 JUCE 的 DPI 感知初始化（per-monitor DPI v2）：
        // 确保后续插件创建/编辑器窗口在明确且一致的 DPI 语义下工作，
        // 避免编辑器 UI “模糊/只显示一部分”的问题。
        juce::Desktop::getInstance().getDisplays();

        // 与主程序一致的深色主题：安装 MixerLookAndFeel，使插件编辑器
        // 窗口的底色与宿主绘制控件和主界面风格统一。
        lookAndFeel = std::make_unique<minixer::MixerLookAndFeel>();
        juce::LookAndFeel::setDefaultLookAndFeel (lookAndFeel.get());

        server = std::make_shared<minixer::PluginHostServer>();

        if (! server->connect (ipcKey, pluginPath, pluginDescB64, maxFrames, numInputs, numOutputs))
        {
            juce::Logger::writeToLog ("Failed to connect IPC");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        if (mode == "scan")
        {
            // 扫描模式保持同步执行，结果经 IPC 发回后进程随即退出。
            const int result = server->runScanMode();
            setApplicationReturnValue (result);
            quit();
            return;
        }

        // 运行期模式：控制循环运行在后台线程，消息线程进入 runDispatchLoop
        // 以驱动插件编辑器 UI；initialise() 立即返回（不再阻塞）。
        controlThread = std::make_unique<PluginHostControlThread> (server);
        controlThread->startThread();
    }

    void shutdown() override
    {
        // 正常退出时控制线程在结束前已调用 stopDispatchLoop；这里兜底等待退出，
        // 防止进程在控制线程仍在运行时销毁（边界情况为主进程强杀场景）。
        if (controlThread != nullptr)
        {
            controlThread->stopThread (3000);
            controlThread.reset();
        }

        server.reset();

        // 编辑器窗口/控件已全部销毁后才能安全卸载 LookAndFeel。
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        lookAndFeel.reset();
    }
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

private:
    std::shared_ptr<minixer::PluginHostServer> server;
    std::unique_ptr<PluginHostControlThread> controlThread;
    std::unique_ptr<juce::Logger> logOwner;
    std::unique_ptr<minixer::MixerLookAndFeel> lookAndFeel;
};

//==============================================================================
START_JUCE_APPLICATION (PluginHostApplication)
