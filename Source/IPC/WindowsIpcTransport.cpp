/*
  ==============================================================================

    WindowsIpcTransport.cpp

  ==============================================================================
*/

#include "WindowsIpcTransport.h"

#if JUCE_WINDOWS

namespace minixer
{

namespace
{

juce::String makePipeName (const juce::String& key)
{
    return "\\\\.\\pipe\\" + key;
}

} // anonymous namespace

//==============================================================================
WindowsIpcTransport::WindowsIpcTransport() = default;

WindowsIpcTransport::~WindowsIpcTransport()
{
    close();
}

//==============================================================================
bool WindowsIpcTransport::connect (const juce::String& key)
{
    close();

    const auto pipeNameStr = makePipeName (key);
    const auto* pipeName = pipeNameStr.toWideCharPointer();

    // 命名管道服务端可能稍后才创建，重试一段时间。
    for (int attempt = 0; attempt < 50; ++attempt)
    {
        pipe = CreateFileW (pipeName,
                            GENERIC_READ | GENERIC_WRITE,
                            0,
                            nullptr,
                            OPEN_EXISTING,
                            0,
                            nullptr);

        if (pipe != INVALID_HANDLE_VALUE)
            break;

        if (GetLastError() != ERROR_PIPE_BUSY)
        {
            Sleep (20);
            continue;
        }

        if (! WaitNamedPipeW (pipeName, 1000))
        {
            Sleep (20);
            continue;
        }
    }

    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    // 与服务端一致使用字节流读模式（协议自带长度前缀，字节流最稳健）
    DWORD mode = PIPE_READMODE_BYTE;
    if (! SetNamedPipeHandleState (pipe, &mode, nullptr, nullptr))
    {
        close();
        return false;
    }

    connected = true;
    return true;
}

//==============================================================================
bool WindowsIpcTransport::accept (const juce::String& key)
{
    close();

    const auto pipeNameStr = makePipeName (key);
    const auto* pipeName = pipeNameStr.toWideCharPointer();

    // 使用字节流模式（PIPE_TYPE_BYTE）而不是消息模式：
    // 协议本身已带长度前缀，字节流可完全消除“消息边界错位/合并”的隐患，
    // reads/writes 全部按字节循环累计，任意分片/合并均安全。
    pipe = CreateNamedPipeW (pipeName,
                             PIPE_ACCESS_DUPLEX,
                             PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                             1,
                             64 * 1024,
                             64 * 1024,
                             0,
                             nullptr);

    if (pipe == INVALID_HANDLE_VALUE)
        return false;

    if (! ConnectNamedPipe (pipe, nullptr))
    {
        if (GetLastError() != ERROR_PIPE_CONNECTED)
        {
            close();
            return false;
        }
    }

    connected = true;
    return true;
}

//==============================================================================
bool WindowsIpcTransport::sendMessage (const juce::MemoryBlock& data)
{
    if (! isConnected())
        return false;

    const uint32_t totalSize = static_cast<uint32_t> (juce::jmin<size_t> (data.getSize(), (std::numeric_limits<uint32_t>::max)()));

    // 长度头 + 负载合并为一次 WriteFile 写入。字节流模式下边界无意义，
    // 但对端按“先读 4 字节长度、再读负载”的协议取帧，单次写入可减少系统调用
    // 并避免协议头与负载之间被其它线程的命令穿插。
    juce::MemoryBlock frame;
    frame.append (&totalSize, sizeof (totalSize));

    if (totalSize > 0)
        frame.append (data.getData(), totalSize);

    return sendExact (frame.getData(), static_cast<DWORD> (frame.getSize()));
}

//==============================================================================
bool WindowsIpcTransport::readMessage (juce::MemoryBlock& data, int timeoutMs)
{
    if (! isConnected())
        return false;

    uint32_t totalSize = 0;
    if (! readExact (&totalSize, sizeof (totalSize), timeoutMs))
        return false;

    if (totalSize == 0)
    {
        data.reset();
        return true;
    }

    if (totalSize > 64 * 1024 * 1024) // 64MB 上限，防止畸形消息
        return false;

    data.setSize (totalSize, false);
    return readExact (data.getData(), totalSize, timeoutMs);
}

//==============================================================================
void WindowsIpcTransport::close()
{
    if (pipe != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers (pipe);
        DisconnectNamedPipe (pipe);
        CloseHandle (pipe);
        pipe = INVALID_HANDLE_VALUE;
    }

    connected = false;
}

//==============================================================================
bool WindowsIpcTransport::isConnected() const
{
    return connected && pipe != INVALID_HANDLE_VALUE;
}

//==============================================================================
bool WindowsIpcTransport::readExact (void* buffer, DWORD bytesToRead, int timeoutMs)
{
    auto* dest = static_cast<uint8_t*> (buffer);
    DWORD totalRead = 0;
    const auto deadline = juce::Time::getCurrentTime()
                        + juce::RelativeTime::milliseconds (juce::jmax (0, timeoutMs));

    while (totalRead < bytesToRead)
    {
        if (timeoutMs > 0)
        {
            DWORD available = 0;
            DWORD left = bytesToRead - totalRead;

            if (! PeekNamedPipe (pipe, nullptr, 0, nullptr, &available, nullptr))
                return false;

            if (available < left)
            {
                if (juce::Time::getCurrentTime() >= deadline)
                    return false;

                Sleep (1);
                continue;
            }
        }

        DWORD bytesRead = 0;
        if (! ReadFile (pipe, dest + totalRead, bytesToRead - totalRead, &bytesRead, nullptr))
            return false;

        if (bytesRead == 0)
            return false;

        totalRead += bytesRead;
    }

    return true;
}

//==============================================================================
bool WindowsIpcTransport::sendExact (const void* buffer, DWORD bytesToSend)
{
    const auto* src = static_cast<const uint8_t*> (buffer);
    DWORD totalWritten = 0;

    while (totalWritten < bytesToSend)
    {
        DWORD bytesWritten = 0;
        if (! WriteFile (pipe, src + totalWritten, bytesToSend - totalWritten, &bytesWritten, nullptr))
            return false;

        if (bytesWritten == 0)
            return false;

        totalWritten += bytesWritten;
    }

    return true;
}

//==============================================================================
std::unique_ptr<IpcTransport> createDefaultIpcTransport()
{
    return std::make_unique<WindowsIpcTransport>();
}

} // namespace minixer

#else

namespace minixer
{
std::unique_ptr<IpcTransport> createDefaultIpcTransport() { return {}; }
} // namespace minixer

#endif
