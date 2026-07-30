#include "SettingsComponent.h"
#include "../LookAndFeel/MixerLookAndFeel.h"
#include "../Plugin/PluginRegistry.h"

namespace minixer
{

//==============================================================================
SettingsComponent::SettingsComponent (juce::AudioDeviceManager& manager)
    : deviceManager (manager)
{
    // 音频设备区
    setupSectionLabel (audioSectionLabel, TRANS ("Audio"));
    setupLabel (driverLabel, TRANS ("Driver"));
    setupComboBox (driverComboBox);

    setupLabel (inputDeviceLabel, TRANS ("Input Device"));
    setupComboBox (inputDeviceComboBox);

    setupLabel (outputDeviceLabel, TRANS ("Output Device"));
    setupComboBox (outputDeviceComboBox);

    asioControlPanelButton.setLookAndFeel (&getLookAndFeel());
    asioControlPanelButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    asioControlPanelButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    asioControlPanelButton.addListener (this);
    asioControlPanelButton.setVisible (false);
    addAndMakeVisible (asioControlPanelButton);

    setupLabel (sampleRateLabel, TRANS ("Sample Rate"));
    setupComboBox (sampleRateComboBox);

    setupLabel (bufferSizeLabel, TRANS ("Buffer Size"));
    setupComboBox (bufferSizeComboBox);

    // 偏好设置区
    setupSectionLabel (preferencesSectionLabel, TRANS ("Preferences"));
    setupToggleButton (launchOnStartupToggle,   TRANS ("Launch on startup"));
    setupToggleButton (startMinimizedToggle,    TRANS ("Start minimized"));
    setupToggleButton (minimizeToTrayToggle,    TRANS ("Minimize to system tray"));
    setupToggleButton (autoLoadPresetToggle,    TRANS ("Auto-load preset on startup"));
    setupLabel (autoLoadPresetLabel, TRANS ("Preset to load"));

    browseAutoLoadPresetButton.setLookAndFeel (&getLookAndFeel());
    browseAutoLoadPresetButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    browseAutoLoadPresetButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    browseAutoLoadPresetButton.addListener (this);
    addAndMakeVisible (browseAutoLoadPresetButton);

    autoLoadPresetPathLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    autoLoadPresetPathLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    autoLoadPresetPathLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (autoLoadPresetPathLabel);

    // 插件管理区
    setupSectionLabel (pluginsSectionLabel, TRANS ("Plugins"));
    managePluginsButton.setLookAndFeel (&getLookAndFeel());
    managePluginsButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    managePluginsButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    managePluginsButton.addListener (this);
    addAndMakeVisible (managePluginsButton);
    setupLabel (pluginCountLabel, TRANS ("Scanned plugins: 0"));

    // 快捷键设置区
    setupSectionLabel (shortcutsSectionLabel, TRANS ("Shortcuts"));
    configureShortcutsButton.setLookAndFeel (&getLookAndFeel());
    configureShortcutsButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    configureShortcutsButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    configureShortcutsButton.addListener (this);
    addAndMakeVisible (configureShortcutsButton);

    // 关于
    setupSectionLabel (aboutSectionLabel, TRANS ("About"));
    aboutButton.setLookAndFeel (&getLookAndFeel());
    aboutButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    aboutButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    aboutButton.addListener (this);
    addAndMakeVisible (aboutButton);

    auto& settings = AppSettings::getInstance();
    updatingUI = true;
    launchOnStartupToggle.setToggleState   (settings.getLaunchOnStartup(),   juce::dontSendNotification);
    startMinimizedToggle.setToggleState    (settings.getStartMinimized(),    juce::dontSendNotification);
    minimizeToTrayToggle.setToggleState    (settings.getMinimizeToTray(),    juce::dontSendNotification);
    autoLoadPresetToggle.setToggleState    (settings.getAutoLoadPreset(),    juce::dontSendNotification);
    setAutoLoadPresetFile (settings.getAutoLoadPresetFile());
    updateAutoLoadPresetEnabledState();
    updatingUI = false;

    deviceManager.addChangeListener (this);
    PluginRegistry::getInstance().getKnownPluginList().addChangeListener (this);

    refreshPluginCount();
    refreshDriverList();
    refreshDeviceLists();
    refreshSampleRatesAndBufferSizes();
    updateUIFromSetup();
}

//==============================================================================
SettingsComponent::~SettingsComponent()
{
    deviceManager.removeChangeListener (this);
    PluginRegistry::getInstance().getKnownPluginList().removeChangeListener (this);
}

//==============================================================================
void SettingsComponent::grabInitialFocus()
{
    driverComboBox.grabKeyboardFocus();
}

//==============================================================================
void SettingsComponent::resized()
{
    auto bounds = getLocalBounds().reduced (16, 16);
    const auto labelWidth = juce::jmin (120, bounds.getWidth() / 3);
    const auto gap = 8;
    const int numRows = 19;
    const int numGaps = 18;
    const auto rowHeight = juce::jmax (1, juce::jmin (32, (bounds.getHeight() - numGaps * gap)
                                                            / juce::jmax (1, numRows)));

    // 将表单项在主区域内垂直居中
    const auto totalHeight = numRows * rowHeight + numGaps * gap;
    bounds.removeFromTop (juce::jmax (0, (bounds.getHeight() - totalHeight) / 2));

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

    layoutFullRow (audioSectionLabel);
    layoutRow (driverLabel,        driverComboBox);
    layoutRow (inputDeviceLabel,   inputDeviceComboBox);

    if (isAsioMode())
        layoutRow (outputDeviceLabel, asioControlPanelButton);
    else
        layoutRow (outputDeviceLabel, outputDeviceComboBox);

    layoutRow (sampleRateLabel,    sampleRateComboBox);
    layoutRow (bufferSizeLabel,    bufferSizeComboBox);

    layoutFullRow (preferencesSectionLabel);
    layoutFullRow (launchOnStartupToggle);
    layoutFullRow (startMinimizedToggle);
    layoutFullRow (minimizeToTrayToggle);
    layoutFullRow (autoLoadPresetToggle);

    // Preset to load 行：Label | [Browse...][路径]
    {
        auto row = bounds.removeFromTop (rowHeight);
        auto left = row.removeFromLeft (labelWidth);
        autoLoadPresetLabel.setBounds (left);
        row.removeFromLeft (gap);

        const auto browseWidth = juce::jmin (80, row.getWidth() / 4);
        browseAutoLoadPresetButton.setBounds (row.removeFromLeft (browseWidth));
        row.removeFromLeft (gap);
        autoLoadPresetPathLabel.setBounds (row);

        bounds.removeFromTop (gap);
    }

    layoutFullRow (pluginsSectionLabel);
    layoutFullRow (managePluginsButton);
    layoutFullRow (pluginCountLabel);

    layoutFullRow (shortcutsSectionLabel);
    layoutFullRow (configureShortcutsButton);

    layoutFullRow (aboutSectionLabel);
    layoutFullRow (aboutButton);
}

//==============================================================================
void SettingsComponent::comboBoxChanged (juce::ComboBox* comboBox)
{
    if (updatingUI)
        return;

    if (comboBox == &driverComboBox)
    {
        auto typeIndex = driverComboBox.getSelectedId() - 1;
        auto& types = deviceManager.getAvailableDeviceTypes();

        if (juce::isPositiveAndBelow (typeIndex, types.size()))
        {
            updatingUI = true;
            deviceManager.setCurrentAudioDeviceType (types[typeIndex]->getTypeName(), true);
            refreshDeviceLists();
            refreshSampleRatesAndBufferSizes();
            updateUIFromSetup();
            updatingUI = false;
        }
    }
    else if (comboBox == &inputDeviceComboBox && isAsioMode())
    {
        // ASIO 模式下 input 下拉框实际上用于选择单一 ASIO 设备
        // 选择后需要同时更新 input/output 设备名
        applyAsioDeviceSelection();
    }
    else
    {
        applyAudioSetup();
    }
}

//==============================================================================
void SettingsComponent::buttonClicked (juce::Button* button)
{
    if (updatingUI)
        return;

    auto& settings = AppSettings::getInstance();

    if (button == &launchOnStartupToggle)
    {
        settings.setLaunchOnStartup (launchOnStartupToggle.getToggleState());
    }
    else if (button == &startMinimizedToggle)
    {
        settings.setStartMinimized (startMinimizedToggle.getToggleState());
    }
    else if (button == &minimizeToTrayToggle)
    {
        settings.setMinimizeToTray (minimizeToTrayToggle.getToggleState());
    }
    else if (button == &autoLoadPresetToggle)
    {
        settings.setAutoLoadPreset (autoLoadPresetToggle.getToggleState());
        updateAutoLoadPresetEnabledState();
    }
    else if (button == &browseAutoLoadPresetButton)
    {
        launchAutoLoadPresetFileChooser();
        return; // 文件选择器异步回调里会通知偏好变更
    }
    else if (button == &managePluginsButton)
    {
        showPluginManagerWindow();
    }
    else if (button == &configureShortcutsButton)
    {
        showShortcutsSettingsWindow();
    }
    else if (button == &aboutButton)
    {
        showAboutWindow();
        return;
    }
    else if (button == &asioControlPanelButton)
    {
        openAsioControlPanel();
        return;
    }

    notifyPreferencesChanged();
}

//==============================================================================
void SettingsComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &deviceManager)
    {
        if (updatingUI)
            return;

        updateUIFromSetup();
        return;
    }

    if (source == &PluginRegistry::getInstance().getKnownPluginList())
    {
        PluginRegistry::getInstance().saveList();
        refreshPluginCount();
        return;
    }
}

