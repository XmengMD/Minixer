#include "AsioAdvancedSettingsComponent.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

//==============================================================================
AsioAdvancedSettingsComponent::AsioAdvancedSettingsComponent (juce::AudioDeviceManager& manager,
                                                              std::function<void()> onChannelsChangedCallback)
    : deviceManager (manager),
      onChannelsChanged (std::move (onChannelsChangedCallback))
{
    setSize (360, 400);

    setupLabel (deviceNameLabel, {});
    deviceNameLabel.setJustificationType (juce::Justification::centred);
    deviceNameLabel.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());

    setupSectionLabel (inputSectionLabel,  TRANS ("Input Channels"));
    setupLabel (inputModeLabel,           TRANS ("Mode"));
    setupComboBox (inputModeComboBox);
    inputModeComboBox.addItem (TRANS ("Mono"),   1);
    inputModeComboBox.addItem (TRANS ("Stereo"), 2);

    setupLabel (inputLeftLabel,  TRANS ("Input L"));
    setupLabel (inputRightLabel, TRANS ("Input R"));
    setupComboBox (inputLeftComboBox);
    setupComboBox (inputRightComboBox);

    setupSectionLabel (outputSectionLabel, TRANS ("Output Channels"));
    setupLabel (outputModeLabel,           TRANS ("Mode"));
    setupComboBox (outputModeComboBox);
    outputModeComboBox.addItem (TRANS ("Mono"),   1);
    outputModeComboBox.addItem (TRANS ("Stereo"), 2);

    setupLabel (outputLeftLabel,  TRANS ("Output L"));
    setupLabel (outputRightLabel, TRANS ("Output R"));
    setupComboBox (outputLeftComboBox);
    setupComboBox (outputRightComboBox);

    setupButton (okButton,     TRANS ("OK"));
    setupButton (cancelButton, TRANS ("Cancel"));
    setupButton (resetButton,  TRANS ("Reset"));

    refreshChannelLists();
    updateUIFromSetup();
}

//==============================================================================
AsioAdvancedSettingsComponent::~AsioAdvancedSettingsComponent() = default;

//==============================================================================
void AsioAdvancedSettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced (16, 16);
    const auto labelWidth = 80;
    const auto gap = 8;
    const auto rowHeight = 28;

    auto layoutRow = [&bounds, labelWidth, rowHeight, gap] (juce::Component& left, juce::Component& right)
    {
        auto row = bounds.removeFromTop (rowHeight);
        left.setBounds (row.removeFromLeft (labelWidth));
        row.removeFromLeft (gap);
        right.setBounds (row);
        bounds.removeFromTop (gap);
    };

    auto layoutFullRow = [&bounds, rowHeight, gap] (juce::Component& comp)
    {
        comp.setBounds (bounds.removeFromTop (rowHeight));
        bounds.removeFromTop (gap);
    };

    layoutFullRow (deviceNameLabel);
    layoutFullRow (inputSectionLabel);
    layoutRow (inputModeLabel,  inputModeComboBox);
    layoutRow (inputLeftLabel,  inputLeftComboBox);
    layoutRow (inputRightLabel, inputRightComboBox);
    layoutFullRow (outputSectionLabel);
    layoutRow (outputModeLabel,  outputModeComboBox);
    layoutRow (outputLeftLabel,  outputLeftComboBox);
    layoutRow (outputRightLabel, outputRightComboBox);

    // 按钮行：Reset 左对齐，OK / Cancel 右对齐
    {
        const auto okCancelWidth = 80;
        const auto resetWidth    = 70;
        auto row = bounds.removeFromTop (rowHeight);
        cancelButton.setBounds (row.removeFromRight (okCancelWidth));
        row.removeFromRight (gap);
        okButton.setBounds (row.removeFromRight (okCancelWidth));
        resetButton.setBounds (row.removeFromLeft (resetWidth));
    }
}

//==============================================================================
void AsioAdvancedSettingsComponent::buttonClicked (juce::Button* button)
{
    if (button == &okButton)
    {
        applyChannelSetup();

        if (onChannelsChanged != nullptr)
            onChannelsChanged();

        closeDialog();
    }
    else if (button == &cancelButton)
    {
        closeDialog();
    }
    else if (button == &resetButton)
    {
        resetUIToDefaults();
    }
}

