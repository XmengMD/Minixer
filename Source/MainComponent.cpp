#include "MainComponent.h"
#include "Plugin/PluginRegistry.h"
#include "Plugin/PluginArchitecture.h"
#include "Plugin/PluginBridgeNode.h"
#include "Settings/AppSettings.h"

namespace minixer
{

//==============================================================================
MonoToStereoProcessor::MonoToStereoProcessor()
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::mono(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

void MonoToStereoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    auto numChannels = buffer.getNumChannels();
    auto numSamples = buffer.getNumSamples();

    if (numChannels >= 2 && numSamples > 0)
    {
        // 仅当输入总线实际为单声道、输出为立体声时才上混；
        // 立体声输入时保持原样，避免覆盖右声道。
        if (getBus (true, 0)->getNumberOfChannels() == 1)
            buffer.copyFrom (1, 0, buffer.getReadPointer (0), numSamples);
    }
}

bool MonoToStereoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 仅支持单声道输入、立体声输出，或立体声输入/输出
    if (layouts.getMainOutputChannelSet() != juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
        return false;

    if (layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo())
        return true;

    if (layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo()
        && layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo())
        return true;

    return false;
}

//==============================================================================
InputTrimProcessor::InputTrimProcessor()
{
    // 默认总线：立体声输入/立体声输出
    setBusesLayout (juce::AudioProcessor::BusesLayout {
        { juce::AudioChannelSet::stereo() },
        { juce::AudioChannelSet::stereo() }
    });
}

void InputTrimProcessor::setTrimDb (float trimDb)
{
    trimGain.set (juce::Decibels::decibelsToGain (trimDb, -144.0f));
}

void InputTrimProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    auto gain = trimGain.get();
    buffer.applyGain (gain);
}

bool InputTrimProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return (layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo())
        && (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

//==============================================================================
ChannelStripProcessor::ChannelStripProcessor()
{
    // 默认总线：立体声输入/立体声输出
    setBusesLayout (juce::AudioProcessor::BusesLayout {
        { juce::AudioChannelSet::stereo() },
        { juce::AudioChannelSet::stereo() }
    });
}

void ChannelStripProcessor::setPan (float pan)
{
    // 使用恒定增益声像法则：pan ∈ [-1, 1]
    // 居中时两声道均为单位增益，保证默认状态下输入/输出电平表读数一致；
    // 向一侧偏转时该侧保持 1.0，对侧按线性衰减至 0。
    auto clampedPan = juce::jlimit (-1.0f, 1.0f, pan);

    if (clampedPan <= 0.0f)
    {
        panLeft.set  (1.0f);
        panRight.set (1.0f + clampedPan);
    }
    else
    {
        panLeft.set  (1.0f - clampedPan);
        panRight.set (1.0f);
    }
}

void ChannelStripProcessor::setStereoSeparation (float separationPercent)
{
    // separationPercent ∈ [-100, 100]
    // < 0：扩展立体声宽度（separated）；> 0：压缩为单声道（merged）；0：不变
    auto norm = juce::jlimit (-100.0f, 100.0f, separationPercent) / 100.0f;
    sideGain.set (juce::jlimit (0.0f, 2.0f, 1.0f - norm));
}

void ChannelStripProcessor::setOutputDb (float outputDb)
{
    outputGain.set (juce::Decibels::decibelsToGain (outputDb, -144.0f));
}

bool ChannelStripProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return (layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo())
        && (layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo());
}

void ChannelStripProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    auto numChannels = buffer.getNumChannels();
    auto numSamples = buffer.getNumSamples();

    if (numSamples == 0)
        return;

    auto leftGain  = panLeft.get()  * outputGain.get();
    auto rightGain = panRight.get() * outputGain.get();
    auto side      = sideGain.get();

    if (numChannels >= 2)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            auto l = buffer.getSample (0, i);
            auto r = buffer.getSample (1, i);

            // Mid/Side 宽度处理
            auto m = (l + r) * 0.5f;
            auto s = (l - r) * 0.5f * side;

            l = (m + s) * leftGain;
            r = (m - s) * rightGain;

            buffer.setSample (0, i, l);
            buffer.setSample (1, i, r);
        }
    }
    else if (numChannels == 1)
    {
        // 单声道路径：仅应用输出增益（声像/立体声宽度对单声道无意义）
        buffer.applyGain (0, 0, numSamples, outputGain.get());
    }
}

//==============================================================================
MainComponent::MainComponent()
    : settingsPanel (audioDeviceManager)
{
    setSize (380, 640);

    // 应用深色主题
    juce::LookAndFeel::setDefaultLookAndFeel (&mixerLookAndFeel);

    // 设置状态栏
    statusLabel.setText (TRANS("Ready"), juce::dontSendNotification);
    statusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    statusLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getMutedTextColour());
    statusLabel.setJustificationType (juce::Justification::centredLeft);
    addAndMakeVisible (statusLabel);

    // 单声道设备提示标签（默认隐藏）
    monoDeviceLabel.setText (TRANS("MONO DEVICE"), juce::dontSendNotification);
    monoDeviceLabel.setFont (juce::Font (juce::FontOptions (11.0f)).boldened());
    monoDeviceLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getClipColour());
    monoDeviceLabel.setJustificationType (juce::Justification::centredRight);
    monoDeviceLabel.setVisible (false);
    addAndMakeVisible (monoDeviceLabel);

    // 通道条
    channelStrip.addListener (this);
    addAndMakeVisible (channelStrip);

    // 预设栏
    presetBar.addListener (this);
    addAndMakeVisible (presetBar);

    // 设置面板（初始隐藏，展开后占满主窗口区域）
    settingsPanel.addListener (this);
    addChildComponent (settingsPanel);

    // 恢复并持久化电平表计量标准
    {
        auto& settings = AppSettings::getInstance();
        channelStrip.getInputMeter().setCurrentStandard (
            static_cast<MeterStandard> (juce::jlimit (0, 3, settings.getInputMeterStandard())));
        channelStrip.getOutputMeter().setCurrentStandard (
            static_cast<MeterStandard> (juce::jlimit (0, 3, settings.getOutputMeterStandard())));

        channelStrip.getInputMeter().onStandardChanged = [] (MeterStandard standard)
        {
            AppSettings::getInstance().setInputMeterStandard (static_cast<int> (standard));
        };

        channelStrip.getOutputMeter().onStandardChanged = [] (MeterStandard standard)
        {
            AppSettings::getInstance().setOutputMeterStandard (static_cast<int> (standard));
        };
    }

    // 初始化音频图（先创建节点，不建立连接）
    setupAudioGraph();

    // 初始化音频设备管理器（优先恢复上次保存的设备设置）
    auto deviceState = AppSettings::getInstance().loadAudioDeviceState();
    audioDeviceManager.initialise (2, 2, deviceState.get(), true, {}, nullptr);

    // 必须在 addAudioCallback 之后 setProcessor，这样 AudioProcessorPlayer 才能根据
    // 实际打开的音频设备获得正确的 sampleRate / blockSize，并据此配置 graph 总线。
    audioDeviceManager.addAudioCallback (&processorPlayer);
    audioDeviceManager.addChangeListener (this);
    processorPlayer.setProcessor (audioGraph.get());

    // 校正通道启用位图，确保立体声设备的两个通道均被启用。
    // 设置标志位，防止 setAudioDeviceSetup 同步触发 changeListenerCallback 造成递归。
    isReconfiguringDevice = true;
    ensureStereoChannelsIfAvailable();
    isReconfiguringDevice = false;

    // 同步 UI 状态与设备实际通道数
    updateMonoDeviceState();

    // graph 总线已配置完成，根据当前单声道/立体声状态建立正确的节点连接
    rebuildPluginChain();

    // 将通道条默认参数同步到音频处理器
    channelStripParameterChanged();

    // 注册窗口内快捷键
    setWantsKeyboardFocus (true);
    addKeyListener (this);

    // 启动定时器更新 UI 电平
    startTimerHz (30);

    // 应用启动设置（自动加载预设等）
    applyStartupSettings();

    // 应用 holdToggle 快捷键的默认旁通状态
    applyShortcutDefaults();

    // 注册 MIDI / HID 快捷键监听器
    auto& midi = AppSettings::getInstance().getMidiShortcutInputManager();
    auto& hid  = AppSettings::getInstance().getHidShortcutInputManager();

    midi.addListener (this);
    hid.addListener (this);
}