//==============================================================================
void SettingsComponent::setAutoLoadPresetFile (const juce::File& presetFile)
{
    if (presetFile.existsAsFile())
    {
        autoLoadPresetPathLabel.setText (presetFile.getFileNameWithoutExtension(), juce::dontSendNotification);
        autoLoadPresetPathLabel.setTooltip (presetFile.getFullPathName());
    }
    else
    {
        autoLoadPresetPathLabel.setText (TRANS("--"), juce::dontSendNotification);
        autoLoadPresetPathLabel.setTooltip ({});
    }
}

//==============================================================================
void SettingsComponent::launchAutoLoadPresetFileChooser()
{
    auto& settings = AppSettings::getInstance();
    auto lastPresetDir = settings.getAutoLoadPresetFile().getParentDirectory();
    auto initialDir = lastPresetDir.isDirectory() ? lastPresetDir
                                                  : settings.getPresetsDirectory();

    auto chooser = std::make_shared<juce::FileChooser> (TRANS("Select Preset to Auto-Load"),
                                                        initialDir,
                                                        "*.minixer",
                                                        true);

    chooser->launchAsync (juce::FileBrowserComponent::openMode
                          | juce::FileBrowserComponent::canSelectFiles,
                          [this, chooser, &settings] (const juce::FileChooser& fc)
    {
        auto result = fc.getResult();

        if (result == juce::File{})
            return;

        settings.setAutoLoadPresetFile (result);
        setAutoLoadPresetFile (result);
        notifyPreferencesChanged();
    });
}

