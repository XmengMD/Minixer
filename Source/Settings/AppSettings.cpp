#include "AppSettings.h"

namespace minixer
{

//==============================================================================
AppSettings& AppSettings::getInstance()
{
    static AppSettings* instance = new AppSettings();
    return *instance;
}

//==============================================================================
AppSettings::AppSettings()
{
    juce::PropertiesFile::Options options;
    options.applicationName      = "Minixer";
    options.filenameSuffix       = "settings";
    options.folderName           = "Minixer";
    options.osxLibrarySubFolder  = "Application Support";
    options.commonToAllUsers     = false;
    options.ignoreCaseOfKeyNames = true;

    props = std::make_unique<juce::ApplicationProperties>();
    props->setStorageParameters (options);

    if (auto* user = props->getUserSettings())
    {
        launchOnStartup    = user->getBoolValue   ("launchOnStartup",    false);
        startMinimized     = user->getBoolValue   ("startMinimized",     false);
        minimizeToTray     = user->getBoolValue   ("minimizeToTray",     false);
        autoLoadPreset     = user->getBoolValue   ("autoLoadPreset",     false);
        autoLoadPresetPath = user->getValue       ("autoLoadPresetPath", juce::String());
        inputMeterStandard = user->getIntValue    ("inputMeterStandard", 0);
        outputMeterStandard= user->getIntValue    ("outputMeterStandard",0);
        pluginSelectorSearchMode = user->getIntValue ("pluginSelectorSearchMode", 0);

        // 兼容旧版只保存了 preset 名称的配置：将其视为默认 Presets 目录下的文件。
        if (autoLoadPresetPath.isNotEmpty() && ! juce::File::isAbsolutePath (autoLoadPresetPath))
            autoLoadPresetPath = getPresetFile (autoLoadPresetPath).getFullPathName();

        midiShortcutDeviceName = user->getValue (MidiShortcutInputManager::getSettingsKey(), juce::String());
    }

    midiShortcutInputManager = std::make_unique<MidiShortcutInputManager>();
    hidShortcutInputManager  = std::make_unique<HidShortcutInputManager>();

    loadShortcutSettings();

    // 启动时同步一次注册表，确保外部修改后仍能保持一致
    applyLaunchOnStartup();

    // 尝试连接上次使用的 MIDI 快捷键输入设备
    midiShortcutInputManager->setDevice (midiShortcutDeviceName);

    // 启动 HID Raw Input 监听（Windows 平台）
    hidShortcutInputManager->start();
}

//==============================================================================
AppSettings::~AppSettings()
{
    save();
}

//==============================================================================
void AppSettings::save()
{
    if (props != nullptr)
        props->saveIfNeeded();
}

//==============================================================================
void AppSettings::setLaunchOnStartup (bool shouldLaunch)
{
    if (launchOnStartup == shouldLaunch)
        return;

    launchOnStartup = shouldLaunch;

    if (auto* user = props->getUserSettings())
        user->setValue ("launchOnStartup", launchOnStartup);

    applyLaunchOnStartup();
    save();
}

//==============================================================================
void AppSettings::setStartMinimized (bool shouldStartMinimized)
{
    if (startMinimized == shouldStartMinimized)
        return;

    startMinimized = shouldStartMinimized;

    if (auto* user = props->getUserSettings())
        user->setValue ("startMinimized", startMinimized);

    save();
}

//==============================================================================
void AppSettings::setMinimizeToTray (bool shouldMinimizeToTray)
{
    if (minimizeToTray == shouldMinimizeToTray)
        return;

    minimizeToTray = shouldMinimizeToTray;

    if (auto* user = props->getUserSettings())
        user->setValue ("minimizeToTray", minimizeToTray);

    save();
}

//==============================================================================
void AppSettings::setAutoLoadPreset (bool shouldAutoLoad)
{
    if (autoLoadPreset == shouldAutoLoad)
        return;

    autoLoadPreset = shouldAutoLoad;

    if (auto* user = props->getUserSettings())
        user->setValue ("autoLoadPreset", autoLoadPreset);

    save();
}

//==============================================================================
void AppSettings::setAutoLoadPresetFile (const juce::File& file)
{
    auto path = file.getFullPathName();

    if (autoLoadPresetPath == path)
        return;

    autoLoadPresetPath = path;

    if (auto* user = props->getUserSettings())
        user->setValue ("autoLoadPresetPath", autoLoadPresetPath);

    save();
}

//==============================================================================
void AppSettings::setInputMeterStandard (int standard)
{
    standard = juce::jlimit (0, 3, standard);

    if (inputMeterStandard == standard)
        return;

    inputMeterStandard = standard;

    if (auto* user = props->getUserSettings())
        user->setValue ("inputMeterStandard", inputMeterStandard);

    save();
}

//==============================================================================
void AppSettings::setOutputMeterStandard (int standard)
{
    standard = juce::jlimit (0, 3, standard);

    if (outputMeterStandard == standard)
        return;

    outputMeterStandard = standard;

    if (auto* user = props->getUserSettings())
        user->setValue ("outputMeterStandard", outputMeterStandard);

    save();
}

//==============================================================================
void AppSettings::setPluginSelectorSearchMode (int mode)
{
    mode = juce::jlimit (0, 3, mode);

    if (pluginSelectorSearchMode == mode)
        return;

    pluginSelectorSearchMode = mode;

    if (auto* user = props->getUserSettings())
        user->setValue ("pluginSelectorSearchMode", pluginSelectorSearchMode);

    save();
}

//==============================================================================
juce::Array<int> AppSettings::getPluginSelectorColumnWidths() const
{
    juce::Array<int> defaults { 280, 180, 140 };

    if (auto* user = props->getUserSettings())
    {
        auto str = user->getValue ("pluginSelectorColumnWidths");

        if (str.isNotEmpty())
        {
            auto parts = juce::StringArray::fromTokens (str, ",", {});

            if (parts.size() == 3)
            {
                juce::Array<int> result;

                for (auto& part : parts)
                    result.add (juce::jlimit (40, 800, part.getIntValue()));

                if (result.size() == 3)
                    return result;
            }
        }
    }

    return defaults;
}

//==============================================================================
void AppSettings::setPluginSelectorColumnWidths (const juce::Array<int>& widths)
{
    if (widths.size() != 3)
        return;

    juce::String str;

    for (int i = 0; i < 3; ++i)
    {
        if (i > 0)
            str += ",";

        str += juce::String (juce::jlimit (40, 800, widths[i]));
    }

    if (auto* user = props->getUserSettings())
        user->setValue ("pluginSelectorColumnWidths", str);

    save();
}

//==============================================================================
void AppSettings::applyLaunchOnStartup()
{
   #if JUCE_WINDOWS
    static const juce::String regPath
        = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\Minixer";

    if (launchOnStartup)
    {
        auto exePath = juce::File::getSpecialLocation (juce::File::currentExecutableFile)
                            .getFullPathName();
        juce::WindowsRegistry::setValue (regPath, "\"" + exePath + "\"");
    }
    else
    {
        juce::WindowsRegistry::deleteValue (regPath);
    }
   #else
    juce::ignoreUnused (launchOnStartup);
   #endif
}

//==============================================================================
juce::File AppSettings::getAppDataDirectory() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("Minixer");

