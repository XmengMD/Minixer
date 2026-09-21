/*
  ==============================================================================

    PluginHostServer.cpp

  ==============================================================================
*/

#include "PluginHostServer.h"
#include "PluginWrapper.h"
#include "PluginScanner/VST3Scanner.h"
#include "PluginScanner/VST3PluginDescriptionMapper.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace minixer
{

namespace
{

//==============================================================================
/** 32 位扫描：自主扫描器 SEH 桥。
    MSVC 禁止 __try 与 C++ 对象位于同一函数（C2712），采用“纯指针桥”模式。 */
#if JUCE_WINDOWS
struct CustomScanJob
{
    vst3scan::VST3Scanner*         scanner;
    const juce::File*              file;
    vst3scan::PluginModuleRecord*  out;
};

int runCustomScanJob (CustomScanJob* job)
{
    if (job == nullptr || job->scanner == nullptr || job->file == nullptr || job->out == nullptr)
        return 0;

    *job->out = job->scanner->scanModule (*job->file, nullptr);
    return 1;
}

int customScanShim (CustomScanJob* job)
{
    __try
    {
        return runCustomScanJob (job);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
}
#endif

//==============================================================================
/** 将 PluginDescription 数组序列化为 XML 字符串。 */
juce::String serializeDescriptions (const juce::OwnedArray<juce::PluginDescription>& descriptions)
{
    juce::XmlElement root ("Plugins");

    for (auto* desc : descriptions)
    {
        if (desc != nullptr)
        {
            auto xml = desc->createXml();
            if (xml != nullptr)
                root.addChildElement (xml.release());
        }
    }

    return root.toString();
}

//==============================================================================
/** 校验扫描得到的插件描述是否包含必要字段。

    VST3 SDK 的 PClassInfo2/PClassInfoW 中 version 字段只是“版本字符串”，
    并非强制非空；JUCE 的 PluginDescription 也允许 version 为空。
    因此校验时不应把空版本当成无效插件，否则会导致大量合法 VST3 在
    32-bit 桥接扫描时被误报为加载失败。
*/
bool isDescriptionValid (const juce::PluginDescription& desc)
{
    return desc.name.isNotEmpty()
        && desc.numInputChannels >= 0
        && desc.numOutputChannels >= 0;
}

} // anonymous namespace

//==============================================================================
PluginHostServer::PluginHostServer() = default;

PluginHostServer::~PluginHostServer() = default;

//==============================================================================
bool PluginHostServer::connect (const juce::String& key,
                                const juce::String& path,
                                const juce::String& pluginDescriptionXmlB64,
                                uint32_t maxFrames,
                                uint32_t numInputs,
                                uint32_t numOutputs)
{
    ipcKey = key;
    pluginPath = path;

    if (pluginDescriptionXmlB64.isNotEmpty())
    {
        juce::MemoryOutputStream decoded;
        if (juce::Base64::convertFromBase64 (decoded, pluginDescriptionXmlB64))
        {
            auto xmlString = decoded.getMemoryBlock().toString();
            auto xml = juce::XmlDocument::parse (xmlString);

            if (xml != nullptr)
                pluginDescription.loadFromXml (*xml);
        }
    }

    transport = createDefaultIpcTransport();

    if (transport == nullptr || ! transport->accept (ipcKey))
    {
        juce::Logger::writeToLog ("PluginHost failed to accept IPC connection");
        return false;
    }

    sharedMemory = createDefaultSharedMemoryRegion();

    if (sharedMemory == nullptr)
        return false;

    const size_t audioShmSize = AudioSharedMemoryLayout::getTotalSize (maxFrames,
                                                                       numInputs,
                                                                       numOutputs);

    if (! sharedMemory->open (ipcKey, audioShmSize))
    {
        juce::Logger::writeToLog ("PluginHost failed to open shared memory");
        return false;
    }

    audioLayout = static_cast<AudioSharedMemoryLayout*> (sharedMemory->getAddress());

    if (audioLayout == nullptr)
        return false;

    // 共享内存头部由主进程写入，子进程读取后使用同一布局。
    maxFramesPerBlock  = audioLayout->maxFramesPerBlock;
    numInputChannels   = audioLayout->numInputChannels;
    numOutputChannels  = audioLayout->numOutputChannels;

    if (maxFramesPerBlock == 0)
        maxFramesPerBlock = maxFrames;

    if (numInputChannels == 0)
        numInputChannels = numInputs;

    if (numOutputChannels == 0)
        numOutputChannels = numOutputs;

    return true;
}

//==============================================================================
int PluginHostServer::runScanMode()
{
    // 与主程序一致：使用自主 VST3 扫描器（先 setHostContext 再按完整 128 位
    // CID 枚举），32 位外壳插件同样不再受“索引位移”影响。IPC 格式不变，
    // 主程序 scanPluginViaHost 解析逻辑无需改动。
    //
    // 故意不析构 scanner（同 ScannerTest --worker 的设计）：结果经 IPC 发送后
    // 进程立刻退出，由操作系统回收模块 —— 避免个别插件在 ExitDll()/FreeLibrary
    // 阶段崩溃导致结果丢失或子进程异常退出。
    auto* scanner = new vst3scan::VST3Scanner ([] (const juce::String& line)
    {
        juce::Logger::writeToLog ("[VST3Scan] " + line);
    });

    vst3scan::PluginModuleRecord rec;

   #if JUCE_WINDOWS
    const juce::File file (pluginPath);
    CustomScanJob job { scanner, &file, &rec };

    if (customScanShim (&job) == 0)
    {
        sendScanError ("Plugin scan raised a structured exception");
        return 1;
    }
   #else
    rec = scanner->scanModule (juce::File (pluginPath), nullptr);
   #endif

    if (! rec.loaded)
    {
        sendScanError (rec.loadError.isNotEmpty() ? rec.loadError
                                                  : TRANS ("Failed to load module"));
        return 1;
    }

    juce::OwnedArray<juce::PluginDescription> descriptions;
    vst3scan::addDescriptionsFromModule (rec, descriptions);

    // 过滤无效描述
    for (int i = descriptions.size(); --i >= 0;)
    {
        if (descriptions[i] == nullptr || ! isDescriptionValid (*descriptions[i]))
            descriptions.remove (i);
    }

    if (! sendScanResult (descriptions))
        return 1;

    return 0;
}

//==============================================================================
int PluginHostServer::runRuntimeMode()
{
    if (! loadPlugin())
        return 1;

    while (handleControlLoop())
    {
    }

    wrapper.reset();
    return 0;
}

//==============================================================================
bool PluginHostServer::loadPlugin()
{
    wrapper = std::make_shared<PluginWrapper>();

    // 编辑器窗口被用户关闭（原生 X）时，在消息线程销毁编辑器和窗口，
    // 释放插件 UI 占用的 GPU 资源。用弱引用避免形成 shared_ptr 循环。
    std::weak_ptr<PluginWrapper> weakWrapper (wrapper);

    wrapper->setEditorCloseRequestHandler ([weakWrapper]()
    {
        juce::MessageManager::callAsync ([weakWrapper]()
        {
            if (auto activeWrapper = weakWrapper.lock())
                activeWrapper->hideEditor();
        });
    });

    juce::String error;
    if (! wrapper->loadFromDescription (pluginDescription, 48000.0,
                                        static_cast<int> (maxFramesPerBlock), error))
    {
        sendError (error);
        wrapper.reset();
        return false;
    }

    return true;
}

//==============================================================================
bool PluginHostServer::sendScanResult (const juce::OwnedArray<juce::PluginDescription>& descriptions)
{
    MessageBuilder builder;
    builder.writeString (serializeDescriptions (descriptions));
    auto payload = builder.buildWithHeader (ControlMessageType::ScanResult);
    return transport != nullptr && transport->sendMessage (payload);
}

//==============================================================================
bool PluginHostServer::sendScanError (const juce::String& message)
{
    MessageBuilder builder;
    builder.writeString (message);
    auto payload = builder.buildWithHeader (ControlMessageType::ScanError);
    return transport != nullptr && transport->sendMessage (payload);
}

//==============================================================================
void PluginHostServer::runOnMessageThreadAndWait (std::function<void()> task)
{
    if (juce::MessageManager::getInstance()->isThisTheMessageThread())
    {
        task();
        return;
    }

    // 转投消息线程并阻塞等待完成。callAsync 排队执行，消息线程运行完毕后
    // signal；本线程 wait() 返回前控制循环不会读取下一条消息，保证
    // ProcessBlock 等后续命令严格排在本次插件操作之后处理。
    juce::WaitableEvent completion;
    juce::MessageManager::callAsync ([&completion, task]()
    {
        task();
        completion.signal();
    });

    completion.wait();
}

//==============================================================================
bool PluginHostServer::handleControlLoop()
{
    if (transport == nullptr || ! transport->isConnected())
        return false;

    juce::MemoryBlock frame;

    if (! transport->readMessage (frame, -1))
        return false;

    if (frame.getSize() < ControlHeader::size)
        return true;

    ControlHeader header;
    std::memcpy (&header, frame.getData(), ControlHeader::size);

    if (! header.isValid())
    {
        sendError ("Invalid control header");
        return true;
    }

    MessageReader reader (static_cast<const uint8_t*> (frame.getData()) + ControlHeader::size,
                          frame.getSize() - ControlHeader::size);

    const auto type = static_cast<ControlMessageType> (header.type);

    switch (type)
    {
        case ControlMessageType::Init:
        {
            uint32_t sampleRateInt = 0;
            uint32_t bufferSize = 0;
            uint32_t requestedInputs = 0;
            uint32_t requestedOutputs = 0;

            reader.readUInt32 (sampleRateInt);
            reader.readUInt32 (bufferSize);
            reader.readUInt32 (requestedInputs);
            reader.readUInt32 (requestedOutputs);

            bool success = false;
            juce::String error;

            if (wrapper != nullptr)
            {
                // 布局设置失败不回退：插件保留默认布局继续运行（processBlock
                // 已按插件实际输入/输出通道适配缓冲），Init 只需 wrapper 存在。
                // 这样个别不支持请求布局的插件（如仅 mono 的外壳子插件）仍能
                // 正常出声，而不会导致整条链路由 Init 失败而静音。
                //
                // 插件准备必须在消息线程执行（VST3 规范 + JUCE 包装层强制），
                // 且与编辑器窗口的创建/绘制串行；本调用会阻塞控制线程直到
                // 消息线程完成，之后才发送 InitResult。
                auto wrapperRef = wrapper;
                const uint32_t maxFrames = maxFramesPerBlock;

                runOnMessageThreadAndWait ([wrapperRef, requestedInputs, requestedOutputs,
                                            sampleRateInt, maxFrames]()
                {
                    if (wrapperRef == nullptr)
                        return;

                    juce::String layoutError;

                    if (! wrapperRef->setChannelLayout (requestedInputs, requestedOutputs, layoutError))
                        juce::Logger::writeToLog ("Init: setChannelLayout failed, using plugin default layout: " + layoutError);

                    wrapperRef->prepareToPlay (static_cast<double> (sampleRateInt),
                                               static_cast<int> (maxFrames));
                });

                success = true;
            }
            else
            {
                error = "Plugin not loaded";
            }

            MessageBuilder response;
            response.writeBool (success);
            response.writeString (error);
            sendResponse (ControlMessageType::InitResult, response.getData(), header.requestId);
            break;
        }

        case ControlMessageType::PrepareToPlay:
        {
            uint32_t sampleRateInt = 0;
            uint32_t bufferSize = 0;
            reader.readUInt32 (sampleRateInt);
            reader.readUInt32 (bufferSize);

            // 与 Init 一致：按共享缓冲硬上限 maxFramesPerBlock 准备插件。
            // 设备 Buffer Size 每次变更不一定都会先发来 PrepareToPlay（例如在
            // ASIO 内置 Control Panel 内改动），保持插件内部缓冲始终 ≥ 上限，
            // 任何 ≤ 上限的块都不会越界。
            //
            // 插件准备在消息线程执行：既符合 VST3 规范，也与此前已打开的
            // 编辑器窗口串行，避免重配置破坏插件 GUI 状态。
            auto wrapperRef = wrapper;
            const uint32_t maxFrames = maxFramesPerBlock;

            runOnMessageThreadAndWait ([wrapperRef, sampleRateInt, maxFrames]()
            {
                if (wrapperRef != nullptr)
                    wrapperRef->prepareToPlay (static_cast<double> (sampleRateInt),
                                               static_cast<int> (maxFrames));
            });
            break;
        }

        case ControlMessageType::ReleaseResources:
        {
            auto wrapperRef = wrapper;

            runOnMessageThreadAndWait ([wrapperRef]()
            {
                if (wrapperRef != nullptr)
                    wrapperRef->releaseResources();
            });
            break;
        }

        case ControlMessageType::ProcessBlock:
        {
            if (wrapper == nullptr || audioLayout == nullptr)
                break;

            uint32_t requestedSamples = 0;
            if (! reader.readUInt32 (requestedSamples))
                break;

            const uint32_t numSamples = juce::jmin (requestedSamples, maxFramesPerBlock);

            const uint32_t hostWrite = audioLayout->hostWriteSeq.load (std::memory_order_acquire);
            const uint32_t pluginRead = audioLayout->pluginReadSeq.load (std::memory_order_relaxed);

            if (hostWrite == pluginRead)
                break; // 无新数据

            std::vector<const float*> inputPointers (numInputChannels);
            std::vector<float*>       outputPointers (numOutputChannels);

            for (uint32_t ch = 0; ch < numInputChannels; ++ch)
                inputPointers[ch] = audioLayout->getInputChannelData (ch, maxFramesPerBlock,
                                                                       numInputChannels, numOutputChannels);

            for (uint32_t ch = 0; ch < numOutputChannels; ++ch)
                outputPointers[ch] = audioLayout->getOutputChannelData (ch, maxFramesPerBlock,
                                                                         numInputChannels, numOutputChannels);

            wrapper->processBlock (inputPointers.data(),  numInputChannels,
                                   outputPointers.data(), numOutputChannels,
                                   numSamples);

            audioLayout->pluginReadSeq.store (hostWrite, std::memory_order_release);
            audioLayout->pluginWriteSeq.store (hostWrite, std::memory_order_release);
            audioLayout->hostReadSeq.store (hostWrite, std::memory_order_release);
            break;
        }

        case ControlMessageType::SetState:
        {
            juce::MemoryBlock state;
            reader.readMemoryBlock (state);

            if (wrapper != nullptr)
                wrapper->setStateInformation (state);
            break;
        }

        case ControlMessageType::GetState:
        {
            juce::MemoryBlock state;

            if (wrapper != nullptr)
                wrapper->getStateInformation (state);

            MessageBuilder response;
            response.writeMemoryBlock (state);
            sendResponse (ControlMessageType::StateData, response.getData(), header.requestId);
            break;
        }

        case ControlMessageType::SetParameter:
        {
            uint32_t index = 0;
            float value = 0.0f;
            reader.readUInt32 (index);
            reader.readFloat (value);

            if (wrapper != nullptr)
                wrapper->setParameter (static_cast<int> (index), value);
            break;
        }

        case ControlMessageType::GetLatency:
        {
            MessageBuilder response;
            response.writeUInt32 (wrapper != nullptr ? static_cast<uint32_t> (wrapper->getLatencySamples()) : 0u);
            sendResponse (ControlMessageType::LatencyInfo, response.getData(), header.requestId);
            break;
        }

        case ControlMessageType::ShowEditor:
        {
            uint64_t handleValue = 0;
            reader.readUInt64 (handleValue);

            if (wrapper == nullptr)
                break;

            // 编辑器窗口创建/销毁必须在消息线程进行（JUCE UI 规范）。
            // 控制循环运行在后台线程，这里经 callAsync 转投消息线程执行，
            // 并持有 wrapper 的 shared_ptr 保证生命周期安全。
            auto wrapperRef = wrapper;

            // 窗口标题：只显示插件自身名称（不含进程名/IPC key 等内部信息）。
            const auto title = wrapper->getName();

            juce::MessageManager::callAsync ([wrapperRef, title, handleValue]() mutable
            {
                if (wrapperRef == nullptr)
                    return;

                wrapperRef->showEditor (title,
                                        reinterpret_cast<void*> (static_cast<uintptr_t> (handleValue)));
            });
            break;
        }

        case ControlMessageType::HideEditor:
        {
            auto wrapperRef = wrapper;

            if (wrapperRef == nullptr)
                break;

            juce::MessageManager::callAsync ([wrapperRef]() mutable
            {
                if (wrapperRef != nullptr)
                    wrapperRef->hideEditor();
            });
            break;
        }

        case ControlMessageType::Shutdown:
        {
            wrapper.reset();
            return false;
        }

        default:
            sendError ("Unknown control message type");
            break;
    }

    return true;
}

//==============================================================================
bool PluginHostServer::sendResponse (ControlMessageType type, const juce::MemoryBlock& payload, uint64_t requestId)
{
    if (transport == nullptr)
        return false;

    ControlHeader header;
    header.type = static_cast<uint32_t> (type);
    header.payloadSize = static_cast<uint32_t> (juce::jmin<size_t> (payload.getSize(), (std::numeric_limits<uint32_t>::max)()));
    header.requestId = requestId;

    juce::MemoryBlock frame;
    frame.append (&header, ControlHeader::size);
    frame.append (payload.getData(), payload.getSize());
    return transport->sendMessage (frame);
}

//==============================================================================
bool PluginHostServer::sendLog (const juce::String& message)
{
    MessageBuilder builder;
    builder.writeUInt32 (0); // severity info
    builder.writeString (message);
    return sendResponse (ControlMessageType::LogMessage, builder.getData(), 0);
}

//==============================================================================
bool PluginHostServer::sendError (const juce::String& message)
{
    MessageBuilder builder;
    builder.writeUInt32 (1); // error code
    builder.writeString (message);
    return sendResponse (ControlMessageType::Error, builder.getData(), 0);
}

} // namespace minixer