//==============================================================================
MainComponent::~MainComponent()
{
    auto& midi = AppSettings::getInstance().getMidiShortcutInputManager();
    auto& hid  = AppSettings::getInstance().getHidShortcutInputManager();

    midi.removeListener (this);
    hid.removeListener (this);

    stopTimer();

    closeAllPluginEditors();

    // 保存当前音频设备状态
    if (auto xml = audioDeviceManager.createStateXml())
        AppSettings::getInstance().saveAudioDeviceState (*xml);

    audioDeviceManager.removeChangeListener (this);
    audioDeviceManager.removeAudioCallback (&processorPlayer);
    processorPlayer.setProcessor (nullptr);

    if (audioGraph != nullptr)
        audioGraph->clear();

    audioGraph = nullptr;
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

//==============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (MixerLookAndFeel::getBackgroundColour());
}

//==============================================================================
void MainComponent::resized()
{
    auto bounds = getLocalBounds();

    // 顶部预设栏（窄窗口下改为多行垂直布局）
    presetBar.setBounds (bounds.removeFromTop (76));

    // 底部状态栏：左侧状态文本，右侧单声道提示
    auto statusArea = bounds.removeFromBottom (24);
    auto monoLabelWidth = juce::jmin (110, statusArea.getWidth() / 3);
    monoDeviceLabel.setBounds (statusArea.removeFromRight (monoLabelWidth).reduced (4, 2));
    statusLabel.setBounds (statusArea.reduced (4, 0));

    // 设置面板展开时占满整个主区域，隐藏通道条以免被遮挡或误触
    if (settingsPanel.isVisible())
    {
        channelStrip.setVisible (false);
        settingsPanel.setBounds (bounds);
        settingsPanel.toFront (false);
    }
    else
    {
        channelStrip.setVisible (true);
        auto stripWidth = juce::jmax (360, bounds.getWidth() - 12);
        channelStrip.setBounds (bounds.withSizeKeepingCentre (stripWidth, bounds.getHeight()).reduced (0, 4));
    }
}

//==============================================================================
void MainComponent::timerCallback()
{
    auto updateMeter = [] (LevelMeterComponent& meter,
                           const juce::Atomic<float>* peakDb,
                           const juce::Atomic<float>* rmsDb,
                           juce::Atomic<float>& lufsM,
                           juce::Atomic<float>& lufsS,
                           bool mono)
    {
        float left = -60.0f, right = -60.0f;

        switch (meter.getCurrentStandard())
        {
            case MeterStandard::RMS:
                left  = rmsDb[0].get();
                right = rmsDb[1].get();
                break;

            case MeterStandard::LUFS_Momentary:
                left = right = lufsM.get();
                break;

            case MeterStandard::LUFS_ShortTerm:
                left = right = lufsS.get();
                break;

            case MeterStandard::dBFS:
            default:
                left  = peakDb[0].get();
                right = peakDb[1].get();
                break;
        }

        if (mono)
            right = left;

        meter.setLevels (left, right);
    };

    updateMeter (channelStrip.getInputMeter(),
                 inputPeakDb, inputRmsDb, inputLufsM, inputLufsS,
                 isMonoDevice);

    updateMeter (channelStrip.getOutputMeter(),
                 outputPeakDb, outputRmsDb, outputLufsM, outputLufsS,
                 false);

    // 检测焦点外的系统全局插槽快捷键
    pollGlobalSlotShortcuts();
}

//==============================================================================
void MainComponent::pollGlobalSlotShortcuts()
{
    auto& shortcutSettings = AppSettings::getInstance().getShortcutSettings();
    const bool isForeground = juce::Process::isForegroundProcess();
    const auto keyboardMask = juce::ModifierKeys::allKeyboardModifiers;
    const auto currentMods  = juce::ModifierKeys::getCurrentModifiersRealtime();

    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        const auto& shortcut = shortcutSettings.getSlotShortcut (i);

        // 仅对键盘来源做轮询；MIDI / HID 为事件驱动
        if (! shortcut.inputSource.isKeyboard())
        {
            previousGlobalShortcutStates[i] = false;
            continue;
        }

        const auto& keyPress = shortcut.inputSource.keyPress;

        if (! keyPress.isValid())
        {
            previousGlobalShortcutStates[i] = false;
            continue;
        }

        const bool keyDown   = juce::KeyPress::isKeyCurrentlyDown (keyPress.getKeyCode());
        const bool modsMatch = ((currentMods.getRawFlags() & keyboardMask)
                                == (keyPress.getModifiers().getRawFlags() & keyboardMask));
        const bool isDown    = keyDown && modsMatch;

        // 软件成为焦点时由 KeyListener 处理，这里只补焦点外的情况
        if (! isForeground && isDown != previousGlobalShortcutStates[i])
            handleGlobalSlotShortcutState (i, isDown);

        previousGlobalShortcutStates[i] = isDown;
    }
}

//==============================================================================
void MainComponent::handleGlobalSlotShortcutState (int slotIndex, bool isKeyDown)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    auto& shortcut = AppSettings::getInstance().getShortcutSettings().getSlotShortcut (slotIndex);

    if (shortcut.mode == SlotShortcutMode::cycleToggle)
    {
        if (isKeyDown)
            setSlotBypassState (slotIndex, ! slotStates[slotIndex].bypassed);
    }
    else if (shortcut.mode == SlotShortcutMode::holdToggle)
    {
        if (isKeyDown)
            setSlotBypassState (slotIndex, ! shortcut.defaultBypassed);
        else
            setSlotBypassState (slotIndex, shortcut.defaultBypassed);
    }
}

//==============================================================================
bool MainComponent::keyPressed (const juce::KeyPress& key, juce::Component* /*originatingComponent*/)
{
    // 全局快捷键优先
    if (handleGlobalShortcut (key))
        return true;

    // 插槽 bypass 快捷键
    if (handleSlotShortcutPressed (key))
        return true;

    return false;
}

