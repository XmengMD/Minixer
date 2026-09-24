/*
  ==============================================================================

    IpcProtocol.h
    Minixer 主进程 ↔ PluginHost 子进程的二进制 IPC 协议定义。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include <atomic>
#include <cstdint>
#include <limits>
#include <cstring>

namespace minixer
{

//==============================================================================
/** 控制消息头，所有控制通道消息共享此头部。 */
struct ControlHeader
{
    static constexpr uint32_t magicValue = 0x4D494E58; // 'MINX'
    static constexpr uint32_t versionValue = 1;

    uint32_t magic = magicValue;
    uint32_t version = versionValue;
    uint32_t type = 0;
    uint32_t payloadSize = 0;
    uint64_t requestId = 0;

    static constexpr size_t size = sizeof (uint32_t) * 4 + sizeof (uint64_t);

    bool isValid() const noexcept { return magic == magicValue && version == versionValue; }
};

static_assert (ControlHeader::size == 24, "ControlHeader layout mismatch");

//==============================================================================
/** 控制消息类型。 */
enum class ControlMessageType : uint32_t
{
    // 主进程 -> 子进程
    Init            = 0x0001,
    PrepareToPlay   = 0x0002,
    ReleaseResources= 0x0003,
    ProcessBlock    = 0x0004,
    SetState        = 0x0005,
    GetState        = 0x0006,
    SetParameter    = 0x0007,
    GetLatency      = 0x0008,
    ShowEditor      = 0x0009,
    HideEditor      = 0x000A,
    Shutdown        = 0x000B,

    // 子进程 -> 主进程
    InitResult      = 0x1001,
    StateData       = 0x1002,
    LatencyInfo     = 0x1003,
    EditorClosed    = 0x1004,
    LogMessage      = 0x1005,
    Error           = 0x1006,

    // 扫描模式专用
    ScanResult      = 0x2001,
    ScanError       = 0x2002
};

//==============================================================================
/** 序列化/反序列化辅助类。

    使用固定宽度类型（uint32_t / uint64_t / float）确保 32-bit 与 64-bit
    进程间的内存布局一致。
*/
class MessageBuilder
{
public:
    MessageBuilder() = default;

    explicit MessageBuilder (juce::MemoryBlock& existing)
        : data (existing) {}

    MessageBuilder& operator= (const MessageBuilder&) = delete;

    //==========================================================================
    void writeUInt32 (uint32_t value)
    {
        data.append (&value, sizeof (value));
    }

    void writeUInt64 (uint64_t value)
    {
        data.append (&value, sizeof (value));
    }

    void writeFloat (float value)
    {
        data.append (&value, sizeof (value));
    }

    void writeBool (bool value)
    {
        writeUInt32 (value ? 1u : 0u);
    }

    void writeString (const juce::String& value)
    {
        auto utf8 = value.toUTF8();
        const uint32_t len = static_cast<uint32_t> (value.getNumBytesAsUTF8());
        writeUInt32 (len);
        if (len > 0)
            data.append (utf8.getAddress(), static_cast<size_t> (len));
    }

    void writeMemoryBlock (const juce::MemoryBlock& block)
    {
        const uint32_t size = static_cast<uint32_t> (juce::jmin<size_t> (block.getSize(), (std::numeric_limits<uint32_t>::max)()));
        writeUInt32 (size);
        if (size > 0)
            data.append (block.getData(), static_cast<size_t> (size));
    }

    void writeRaw (const void* source, size_t numBytes)
    {
        if (source != nullptr && numBytes > 0)
            data.append (source, numBytes);
    }

    //==========================================================================
    juce::MemoryBlock buildWithHeader (ControlMessageType type, uint64_t requestId = 0) const
    {
        ControlHeader header;
        header.type = static_cast<uint32_t> (type);
        header.payloadSize = static_cast<uint32_t> (juce::jmin<size_t> (data.getSize(), (std::numeric_limits<uint32_t>::max)()));
        header.requestId = requestId;

        juce::MemoryBlock result;
        result.append (&header, ControlHeader::size);
        result.append (data.getData(), static_cast<size_t> (header.payloadSize));
        return result;
    }

    //==========================================================================
    juce::MemoryBlock& getData() noexcept { return data; }
    const juce::MemoryBlock& getData() const noexcept { return data; }

private:
    juce::MemoryBlock data;
};

//==============================================================================
/** 消息读取辅助类，从 MemoryBlock 中按顺序读取字段。 */
class MessageReader
{
public:
    explicit MessageReader (const juce::MemoryBlock& source)
        : data (static_cast<const uint8_t*> (source.getData())),
          totalSize (source.getSize()) {}

    explicit MessageReader (const void* source, size_t numBytes)
        : data (static_cast<const uint8_t*> (source)),
          totalSize (numBytes) {}

    //==========================================================================
    bool canRead (size_t numBytes) const noexcept
    {
        return position + numBytes <= totalSize;
    }

    bool readUInt32 (uint32_t& value)
    {
        if (! canRead (sizeof (value)))
            return false;

        std::memcpy (&value, data + position, sizeof (value));
        position += sizeof (value);
        return true;
    }

    bool readUInt64 (uint64_t& value)
    {
        if (! canRead (sizeof (value)))
            return false;

        std::memcpy (&value, data + position, sizeof (value));
        position += sizeof (value);
        return true;
    }

