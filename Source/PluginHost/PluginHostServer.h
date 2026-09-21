/*
  ==============================================================================

    PluginHostServer.h
    PluginHost 子进程中的 IPC 服务端。

  ==============================================================================
*/

#pragma once

#include <functional>
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

    /** 在消息线程上运行任务并同步等待其完成。

        VST3 规范要求 IComponent::setupProcessing()/setActive() 等必须在消息
        线程调用（JUCE VST3 包装层同样强制，见 juce_VST3PluginFormat.cpp 的
        prepareToPlay 注释）。插件编辑器窗口也由消息线程驱动，因此把插件的
        准备/释放统一转投消息线程并等待完成：既符合规范，又与编辑器串行，
        避免重配置时插件 GUI 状态被并发线程破坏（表现为编辑器窗口消失）。

        等待发生在控制线程上：本函数返回前，控制循环不会继续读取下一条
        消息，从而保证后续 ProcessBlock 一定在本次准备完成后才被处理。
    */
    void runOnMessageThreadAndWait (std::function<void()> task);

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

    // 共享指针：编辑器窗口创建/销毁在消息线程完成（callAsync），
    // 需要在线程间安全持有 wrapper。
    std::shared_ptr<PluginWrapper> wrapper;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginHostServer)
};

} // namespace minixer
