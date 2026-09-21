/*
  ==============================================================================

    PluginWrapper.cpp

  ==============================================================================
*/

#include "PluginWrapper.h"

#include "PluginScanner/VST3Scanner.h"
#include "LookAndFeel/MixerLookAndFeel.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace minixer
{

//==============================================================================
/** 插件编辑器所在的浮动窗口。

    使用 Windows 原生标题栏（与系统/其他窗口一致）。关闭按钮触发宿主
    注入的 closeRequestedHandler：由宿主在消息线程销毁编辑器和窗口，
    以释放插件 UI 占用的 GPU 资源。
*/
class PluginEditorWindow final : public juce::DocumentWindow
{
public:
    using juce::DocumentWindow::DocumentWindow;

    /** 宿主注入的“关闭请求”处理器（在消息线程执行销毁）。 */
    std::function<void()> closeRequestedHandler;

    void closeButtonPressed() override
    {
        setVisible (false);

        if (closeRequestedHandler != nullptr)
            closeRequestedHandler();
    }
};

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

//==============================================================================
PluginWrapper::PluginWrapper() = default;

PluginWrapper::~PluginWrapper()
{
    closeEditor();
    releaseResources();
    plugin.reset();
}

//==============================================================================
bool PluginWrapper::loadFromDescription (const juce::PluginDescription& description,
                                       double sampleRate,
                                       int bufferSize,
                                       juce::String& error)
{
    juce::AudioPluginFormatManager formatManager;
    formatManager.addDefaultFormats();

    juce::AudioPluginFormat* vst3Format = nullptr;

    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        auto* format = formatManager.getFormat (i);
        if (format != nullptr && format->getName() == juce::VST3PluginFormat::getFormatName())
        {
            vst3Format = format;
            break;
        }
    }

    if (vst3Format == nullptr)
    {
        error = "VST3 format not available";
        return false;
    }

    // —— 加载侧修复 ——
    // JUCE 的 VST3PluginFormat 在 createPluginInstance 内部会先按描述解析类索引、
    // 之后才调用 IPluginFactory3::setHostContext。外壳插件（WaveShell / IKM 等）
    // 在设置宿主上下文后会重排/追加工厂类表，导致此前解析的索引失效而“点 A 出 B”。
    // 方案：先用自有宿主上下文预加载（prewarmForLoad）完成至少一次展开；随后
    // 创建后再按插件自报名称校验，不符则重建一次（重建时类表已进入上一轮
    // createFrom（JUCE 宿主）设置后的稳定排序，第二次解析到的索引与最终表一致）。
    if (description.pluginFormatName == "VST3" && description.fileOrIdentifier.isNotEmpty())
    {
        juce::String prewarmError;
        minixer::vst3scan::VST3Scanner::prewarmForLoad (juce::File (description.fileOrIdentifier),
                                                        &prewarmError);

        if (prewarmError.isNotEmpty())
            juce::Logger::writeToLog ("VST3 preload skipped for " + description.fileOrIdentifier + ": " + prewarmError);
    }

    juce::String instanceError;
    plugin = vst3Format->createInstanceFromDescription (description,
                                                         sampleRate,
                                                         bufferSize,
                                                         instanceError);

    // 外壳插件防护：JUCE 按“先解析索引、后 setHostContext”创建，外壳在每次
    // 宿主上下文设置后可能重排类表，导致按索引读出的类与实际不符（点 A 出 B）。
    // 这里以插件自报名称做最终校验；不符则销毁重建一次（重建时类表已处于
    // 上一轮 createFrom 设置后的稳定排序，此时解析到的索引即与最终表一致）。
    if (plugin != nullptr
        && plugin->getName() != description.name
        && plugin->getName().isNotEmpty())
    {
        juce::Logger::writeToLog ("VST3 shell mismatch: requested=" + description.name
                                 + " got=" + plugin->getName() + ", retrying once");
        plugin.reset();

        instanceError.clear();
        plugin = vst3Format->createInstanceFromDescription (description,
                                                            sampleRate,
                                                            bufferSize,
                                                            instanceError);

        if (plugin != nullptr)
            juce::Logger::writeToLog ("VST3 shell retry: requested=" + description.name
                                     + " got=" + plugin->getName());
    }

    if (plugin == nullptr)
    {
        error = "Failed to create plugin instance: " + instanceError;
        return false;
    }

    plugin->enableAllBuses();

    juce::String layoutError;
    if (! setChannelLayout (currentInputChannels, currentOutputChannels, layoutError))
    {
        // 保持默认总线布局，仍允许插件运行
        juce::Logger::writeToLog ("PluginWrapper: setChannelLayout failed, falling back to default layout: " + layoutError);
    }

    prepareToPlay (sampleRate, bufferSize);
    return true;
}

//==============================================================================
juce::String PluginWrapper::getName() const
{
    return plugin != nullptr ? plugin->getName() : juce::String();
}