//==============================================================================
bool MainComponent::keyStateChanged (bool isKeyDown, juce::Component* /*originatingComponent*/)
{
    if (! isKeyDown)
        releaseHeldSlotShortcuts();

    return false;
}

//==============================================================================
bool MainComponent::handleGlobalShortcut (const juce::KeyPress& key)
{
    auto& shortcutSettings = AppSettings::getInstance().getShortcutSettings();

    // 呼出主窗口
    if (key == shortcutSettings.getGlobalShortcut (GlobalShortcutAction::bringWindowToFront))
    {
        if (auto* window = findParentComponentOfClass<juce::DocumentWindow>())
        {
            window->setVisible (true);
            window->setMinimised (false);
            window->toFront (true);
        }

        statusLabel.setText (TRANS ("Shortcut: bring window to front"), juce::dontSendNotification);
        return true;
    }

    // 旁通/恢复所有效果
    if (key == shortcutSettings.getGlobalShortcut (GlobalShortcutAction::toggleAllPluginsBypass))
    {
        allPluginsBypassed = ! allPluginsBypassed;
        setBypassForAllPlugins (allPluginsBypassed);
        statusLabel.setText (allPluginsBypassed ? TRANS ("All plugins bypassed")
                                                : TRANS ("All plugins unbypassed"),
                             juce::dontSendNotification);
        return true;
    }

    // 切换设置面板显示
    if (key == shortcutSettings.getGlobalShortcut (GlobalShortcutAction::toggleSettingsPanel))
    {
        toggleSettings();
        return true;
    }

    // 删除当前焦点插件槽
    if (key == shortcutSettings.getGlobalShortcut (GlobalShortcutAction::deleteFocusedSlot))
    {
        auto focusedSlot = channelStrip.getFocusedPluginSlotIndex();

        if (focusedSlot >= 0)
        {
            pluginSlotDeleteRequested (focusedSlot);
            return true;
        }
    }

    return false;
}

//==============================================================================
bool MainComponent::handleSlotShortcutPressed (const juce::KeyPress& key)
{
    if (! key.isValid())
        return false;

    return handleSlotShortcutSourcePressed (ShortcutInputSource (key));
}

//==============================================================================
bool MainComponent::handleSlotShortcutSourcePressed (const ShortcutInputSource& source)
{
    auto& shortcutSettings = AppSettings::getInstance().getShortcutSettings();
    auto matchedIndices = shortcutSettings.findSlotIndicesForInputSource (source);

    if (matchedIndices.isEmpty())
        return false;

    bool anyHandled = false;

    for (auto slotIndex : matchedIndices)
    {
        auto& shortcut = shortcutSettings.getSlotShortcut (slotIndex);

        if (shortcut.mode == SlotShortcutMode::cycleToggle)
        {
            // 点按循环：直接翻转当前旁通状态
            setSlotBypassState (slotIndex, ! slotStates[slotIndex].bypassed);
            anyHandled = true;
        }
        else if (shortcut.mode == SlotShortcutMode::holdToggle)
        {
            // 长按切换：触发时进入与默认相反的状态；MIDI/HID 没有明确的“释放”事件，
            // 因此在一个短暂定时后自动恢复默认状态。
            if (! heldSlotShortcutIndices.contains (slotIndex))
            {
                heldSlotShortcutIndices.add (slotIndex);
                setSlotBypassState (slotIndex, ! shortcut.defaultBypassed);
                anyHandled = true;

                if (! source.isKeyboard())
                {
                    juce::Timer::callAfterDelay (200, [this, slotIndex]()
                    {
                        auto& shortcut = AppSettings::getInstance().getShortcutSettings().getSlotShortcut (slotIndex);
                        setSlotBypassState (slotIndex, shortcut.defaultBypassed);
                        heldSlotShortcutIndices.removeFirstMatchingValue (slotIndex);
                    });
                }
            }
        }
    }

    return anyHandled;
}

//==============================================================================
void MainComponent::releaseHeldSlotShortcuts()
{
    auto& shortcutSettings = AppSettings::getInstance().getShortcutSettings();

    for (int i = heldSlotShortcutIndices.size() - 1; i >= 0; --i)
    {
        auto slotIndex = heldSlotShortcutIndices[i];
        auto& shortcut = shortcutSettings.getSlotShortcut (slotIndex);

        // 仅对键盘来源检测按键释放；MIDI / HID 的 hold 模式在收到事件时立即处理
        if (! shortcut.inputSource.isKeyboard())
        {
            setSlotBypassState (slotIndex, shortcut.defaultBypassed);
            heldSlotShortcutIndices.remove (i);
            continue;
        }

        // 当按键已释放时恢复到默认旁通状态
        if (! juce::KeyPress::isKeyCurrentlyDown (shortcut.inputSource.keyPress.getKeyCode()))
        {
            setSlotBypassState (slotIndex, shortcut.defaultBypassed);
            heldSlotShortcutIndices.remove (i);
        }
    }
}

//==============================================================================
void MainComponent::setSlotBypassState (int slotIndex, bool shouldBypass, bool updateStatus)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    slotStates[slotIndex].bypassed = shouldBypass;

    if (pluginSlotNodes[slotIndex] != nullptr)
        pluginSlotNodes[slotIndex]->setBypassed (shouldBypass);

    if (slotStates[slotIndex].pluginIdentifier.isNotEmpty())
        channelStrip.setPluginSlotInfo (slotIndex, slotStates[slotIndex].pluginName, shouldBypass);

    if (updateStatus)
    {
        auto text = TRANS ("Slot ") + juce::String (slotIndex + 1)
                    + (shouldBypass ? TRANS (" bypass: on") : TRANS (" bypass: off"));
        statusLabel.setText (text, juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::applySlotBypassDefault (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    if (slotStates[slotIndex].pluginIdentifier.isEmpty())
        return;

    bool shouldBypass = allPluginsBypassed;

    auto& shortcut = AppSettings::getInstance().getShortcutSettings().getSlotShortcut (slotIndex);

    if (shortcut.mode == SlotShortcutMode::holdToggle)
        shouldBypass = shouldBypass || shortcut.defaultBypassed;

    setSlotBypassState (slotIndex, shouldBypass);
}

void MainComponent::applyShortcutDefaults()
{
    for (int i = 0; i < defaultNumPluginSlots; ++i)
        applySlotBypassDefault (i);
}

//==============================================================================
void MainComponent::midiShortcutMessageReceived (const juce::MidiMessage& message)
{
    // Note On with velocity 0 实际上是 Note Off，忽略
    if (message.isNoteOn() && message.getVelocity() == 0)
        return;

    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        auto& shortcut = AppSettings::getInstance().getShortcutSettings().getSlotShortcut (i);

        if (shortcut.inputSource.matchesMidiMessage (message))
        {
            if (shortcut.mode == SlotShortcutMode::cycleToggle)
            {
                setSlotBypassState (i, ! slotStates[i].bypassed);
            }
            else if (shortcut.mode == SlotShortcutMode::holdToggle)
            {
                // MIDI CC 通常持续发送；用 200ms 自动恢复实现“按住”效果
                setSlotBypassState (i, ! shortcut.defaultBypassed);

                juce::Timer::callAfterDelay (200, [this, i]()
                {
                    auto& shortcut = AppSettings::getInstance().getShortcutSettings().getSlotShortcut (i);
                    setSlotBypassState (i, shortcut.defaultBypassed);
                });
            }
        }
    }
}

//==============================================================================
void MainComponent::hidShortcutEventReceived (const HidShortcutEvent& event)
{
    if (! event.isPressed)
        return;

    auto source = ShortcutInputSource::hidButton (event.vendorId, event.productId,
                                                   event.usagePage, event.usage,
                                                   event.controlId);

    handleSlotShortcutSourcePressed (source);
}

//==============================================================================
void MainComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &audioDeviceManager)
    {
        // ensureStereoChannelsIfAvailable() 内部会再次调用 setAudioDeviceSetup，
        // 从而触发一次新的 changeListenerCallback。用标志避免递归。
        if (isReconfiguringDevice)
            return;

        isReconfiguringDevice = true;

        // 先校正通道启用位图，避免旧状态导致立体声被误配置为单声道
        ensureStereoChannelsIfAvailable();
        updateMonoDeviceState();

        // 设备设置改变后，AudioProcessorPlayer 会重新配置 graph 总线，
        // 需要重建连接以匹配新的通道布局。
        rebuildPluginChain();

        isReconfiguringDevice = false;
    }
}