//==============================================================================
void AsioAdvancedSettingsComponent::comboBoxChanged (juce::ComboBox* comboBox)
{
    if (updatingUI)
        return;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    if (comboBox == &inputModeComboBox)
    {
        handleModeChanged (inputModeComboBox,
                           inputLeftComboBox,
                           inputRightComboBox,
                           static_cast<int> (device->getInputChannelNames().size()));
    }
    else if (comboBox == &outputModeComboBox)
    {
        handleModeChanged (outputModeComboBox,
                           outputLeftComboBox,
                           outputRightComboBox,
                           static_cast<int> (device->getOutputChannelNames().size()));
    }
    else if (comboBox == &inputLeftComboBox || comboBox == &inputRightComboBox)
    {
        if (inputModeComboBox.getSelectedId() == 2)
            ensureStereoChannelsDistinct (inputLeftComboBox,
                                          inputRightComboBox,
                                          static_cast<int> (device->getInputChannelNames().size()));
    }
    else if (comboBox == &outputLeftComboBox || comboBox == &outputRightComboBox)
    {
        if (outputModeComboBox.getSelectedId() == 2)
            ensureStereoChannelsDistinct (outputLeftComboBox,
                                          outputRightComboBox,
                                          static_cast<int> (device->getOutputChannelNames().size()));
    }
}

//==============================================================================
void AsioAdvancedSettingsComponent::setupLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (12.0f)));
    label.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

//==============================================================================
void AsioAdvancedSettingsComponent::setupSectionLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (13.0f)).boldened());
    label.setColour (juce::Label::textColourId, MixerLookAndFeel::getAccentColour());
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

//==============================================================================
void AsioAdvancedSettingsComponent::setupComboBox (juce::ComboBox& comboBox)
{
    comboBox.setLookAndFeel (&getLookAndFeel());
    comboBox.addListener (this);
    addAndMakeVisible (comboBox);
}

//==============================================================================
void AsioAdvancedSettingsComponent::setupButton (juce::TextButton& button, const juce::String& text)
{
    button.setButtonText (text);
    button.setLookAndFeel (&getLookAndFeel());
    button.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    button.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    button.addListener (this);
    addAndMakeVisible (button);
}

//==============================================================================
void AsioAdvancedSettingsComponent::refreshChannelLists()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        deviceNameLabel.setText (TRANS ("No ASIO device is currently open."), juce::dontSendNotification);
        inputLeftComboBox.clear  (juce::dontSendNotification);
        inputRightComboBox.clear (juce::dontSendNotification);
        outputLeftComboBox.clear (juce::dontSendNotification);
        outputRightComboBox.clear(juce::dontSendNotification);
        return;
    }

    deviceNameLabel.setText (device->getName(), juce::dontSendNotification);

    auto populate = [] (juce::ComboBox& box, const juce::StringArray& names)
    {
        box.clear (juce::dontSendNotification);
        for (int i = 0; i < names.size(); ++i)
            box.addItem (names[i], i + 1);
    };

    populate (inputLeftComboBox,  device->getInputChannelNames());
    populate (inputRightComboBox, device->getInputChannelNames());
    populate (outputLeftComboBox, device->getOutputChannelNames());
    populate (outputRightComboBox, device->getOutputChannelNames());

    updateModeVisibility();
}

