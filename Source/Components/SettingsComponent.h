#pragma once

#include <JuceHeader.h>
#include "../Settings/AppSettings.h"
#include "../Plugin/PluginManagerComponent.h"
#include "ShortcutsSettingsComponent.h"
#include "AboutComponent.h"

namespace minixer
{

//==============================================================================
/** 设置面板组件。

    由原 DeviceSelectorComponent 扩展而来，保留音频设备选择功能，
    并增加应用级偏好设置：开机自启、最小化启动、启动时自动加载预设。
*/
class SettingsComponent  : public juce::Component,
                           public juce::ComboBox::Listener,
                           public juce::Button::Listener,
                           public juce::ChangeListener,
                           public ShortcutsSettingsComponent::Listener
{
public:
    //==============================================================================
    class Listener
    {
    public:
        virtual ~Listener() = default;
        virtual void audioSettingsChanged() = 0;
        virtual void preferencesChanged() = 0;
        virtual void shortcutsChanged() {}
    };

    //==============================================================================
    SettingsComponent (juce::AudioDeviceManager& deviceManager);
    ~SettingsComponent() override;

    //==============================================================================
    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

    //==============================================================================
    void resized() override;
    void comboBoxChanged (juce::ComboBox* comboBox) override;
    void buttonClicked (juce::Button* button) override;
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    //==============================================================================
    /** 将键盘焦点移到设置面板内的第一个可聚焦控件。 */
    void grabInitialFocus();

    //==============================================================================
    /** 设置当前选中的自动加载 preset 文件。 */
    void setAutoLoadPresetFile (const juce::File& presetFile);

private:
    //==============================================================================
    void setupComboBox (juce::ComboBox& comboBox);
    void setupLabel (juce::Label& label, const juce::String& text);
    void setupSectionLabel (juce::Label& label, const juce::String& text);
    void setupToggleButton (juce::ToggleButton& button, const juce::String& text);

    void refreshDriverList();
    void refreshDeviceLists();
    void refreshSampleRatesAndBufferSizes();
    void applyAudioSetup();
    void applyAsioDeviceSelection();
    void openAsioControlPanel();
    void updateUIFromSetup();
    void updateAutoLoadPresetEnabledState();
    void launchAutoLoadPresetFileChooser();
    void notifyAudioSettingsChanged();
    void notifyPreferencesChanged();
    void notifyShortcutsChanged();
    void showPluginManagerWindow();
    void showShortcutsSettingsWindow();
    void showAboutWindow();
    void refreshPluginCount();

    //==============================================================================
    // ShortcutsSettingsComponent::Listener
    void shortcutsSettingsApplied() override;
    void shortcutsSettingsCancelled() override;

    juce::AudioDeviceManager& deviceManager;

    //==============================================================================
    juce::AudioIODeviceType* getCurrentDeviceType() const;
    int findDeviceIndex (const juce::String& deviceName) const;
    juce::String getDisplayNameForDevice (int index) const;

    // 音频设备
    juce::Label audioSectionLabel;
    juce::Label driverLabel;
    juce::ComboBox driverComboBox;

    juce::Label inputDeviceLabel;
    juce::ComboBox inputDeviceComboBox;

    juce::Label outputDeviceLabel;
    juce::ComboBox outputDeviceComboBox;
    juce::TextButton asioControlPanelButton { TRANS("Open ASIO Control Panel") };

    juce::Label sampleRateLabel;
    juce::ComboBox sampleRateComboBox;

    juce::Label bufferSizeLabel;
    juce::ComboBox bufferSizeComboBox;

    //==============================================================================
    bool isAsioMode() const;

    // 应用偏好
    juce::Label preferencesSectionLabel;
    juce::ToggleButton launchOnStartupToggle;
    juce::ToggleButton startMinimizedToggle;
    juce::ToggleButton minimizeToTrayToggle;
    juce::ToggleButton autoLoadPresetToggle;
    juce::Label autoLoadPresetLabel;
    juce::TextButton browseAutoLoadPresetButton { TRANS("Browse") };
    juce::Label autoLoadPresetPathLabel;

    // 插件管理
    juce::Label pluginsSectionLabel;
    juce::TextButton managePluginsButton { TRANS("Manage Plugins") };
    juce::Label pluginCountLabel;

    // 快捷键设置区
    juce::Label shortcutsSectionLabel;
    juce::TextButton configureShortcutsButton { TRANS("Configure Shortcuts...") };

    // 关于
    juce::Label aboutSectionLabel;
    juce::TextButton aboutButton { TRANS("About Minixer") };

    juce::StringArray allDeviceNames;
    juce::Array<bool> deviceIsInput;
    juce::Array<bool> deviceIsOutput;

    bool updatingUI = false;

    std::unique_ptr<juce::DocumentWindow> pluginManagerWindow;
    std::unique_ptr<juce::DocumentWindow> shortcutsSettingsWindow;
    std::unique_ptr<juce::DocumentWindow> aboutWindow;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SettingsComponent)
};

} // namespace minixer
