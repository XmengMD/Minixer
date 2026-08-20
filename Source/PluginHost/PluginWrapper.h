/*
  ==============================================================================

    PluginWrapper.h
    PluginHost 子进程中的 VST3 插件封装。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace minixer
{

//==============================================================================
/** 封装单个 VST3 插件实例，隐藏 JUCE AudioPluginInstance 的细节。

    负责加载、总线配置、音频处理、参数/状态读写以及浮动编辑器窗口。
    PluginHostServer 通过本类调用插件，保持控制逻辑与插件操作分离。
*/
class PluginWrapper
{
public:
    //==============================================================================
    PluginWrapper();
    ~PluginWrapper();

    //==============================================================================
    /** 从完整的 PluginDescription 加载插件并创建实例。

        对 Waves 等 shell 插件（一个 .vst3 文件内包含多个子插件）必须传入
        包含 uid 的完整描述，否则只会加载该文件里的第一个插件。
    */
    bool loadFromDescription (const juce::PluginDescription& description,
                              double sampleRate,
                              int bufferSize,
                              juce::String& error);

    /** 返回是否已成功加载插件。 */
    bool isLoaded() const noexcept { return plugin != nullptr; }

    /** 返回插件名称。 */
    juce::String getName() const;

    //==============================================================================
    /** 设置插件主输入/输出总线通道数。 */
    bool setChannelLayout (uint32_t numInputs, uint32_t numOutputs, juce::String& error);

    /** 准备播放。 */
    void prepareToPlay (double sampleRate, int bufferSize);

    /** 释放资源。 */
    void releaseResources();

    //==============================================================================
    /** 处理一帧音频。

        inputChannels / outputChannels 为非交错 float 指针数组，分别包含
        numInputChannels / numOutputChannels 个通道。
    */
    void processBlock (const float* const* inputChannels,  uint32_t numInputChannels,
                       float* const*       outputChannels, uint32_t numOutputChannels,
                       uint32_t numSamples);

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData);
    void setStateInformation (const juce::MemoryBlock& stateData);

    void setParameter (int index, float value);
    int  getLatencySamples() const;

    //==============================================================================
    bool hasEditor() const;
    void showEditor (const juce::String& windowTitle, void* parentWindowHandle);
    void hideEditor();
    bool isEditorOpen() const noexcept { return editorWindow != nullptr; }

private:
    //==============================================================================
    void closeEditor();

    //==============================================================================
    std::unique_ptr<juce::AudioPluginInstance> plugin;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    std::unique_ptr<juce::DocumentWindow> editorWindow;

    uint32_t currentInputChannels = 2;
    uint32_t currentOutputChannels = 2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginWrapper)
};

} // namespace minixer