//==============================================================================
void MainComponent::updateMonoDeviceState()
{
    auto setup = audioDeviceManager.getAudioDeviceSetup();
    auto activeInputChannels = setup.inputChannels.countNumberOfSetBits();

    // 当实际启用的输入通道数为 1 时视为单声道设备
    isMonoDevice = (activeInputChannels == 1);
    monoDeviceLabel.setVisible (isMonoDevice);

    if (isMonoDevice)
        monoDeviceLabel.setText (TRANS ("MONO DEVICE"), juce::dontSendNotification);
}

//==============================================================================
bool MainComponent::ensureStereoChannelsIfAvailable()
{
    auto* device = audioDeviceManager.getCurrentAudioDevice();
    if (device == nullptr)
        return false;

    auto setup = audioDeviceManager.getAudioDeviceSetup();

    // 查询当前设备实际可用的输入/输出通道数，
    // 并尝试启用前两个通道（最多立体声）。
    auto maxInputChannels  = device->getInputChannelNames().size();
    auto maxOutputChannels = device->getOutputChannelNames().size();
    auto desiredInputChannels  = juce::jmin (2, maxInputChannels);
    auto desiredOutputChannels = juce::jmin (2, maxOutputChannels);

    bool needsUpdate = false;
    const bool isAsio = audioDeviceManager.getCurrentAudioDeviceType().equalsIgnoreCase ("ASIO");

    auto configureChannels = [isAsio, &needsUpdate] (juce::BigInteger& channels,
                                                     int desiredCount,
                                                     int maxCount)
    {
        // 清除超出当前设备可用范围的无效位
        for (int i = channels.getHighestBit(); --i >= maxCount;)
        {
            if (channels[i])
            {
                channels.clearBit (i);
                needsUpdate = true;
            }
        }

        auto activeCount = channels.countNumberOfSetBits();

        if (activeCount == 0 && maxCount > 0)
        {
            // 没有任何通道启用时，启用默认前两个（或唯一一个）通道
            channels.clear();
            for (int i = 0; i < desiredCount; ++i)
                channels.setBit (i);

            needsUpdate = true;
        }
        else if (! isAsio && activeCount < desiredCount)
        {
            // 非 ASIO 模式下，强制启用前两个通道，避免驱动默认只开一个
            channels.clear();
            for (int i = 0; i < desiredCount; ++i)
                channels.setBit (i);

            needsUpdate = true;
        }
        // ASIO 模式下保留用户显式选择的通道（只要有效且至少有一个）
    };

    configureChannels (setup.inputChannels,  desiredInputChannels,  maxInputChannels);
    configureChannels (setup.outputChannels, desiredOutputChannels, maxOutputChannels);

    if (needsUpdate)
    {
        auto error = audioDeviceManager.setAudioDeviceSetup (setup, true);

        if (error.isNotEmpty())
        {
            statusLabel.setText (TRANS ("Audio device setup error: ") + error,
                                 juce::dontSendNotification);
            return false;
        }

        return true;
    }

    return false;
}

//==============================================================================
void MainComponent::toggleSettings()
{
    const bool nowVisible = ! settingsPanel.isVisible();
    settingsPanel.setVisible (nowVisible);

    if (nowVisible)
        settingsPanel.grabInitialFocus();
    else
        grabKeyboardFocus();

    resized();
}

//==============================================================================
void MainComponent::applyStartupSettings()
{
    auto& settings = AppSettings::getInstance();

    if (settings.getAutoLoadPreset())
    {
        auto presetFile = settings.getAutoLoadPresetFile();
        if (presetFile.existsAsFile())
            loadPresetRequested (presetFile);
    }
}

//==============================================================================
void MainComponent::setupAudioGraph()
{
    audioGraph = std::make_unique<juce::AudioProcessorGraph>();

    inputNode = audioGraph->addNode (std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>
                                        (juce::AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode));

    inputMeterNode = audioGraph->addNode (std::make_unique<LevelMeterProcessor> (
        [this] (const LevelMeterProcessor::MeterData& data)
        {
            inputPeakDb[0].set (data.dbfs[0]);
            inputPeakDb[1].set (data.dbfs[1]);
            inputRmsDb[0].set (data.rms[0]);
            inputRmsDb[1].set (data.rms[1]);
            inputLufsM.set (data.lufsM);
            inputLufsS.set (data.lufsS);
        }));

    inputTrimNode = audioGraph->addNode (std::make_unique<InputTrimProcessor>());

    monoToStereoNode = audioGraph->addNode (std::make_unique<MonoToStereoProcessor>());

    outputMeterNode = audioGraph->addNode (std::make_unique<LevelMeterProcessor> (
        [this] (const LevelMeterProcessor::MeterData& data)
        {
            outputPeakDb[0].set (data.dbfs[0]);
            outputPeakDb[1].set (data.dbfs[1]);
            outputRmsDb[0].set (data.rms[0]);
            outputRmsDb[1].set (data.rms[1]);
            outputLufsM.set (data.lufsM);
            outputLufsS.set (data.lufsS);
        }));

    channelStripNode = audioGraph->addNode (std::make_unique<ChannelStripProcessor>());

    outputNode = audioGraph->addNode (std::make_unique<juce::AudioProcessorGraph::AudioGraphIOProcessor>
                                         (juce::AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode));

    // JUCE 8 的 AudioProcessorGraph 不再提供 setLatencyCompensationEnabled。
    // 延迟补偿先以各插件自身报告的 LatencySamples 为准，后续如需统一补偿再扩展。

    // 注意：节点连接必须在 AudioProcessorPlayer 根据实际设备配置好 graph 的总线布局之后
    // 再进行。否则 AudioGraphIOProcessor 的通道数尚未确定，会导致连接建立失败。
}