//==============================================================================
void SettingsComponent::setupComboBox (juce::ComboBox& comboBox)
{
    comboBox.setLookAndFeel (&getLookAndFeel());
    comboBox.addListener (this);
    addAndMakeVisible (comboBox);
}

//==============================================================================
void SettingsComponent::setupLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (12.0f)));
    label.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

//==============================================================================
void SettingsComponent::setupSectionLabel (juce::Label& label, const juce::String& text)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (juce::Font (juce::FontOptions (14.0f)).boldened());
    label.setColour (juce::Label::textColourId, MixerLookAndFeel::getAccentColour());
    label.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (label);
}

//==============================================================================
void SettingsComponent::setupToggleButton (juce::ToggleButton& button, const juce::String& text)
{
    button.setButtonText (text);
    button.setLookAndFeel (&getLookAndFeel());
    button.setColour (juce::ToggleButton::textColourId, MixerLookAndFeel::getTextColour());
    button.addListener (this);
    addAndMakeVisible (button);
}

//==============================================================================
void SettingsComponent::refreshDriverList()
{
    driverComboBox.clear (juce::dontSendNotification);

    auto& types = deviceManager.getAvailableDeviceTypes();
    for (int i = 0; i < types.size(); ++i)
        driverComboBox.addItem (types[i]->getTypeName(), i + 1);
}