//==============================================================================
bool PluginWrapper::setChannelLayout (uint32_t numInputs, uint32_t numOutputs, juce::String& error)
{
    if (plugin == nullptr)
    {
        error = "Plugin not loaded";
        return false;
    }

    juce::AudioProcessor::BusesLayout layout;
    const auto mainInputSet  = channelSetFromCount (numInputs);
    const auto mainOutputSet = channelSetFromCount (numOutputs);

    const int numInputBuses  = plugin->getBusCount (true);
    const int numOutputBuses = plugin->getBusCount (false);

    for (int i = 0; i < numInputBuses; ++i)
        layout.inputBuses.add (i == 0 ? mainInputSet : juce::AudioChannelSet::disabled());

    for (int i = 0; i < numOutputBuses; ++i)
        layout.outputBuses.add (i == 0 ? mainOutputSet : juce::AudioChannelSet::disabled());

    if (! plugin->setBusesLayout (layout))
    {
        error = "Plugin does not support the requested channel layout";
        return false;
    }

    currentInputChannels  = numInputs;
    currentOutputChannels = numOutputs;
    return true;
}

//==============================================================================
void PluginWrapper::prepareToPlay (double sampleRate, int bufferSize)
{
    if (plugin != nullptr)
        plugin->prepareToPlay (sampleRate, bufferSize);
}

//==============================================================================
void PluginWrapper::releaseResources()
{
    if (plugin != nullptr)
        plugin->releaseResources();
}

//==============================================================================
void PluginWrapper::processBlock (const float* const* inputChannels,  uint32_t numInputChannels,
                                  float* const*       outputChannels, uint32_t numOutputChannels,
                                  uint32_t numSamples)
{
    if (plugin == nullptr || numSamples == 0)
        return;

    const int pluginInCh  = plugin->getTotalNumInputChannels();
    const int pluginOutCh = plugin->getTotalNumOutputChannels();

    // 插件没有可用总线（布局未建立）：把输出区域清零，避免把共享内存中的残留
    // 数据当作有效音频。
    if (pluginInCh <= 0 || pluginOutCh <= 0)
    {
        for (uint32_t ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputChannels[ch] != nullptr)
                juce::FloatVectorOperations::clear (outputChannels[ch],
                                                    static_cast<int> (numSamples));
        }

        return;
    }

    // 常规路径：插件输入/输出通道数都不超出宿主提供的缓冲（插件从同一个
    // 处理缓冲读出输入并写入输出，因此其输入通道也要落在缓冲范围内）。
    if (pluginInCh <= static_cast<int> (juce::jmin (numInputChannels, numOutputChannels))
        && pluginOutCh <= static_cast<int> (numOutputChannels))
    {
        // 注意：JUCE 的 AudioProcessor::processBlock 是“原位处理”——插件从
        // 传入缓冲的输入通道读出、向同一缓冲的输出通道写入。因此必须以输出
        // 区域为处理缓冲：先清零并把输入拷入（仅插件实际需要的输入通道），
        // 处理结果直接落在共享内存输出区，宿主从输出区取回。
        juce::AudioBuffer<float> outputBuffer (outputChannels,
                                               static_cast<int> (numOutputChannels),
                                               static_cast<int> (numSamples));
        outputBuffer.clear();

        // mono-in 插件：按主流 DAW 插入 mono 效果器的惯例，把立体声输入
        // 求合成单声道（L+R，0dB 不缩放，与 REAPER/Cakewalk 等一致）再送入
        // 插件，而不是只取 L 声道、静默丢弃 R 声道内容。
        if (pluginInCh == 1
            && numInputChannels >= 2
            && numOutputChannels >= 1
            && inputChannels[0] != nullptr
            && inputChannels[1] != nullptr)
        {
            const float* left  = inputChannels[0];
            const float* right = inputChannels[1];
            auto* mono = outputBuffer.getWritePointer (0);

            for (uint32_t i = 0; i < numSamples; ++i)
                mono[i] = left[i] + right[i];
        }
        else
        {
            const uint32_t minSeed = juce::jmin (static_cast<uint32_t> (pluginInCh),
                                                 numInputChannels, numOutputChannels);

            for (uint32_t ch = 0; ch < minSeed; ++ch)
            {
                if (inputChannels[ch] != nullptr)
                    outputBuffer.copyFrom (static_cast<int> (ch), 0,
                                           inputChannels[ch],
                                           static_cast<int> (numSamples));
            }
        }

        juce::MidiBuffer midi;
        plugin->processBlock (outputBuffer, midi);

        // mono-out 插件：结果只写入其声明的输出通道（通道 0）。为确保最终
        // 输出同时具有 L/R 两路，把处理结果复制到第二个输出通道，由宿主补全。
        // stereo-out 插件（pluginOutCh == 2）会自己写满两路，无需处理。
        if (pluginOutCh == 1 && numOutputChannels >= 2)
        {
            jassert (outputBuffer.getNumChannels() >= 2);
            outputBuffer.copyFrom (1, 0, outputBuffer.getReadPointer (0),
                                   static_cast<int> (numSamples));
        }

        return;
    }

    // 回退路径：插件要求的通道数超过宿主缓冲（如默认布局含多余总线）。
    // 用内部缓冲垫足插件通道，输入不足的通道补零，输出按宿主可容纳的
    // 通道数回拷。
    const uint32_t inCh  = static_cast<uint32_t> (juce::jmax (pluginInCh, static_cast<int> (numInputChannels)));
    const uint32_t outCh = static_cast<uint32_t> (juce::jmax (pluginOutCh, static_cast<int> (numOutputChannels)));

    ensureTempBuffer (static_cast<int> (juce::jmax (inCh, outCh)),
                      static_cast<int> (numSamples));

    tempBuffer.clear (0, static_cast<int> (numSamples));

    for (uint32_t ch = 0; ch < numInputChannels && ch < inCh; ++ch)
    {
        if (inputChannels[ch] != nullptr)
            tempBuffer.copyFrom (static_cast<int> (ch), 0,
                                 inputChannels[ch],
                                 static_cast<int> (numSamples));
    }

    juce::MidiBuffer midi;
    plugin->processBlock (tempBuffer, midi);

    for (uint32_t ch = 0; ch < numOutputChannels && ch < outCh; ++ch)
    {
        if (outputChannels[ch] != nullptr)
            juce::FloatVectorOperations::copy (outputChannels[ch],
                                               tempBuffer.getReadPointer (static_cast<int> (ch)),
                                               static_cast<int> (numSamples));
    }
}

