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
class PluginHostApplication  : public juce::JUCEApplicationBase
{
public:
    PluginHostApplication() = default;

    const juce::String getApplicationName() override { return "Minixer PluginHost"; }
    const juce::String getApplicationVersion() override { return "0.4.1 Beta"; }
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

        minixer::PluginHostServer server;

        if (! server.connect (ipcKey, pluginPath, pluginDescB64, maxFrames, numInputs, numOutputs))
        {
            juce::Logger::writeToLog ("Failed to connect IPC");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        int result = 0;

        if (mode == "scan")
            result = server.runScanMode();
        else
            result = server.runRuntimeMode();

        setApplicationReturnValue (result);
        quit();
    }

    void shutdown() override {}
    void systemRequestedQuit() override { quit(); }
    void anotherInstanceStarted (const juce::String&) override {}
    void suspended() override {}
    void resumed() override {}
    void unhandledException (const std::exception*, const juce::String&, int) override {}

private:
    std::unique_ptr<juce::Logger> logOwner;
};

//==============================================================================
START_JUCE_APPLICATION (PluginHostApplication)
