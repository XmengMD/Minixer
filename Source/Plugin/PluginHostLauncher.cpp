/*
  ==============================================================================

    PluginHostLauncher.cpp

  ==============================================================================
*/

#include "PluginHostLauncher.h"

namespace minixer
{

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

} // namespace minixer