//==============================================================================
void SettingsComponent::refreshDeviceLists()
{
    allDeviceNames.clear();
    deviceIsInput.clear();
    deviceIsOutput.clear();

    inputDeviceComboBox.clear (juce::dontSendNotification);
    outputDeviceComboBox.clear (juce::dontSendNotification);

    auto* type = getCurrentDeviceType();
    if (type == nullptr)
        return;

    type->scanForDevices();

    if (isAsioMode())
    {
        // ASIO 模式下 input 下拉框用于选择单一 ASIO 设备
        auto names = type->getDeviceNames (true);

        for (auto& name : names)
        {
            if (allDeviceNames.indexOf (name) < 0)
            {
                allDeviceNames.add (name);
                deviceIsInput.add (true);
                deviceIsOutput.add (true);
            }
        }

        // 若输入列表为空，尝试输出列表（ASIO 通常一致，但为防万一）
        if (allDeviceNames.isEmpty())
        {
            names = type->getDeviceNames (false);

            for (auto& name : names)
            {
                if (allDeviceNames.indexOf (name) < 0)
                {
                    allDeviceNames.add (name);
                    deviceIsInput.add (true);
                    deviceIsOutput.add (true);
                }
            }
        }

        for (int i = 0; i < allDeviceNames.size(); ++i)
            inputDeviceComboBox.addItem (allDeviceNames[i], i + 1);

        return;
    }

    auto inputNames = type->getDeviceNames (true);
    auto outputNames = type->getDeviceNames (false);

    // 合并输入设备名
    for (auto& name : inputNames)
    {
        auto idx = allDeviceNames.indexOf (name);
        if (idx < 0)
        {
            allDeviceNames.add (name);
            deviceIsInput.add (true);
            deviceIsOutput.add (false);
        }
        else
        {
            deviceIsInput.set (idx, true);
        }
    }

    // 合并输出设备名
    for (auto& name : outputNames)
    {
        auto idx = allDeviceNames.indexOf (name);
        if (idx < 0)
        {
            allDeviceNames.add (name);
            deviceIsInput.add (false);
            deviceIsOutput.add (true);
        }
        else
        {
            deviceIsOutput.set (idx, true);
        }
    }

    // 填充两个下拉框（内容一致）
    for (int i = 0; i < allDeviceNames.size(); ++i)
    {
        auto displayName = getDisplayNameForDevice (i);
        inputDeviceComboBox.addItem  (displayName, i + 1);
        outputDeviceComboBox.addItem (displayName, i + 1);
    }
}

//==============================================================================
void SettingsComponent::refreshSampleRatesAndBufferSizes()
{
    sampleRateComboBox.clear (juce::dontSendNotification);
    bufferSizeComboBox.clear (juce::dontSendNotification);

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return;

    for (auto rate : device->getAvailableSampleRates())
        sampleRateComboBox.addItem (juce::String (rate, 0) + " Hz", static_cast<int> (rate));

    for (auto size : device->getAvailableBufferSizes())
        bufferSizeComboBox.addItem (juce::String (size), size);
}