    bool readFloat (float& value)
    {
        if (! canRead (sizeof (value)))
            return false;

        std::memcpy (&value, data + position, sizeof (value));
        position += sizeof (value);
        return true;
    }

    bool readBool (bool& value)
    {
        uint32_t v = 0;
        if (! readUInt32 (v))
            return false;

        value = (v != 0);
        return true;
    }

    bool readString (juce::String& value)
    {
        uint32_t len = 0;
        if (! readUInt32 (len))
            return false;

        if (len == 0)
        {
            value = {};
            return true;
        }

        if (! canRead (len))
            return false;

        value = juce::String::fromUTF8 (reinterpret_cast<const char*> (data + position), static_cast<int> (len));
        position += len;
        return true;
    }

    bool readMemoryBlock (juce::MemoryBlock& value)
    {
        uint32_t size = 0;
        if (! readUInt32 (size))
            return false;

        if (size == 0)
        {
            value.reset();
            return true;
        }

        if (! canRead (size))
            return false;

        value.replaceAll (data + position, static_cast<size_t> (size));
        position += size;
        return true;
    }

    //==========================================================================
    size_t getPosition() const noexcept { return position; }
    size_t getRemaining() const noexcept { return totalSize - position; }

private:
    const uint8_t* data = nullptr;
    size_t totalSize = 0;
    size_t position = 0;
};

//==============================================================================
/** 音频共享缓冲的单通道帧数硬上限。

    共享内存按此上限一次性分配（见 PluginHostClient::connect），而不是按
    “插件加载时的 Buffer Size”。此后任何时机（包括在 ASIO 设备内置
    Control Panel 内改动 Buffer Size）到达的实际块大小都必须钳制到 ≤ 该值，
    音频线程的拷贝/读取才不会越过映射边界造成访问冲突。

    该值远大于常见设备 Buffer Size（ASIO 通常 32~8192，个别专业接口才到
    16384）。按 2 进 2 出计算整块约 256KB，即使 4 个插件槽合计约 1MB，可忽略。
*/
static constexpr uint32_t kMaxAudioBufferFrames = 16384;

//==============================================================================
/** 音频共享内存布局，32-bit 与 64-bit 进程必须看到相同的字段偏移。

    使用 std::atomic<uint32_t> 作为序列号；在共享内存中通过显式对齐保证
    cache-line 隔离。
*/
struct AudioSharedMemoryLayout
{
    static constexpr size_t cacheLineSize = 64;

    alignas (cacheLineSize) std::atomic<uint32_t> hostWriteSeq;
    alignas (cacheLineSize) std::atomic<uint32_t> hostReadSeq;
    alignas (cacheLineSize) std::atomic<uint32_t> pluginWriteSeq;
    alignas (cacheLineSize) std::atomic<uint32_t> pluginReadSeq;

    uint32_t maxFramesPerBlock = 0;
    uint32_t numInputChannels = 0;
    uint32_t numOutputChannels = 0;

    // 实际采样缓冲区紧跟在结构体之后：
    // float inputChannels[numInputChannels][maxFramesPerBlock];
    // float outputChannels[numOutputChannels][maxFramesPerBlock];

    static size_t getTotalSize (uint32_t maxFrames, uint32_t numInputs, uint32_t numOutputs)
    {
        const size_t headerSize = sizeof (AudioSharedMemoryLayout);
        const size_t inputSize  = static_cast<size_t> (numInputs)  * static_cast<size_t> (maxFrames) * sizeof (float);
        const size_t outputSize = static_cast<size_t> (numOutputs) * static_cast<size_t> (maxFrames) * sizeof (float);
        return headerSize + inputSize + outputSize;
    }

    float* getInputChannelData (uint32_t channel, uint32_t maxFrames, uint32_t numInputs, uint32_t numOutputs)
    {
        auto* base = reinterpret_cast<uint8_t*> (this) + sizeof (AudioSharedMemoryLayout);
        auto* inputs = reinterpret_cast<float*> (base);
        (void) numOutputs;
        (void) numInputs;
        return inputs + static_cast<size_t> (channel) * static_cast<size_t> (maxFrames);
    }

    const float* getInputChannelData (uint32_t channel, uint32_t maxFrames, uint32_t numInputs, uint32_t numOutputs) const
    {
        return const_cast<AudioSharedMemoryLayout*> (this)->getInputChannelData (channel, maxFrames, numInputs, numOutputs);
    }

    float* getOutputChannelData (uint32_t channel, uint32_t maxFrames, uint32_t numInputs, uint32_t numOutputs)
    {
        auto* base = reinterpret_cast<uint8_t*> (this) + sizeof (AudioSharedMemoryLayout);
        auto* inputs = reinterpret_cast<float*> (base);
        auto* outputs = inputs + static_cast<size_t> (numInputs) * static_cast<size_t> (maxFrames);
        return outputs + static_cast<size_t> (channel) * static_cast<size_t> (maxFrames);
    }

    const float* getOutputChannelData (uint32_t channel, uint32_t maxFrames, uint32_t numInputs, uint32_t numOutputs) const
    {
        return const_cast<AudioSharedMemoryLayout*> (this)->getOutputChannelData (channel, maxFrames, numInputs, numOutputs);
    }
};

} // namespace minixer