//==============================================================================
void MainComponent::reconfigureAudioGraphForCurrentDevice()
{
    if (audioGraph == nullptr)
        return;

    // AudioProcessorPlayer 会在音频设备打开/改变时自动调用 graph 的
    // setPlayConfigDetails / prepareToPlay，因此这里只需在布局可能变化后
    // 重建节点连接，确保 AudioGraphIOProcessor 的新通道数被正确连接。
    rebuildPluginChain();
}

//==============================================================================
void MainComponent::connectNodes (juce::AudioProcessorGraph& graph,
                                  juce::AudioProcessorGraph::Node::Ptr source,
                                  juce::AudioProcessorGraph::Node::Ptr dest)
{
    if (source == nullptr || dest == nullptr)
        return;

    auto sourceCh = juce::jmin (2, source->getProcessor()->getTotalNumOutputChannels());
    auto destCh   = juce::jmin (2, dest->getProcessor()->getTotalNumInputChannels());

    // 常规连接：逐个通道相连
    auto commonCh = juce::jmin (sourceCh, destCh);
    for (int ch = 0; ch < commonCh; ++ch)
        graph.addConnection ({ { source->nodeID, ch }, { dest->nodeID, ch } });

    // 单声道插件输出到立体声目标：复制到右声道
    if (sourceCh == 1 && destCh == 2)
        graph.addConnection ({ { source->nodeID, 0 }, { dest->nodeID, 1 } });
}

//==============================================================================
void MainComponent::rebuildPluginChain()
{
    if (audioGraph == nullptr)
        return;

    // JUCE 8 没有 clearConnections()；先复制连接列表再逐个移除，保留所有节点。
    auto connections = audioGraph->getConnections();
    for (auto& c : connections)
        audioGraph->removeConnection (c);

    // 输入 -> 输入电平表
    connectNodes (*audioGraph, inputNode, inputMeterNode);

    // 输入电平表 -> 输入增益
    connectNodes (*audioGraph, inputMeterNode, inputTrimNode);

    // 当设备输入为单声道时，使用 MonoToStereoProcessor 上混为立体声；
    // 当设备输入为立体声时，跳过该节点，避免其默认单声道输入总线把立体声
    // 输入下混为单声道（JUCE AudioProcessorGraph 不为普通内部节点自动设置
    // 总线布局，节点保持默认布局运行）。
    juce::AudioProcessorGraph::Node::Ptr previousNode;

    if (isMonoDevice)
    {
        connectNodes (*audioGraph, inputTrimNode, monoToStereoNode);
        previousNode = monoToStereoNode;
    }
    else
    {
        previousNode = inputTrimNode;
    }

    // 插件链 -> 通道条处理
    for (auto& slotNode : pluginSlotNodes)
    {
        if (slotNode != nullptr)
        {
            connectNodes (*audioGraph, previousNode, slotNode);
            previousNode = slotNode;
        }
    }

    connectNodes (*audioGraph, previousNode, channelStripNode);

    // 通道条处理 -> 输出电平表
    connectNodes (*audioGraph, channelStripNode, outputMeterNode);

    // 输出电平表 -> 输出
    connectNodes (*audioGraph, outputMeterNode, outputNode);
}