//==============================================================================
void PluginWrapper::ensureTempBuffer (int numChannels, int numSamples)
{
    if (tempBuffer.getNumChannels() < numChannels || tempBuffer.getNumSamples() < numSamples)
        tempBuffer.setSize (numChannels, numSamples, false, false, true);
}

//==============================================================================
void PluginWrapper::getStateInformation (juce::MemoryBlock& destData)
{
    if (plugin != nullptr)
        plugin->getStateInformation (destData);
}

//==============================================================================
void PluginWrapper::setStateInformation (const juce::MemoryBlock& stateData)
{
    if (plugin != nullptr && stateData.getSize() > 0)
        plugin->setStateInformation (stateData.getData(), static_cast<int> (stateData.getSize()));
}

//==============================================================================
void PluginWrapper::setParameter (int index, float value)
{
    if (plugin == nullptr)
        return;

    auto& params = plugin->getParameters();

    if (juce::isPositiveAndBelow (index, params.size()))
    {
        if (auto* param = params[index])
            param->setValue (value);
    }
}

//==============================================================================
int PluginWrapper::getLatencySamples() const
{
    return plugin != nullptr ? plugin->getLatencySamples() : 0;
}

//==============================================================================
bool PluginWrapper::hasEditor() const
{
    return plugin != nullptr && plugin->hasEditor();
}

//==============================================================================
void PluginWrapper::showEditor (const juce::String& windowTitle, void* /*parentWindowHandle*/)
{
    // 窗口在正常关闭时会被整体销毁，这里只需拦截重复的打开请求。
    if (editorWindow != nullptr || ! hasEditor())
        return;

    editor.reset (plugin->createEditorIfNeeded());

    if (editor == nullptr)
        return;

    // 与主程序 PluginEditorWindow 一致：深色背景 + Windows 原生标题栏
    // （原生标题栏与系统/其他窗口的默认样式一致）。
    editorWindow = std::make_unique<PluginEditorWindow> (windowTitle,
                                                         MixerLookAndFeel::getBackgroundColour(),
                                                         juce::DocumentWindow::closeButton);
    editorWindow->setUsingNativeTitleBar (true);

    // 关闭按钮（原生 X）：由宿主在消息线程销毁编辑器和窗口，
    // 释放插件 UI 占用的 GPU 资源。
    editorWindow->closeRequestedHandler = editorCloseRequested;

    // 与官方 AudioPluginHost 一致：先给窗口一个默认尺寸，setContentNonOwned 的
    // resizeToFitWhenContentChangesSize=true 会让窗口通过 childBoundsChanged 自动
    // 缩放到插件编辑器的实际尺寸（逻辑像素），DPI 由 JUCE per-monitor DPI v2
    // 自动换算，保证“尺寸正确、不模糊、不裁切”。
    editorWindow->setSize (400, 300);
    editorWindow->setContentNonOwned (editor.get(), true);
    editorWindow->setResizable (editor->isResizable(), false);

    // 以窗口当前的（跟随编辑器后的）尺寸居中
    editorWindow->centreWithSize (editorWindow->getWidth(), editorWindow->getHeight());
    editorWindow->setVisible (true);
}

//==============================================================================
void PluginWrapper::hideEditor()
{
    closeEditor();
}

//==============================================================================
void PluginWrapper::closeEditor()
{
    if (editorWindow != nullptr)
        editorWindow.reset();

    editor.reset();
}

} // namespace minixer