//==============================================================================
void SettingsComponent::applyAudioSetup()
{
    if (updatingUI)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();

    auto inputId = inputDeviceComboBox.getSelectedId();
    auto outputId = outputDeviceComboBox.getSelectedId();

    if (juce::isPositiveAndBelow (inputId - 1, allDeviceNames.size()))
        setup.inputDeviceName = allDeviceNames[inputId - 1];

    if (juce::isPositiveAndBelow (outputId - 1, allDeviceNames.size()))
        setup.outputDeviceName = allDeviceNames[outputId - 1];

    auto sampleRateId = sampleRateComboBox.getSelectedId();
    if (sampleRateId > 0)
        setup.sampleRate = static_cast<double> (sampleRateId);

    auto bufferSizeId = bufferSizeComboBox.getSelectedId();
    if (bufferSizeId > 0)
        setup.bufferSize = bufferSizeId;

    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                TRANS ("Audio Device Error"),
                                                error);
    }

    notifyAudioSettingsChanged();
}

//==============================================================================
void SettingsComponent::applyAsioDeviceSelection()
{
    if (updatingUI)
        return;

    auto setup = deviceManager.getAudioDeviceSetup();

    auto inputId = inputDeviceComboBox.getSelectedId();
    if (juce::isPositiveAndBelow (inputId - 1, allDeviceNames.size()))
    {
        // ASIO 设备同时作为输入和输出
        setup.inputDeviceName  = allDeviceNames[inputId - 1];
        setup.outputDeviceName = allDeviceNames[inputId - 1];
    }

    auto sampleRateId = sampleRateComboBox.getSelectedId();
    if (sampleRateId > 0)
        setup.sampleRate = static_cast<double> (sampleRateId);

    auto bufferSizeId = bufferSizeComboBox.getSelectedId();
    if (bufferSizeId > 0)
        setup.bufferSize = bufferSizeId;

    auto error = deviceManager.setAudioDeviceSetup (setup, true);

    if (error.isNotEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                TRANS ("Audio Device Error"),
                                                error);
    }

    notifyAudioSettingsChanged();
}

//==============================================================================
void SettingsComponent::openAsioControlPanel()
{
    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                TRANS ("ASIO Control Panel"),
                                                TRANS ("No ASIO device is currently open."));
        return;
    }

    if (! device->hasControlPanel())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::WarningIcon,
                                                TRANS ("ASIO Control Panel"),
                                                TRANS ("The current device does not provide a control panel."));
        return;
    }

    device->showControlPanel();

    // 控制面板关闭后重新枚举，因为用户可能改动了采样率/缓冲区大小
    refreshSampleRatesAndBufferSizes();
    updateUIFromSetup();
}

//==============================================================================
void SettingsComponent::updateUIFromSetup()
{
    updatingUI = true;

    // 同步 Driver 选择
    auto currentTypeName = deviceManager.getCurrentAudioDeviceType();
    auto& types = deviceManager.getAvailableDeviceTypes();

    for (int i = 0; i < types.size(); ++i)
    {
        if (types[i]->getTypeName() == currentTypeName)
        {
            driverComboBox.setSelectedId (i + 1, juce::dontSendNotification);
            break;
        }
    }

    // 重新枚举当前类型的设备
    refreshDeviceLists();

    const bool asio = isAsioMode();

    // 切换 ASIO 模式下的控件显示/文本
    inputDeviceLabel.setText (asio ? TRANS ("ASIO Device") : TRANS ("Input Device"),
                              juce::dontSendNotification);
    outputDeviceLabel.setText (asio ? TRANS ("ASIO Settings") : TRANS ("Output Device"),
                               juce::dontSendNotification);

    outputDeviceComboBox.setVisible (! asio);
    asioControlPanelButton.setVisible (asio);

    // 同步 Input / Output 选择
    auto setup = deviceManager.getAudioDeviceSetup();

    auto inputIdx = findDeviceIndex (setup.inputDeviceName);
    if (inputIdx >= 0)
        inputDeviceComboBox.setSelectedId (inputIdx + 1, juce::dontSendNotification);

    if (! asio)
    {
        auto outputIdx = findDeviceIndex (setup.outputDeviceName);
        if (outputIdx >= 0)
            outputDeviceComboBox.setSelectedId (outputIdx + 1, juce::dontSendNotification);
    }

    // 同步采样率/缓冲区
    refreshSampleRatesAndBufferSizes();

    if (setup.sampleRate > 0.0)
        sampleRateComboBox.setSelectedId (static_cast<int> (setup.sampleRate), juce::dontSendNotification);

    if (setup.bufferSize > 0)
        bufferSizeComboBox.setSelectedId (setup.bufferSize, juce::dontSendNotification);

    // 模式切换后需要重新布局，确保 ASIO 控制面板按钮获得正确 bounds
    resized();

    updatingUI = false;
}

