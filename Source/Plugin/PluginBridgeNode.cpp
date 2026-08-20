/*
  ==============================================================================

    PluginBridgeNode.cpp

  ==============================================================================
*/

#include "PluginBridgeNode.h"

namespace minixer
{

//==============================================================================
namespace
{

juce::AudioChannelSet channelSetFromCount (uint32_t numChannels)
{
    switch (numChannels)
    {
        case 0:  return juce::AudioChannelSet::disabled();
        case 1:  return juce::AudioChannelSet::mono();
        case 2:  return juce::AudioChannelSet::stereo();
        default: return juce::AudioChannelSet::discreteChannels (static_cast<int> (numChannels));
    }
}

} // anonymous namespace

PluginBridgeNode::PluginBridgeNode (const juce::PluginDescription& description,
                                    PluginArchitecture arch)
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties()
                                .withInput  ("Input",  channelSetFromCount (static_cast<uint32_t> (description.numInputChannels)),  true)
                                .withOutput ("Output", channelSetFromCount (static_cast<uint32_t> (description.numOutputChannels)), true)),
      pluginDescription (description),
      pluginName (description.name),
      architecture (arch),
      currentInputChannels (static_cast<uint32_t> (juce::jmax (0, description.numInputChannels))),
      currentOutputChannels (static_cast<uint32_t> (juce::jmax (0, description.numOutputChannels))),
      crashState (std::make_shared<CrashState>())
{
}

PluginBridgeNode::~PluginBridgeNode()
{
    shutdown();
}

//==============================================================================
void PluginBridgeNode::addListener (Listener* listener)
{
    listeners.add (listener);
}

void PluginBridgeNode::removeListener (Listener* listener)
{
    listeners.remove (listener);
}

//==============================================================================
bool PluginBridgeNode::initialize (double sampleRate, int bufferSize, juce::String& errorMessage)
{
    shutdown();

    isShuttingDown = false;
    crashed.store (false);
    crashReason.clear();

    currentSampleRate = sampleRate;
    currentBufferSize = bufferSize;

    auto ipcKey = juce::Uuid().toString();

    launcher = std::make_unique<PluginHostLauncher>();

    PluginHostLaunchOptions options;
    options.pluginId         = ipcKey;
    options.pluginPath       = pluginDescription.fileOrIdentifier;
    options.ipcKey           = ipcKey;
    options.mode             = "runtime";
    options.architecture     = architecture;
    options.maxFramesPerBlock = static_cast<uint32_t> (bufferSize);

    if (auto xml = pluginDescription.createXml())
        options.pluginDescriptionXmlB64 = juce::Base64::toBase64 (xml->toString ());

    if (! launcher->launch (options))
    {
        errorMessage = launcher->getLastError();
        launcher.reset();
        return false;
    }

    client = std::make_unique<PluginHostClient>();

    if (! client->connect (ipcKey, options.maxFramesPerBlock, currentInputChannels, currentOutputChannels))
    {
        errorMessage = client->getLastError();
        client.reset();
        launcher.reset();
        return false;
    }

    // 等待子进程连接并初始化插件
    if (! client->initPlugin (sampleRate, bufferSize))
    {
        errorMessage = client->getLastError();

        if (launcher != nullptr && launcher->didCrash())
            PluginBlacklist::getInstance().recordCrash (pluginDescription.fileOrIdentifier,
                                                        launcher->getExitCode());

        shutdown();
        return false;
    }

    initialized = true;
    return true;
}

//==============================================================================
void PluginBridgeNode::shutdown()
{
    isShuttingDown = true;

    if (crashState != nullptr)
        crashState->alive.store (false);

    // 1. 先关闭编辑器窗口，避免子进程退出后留下悬空窗口。
    hideEditorWindow();

    if (client != nullptr)
    {
        client->shutdown();
        client.reset();
    }

    if (launcher != nullptr)
    {
        if (launcher->isRunning())
        {
            if (! launcher->waitForExit (2000))
                launcher->terminateProcess();
        }

        launcher.reset();
    }

    initialized = false;
    crashed.store (false);
    crashReason.clear();
}

