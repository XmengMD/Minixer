/*
  ==============================================================================

    PluginHostClient.cpp

  ==============================================================================
*/

#include "PluginHostClient.h"

namespace minixer
{

//==============================================================================
// Init 握手等待子进程加载插件（含 license 校验）完成的最长时间。
// UI 加载已异步化不阻塞，此值只作为防挂死兜底，因而取较宽松的 120 秒。
static constexpr int kInitResultTimeoutMs = 120000;

//==============================================================================
PluginHostClient::PluginHostClient() = default;

PluginHostClient::~PluginHostClient()
{
    disconnect();
}

//==============================================================================
bool PluginHostClient::connect (const juce::String& ipcKey,
                                uint32_t maxFrames,
                                uint32_t numInputs,
                                uint32_t numOutputs)
{
    disconnect();

    maxFramesPerBlock = maxFrames;

    // 共享内存尺寸必须以两侧一致的最小通道数计算：子进程固定按 (2,2) 打开映射，
    // 而插件描述里扫描得到的通道数可能为 0 / 不准确。这里统一钳到至少 2 通道，
    // 保证主进程创建的映射 ≥ 子进程打开所需尺寸（MapViewOfFile 不允许超尺寸视图）。
    numInputChannels  = juce::jmax (2u, numInputs);
    numOutputChannels = juce::jmax (2u, numOutputs);

    sharedMemory = createDefaultSharedMemoryRegion();

    if (sharedMemory == nullptr)
    {
        lastError = "Shared memory implementation not available";
        return false;
    }

    const size_t audioShmSize = AudioSharedMemoryLayout::getTotalSize (maxFramesPerBlock,
                                                                       numInputChannels,
                                                                       numOutputChannels);

    if (! sharedMemory->create (ipcKey, audioShmSize))
    {
        lastError = "Failed to create shared memory";
        return false;
    }

    audioLayout = static_cast<AudioSharedMemoryLayout*> (sharedMemory->getAddress());

    if (audioLayout == nullptr)
    {
        lastError = "Failed to map shared memory";
        return false;
    }

    audioLayout->maxFramesPerBlock = maxFramesPerBlock;
    audioLayout->numInputChannels = numInputChannels;
    audioLayout->numOutputChannels = numOutputChannels;

    transport = createDefaultIpcTransport();

    if (transport == nullptr)
    {
        lastError = "IPC transport implementation not available";
        return false;
    }

    if (! transport->connect (ipcKey))
    {
        lastError = "Failed to connect IPC transport";
        return false;
    }

    return true;
}

//==============================================================================
void PluginHostClient::disconnect()
{
    if (transport != nullptr && transport->isConnected())
    {
        shutdown();
        transport->close();
    }

    transport.reset();

    audioLayout = nullptr;
    sharedMemory.reset();
}

//==============================================================================
bool PluginHostClient::initPlugin (double sampleRate, int bufferSize)
{
    MessageBuilder payload;
    payload.writeUInt32 (static_cast<uint32_t> (sampleRate));
    payload.writeUInt32 (static_cast<uint32_t> (bufferSize));
    payload.writeUInt32 (numInputChannels);
    payload.writeUInt32 (numOutputChannels);

    const auto requestId = nextRequestId();

    if (! sendCommand (ControlMessageType::Init, payload.getData(), requestId))
        return false;

    juce::MemoryBlock response;
    if (! readResponse (response, ControlMessageType::InitResult, requestId, kInitResultTimeoutMs))
        return false;

    MessageReader reader (response);
    bool success = false;
    reader.readBool (success);
    return success;
}

//==============================================================================
bool PluginHostClient::prepareToPlay (double sampleRate, int bufferSize)
{
    MessageBuilder payload;
    payload.writeUInt32 (static_cast<uint32_t> (sampleRate));
    payload.writeUInt32 (static_cast<uint32_t> (bufferSize));

    return sendCommand (ControlMessageType::PrepareToPlay, payload.getData(), nextRequestId());
}

//==============================================================================
bool PluginHostClient::releaseResources()
{
    return sendCommand (ControlMessageType::ReleaseResources, {}, nextRequestId());
}

//==============================================================================
bool PluginHostClient::processBlock (int numSamples)
{
    if (audioLayout == nullptr)
        return false;

    const uint32_t currentSeq = audioLayout->hostWriteSeq.load (std::memory_order_relaxed) + 1;
    audioLayout->hostWriteSeq.store (currentSeq, std::memory_order_release);

    MessageBuilder payload;
    payload.writeUInt32 (static_cast<uint32_t> (numSamples));

    const auto requestId = nextRequestId();
    if (! sendCommand (ControlMessageType::ProcessBlock, payload.getData(), requestId))
        return false;

    // 等待子进程完成：pluginWriteSeq 等于 currentSeq 表示处理完成。
    //
    // 这是音频实时线程：旧实现每块最长 Thread::sleep 累计 5 秒，设备重启时
    // 消息线程的 setAudioDeviceSetup（停止设备）会同步等待本回调返回，从而
    // 长时间卡死界面造成"无响应"。因此改为有界自旋：
    //  - 正常插件由子进程控制线程亚毫秒级完成，循环体通常一次都不执行；
    //  - 仅在子进程真正挂起/失联时才耗尽 1000ms 上限，且随后桥节点会立即
    //    终止子进程（handleProcessFailure），后续块的 WriteFile 快速失败，
    //    不会反复等待。
    constexpr int kProcessBlockWaitMs = 1000;
    const double waitStartMs = juce::Time::getMillisecondCounterHiRes();

    while (audioLayout->pluginWriteSeq.load (std::memory_order_acquire) != currentSeq)
    {
        if (juce::Time::getMillisecondCounterHiRes() - waitStartMs > kProcessBlockWaitMs)
        {
            lastError = "PluginHost process block timeout";
            return false;
        }

        // 不用 Thread::sleep：实时线程上避免任何 OS 级睡眠带来的抖动。
        juce::Thread::yield();
    }

    return true;
}

//==============================================================================
void PluginHostClient::writeInput (const juce::AudioBuffer<float>& inputBuffer, int numSamples)
{
    if (audioLayout == nullptr)
        return;

    const auto inputChans = static_cast<uint32_t> (inputBuffer.getNumChannels());
    const auto chansToWrite = juce::jmin (numInputChannels, inputChans);

    // 钳制到共享缓冲容量（连接时按硬上限 kMaxAudioBufferFrames 分配）。
    // 设备 Buffer Size 在运行期发生改动后，实际块大小必须 ≤ 该上限，否则
    // 循此拷贝会越过映射边界造成访问冲突（旧实现即因此随机崩溃）。
    const auto framesToCopy = static_cast<uint32_t> (juce::jmin (numSamples,
                                                                 static_cast<int> (maxFramesPerBlock)));

    for (uint32_t ch = 0; ch < chansToWrite; ++ch)
    {
        auto* dst = audioLayout->getInputChannelData (ch, maxFramesPerBlock,
                                                       numInputChannels, numOutputChannels);
        std::memcpy (dst, inputBuffer.getReadPointer (static_cast<int> (ch)),
                     static_cast<size_t> (framesToCopy) * sizeof (float));
    }
}

//==============================================================================
void PluginHostClient::readOutput (juce::AudioBuffer<float>& outputBuffer, int numSamples)
{
    if (audioLayout == nullptr)
        return;

    const auto outputChans = static_cast<uint32_t> (outputBuffer.getNumChannels());
    const auto chansToRead = juce::jmin (numOutputChannels, outputChans);

    // 与 writeInput 一致：钳制到共享缓冲容量，防止设备 Buffer Size 在运行期
    // 被改大后从这里越界读出共享映射之外的残留内存。
    const auto framesToCopy = static_cast<uint32_t> (juce::jmin (numSamples,
                                                                 static_cast<int> (maxFramesPerBlock)));

    for (uint32_t ch = 0; ch < chansToRead; ++ch)
    {
        auto* src = audioLayout->getOutputChannelData (ch, maxFramesPerBlock,
                                                        numInputChannels, numOutputChannels);
        std::memcpy (outputBuffer.getWritePointer (static_cast<int> (ch)),
                     src,
                     static_cast<size_t> (framesToCopy) * sizeof (float));
    }
}

//==============================================================================
bool PluginHostClient::setState (const juce::MemoryBlock& state)
{
    MessageBuilder payload;
    payload.writeMemoryBlock (state);
    return sendCommand (ControlMessageType::SetState, payload.getData(), nextRequestId());
}

//==============================================================================
bool PluginHostClient::getState (juce::MemoryBlock& state)
{
    const auto requestId = nextRequestId();

    if (! sendCommand (ControlMessageType::GetState, {}, requestId))
        return false;

    juce::MemoryBlock response;
    if (! readResponse (response, ControlMessageType::StateData, requestId, 30000))
        return false;

    MessageReader reader (response);
    return reader.readMemoryBlock (state);
}

//==============================================================================
bool PluginHostClient::setParameter (int index, float value)
{
    MessageBuilder payload;
    payload.writeUInt32 (static_cast<uint32_t> (index));
    payload.writeFloat (value);
    return sendCommand (ControlMessageType::SetParameter, payload.getData(), nextRequestId());
}

//==============================================================================
int PluginHostClient::getLatencySamples()
{
    const auto requestId = nextRequestId();

    if (! sendCommand (ControlMessageType::GetLatency, {}, requestId))
        return 0;

    juce::MemoryBlock response;
    if (! readResponse (response, ControlMessageType::LatencyInfo, requestId, 30000))
        return 0;

    MessageReader reader (response);
    uint32_t latency = 0;
    reader.readUInt32 (latency);
    return static_cast<int> (latency);
}

//==============================================================================
bool PluginHostClient::showEditor (void* parentWindowHandle)
{
    MessageBuilder payload;
    payload.writeUInt64 (reinterpret_cast<uintptr_t> (parentWindowHandle));
    return sendCommand (ControlMessageType::ShowEditor, payload.getData(), nextRequestId());
}

//==============================================================================
bool PluginHostClient::hideEditor()
{
    return sendCommand (ControlMessageType::HideEditor, {}, nextRequestId());
}

//==============================================================================
bool PluginHostClient::shutdown()
{
    if (transport == nullptr || ! transport->isConnected())
        return false;

    return sendCommand (ControlMessageType::Shutdown, {}, nextRequestId());
}

//==============================================================================
bool PluginHostClient::isConnected() const
{
    return transport != nullptr && transport->isConnected();
}

//==============================================================================
bool PluginHostClient::sendCommand (ControlMessageType type,
                                    const juce::MemoryBlock& payload,
                                    uint64_t requestId)
{
    if (transport == nullptr)
    {
        lastError = "Transport not connected";
        return false;
    }

    MessageBuilder builder;
    builder.getData() = payload;
    auto frame = builder.buildWithHeader (type, requestId);
    return transport->sendMessage (frame);
}

//==============================================================================
bool PluginHostClient::readResponse (juce::MemoryBlock& payload,
                                     ControlMessageType expectedType,
                                     uint64_t requestId,
                                     int timeoutMs)
{
    if (transport == nullptr)
        return false;

    const auto deadline = juce::Time::getCurrentTime() + juce::RelativeTime::milliseconds (timeoutMs);

    while (true)
    {
        juce::MemoryBlock frame;
        const int remainingMs = juce::jmax (0, static_cast<int> ((deadline - juce::Time::getCurrentTime()).inMilliseconds()));

        if (! transport->readMessage (frame, remainingMs))
        {
            lastError = "Timeout waiting for response";
            return false;
        }

        if (frame.getSize() < ControlHeader::size)
            continue;

        ControlHeader header;
        std::memcpy (&header, frame.getData(), ControlHeader::size);

        if (! header.isValid())
            continue;

        payload.replaceAll (static_cast<const uint8_t*> (frame.getData()) + ControlHeader::size,
                            frame.getSize() - ControlHeader::size);

        if (header.requestId == requestId && static_cast<ControlMessageType> (header.type) == expectedType)
            return true;

        // 异步通知（日志、错误、编辑器关闭）暂不处理，继续等待目标响应。
        if (static_cast<ControlMessageType> (header.type) == ControlMessageType::Error)
        {
            MessageReader reader (payload);
            uint32_t code = 0;
            juce::String msg;
            reader.readUInt32 (code);
            reader.readString (msg);
            lastError = msg;
        }
    }
}

//==============================================================================
uint64_t PluginHostClient::nextRequestId()
{
    return currentRequestId++;
}

} // namespace minixer
