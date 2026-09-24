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

// （桥梁节点现统一按立体声总线构建，见构造函数注释。）

} // anonymous namespace

PluginBridgeNode::PluginBridgeNode (const juce::PluginDescription& description,
                                    PluginArchitecture arch)
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      pluginDescription (description),
      pluginName (description.name),
      architecture (arch),
      // 音频缓冲按立体声（与本机混音器/子进程 2 通道布局一致）。
      // 不能使用扫描描述中的通道数：VST3 扫描结果往往为 0/0（外壳与多数插件
      // 不报告有效通道数），若按 0 构建总线会导致节点无通道、图无法连接而全程无声。
      currentInputChannels (2),
      currentOutputChannels (2),
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
    // 共享音频缓冲按硬上限 kMaxAudioBufferFrames 一次性分配（见 PluginHostClient::connect），
    // 而非加载时的 bufferSize：之后任何时机（含 ASIO 设备内置 Control Panel 内）
    // 改变 Buffer Size，都不会让音频线程的拷贝越过共享内存边界。
    options.maxFramesPerBlock = kMaxAudioBufferFrames;

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
void PluginBridgeNode::setShutdownCompletionCallback (std::function<void()> callback)
{
    shutdownCompletionCallback = std::move (callback);
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

    // 2. 子进程的退出（卸载 DLL、释放采样库）可能耗时数秒，且本函数可能在
    //    AudioProcessorGraph 回收渲染序列时的消息线程上被调用，因此绝不能在
    //    调用线程上等待：交给后台收割器完成「等待退出 → 超时强杀」。
    auto completionCallback = std::move (shutdownCompletionCallback);
    shutdownCompletionCallback = nullptr;

    if (launcher != nullptr)
    {
        PluginHostProcessReaper::getInstance().reapAsync (
            std::shared_ptr<PluginHostLauncher> (std::move (launcher)),
            std::move (completionCallback));
    }
    else if (completionCallback != nullptr)
    {
        // 没有子进程可等待：立即回调，避免槽位一直停留在“卸载中”
        juce::MessageManager::callAsync (std::move (completionCallback));
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