//==============================================================================
void AsioAdvancedSettingsComponent::updateUIFromSetup()
{
    updatingUI = true;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        updatingUI = false;
        return;
    }

    refreshChannelLists();

    auto setup = deviceManager.getAudioDeviceSetup();

    auto autoSelect = [] (const juce::BigInteger& channels, int maxChannels) -> std::pair<int, int>
    {
        if (maxChannels <= 0)
            return { -1, -1 };

        auto left  = getNthSetBit (channels, 0);
        auto right = getNthSetBit (channels, 1);

        // 没有任何位被设置时，默认选择前两个通道（或唯一通道）
        if (left < 0)
            left = 0;

        if (right < 0 && maxChannels > 1)
            right = juce::jmin (1, maxChannels - 1);

        // 右声道无效或单声道设备时禁用
        if (right >= maxChannels)
            right = -1;

        return { left, right };
    };

    auto inputChannelCount  = static_cast<int> (device->getInputChannelNames().size());
    auto outputChannelCount = static_cast<int> (device->getOutputChannelNames().size());

    auto [inL, inR]   = autoSelect (setup.inputChannels,  inputChannelCount);
    auto [outL, outR] = autoSelect (setup.outputChannels, outputChannelCount);

    // 根据当前实际启用的通道数推断 Mono/Stereo 模式。
    // 单声道设备或只启用 1 个通道时显示为 Mono 模式。
    bool inputStereo  = (setup.inputChannels.countNumberOfSetBits()  >= 2) && (inputChannelCount  > 1);
    bool outputStereo = (setup.outputChannels.countNumberOfSetBits() >= 2) && (outputChannelCount > 1);

    inputModeComboBox.setSelectedId  (inputStereo  ? 2 : 1, juce::dontSendNotification);
    outputModeComboBox.setSelectedId (outputStereo ? 2 : 1, juce::dontSendNotification);

    inputLeftComboBox.setSelectedId   (inL  >= 0 ? inL  + 1 : 0, juce::dontSendNotification);
    inputRightComboBox.setSelectedId  (inR  >= 0 ? inR  + 1 : 0, juce::dontSendNotification);
    outputLeftComboBox.setSelectedId  (outL >= 0 ? outL + 1 : 0, juce::dontSendNotification);
    outputRightComboBox.setSelectedId (outR >= 0 ? outR + 1 : 0, juce::dontSendNotification);

    updateModeVisibility();

    updatingUI = false;
}

//==============================================================================
void AsioAdvancedSettingsComponent::resetUIToDefaults()
{
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    auto inputChannelCount  = static_cast<int> (device->getInputChannelNames().size());
    auto outputChannelCount = static_cast<int> (device->getOutputChannelNames().size());

    // Reset 默认恢复到 Stereo（设备支持时），否则 Mono
    bool inputStereo  = inputChannelCount  > 1;
    bool outputStereo = outputChannelCount > 1;

    inputModeComboBox.setSelectedId  (inputStereo  ? 2 : 1, juce::dontSendNotification);
    outputModeComboBox.setSelectedId (outputStereo ? 2 : 1, juce::dontSendNotification);

    auto selectDefault = [] (juce::ComboBox& leftBox, juce::ComboBox& rightBox,
                             bool isStereo, int maxChannels)
    {
        if (maxChannels <= 0)
        {
            leftBox.setSelectedId  (0, juce::dontSendNotification);
            rightBox.setSelectedId (0, juce::dontSendNotification);
            return;
        }

        leftBox.setSelectedId (1, juce::dontSendNotification);

        if (isStereo && maxChannels > 1)
            rightBox.setSelectedId (2, juce::dontSendNotification);
        else
            rightBox.setSelectedId (0, juce::dontSendNotification);
    };

    selectDefault (inputLeftComboBox,  inputRightComboBox,  inputStereo,  inputChannelCount);
    selectDefault (outputLeftComboBox, outputRightComboBox, outputStereo, outputChannelCount);

    updateModeVisibility();
}

//==============================================================================
void AsioAdvancedSettingsComponent::applyChannelSetup()
{
    auto setup = deviceManager.getAudioDeviceSetup();
    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    auto apply = [] (juce::BigInteger& channels, juce::ComboBox& leftBox,
                     juce::ComboBox& rightBox, bool isStereo, int maxChannels)
    {
        channels.clear();

        if (maxChannels <= 0)
            return;

        auto leftIdx = leftBox.getSelectedId() - 1;
        if (isValidChannelIndex (leftIdx, maxChannels))
            channels.setBit (leftIdx);

        if (isStereo)
        {
            auto rightIdx = rightBox.getSelectedId() - 1;

            // 立体声模式下左右通道不能相同；若相同则自动修正到下一个可用通道
            if (rightIdx == leftIdx)
                rightIdx = findNextDistinctChannelIndex (leftIdx, maxChannels);

            if (isValidChannelIndex (rightIdx, maxChannels))
                channels.setBit (rightIdx);
        }
    };

    bool inputStereo  = (inputModeComboBox.getSelectedId()  == 2);
    bool outputStereo = (outputModeComboBox.getSelectedId() == 2);

    apply (setup.inputChannels,  inputLeftComboBox,  inputRightComboBox,  inputStereo,  static_cast<int> (device->getInputChannelNames().size()));
    apply (setup.outputChannels, outputLeftComboBox, outputRightComboBox, outputStereo, static_cast<int> (device->getOutputChannelNames().size()));

    // 标记通道为用户显式选择，防止 JUCE 在 setAudioDeviceSetup 内部
    // 按 "useDefault*Channels" 把位图强制重置为前 N 个通道。
    setup.useDefaultInputChannels  = false;
    setup.useDefaultOutputChannels = false;

    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                TRANS ("ASIO Channel Routing"),
                                                error);
    }
}