//==============================================================================
void MainComponent::channelStripParameterChanged()
{
    auto inputTrimDb = static_cast<float> (channelStrip.getInputTrim().getSlider().getValue());
    auto pan         = static_cast<float> (channelStrip.getPanKnob().getSlider().getValue());
    auto stereoSep   = static_cast<float> (channelStrip.getStereoSeparation().getSlider().getValue());
    auto outputDb    = static_cast<float> (channelStrip.getOutputFader().getSlider().getValue());

    if (auto* trimProcessor = dynamic_cast<InputTrimProcessor*> (inputTrimNode->getProcessor()))
        trimProcessor->setTrimDb (inputTrimDb);

    if (auto* stripProcessor = dynamic_cast<ChannelStripProcessor*> (channelStripNode->getProcessor()))
    {
        stripProcessor->setPan (pan);
        stripProcessor->setStereoSeparation (stereoSep);
        stripProcessor->setOutputDb (outputDb);
    }

    auto panText = channelStrip.getPanKnob().getDisplayValueText();

    juce::String stereoText;
    if (stereoSep < -0.05f)
        stereoText = juce::String::formatted (TRANS ("separated %.0f%%"), -stereoSep);
    else if (stereoSep > 0.05f)
        stereoText = juce::String::formatted (TRANS ("merged %.0f%%"), stereoSep);
    else
        stereoText = TRANS ("off");

    juce::String statusText;
    statusText << TRANS ("Input ") << juce::String (inputTrimDb, 1) << TRANS (" dB | Pan ")
               << panText << TRANS (" | Stereo ") << stereoText
               << TRANS (" | Output ") << juce::String (outputDb, 1) << TRANS (" dB");
    statusLabel.setText (statusText, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::pluginSlotClicked (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    if (pluginSlotNodes[slotIndex] != nullptr)
    {
        openPluginEditor (slotIndex);
    }
    else
    {
        showPluginSelectionMenu (slotIndex);
    }
}

//==============================================================================
void MainComponent::pluginSlotReplaceRequested (int slotIndex)
{
    showPluginSelectionMenu (slotIndex);
}

//==============================================================================
void MainComponent::pluginSlotBypassToggled (int slotIndex, bool shouldBypass)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    slotStates[slotIndex].bypassed = shouldBypass;

    if (pluginSlotNodes[slotIndex] != nullptr)
        pluginSlotNodes[slotIndex]->setBypassed (shouldBypass);

    auto text = TRANS ("Slot ") + juce::String (slotIndex + 1)
                + (shouldBypass ? TRANS (" bypass: on") : TRANS (" bypass: off"));
    statusLabel.setText (text, juce::dontSendNotification);
}

//==============================================================================
void MainComponent::pluginSlotDeleteRequested (int slotIndex)
{
    removePluginFromSlot (slotIndex, true);
    statusLabel.setText (juce::String::formatted (TRANS ("Slot %d cleared"), slotIndex + 1),
                         juce::dontSendNotification);
}

//==============================================================================
void MainComponent::pluginSlotCopyRequested (int slotIndex)
{
    auto& slot = channelStrip.getPluginSlot (slotIndex);

    if (slot.hasPlugin())
    {
        copiedSlotState = getSlotState (slotIndex);

        // 同时写入系统剪贴板，允许跨实例粘贴
        juce::SystemClipboard::copyTextToClipboard ("MinixerPlugin:" + copiedSlotState.pluginName
                                                    + ":" + juce::String (copiedSlotState.bypassed ? 1 : 0));

        statusLabel.setText (TRANS ("Copied ") + copiedSlotState.pluginName + TRANS (" from slot ") + juce::String (slotIndex + 1),
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::pluginSlotPasteRequested (int slotIndex)
{
    auto text = juce::SystemClipboard::getTextFromClipboard();

    if (text.startsWith ("MinixerPlugin:"))
    {
        auto parts = juce::StringArray::fromTokens (text.substring (14), ":", {});

        if (parts.size() >= 1 && parts[0].isNotEmpty())
        {
            auto name = parts[0];
            auto bypassed = parts.size() >= 2 ? (parts[1].getIntValue() != 0) : false;

            // 按名称在已扫描列表中查找对应插件并真正加载
            auto& knownList = PluginRegistry::getInstance().getKnownPluginList();
            auto types = knownList.getTypes();
            const juce::PluginDescription* matchedDesc = nullptr;

            for (auto& desc : types)
            {
                if (desc.name == name)
                {
                    matchedDesc = &desc;
                    break;
                }
            }

            if (matchedDesc != nullptr)
            {
                PluginSlotState state;
                state.pluginIdentifier = matchedDesc->createIdentifierString();
                state.pluginName       = matchedDesc->name;
                state.bypassed         = bypassed;

                applySlotState (slotIndex, state);
                statusLabel.setText (TRANS ("Pasted ") + name + TRANS (" into slot ") + juce::String (slotIndex + 1),
                                     juce::dontSendNotification);
            }
            else
            {
                statusLabel.setText (TRANS ("Plugin '") + name + TRANS ("' not found in scanned list"),
                                     juce::dontSendNotification);
            }

            return;
        }
    }

    // 回退到应用内剪贴板
    if (copiedSlotState.isValid())
    {
        applySlotState (slotIndex, copiedSlotState);
        statusLabel.setText (TRANS ("Pasted ") + copiedSlotState.pluginName + TRANS (" into slot ") + juce::String (slotIndex + 1),
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText (TRANS ("No plugin in clipboard"), juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex)
{
    if (fromSlotIndex == toSlotIndex)
        return;

    if (! juce::isPositiveAndBelow (fromSlotIndex, defaultNumPluginSlots)
        || ! juce::isPositiveAndBelow (toSlotIndex, defaultNumPluginSlots))
        return;

    // 禁止移动空槽位，避免无意义地打乱空位或意外清空目标插件。
    if (slotStates[fromSlotIndex].pluginIdentifier.isEmpty())
        return;

    moveSlotContent (fromSlotIndex, toSlotIndex);
    refreshSlotDisplays();
    rebuildPluginChain();

    // 移动后，被移动插件应用目标槽位的快捷键默认值/全局旁通
    applySlotBypassDefault (toSlotIndex);

    statusLabel.setText (TRANS ("Moved ") + slotStates[toSlotIndex].pluginName
                         + TRANS (" from slot ") + juce::String (fromSlotIndex + 1)
                         + TRANS (" to slot ") + juce::String (toSlotIndex + 1),
                         juce::dontSendNotification);
}

//==============================================================================
void MainComponent::refreshSlotDisplays()
{
    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        if (slotStates[i].pluginIdentifier.isNotEmpty())
            channelStrip.setPluginSlotInfo (i, slotStates[i].pluginName, slotStates[i].bypassed);
        else
            channelStrip.setPluginSlotInfo (i, {}, false);
    }
}

//==============================================================================
void MainComponent::moveSlotContent (int fromIndex, int toIndex)
{
    auto sourceState = slotStates[fromIndex];
    auto sourceNode  = pluginSlotNodes[fromIndex];

    if (fromIndex < toIndex)
    {
        for (int i = fromIndex; i < toIndex; ++i)
        {
            slotStates[i]      = slotStates[i + 1];
            pluginSlotNodes[i] = pluginSlotNodes[i + 1];
        }
    }
    else
    {
        for (int i = fromIndex; i > toIndex; --i)
        {
            slotStates[i]      = slotStates[i - 1];
            pluginSlotNodes[i] = pluginSlotNodes[i - 1];
        }
    }

    slotStates[toIndex]      = sourceState;
    pluginSlotNodes[toIndex] = sourceNode;
}

//==============================================================================
void MainComponent::showPluginSelectionMenu (int slotIndex)
{
    auto& knownList = PluginRegistry::getInstance().getKnownPluginList();
    auto types = knownList.getTypes();

    if (types.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                TRANS ("No Plugins"),
                                                TRANS ("No VST3 plugins found. Please scan for plugins in Settings."));
        return;
    }

    juce::PopupMenu menu;
    juce::KnownPluginList::addToMenu (menu, types,
                                      juce::KnownPluginList::sortAlphabetically,
                                      slotStates[slotIndex].pluginIdentifier);

    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [this, slotIndex, types] (int result)
    {
        if (result == 0)
            return;

        auto index = juce::KnownPluginList::getIndexChosenByMenu (types, result);

        if (juce::isPositiveAndBelow (index, types.size()))
            loadPluginIntoSlot (slotIndex, types[index]);
    });
}

//==============================================================================
void MainComponent::loadPluginIntoSlot (int slotIndex, const juce::PluginDescription& description,
                                        const std::optional<PluginSlotState>& stateToRestore)
{
    if (audioGraph == nullptr)
        return;

    // 先移除该槽位已有的插件，避免重复占用
    removePluginFromSlot (slotIndex, false);

    statusLabel.setText (TRANS ("Loading ") + description.name + TRANS ("..."),
                         juce::dontSendNotification);

    auto sampleRate = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                          : 44100.0;

    auto bufferSize = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                          : 512;

    const auto arch = detectPluginArchitecture (juce::File (description.fileOrIdentifier));

    // 32-bit 插件通过独立子进程桥接加载，避免 64-bit 宿主无法直接加载 32-bit DLL。
    if (arch == PluginArchitecture::x86 && ! canHostLoadArchitectureDirectly (arch))
    {
        auto bridgeNode = std::make_unique<PluginBridgeNode> (description, arch);
        juce::String error;

        if (bridgeNode->initialize (sampleRate, bufferSize, error))
        {
            bridgeNode->addListener (this);
            bridgeNode->prepareToPlay (sampleRate, bufferSize);

            auto node = audioGraph->addNode (std::move (bridgeNode));

            if (node != nullptr)
            {
                pluginSlotNodes[slotIndex] = node;
                slotStates[slotIndex].pluginIdentifier = description.createIdentifierString();
                slotStates[slotIndex].pluginName       = description.name;

                if (stateToRestore.has_value()
                    && stateToRestore->pluginIdentifier == slotStates[slotIndex].pluginIdentifier)
                {
                    if (stateToRestore->pluginState.getSize() > 0)
                    {
                        node->getProcessor()->setStateInformation (stateToRestore->pluginState.getData(),
                                                                   static_cast<int> (stateToRestore->pluginState.getSize()));
                    }
                }

                applySlotBypassDefault (slotIndex);
                rebuildPluginChain();

                statusLabel.setText (TRANS ("Loaded ") + slotStates[slotIndex].pluginName + TRANS (" in slot ") + juce::String (slotIndex + 1),
                                     juce::dontSendNotification);
            }
            else
            {
                statusLabel.setText (TRANS ("Failed to add plugin to graph"),
                                     juce::dontSendNotification);
            }
        }
        else
        {
            statusLabel.setText (TRANS ("Failed to load bridged plugin: ") + error,
                                 juce::dontSendNotification);
        }

        return;
    }

    // 64-bit 插件仍按原有异步路径直接在宿主进程加载。
    PluginRegistry::getInstance().getFormatManager().createPluginInstanceAsync (
        description,
        sampleRate,
        bufferSize,
        [this, slotIndex, description, stateToRestore] (std::unique_ptr<juce::AudioPluginInstance> instance,
                                                        const juce::String& errorMessage)
    {
        onPluginInstanceCreated (slotIndex, description, std::move (instance), errorMessage, stateToRestore);
    });
}

//==============================================================================
void MainComponent::onPluginInstanceCreated (int slotIndex,
                                             const juce::PluginDescription& description,
                                             std::unique_ptr<juce::AudioPluginInstance> instance,
                                             const juce::String& errorMessage,
                                             const std::optional<PluginSlotState>& stateToRestore)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    if (instance == nullptr)
    {
        statusLabel.setText (TRANS ("Failed to load plugin: ") + errorMessage,
                             juce::dontSendNotification);
        return;
    }

    // 与 JUCE AudioPluginHost 保持一致：先启用所有总线，避免插件默认总线未激活
    // 导致 graph 连接时通道数为 0。
    instance->enableAllBuses();

    auto sampleRate = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                          : 44100.0;

    auto bufferSize = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                          : 512;

    instance->prepareToPlay (sampleRate, bufferSize);

    auto node = audioGraph->addNode (std::move (instance));

    if (node == nullptr)
    {
        statusLabel.setText (TRANS ("Failed to add plugin to graph"),
                             juce::dontSendNotification);
        return;
    }

    pluginSlotNodes[slotIndex] = node;

    auto* processor = node->getProcessor();

    slotStates[slotIndex].pluginIdentifier = description.createIdentifierString();
    slotStates[slotIndex].pluginName       = description.name.isEmpty() ? processor->getName() : description.name;

    // 若需要恢复预设/剪贴板状态，则先恢复插件参数
    if (stateToRestore.has_value()
        && stateToRestore->pluginIdentifier == slotStates[slotIndex].pluginIdentifier)
    {
        if (stateToRestore->pluginState.getSize() > 0)
        {
            processor->setStateInformation (stateToRestore->pluginState.getData(),
                                            static_cast<int> (stateToRestore->pluginState.getSize()));
        }
    }

    // 新插件/替换/预设/粘贴加载后，统一按“全局旁通 + 槽位快捷键默认值”刷新 bypass
    applySlotBypassDefault (slotIndex);

    rebuildPluginChain();

    statusLabel.setText (TRANS ("Loaded ") + slotStates[slotIndex].pluginName + TRANS (" in slot ") + juce::String (slotIndex + 1),
                         juce::dontSendNotification);
}

//==============================================================================
void MainComponent::removePluginFromSlot (int slotIndex, bool rebuildChain)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    if (pluginSlotNodes[slotIndex] != nullptr)
    {
        if (auto* bridge = dynamic_cast<PluginBridgeNode*> (pluginSlotNodes[slotIndex]->getProcessor()))
            bridge->removeListener (this);

        closePluginEditorForProcessor (pluginSlotNodes[slotIndex]->getProcessor());
        audioGraph->removeNode (pluginSlotNodes[slotIndex]->nodeID);
        pluginSlotNodes[slotIndex] = nullptr;
    }

    slotStates[slotIndex].clear();
    channelStrip.setPluginSlotInfo (slotIndex, {}, false);

    if (rebuildChain)
        rebuildPluginChain();
}

//==============================================================================
void MainComponent::openPluginEditor (int slotIndex)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    auto node = pluginSlotNodes[slotIndex];
    if (node == nullptr)
        return;

    auto* processor = node->getProcessor();

    // 桥接插件由子进程自行创建独立浮动窗口，不在宿主进程中嵌入编辑器。
    if (auto* bridge = dynamic_cast<PluginBridgeNode*> (processor))
    {
        bridge->showEditorWindow (this);
        return;
    }

    for (auto& window : pluginEditorWindows)
    {
        if (window->getProcessor() == processor)
        {
            window->toFront (true);
            return;
        }
    }

    auto window = std::make_unique<PluginEditorWindow> (processor,
                                                        slotStates[slotIndex].pluginName);
    window->addListener (this);
    window->setVisible (true);
    pluginEditorWindows.push_back (std::move (window));
}

//==============================================================================
void MainComponent::closePluginEditorForProcessor (juce::AudioProcessor* processor)
{
    if (processor == nullptr)
        return;

    pluginEditorWindows.erase (
        std::remove_if (pluginEditorWindows.begin(),
                        pluginEditorWindows.end(),
                        [processor] (const std::unique_ptr<PluginEditorWindow>& w)
        {
            return w->getProcessor() == processor;
        }),
        pluginEditorWindows.end());
}

//==============================================================================
void MainComponent::closeAllPluginEditors()
{
    pluginEditorWindows.clear();
}

//==============================================================================
void MainComponent::setBypassForAllPlugins (bool shouldBypass)
{
    auto& shortcutSettings = AppSettings::getInstance().getShortcutSettings();

    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        bool bypass = shouldBypass;

        // 关闭全局旁通时，各槽位恢复到快捷键的 defaultBypassed
        if (! shouldBypass)
        {
            auto& shortcut = shortcutSettings.getSlotShortcut (i);

            if (shortcut.mode == SlotShortcutMode::holdToggle)
                bypass = shortcut.defaultBypassed;
        }

        setSlotBypassState (i, bypass, false);
    }
}

