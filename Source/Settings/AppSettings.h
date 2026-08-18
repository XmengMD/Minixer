#pragma once

#include <JuceHeader.h>
#include <memory>
#include "KeyboardShortcut.h"
#include "MidiShortcutInputManager.h"
#include "HidShortcutInputManager.h"

namespace minixer
{

//==============================================================================
/** 管理 Minixer 的应用级偏好设置。

    负责持久化以下配置：
    - 开机自启（Windows 下写入当前用户注册表 Run 项）
    - 最小化启动
    - 启动时自动加载指定预设

    使用 JUCE 的 ApplicationProperties / PropertiesFile 将设置保存到用户目录。
    通过单例模式在 MainWindow、MainComponent、SettingsComponent 之间共享。
*/
class AppSettings  : public juce::DeletedAtShutdown
{
public:
    //==============================================================================
    static AppSettings& getInstance();

    //==============================================================================
    /** 强制保存当前设置到磁盘。 */
    void save();

    //==============================================================================
    bool getLaunchOnStartup() const noexcept          { return launchOnStartup; }
    void setLaunchOnStartup (bool shouldLaunch);

    bool getStartMinimized() const noexcept           { return startMinimized; }
    void setStartMinimized (bool shouldStartMinimized);

    bool getMinimizeToTray() const noexcept           { return minimizeToTray; }
    void setMinimizeToTray (bool shouldMinimizeToTray);

    bool getAutoLoadPreset() const noexcept           { return autoLoadPreset; }
    void setAutoLoadPreset (bool shouldAutoLoad);

    /** 返回启动时自动加载的 preset 文件路径（可能为空）。 */
    juce::File getAutoLoadPresetFile() const          { return juce::File (autoLoadPresetPath); }

    /** 设置启动时自动加载的 preset 文件路径。 */
    void setAutoLoadPresetFile (const juce::File& file);

    /** 返回输入电平表保存的计量标准索引（0=dBFS，默认）。 */
    int getInputMeterStandard() const noexcept        { return inputMeterStandard; }
    void setInputMeterStandard (int standard);

    /** 返回输出电平表保存的计量标准索引（0=dBFS，默认）。 */
    int getOutputMeterStandard() const noexcept       { return outputMeterStandard; }
    void setOutputMeterStandard (int standard);

    /** 返回插件选择对话框的搜索匹配字段索引。
        0=Name, 1=Manufacturer, 2=Category, 3=All。默认 0（仅匹配插件名）。 */
    int getPluginSelectorSearchMode() const noexcept  { return pluginSelectorSearchMode; }
    void setPluginSelectorSearchMode (int mode);

    //==============================================================================
    /** 返回应用数据目录（%AppData%/Minixer），不存在时会自动创建。 */
    juce::File getAppDataDirectory() const;

    /** 返回插件扫描列表文件路径。 */
    juce::File getPluginListFile() const;

    /** 返回扫描崩溃黑名单（dead man's pedal）文件路径。 */
    juce::File getDeadMansPedalFile() const;

    /** 返回插件黑名单 JSON 文件路径。 */
    juce::File getPluginBlacklistFile() const;

    /** 返回预设文件保存目录。 */
    juce::File getPresetsDirectory() const;

    /** 返回指定名称的预设文件路径。 */
    juce::File getPresetFile (const juce::String& presetName) const;

    /** 返回音频设备状态文件路径。 */
    juce::File getAudioDeviceStateFile() const;

    /** 保存 AudioDeviceManager 状态 XML。 */
    void saveAudioDeviceState (const juce::XmlElement& stateXml) const;

    /** 加载 AudioDeviceManager 状态 XML；不存在时返回 nullptr。 */
    std::unique_ptr<juce::XmlElement> loadAudioDeviceState() const;

    /** 保存主窗口位置和尺寸。 */
    void saveMainWindowBounds (const juce::Rectangle<int>& bounds);

    /** 加载主窗口位置和尺寸；未保存时返回空矩形。 */
    juce::Rectangle<int> loadMainWindowBounds() const;

    /** 返回 JUCE PropertiesFile 指针，供 PluginListComponent 等使用。 */
    juce::PropertiesFile* getPropertiesFile() const;

    //==============================================================================
    /** 返回当前快捷键配置（可修改）。 */
    ShortcutSettings& getShortcutSettings() noexcept { return shortcutSettings; }
    const ShortcutSettings& getShortcutSettings() const noexcept { return shortcutSettings; }

    /** 保存快捷键配置到 PropertiesFile。 */
    void saveShortcutSettings();

    /** 从 PropertiesFile 加载快捷键配置。 */
    void loadShortcutSettings();

    //==============================================================================
    /** 返回 MIDI 快捷键输入管理器。 */
    MidiShortcutInputManager& getMidiShortcutInputManager() noexcept { return *midiShortcutInputManager; }

    /** 返回 HID 快捷键输入管理器。 */
    HidShortcutInputManager& getHidShortcutInputManager() noexcept { return *hidShortcutInputManager; }

    /** 当前用于快捷键的 MIDI 输入设备名称。 */
    juce::String getMidiShortcutDeviceName() const noexcept { return midiShortcutDeviceName; }
    void setMidiShortcutDeviceName (const juce::String& name);

private:
    //==============================================================================
    AppSettings();
    ~AppSettings() override;

    void applyLaunchOnStartup();

    //==============================================================================
    std::unique_ptr<juce::ApplicationProperties> props;

    bool launchOnStartup = false;
    bool startMinimized = false;
    bool minimizeToTray = false;
    bool autoLoadPreset = false;
    juce::String autoLoadPresetPath;
    int inputMeterStandard = 0;
    int outputMeterStandard = 0;
    int pluginSelectorSearchMode = 0;

    ShortcutSettings shortcutSettings;

    std::unique_ptr<MidiShortcutInputManager> midiShortcutInputManager;
    std::unique_ptr<HidShortcutInputManager>  hidShortcutInputManager;
    juce::String midiShortcutDeviceName;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AppSettings)
};

} // namespace minixer