//==============================================================================
void AsioAdvancedSettingsComponent::updateModeVisibility()
{
    bool inputStereo  = (inputModeComboBox.getSelectedId()  == 2);
    bool outputStereo = (outputModeComboBox.getSelectedId() == 2);

    inputLeftLabel.setText  (inputStereo  ? TRANS ("Input L")      : TRANS ("Input Channel"),  juce::dontSendNotification);
    outputLeftLabel.setText (outputStereo ? TRANS ("Output L")     : TRANS ("Output Channel"), juce::dontSendNotification);

    inputRightLabel.setVisible  (inputStereo);
    inputRightComboBox.setVisible (inputStereo);
    outputRightLabel.setVisible (outputStereo);
    outputRightComboBox.setVisible (outputStereo);
}

//==============================================================================
void AsioAdvancedSettingsComponent::handleModeChanged (juce::ComboBox& modeComboBox,
                                                       juce::ComboBox& leftBox,
                                                       juce::ComboBox& rightBox,
                                                       int maxChannels)
{
    bool isStereo = (modeComboBox.getSelectedId() == 2);

    // 设备只有 1 个通道时不允许立体声模式
    if (isStereo && maxChannels <= 1)
    {
        modeComboBox.setSelectedId (1, juce::dontSendNotification);
        isStereo = false;
    }

    updateModeVisibility();

    if (isStereo)
        ensureStereoChannelsDistinct (leftBox, rightBox, maxChannels);
}

//==============================================================================
void AsioAdvancedSettingsComponent::ensureStereoChannelsDistinct (juce::ComboBox& leftBox,
                                                                  juce::ComboBox& rightBox,
                                                                  int maxChannels)
{
    if (maxChannels <= 1)
        return;

    auto leftIdx  = getSelectedChannelIndex (leftBox);
    auto rightIdx = getSelectedChannelIndex (rightBox);

    if (! isValidChannelIndex (leftIdx, maxChannels))
        leftIdx = 0;

    if (rightIdx == leftIdx || ! isValidChannelIndex (rightIdx, maxChannels))
    {
        auto nextIdx = findNextDistinctChannelIndex (leftIdx, maxChannels);
        rightBox.setSelectedId (isValidChannelIndex (nextIdx, maxChannels) ? nextIdx + 1 : 0,
                                juce::dontSendNotification);
    }
}

//==============================================================================
int AsioAdvancedSettingsComponent::findNextDistinctChannelIndex (int leftIdx, int maxChannels)
{
    if (maxChannels <= 1)
        return -1;

    // 优先选择 leftIdx 的下一个通道；若越界则回绕到 0
    auto nextIdx = (leftIdx + 1) % maxChannels;

    // 如果回绕后又与 leftIdx 相同（只有 1 个通道时），返回无效值
    if (nextIdx == leftIdx)
        return -1;

    return nextIdx;
}

//==============================================================================
int AsioAdvancedSettingsComponent::getNthSetBit (const juce::BigInteger& channels, int n)
{
    int count = 0;
    auto highest = channels.getHighestBit();

    for (int i = 0; i <= highest; ++i)
    {
        if (channels[i])
        {
            if (count == n)
                return i;

            ++count;
        }
    }

    return -1;
}

//==============================================================================
int AsioAdvancedSettingsComponent::getSelectedChannelIndex (juce::ComboBox& comboBox) const
{
    auto id = comboBox.getSelectedId();
    return id > 0 ? id - 1 : -1;
}

//==============================================================================
bool AsioAdvancedSettingsComponent::isValidChannelIndex (int idx, int maxChannels)
{
    return idx >= 0 && idx < maxChannels;
}

//==============================================================================
void AsioAdvancedSettingsComponent::closeDialog()
{
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
        dw->exitModalState (0);
}

} // namespace minixer
