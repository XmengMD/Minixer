/*
  ==============================================================================

    WindowsSharedMemoryRegion.cpp

  ==============================================================================
*/

#include "WindowsSharedMemoryRegion.h"

#if JUCE_WINDOWS

namespace minixer
{

namespace
{

juce::String makeMappingName (const juce::String& key)
{
    return "Local\\" + key + "_AudioShm";
}

} // anonymous namespace

//==============================================================================
WindowsSharedMemoryRegion::WindowsSharedMemoryRegion() = default;

WindowsSharedMemoryRegion::~WindowsSharedMemoryRegion()
{
    close();
}

//==============================================================================
bool WindowsSharedMemoryRegion::create (const juce::String& key, size_t size)
{
    return openInternal (key, size, FILE_MAP_ALL_ACCESS, true);
}

//==============================================================================
bool WindowsSharedMemoryRegion::open (const juce::String& key, size_t size)
{
    return openInternal (key, size, FILE_MAP_ALL_ACCESS, false);
}

//==============================================================================
void* WindowsSharedMemoryRegion::getAddress() const
{
    return address;
}

//==============================================================================
size_t WindowsSharedMemoryRegion::getSize() const
{
    return mappedSize;
}

//==============================================================================
void WindowsSharedMemoryRegion::close()
{
    if (address != nullptr)
    {
        UnmapViewOfFile (address);
        address = nullptr;
    }

    if (mapping != nullptr)
    {
        CloseHandle (mapping);
        mapping = nullptr;
    }

    mappedSize = 0;
}

//==============================================================================
bool WindowsSharedMemoryRegion::openInternal (const juce::String& key,
                                              size_t size,
                                              DWORD access,
                                              bool createNew)
{
    close();

    if (size == 0)
        return false;

    const auto nameStr = makeMappingName (key);
    const auto* name = nameStr.toWideCharPointer();
    const SIZE_T sz = static_cast<SIZE_T> (size);

    if (createNew)
    {
        mapping = CreateFileMappingW (INVALID_HANDLE_VALUE,
                                      nullptr,
                                      PAGE_READWRITE,
                                      0,
                                      static_cast<DWORD> (sz),
                                      name);
    }
    else
    {
        mapping = OpenFileMappingW (access, FALSE, name);
    }

    if (mapping == nullptr)
        return false;

    // 打开侧（子进程）映射整个区域，而不是按本进程推算的尺寸取视图：
    // 主进程按插件描述通道数（可能为 0/不准确）创建映射，尺寸由主进程决定，
    // 子进程只有在读到共享内存头部的 numInputChannels/numOutputChannels 后
    // 才知道真实的缓冲布局；映射整个区域可避免“视图尺寸与主进程映射不一致
    // → MapViewOfFile 失败或后续指针越界”。
    address = MapViewOfFile (mapping, access, 0, 0, createNew ? sz : 0);

    if (address == nullptr)
    {
        close();
        return false;
    }

    mappedSize = size;

    if (createNew)
        ZeroMemory (address, static_cast<SIZE_T> (size));

    return true;
}

//==============================================================================
std::unique_ptr<SharedMemoryRegion> createDefaultSharedMemoryRegion()
{
    return std::make_unique<WindowsSharedMemoryRegion>();
}

} // namespace minixer

#else

namespace minixer
{
std::unique_ptr<SharedMemoryRegion> createDefaultSharedMemoryRegion() { return {}; }
} // namespace minixer

#endif