//==============================================================================
void MainComponent::pluginEditorWindowClosed (PluginEditorWindow* window)
{
    pluginEditorWindows.erase (
        std::remove_if (pluginEditorWindows.begin(),
                        pluginEditorWindows.end(),
                        [window] (const std::unique_ptr<PluginEditorWindow>& w)
        {
            return w.get() == window;
        }),
        pluginEditorWindows.end());

    // 插件编辑器关闭后，把焦点还回主窗口，确保键盘快捷键继续响应。
    if (auto* mainWindow = findParentComponentOfClass<juce::DocumentWindow>())
        mainWindow->toFront (true);

    grabKeyboardFocus();
}

//==============================================================================
void MainComponent::pluginBridgeNodeCrashed (PluginBridgeNode* node)
{
    if (node == nullptr)
        return;

    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        if (pluginSlotNodes[i] == nullptr)
            continue;

        if (pluginSlotNodes[i]->getProcessor() != node)
            continue;

        // 自动旁通该槽位，避免继续向已崩溃的子进程发送音频数据。
        setSlotBypassState (i, true);

        // 在状态栏提示用户，并附带崩溃原因。
        auto reason = node->getCrashReason();
        juce::String statusText = TRANS("Plugin in slot ") + juce::String (i + 1) + TRANS(" crashed");

        if (reason.isNotEmpty())
            statusText += " (" + reason + ")";

        statusLabel.setText (statusText, juce::dontSendNotification);
        break;
    }
}

//==============================================================================
PluginSlotState MainComponent::getSlotState (int slotIndex) const
{
    PluginSlotState state = slotStates[slotIndex];

    if (pluginSlotNodes[slotIndex] != nullptr)
    {
        auto* processor = pluginSlotNodes[slotIndex]->getProcessor();
        processor->getStateInformation (state.pluginState);
    }

    return state;
}