//==============================================================================
void SettingsComponent::updateAutoLoadPresetEnabledState()
{
    bool enabled = autoLoadPresetToggle.getToggleState();
    autoLoadPresetLabel.setEnabled (enabled);
    browseAutoLoadPresetButton.setEnabled (enabled);
    autoLoadPresetPathLabel.setEnabled (enabled);
}

//==============================================================================
void SettingsComponent::notifyAudioSettingsChanged()
{
    listeners.call ([] (Listener& l) { l.audioSettingsChanged(); });
}

//==============================================================================
void SettingsComponent::notifyPreferencesChanged()
{
    listeners.call ([] (Listener& l) { l.preferencesChanged(); });
}

//==============================================================================
juce::AudioIODeviceType* SettingsComponent::getCurrentDeviceType() const
{
    auto& types = deviceManager.getAvailableDeviceTypes();
    auto currentName = deviceManager.getCurrentAudioDeviceType();

    for (auto* type : types)
    {
        if (type->getTypeName() == currentName)
            return type;
    }

    return types.isEmpty() ? nullptr : types[0];
}

//==============================================================================
bool SettingsComponent::isAsioMode() const
{
    return deviceManager.getCurrentAudioDeviceType().equalsIgnoreCase ("ASIO");
}

//==============================================================================
int SettingsComponent::findDeviceIndex (const juce::String& deviceName) const
{
    return allDeviceNames.indexOf (deviceName);
}

//==============================================================================
juce::String SettingsComponent::getDisplayNameForDevice (int index) const
{
    if (! juce::isPositiveAndBelow (index, allDeviceNames.size()))
        return {};

    auto name = allDeviceNames[index];
    auto isIn = deviceIsInput[index];
    auto isOut = deviceIsOutput[index];

    juce::String tag;
    if (isIn && isOut)
        tag = "IN+OUT";
    else if (isIn)
        tag = "IN";
    else if (isOut)
        tag = "OUT";
    else
        tag = "-";

    return name + " (" + tag + ")";
}

namespace
{
    //==============================================================================
    /** 自定义插件管理器窗口：关闭按钮仅隐藏窗口，不销毁，便于再次打开。 */
    struct PluginManagerWindow  : public juce::DocumentWindow
    {
        PluginManagerWindow (const juce::String& name, juce::Colour bg)
            : juce::DocumentWindow (name, bg, juce::DocumentWindow::closeButton)
        {
        }

        void closeButtonPressed() override
        {
            setVisible (false);
        }
    };
}

//==============================================================================
void SettingsComponent::showPluginManagerWindow()
{
    if (pluginManagerWindow == nullptr)
    {
        auto* content = new PluginManagerComponent();

        pluginManagerWindow.reset (new PluginManagerWindow (TRANS ("Plugin Manager"),
                                                            MixerLookAndFeel::getBackgroundColour()));
        pluginManagerWindow->setContentOwned (content, true);
        pluginManagerWindow->setUsingNativeTitleBar (true);
        pluginManagerWindow->setResizable (true, true);
        pluginManagerWindow->centreWithSize (content->getWidth(), content->getHeight());
    }

    pluginManagerWindow->setVisible (true);
    pluginManagerWindow->toFront (true);
}