    if (! dir.exists())
        dir.createDirectory();

    return dir;
}

//==============================================================================
juce::File AppSettings::getPluginListFile() const
{
    return getAppDataDirectory().getChildFile ("PluginList.xml");
}

//==============================================================================
juce::File AppSettings::getDeadMansPedalFile() const
{
    return getAppDataDirectory().getChildFile ("DeadMansPedal.txt");
}

//==============================================================================
juce::File AppSettings::getPluginBlacklistFile() const
{
    return getAppDataDirectory().getChildFile ("PluginBlacklist.json");
}

//==============================================================================
juce::File AppSettings::getPresetsDirectory() const
{
    auto dir = getAppDataDirectory().getChildFile ("Presets");

    if (! dir.exists())
        dir.createDirectory();

    return dir;
}

//==============================================================================
juce::File AppSettings::getPresetFile (const juce::String& presetName) const
{
    auto safeName = presetName;
    // 去除 Windows 文件名非法字符，避免生成无效路径
    safeName = safeName.replaceCharacters ("\\/:*?\"<>|", "_________");
    return getPresetsDirectory().getChildFile (safeName + ".minixer");
}

//==============================================================================
juce::File AppSettings::getAudioDeviceStateFile() const
{
    return getAppDataDirectory().getChildFile ("AudioDeviceState.xml");
}

//==============================================================================
void AppSettings::saveAudioDeviceState (const juce::XmlElement& stateXml) const
{
    auto file = getAudioDeviceStateFile();
    stateXml.writeTo (file);
}

//==============================================================================
std::unique_ptr<juce::XmlElement> AppSettings::loadAudioDeviceState() const
{
    auto file = getAudioDeviceStateFile();

    if (! file.existsAsFile())
        return nullptr;

    return juce::XmlDocument::parse (file);
}

//==============================================================================
void AppSettings::saveMainWindowBounds (const juce::Rectangle<int>& bounds)
{
    if (auto* user = props->getUserSettings())
    {
        user->setValue ("mainWindowBounds", bounds.toString());
        save();
    }
}

//==============================================================================
juce::Rectangle<int> AppSettings::loadMainWindowBounds() const
{
    if (auto* user = props->getUserSettings())
    {
        auto str = user->getValue ("mainWindowBounds");

        if (str.isNotEmpty())
        {
            auto parts = juce::StringArray::fromTokens (str, " ", {});

            if (parts.size() == 4)
            {
                return { parts[0].getIntValue(),
                         parts[1].getIntValue(),
                         parts[2].getIntValue(),
                         parts[3].getIntValue() };
            }
        }
    }

    return {};
}

//==============================================================================
juce::PropertiesFile* AppSettings::getPropertiesFile() const
{
    return props != nullptr ? props->getUserSettings() : nullptr;
}

//==============================================================================
void AppSettings::saveShortcutSettings()
{
    if (auto* user = props->getUserSettings())
    {
        user->setValue ("shortcutSettings", shortcutSettings.toXmlString());
        save();
    }
}

//==============================================================================
void AppSettings::loadShortcutSettings()
{
    if (auto* user = props->getUserSettings())
    {
        auto text = user->getValue ("shortcutSettings", juce::String());

        if (text.isNotEmpty())
        {
            ShortcutSettings loaded;

            if (loaded.fromXmlString (text))
                shortcutSettings = std::move (loaded);
        }
    }
}

//==============================================================================
void AppSettings::setMidiShortcutDeviceName (const juce::String& name)
{
    if (midiShortcutDeviceName == name)
        return;

    midiShortcutDeviceName = name;

    if (midiShortcutInputManager != nullptr)
        midiShortcutInputManager->setDevice (midiShortcutDeviceName);

    if (auto* user = props->getUserSettings())
        user->setValue (MidiShortcutInputManager::getSettingsKey(), midiShortcutDeviceName);

    save();
}

} // namespace minixer