//==============================================================================
void MainComponent::applySlotState (int slotIndex, const PluginSlotState& state)
{
    if (slotIndex < 0 || slotIndex >= defaultNumPluginSlots)
        return;

    if (! state.isValid())
    {
        removePluginFromSlot (slotIndex, true);
        return;
    }

    // 当前槽位已加载同一插件时，直接恢复状态
    if (pluginSlotNodes[slotIndex] != nullptr
        && slotStates[slotIndex].pluginIdentifier == state.pluginIdentifier)
    {
        auto* processor = pluginSlotNodes[slotIndex]->getProcessor();

        if (state.pluginState.getSize() > 0)
        {
            processor->setStateInformation (state.pluginState.getData(),
                                            static_cast<int> (state.pluginState.getSize()));
        }

        slotStates[slotIndex] = state;

        // 加载后统一按快捷键默认值/全局旁通刷新 bypass
        applySlotBypassDefault (slotIndex);
        return;
    }

    // 否则先按标识符查找 PluginDescription 并重新加载
    auto desc = PluginRegistry::getInstance().findDescriptionForIdentifier (state.pluginIdentifier);

    if (desc == nullptr)
    {
        statusLabel.setText (TRANS ("Plugin not found for slot ") + juce::String (slotIndex + 1)
                             + ": " + state.pluginName,
                             juce::dontSendNotification);
        return;
    }

    loadPluginIntoSlot (slotIndex, *desc, state);
}

//==============================================================================
juce::XmlElement* MainComponent::saveSlotStatesToXml (juce::XmlElement& parent) const
{
    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        auto state = getSlotState (i);

        if (state.isValid())
        {
            auto slotXml = state.toXml();
            slotXml->setAttribute ("index", i);
            parent.addChildElement (slotXml.release());
        }
    }

    return &parent;
}

//==============================================================================
void MainComponent::loadSlotStatesFromXml (const juce::XmlElement& parent)
{
    // 先清空所有槽位
    for (int i = 0; i < defaultNumPluginSlots; ++i)
        removePluginFromSlot (i, false);

    rebuildPluginChain();

    // 按 XML 顺序加载
    for (auto* child = parent.getFirstChildElement(); child != nullptr; child = child->getNextElement())
    {
        if (child->hasTagName ("Slot"))
        {
            auto index = child->getIntAttribute ("index", -1);

            if (juce::isPositiveAndBelow (index, defaultNumPluginSlots))
            {
                PluginSlotState state;

                if (state.fromXml (*child))
                    applySlotState (index, state);
            }
        }
    }
}

//==============================================================================
void MainComponent::saveChannelStripStateToXml (juce::XmlElement& parent)
{
    auto channelXml = std::make_unique<juce::XmlElement> ("ChannelStrip");
    channelXml->setAttribute ("inputTrim",        channelStrip.getInputTrim().getSlider().getValue());
    channelXml->setAttribute ("pan",              channelStrip.getPanKnob().getSlider().getValue());
    channelXml->setAttribute ("stereoSeparation", channelStrip.getStereoSeparation().getSlider().getValue());
    channelXml->setAttribute ("outputFader",      channelStrip.getOutputFader().getSlider().getValue());

    parent.addChildElement (channelXml.release());
}

//==============================================================================
void MainComponent::loadChannelStripStateFromXml (const juce::XmlElement& parent)
{
    if (auto* channelXml = parent.getChildByName ("ChannelStrip"))
    {
        // 使用 sendNotificationSync 让旋钮/推子的值标签、底层处理器同步刷新。
        channelStrip.getInputTrim().getSlider().setValue (
            channelXml->getDoubleAttribute ("inputTrim", 0.0), juce::sendNotificationSync);

        channelStrip.getPanKnob().getSlider().setValue (
            channelXml->getDoubleAttribute ("pan", 0.0), juce::sendNotificationSync);

        channelStrip.getStereoSeparation().getSlider().setValue (
            channelXml->getDoubleAttribute ("stereoSeparation", 0.0), juce::sendNotificationSync);

        channelStrip.getOutputFader().getSlider().setValue (
            channelXml->getDoubleAttribute ("outputFader", 0.0), juce::sendNotificationSync);
    }
    else
    {
        // 缺少通道条参数时恢复默认值
        channelStrip.getInputTrim().getSlider().setValue (0.0, juce::sendNotificationSync);
        channelStrip.getPanKnob().getSlider().setValue (0.0, juce::sendNotificationSync);
        channelStrip.getStereoSeparation().getSlider().setValue (0.0, juce::sendNotificationSync);
        channelStrip.getOutputFader().getSlider().setValue (0.0, juce::sendNotificationSync);
    }
}

//==============================================================================
void MainComponent::savePresetRequested (const juce::File& presetFile)
{
    auto presetName = presetFile.getFileNameWithoutExtension();
    auto xml = std::make_unique<juce::XmlElement> ("Preset");
    xml->setAttribute ("version", 2);
    xml->setAttribute ("name", presetName);

    saveChannelStripStateToXml (*xml);
    saveSlotStatesToXml (*xml);

    if (xml->writeTo (presetFile))
    {
        presetBar.setCurrentPresetName (presetName, presetFile);
        statusLabel.setText (TRANS ("Saved preset: ") + presetName,
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText (TRANS ("Failed to save preset: ") + presetName,
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::loadPresetRequested (const juce::File& presetFile)
{
    if (! presetFile.existsAsFile())
    {
        statusLabel.setText (TRANS ("Preset not found: ") + presetFile.getFullPathName(),
                             juce::dontSendNotification);
        return;
    }

    auto xml = juce::XmlDocument::parse (presetFile);

    if (xml != nullptr)
    {
        loadChannelStripStateFromXml (*xml);
        loadSlotStatesFromXml (*xml);

        auto presetName = presetFile.getFileNameWithoutExtension();
        presetBar.setCurrentPresetName (presetName, presetFile);
        statusLabel.setText (TRANS ("Loaded preset: ") + presetName,
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText (TRANS ("Failed to parse preset: ") + presetFile.getFullPathName(),
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::deletePresetRequested (const juce::File& presetFile)
{
    auto presetName = presetFile.getFileNameWithoutExtension();

    if (presetFile.deleteFile())
    {
        presetBar.clearCurrentPreset();
        statusLabel.setText (TRANS ("Deleted preset: ") + presetName,
                             juce::dontSendNotification);
    }
    else
    {
        statusLabel.setText (TRANS ("Failed to delete preset: ") + presetName,
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::settingsRequested()
{
    toggleSettings();
}

//==============================================================================
void MainComponent::audioSettingsChanged()
{
    updateMonoDeviceState();
    reconfigureAudioGraphForCurrentDevice();
}

//==============================================================================
void MainComponent::preferencesChanged()
{
    // 偏好设置已持久化；当前无需要额外刷新的依赖 UI 状态。
    juce::ignoreUnused (this);
}

//==============================================================================
void MainComponent::shortcutsChanged()
{
    // 快捷键配置已应用，按 holdToggle 模式的默认旁通状态更新插槽。
    // 先清空可能正处于 hold 状态的索引，避免状态恢复冲突。
    heldSlotShortcutIndices.clear();
    applyShortcutDefaults();
}

} // namespace minixer