//==============================================================================
void PluginBridgeNode::prepareToPlay (double sampleRate, int samplesPerBlockExpected)
{
    currentSampleRate = sampleRate;
    currentBufferSize = samplesPerBlockExpected;

    if (client != nullptr && initialized)
        client->prepareToPlay (sampleRate, samplesPerBlockExpected);
}

//==============================================================================
void PluginBridgeNode::releaseResources()
{
    if (client != nullptr && initialized)
        client->releaseResources();
}

//==============================================================================
void PluginBridgeNode::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    if (crashed.load() || ! initialized || client == nullptr || ! client->isConnected())
    {
        buffer.clear();
        return;
    }

    const int numSamples = buffer.getNumSamples();

    client->writeInput (buffer, numSamples);

    if (! client->processBlock (numSamples))
    {
        buffer.clear();

        // processBlock 失败可能是子进程崩溃或挂起。
        if (launcher != nullptr && ! launcher->isRunning())
            handleProcessFailure ("crash");
        else
            handleProcessFailure ("hang");

        return;
    }

    client->readOutput (buffer, numSamples);
}

//==============================================================================
void PluginBridgeNode::getStateInformation (juce::MemoryBlock& destData)
{
    if (client != nullptr && initialized)
        client->getState (destData);
}

//==============================================================================
void PluginBridgeNode::setStateInformation (const void* data, int sizeInBytes)
{
    if (client != nullptr && initialized)
    {
        juce::MemoryBlock state (data, static_cast<size_t> (sizeInBytes));
        client->setState (state);
    }
}

//==============================================================================
bool PluginBridgeNode::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainInputChannelSet()  == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

//==============================================================================
bool PluginBridgeNode::isPluginRunning() const
{
    return initialized && client != nullptr && client->isConnected()
           && launcher != nullptr && launcher->isRunning();
}

//==============================================================================
juce::String PluginBridgeNode::getLastError() const
{
    if (client != nullptr)
        return client->getLastError();

    if (launcher != nullptr)
        return launcher->getLastError();

    return {};
}

//==============================================================================
void PluginBridgeNode::showEditorWindow (juce::Component* parent)
{
    if (client != nullptr && initialized)
    {
        void* handle = nullptr;

        if (parent != nullptr)
            handle = reinterpret_cast<void*> (parent->getWindowHandle());

        client->showEditor (handle);
    }
}

//==============================================================================
void PluginBridgeNode::hideEditorWindow()
{
    if (client != nullptr && initialized)
        client->hideEditor();
}

//==============================================================================
void PluginBridgeNode::handleProcessFailure (const juce::String& reason)
{
    // 避免在主动关闭或已经报告过崩溃时重复记录。
    bool expected = false;

    if (! crashed.compare_exchange_strong (expected, true))
        return;

    crashReason = reason;

    const auto filePath = pluginDescription.fileOrIdentifier;
    const int exitCode  = launcher != nullptr ? launcher->getExitCode() : 0;

    if (reason == "crash")
        PluginBlacklist::getInstance().recordCrash (filePath, exitCode);
    else
        PluginBlacklist::getInstance().recordScanFailure (filePath, reason);

    // 确保子进程不再继续运行，避免音频线程反复超时。
    if (launcher != nullptr && launcher->isRunning())
        launcher->terminateProcess();

    notifyCrashAsync();
}

//==============================================================================
void PluginBridgeNode::notifyCrashAsync()
{
    // 从音频线程安全地切换到消息线程通知监听器。
    auto weakState = std::weak_ptr<CrashState> (crashState);
    auto* self = this;

    juce::MessageManager::callAsync ([self, weakState]()
    {
        auto state = weakState.lock();

        if (state == nullptr || ! state->alive.load())
            return;

        self->listeners.call ([self] (Listener& l)
        {
            l.pluginBridgeNodeCrashed (self);
        });
    });
}

} // namespace minixer