//==============================================================================
void SettingsComponent::refreshPluginCount()
{
    auto count = PluginRegistry::getInstance().getKnownPluginList().getNumTypes();
    pluginCountLabel.setText (TRANS ("Scanned plugins: ") + juce::String (count),
                              juce::dontSendNotification);
}

//==============================================================================
namespace
{
    //==============================================================================
    /** 快捷键设置窗口：关闭按钮仅隐藏窗口，便于再次打开。 */
    struct ShortcutsSettingsWindow  : public juce::DocumentWindow
    {
        ShortcutsSettingsWindow (const juce::String& name, juce::Colour bg)
            : juce::DocumentWindow (name, bg, juce::DocumentWindow::closeButton)
        {
        }

        void closeButtonPressed() override
        {
            if (auto* viewport = dynamic_cast<juce::Viewport*> (getContentComponent()))
            {
                if (auto* content = dynamic_cast<ShortcutsSettingsComponent*> (viewport->getViewedComponent()))
                    content->cancelSettings();
            }

            setVisible (false);
        }
    };
}

//==============================================================================
void SettingsComponent::showShortcutsSettingsWindow()
{
    if (shortcutsSettingsWindow == nullptr)
    {
        auto* viewport = new juce::Viewport (TRANS ("Shortcuts Viewport"));
        auto* content  = new ShortcutsSettingsComponent();

        content->addListener (this);

        viewport->setViewedComponent (content, true);
        viewport->setScrollBarsShown (true, false);
        viewport->setScrollOnDragMode (juce::Viewport::ScrollOnDragMode::never);

        shortcutsSettingsWindow.reset (new ShortcutsSettingsWindow (TRANS ("Configure Shortcuts"),
                                                                    MixerLookAndFeel::getBackgroundColour()));
        shortcutsSettingsWindow->setContentOwned (viewport, true);
        shortcutsSettingsWindow->setUsingNativeTitleBar (true);
        shortcutsSettingsWindow->setResizable (false, false);
        shortcutsSettingsWindow->centreWithSize (560, 520);
    }

    shortcutsSettingsWindow->setVisible (true);
    shortcutsSettingsWindow->toFront (true);
}

//==============================================================================
void SettingsComponent::shortcutsSettingsApplied()
{
    if (shortcutsSettingsWindow != nullptr)
        shortcutsSettingsWindow->setVisible (false);

    notifyShortcutsChanged();
}

//==============================================================================
void SettingsComponent::shortcutsSettingsCancelled()
{
    if (shortcutsSettingsWindow != nullptr)
        shortcutsSettingsWindow->setVisible (false);
}

//==============================================================================
namespace
{
    //==============================================================================
    /** 关于窗口：关闭按钮仅隐藏窗口，便于再次打开。 */
    struct AboutWindow  : public juce::DocumentWindow
    {
        AboutWindow (const juce::String& name, juce::Colour bg)
            : juce::DocumentWindow (name, bg, juce::DocumentWindow::closeButton)
        {
        }

        void closeButtonPressed() override
        {
            setVisible (false);
        }
    };
}

//==============================================================================
void SettingsComponent::showAboutWindow()
{
    if (aboutWindow == nullptr)
    {
        auto* content = new AboutComponent();

        content->onClose = [this]
        {
            if (aboutWindow != nullptr)
                aboutWindow->setVisible (false);
        };

        aboutWindow.reset (new AboutWindow (TRANS ("About Minixer"),
                                            MixerLookAndFeel::getBackgroundColour()));
        aboutWindow->setContentOwned (content, true);
        aboutWindow->setUsingNativeTitleBar (true);
        aboutWindow->setResizable (false, false);
        aboutWindow->centreWithSize (520, 440);
    }

    aboutWindow->setVisible (true);
    aboutWindow->toFront (true);
}

//==============================================================================
void SettingsComponent::notifyShortcutsChanged()
{
    listeners.call ([] (Listener& l) { l.shortcutsChanged(); });
}

} // namespace minixer
