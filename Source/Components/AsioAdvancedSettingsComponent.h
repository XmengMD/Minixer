#pragma once

#include <JuceHeader.h>
#include <utility>

namespace minixer
{

//==============================================================================
/** ASIO 高级设置对话框内容组件。

    允许用户为当前 ASIO 设备显式选择输入/输出的左右通道。
    选择结果通过 AudioDeviceManager::setAudioDeviceSetup 写入
    inputChannels / outputChannels 位图。
*/
class AsioAdvancedSettingsComponent  : public juce::Component,
                                       public juce::Button::Listener
{
public:
    //==============================================================================
    AsioAdvancedSettingsComponent (juce::AudioDeviceManager& manager,
                                   std::function<void()> onChannelsChangedCallback);
    ~AsioAdvancedSettingsComponent() override;

    //==============================================================================
    void resized() override;
    void buttonClicked (juce::Button* button) override;

private:
    //==============================================================================
    void setupLabel (juce::Label& label, const juce::String& text);
    void setupSectionLabel (juce::Label& label, const juce::String& text);
    void setupComboBox (juce::ComboBox& comboBox);
    void setupButton (juce::TextButton& button, const juce::String& text);

    void refreshChannelLists();
    void updateUIFromSetup();
    void resetUIToDefaults();
    void applyChannelSetup();

    static int getNthSetBit (const juce::BigInteger& channels, int n);
    int getSelectedChannelIndex (juce::ComboBox& comboBox) const;
    void closeDialog();

    //==============================================================================
    juce::AudioDeviceManager& deviceManager;
    std::function<void()> onChannelsChanged;

    juce::Label deviceNameLabel;

    juce::Label inputSectionLabel;
    juce::Label inputLeftLabel;
    juce::Label inputRightLabel;
    juce::ComboBox inputLeftComboBox;
    juce::ComboBox inputRightComboBox;

    juce::Label outputSectionLabel;
    juce::Label outputLeftLabel;
    juce::Label outputRightLabel;
    juce::ComboBox outputLeftComboBox;
    juce::ComboBox outputRightComboBox;

    juce::TextButton okButton     { TRANS("OK") };
    juce::TextButton cancelButton { TRANS("Cancel") };
    juce::TextButton resetButton  { TRANS("Reset") };

    bool updatingUI = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AsioAdvancedSettingsComponent)
};

} // namespace minixer
