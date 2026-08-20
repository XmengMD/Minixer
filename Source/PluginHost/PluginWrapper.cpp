/*
  ==============================================================================

    PluginWrapper.cpp

  ==============================================================================
*/

#include "PluginWrapper.h"

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace minixer
{

namespace
{

//==============================================================================
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

    juce::String instanceError;
    plugin = vst3Format->createInstanceFromDescription (description,
                                                         sampleRate,
                                                         bufferSize,
                                                         instanceError);

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

    juce::AudioBuffer<float> inputBuffer (const_cast<float**> (inputChannels),
                                          static_cast<int> (numInputChannels),
                                          static_cast<int> (numSamples));

    juce::AudioBuffer<float> outputBuffer (outputChannels,
                                           static_cast<int> (numOutputChannels),
                                           static_cast<int> (numSamples));
    outputBuffer.clear();

    juce::MidiBuffer midi;
    plugin->processBlock (inputBuffer, midi);
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
    if (! hasEditor() || editorWindow != nullptr)
        return;

    editor.reset (plugin->createEditorIfNeeded());

    if (editor == nullptr)
        return;

    editorWindow = std::make_unique<juce::DocumentWindow> (windowTitle,
                                                           juce::LookAndFeel::getDefaultLookAndFeel()
                                                               .findColour (juce::ResizableWindow::backgroundColourId),
                                                           juce::DocumentWindow::closeButton);

    editorWindow->setContentNonOwned (editor.get(), true);
    editorWindow->centreWithSize (editor->getWidth(), editor->getHeight());
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
