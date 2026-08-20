/*
  ==============================================================================

    PluginHostServer.h
    PluginHost 子进程中的 IPC 服务端。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "../IPC/IpcTransport.h"
#include "../IPC/SharedMemoryRegion.h"
#include "../IPC/IpcProtocol.h"
#include "PluginWrapper.h"

namespace minixer
{

//==============================================================================
/** PluginHost 子进程服务端。

    负责：
    - 作为命名管道服务端等待主进程连接。
    - 接收控制命令并调用 PluginWrapper 执行对应操作。
    - 扫描模式下枚举插件描述并通过控制通道返回。
*/
class PluginHostServer
{
public:
    //==============================================================================
    PluginHostServer();
    ~PluginHostServer();

    //==============================================================================
    /** 连接控制通道与共享内存。 */
    bool connect (const juce::String& ipcKey,
                  const juce::String& pluginPath,
                  const juce::String& pluginDescriptionXmlB64,
                  uint32_t maxFrames,
                  uint32_t numInputs,
                  uint32_t numOutputs);

    /** 运行扫描模式：加载插件、枚举描述、发送结果后返回。 */
    int runScanMode();

    /** 运行运行期模式：处理控制循环直到收到 Shutdown 或连接断开。 */
    int runRuntimeMode();

private:
    //==============================================================================
    bool loadPlugin();
    bool sendScanResult (const juce::OwnedArray<juce::PluginDescription>& descriptions);
    bool sendScanError (const juce::String& message);

    bool handleControlLoop();
    bool sendResponse (ControlMessageType type, const juce::MemoryBlock& payload, uint64_t requestId);
    bool sendLog (const juce::String& message);
    bool sendError (const juce::String& message);

    //==============================================================================
    juce::String ipcKey;
    juce::String pluginPath;
    juce::PluginDescription pluginDescription;
    uint32_t maxFramesPerBlock = 512;
    uint32_t numInputChannels = 2;
    uint32_t numOutputChannels = 2;

    std::unique_ptr<IpcTransport> transport;
    std::unique_ptr<SharedMemoryRegion> sharedMemory;
    AudioSharedMemoryLayout* audioLayout = nullptr;

    std::unique_ptr<PluginWrapper> wrapper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHostServer)
};

} // namespace minixer
