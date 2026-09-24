#include "MainComponent.h"
#include "Plugin/PluginRegistry.h"
#include "Plugin/PluginArchitecture.h"
#include "Plugin/PluginBridgeNode.h"
#include "Plugin/PluginSelectorComponent.h"
#include "Settings/AppSettings.h"

#include <mutex>

namespace minixer
{

//==============================================================================
/** 单个插件槽位的异步加载结果（后台线程 → 消息线程）。 */
struct PluginLoadOutcome
{
    uint64_t generation = 0;                 // 发起加载时的槽位代数
    bool ok = false;
    juce::String error;
    std::unique_ptr<PluginBridgeNode> bridge; // 成功时为已初始化节点
    juce::PluginDescription description;     // 完成时更新 slotStates 用
    std::optional<PluginSlotState> stateToRestore;
};

/** 单个插件槽位的共享加载状态。 */
struct PluginLoadSlot
{
    std::mutex mutex;
    uint64_t generation = 0;                  // 每次新加载/取消递增，作废在途任务
    bool pending = false;
    std::unique_ptr<PluginLoadOutcome> outcome;
};

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
StereoToMonoProcessor::StereoToMonoProcessor()
    : juce::AudioProcessor (juce::AudioProcessor::BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::mono(), true))
{
}

void StereoToMonoProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/)
{
    auto numChannels = buffer.getNumChannels();
    auto numSamples = buffer.getNumSamples();

    if (numChannels >= 2 && numSamples > 0)
    {
        // 立体声下混为单声道：L+R 求和写入通道 0（0dB 不缩放，与子进程
        // mono-in 插件的求和方式保持一致）。输出总线为 mono，下游只取通道 0。
        const float* left  = buffer.getReadPointer (0);
        const float* right = buffer.getReadPointer (1);
        auto* mono = buffer.getWritePointer (0);

        for (int i = 0; i < numSamples; ++i)
            mono[i] = left[i] + right[i];
    }
}

bool StereoToMonoProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    // 仅支持 mono 或 stereo 输入 + 单声道输出
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::mono()
        && (layouts.getMainInputChannelSet() == juce::AudioChannelSet::mono()
            || layouts.getMainInputChannelSet() == juce::AudioChannelSet::stereo());
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

    // 单声道输入提示标签（默认隐藏）
    monoDeviceLabel.setText (TRANS("MONO INPUT"), juce::dontSendNotification);
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

    // 插件后台加载线程池（少量并发，UI 线程不参与加载/校验）与共享槽位状态
    pluginLoaderPool = std::make_unique<juce::ThreadPool> (4);

    for (auto& slot : pluginLoadSlots)
        slot = std::make_shared<PluginLoadSlot>();

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

    // 终止仍在进行中的后台加载任务（等 5s；极端情况下在途任务会继续运行至
    // 自身 120s 超时，进程退出时由操作系统回收，不阻塞 UI 线程）。
    if (pluginLoaderPool != nullptr)
        pluginLoaderPool->removeAllJobs (true, 5000);

    closeAllPluginEditors();

    // 保存当前音频设备状态
    if (auto xml = audioDeviceManager.createStateXml())
        AppSettings::getInstance().saveAudioDeviceState (*xml);

    audioDeviceManager.removeChangeListener (this);
    audioDeviceManager.removeAudioCallback (&processorPlayer);
    processorPlayer.setProcessor (nullptr);

    // 销毁插件节点：节点析构会把各自的子进程交给 PluginHostProcessReaper 后台收割，
    // 不会在消息线程上等待，因此这里不会长时间卡住。
    if (audioGraph != nullptr)
        audioGraph->clear();

    audioGraph = nullptr;

    // 等待后台收割完成（最多 2s），并强杀仍未退出的子进程，避免残留孤儿进程。
    PluginHostProcessReaper::getInstance().drain (2000);

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

    // 取回后台线程完成的插件桥接加载结果（消息线程执行图操作）
    processSlotLoadResults();
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

    // 当实际启用的输入通道数为 1 时视为单声道输入（包括显式配置的 Mono 模式）
    isMonoDevice = (activeInputChannels == 1);
    monoDeviceLabel.setVisible (isMonoDevice);

    if (isMonoDevice)
        monoDeviceLabel.setText (TRANS ("MONO INPUT"), juce::dontSendNotification);

    // 输出侧同理：启用输出通道数为 1 时视为单声道输出，
    // 需要在图尾做 L+R 下混，避免只取 L 声道。
    isMonoOutputDevice = (setup.outputChannels.countNumberOfSetBits() == 1);
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

    downMixNode = audioGraph->addNode (std::make_unique<StereoToMonoProcessor>());

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

    // 输出设备为单声道时：先做 L+R 下混再送入输出节点，
    // 避免立体声内容只取 L 声道、静默丢弃 R 声道。
    if (isMonoOutputDevice)
    {
        connectNodes (*audioGraph, outputMeterNode, downMixNode);

        // 输出电平表 -> 下混 -> 输出
        connectNodes (*audioGraph, downMixNode, outputNode);
    }
    else
    {
        // 输出电平表 -> 输出
        connectNodes (*audioGraph, outputMeterNode, outputNode);
    }
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
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    // 进行中不响应点击（槽位组件已屏蔽交互，这里作为双保险）
    if (isSlotBusy (slotIndex))
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
    if (isSlotBusy (slotIndex))
        return;

    showPluginSelectionMenu (slotIndex);
}

//==============================================================================
void MainComponent::pluginSlotLoadCancelRequested (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    // 仅加载中可取消；卸载中已在后台进行且不可逆
    if (slotBusyState[slotIndex] != PluginSlotBusyState::loading)
        return;

    // 仅作废在途加载；替换场景下原有插件保持不变并继续工作
    cancelSlotLoadsForSlot (slotIndex);

    statusLabel.setText (TRANS ("Cancelled loading in slot ") + juce::String (slotIndex + 1),
                         juce::dontSendNotification);
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
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    // 卸载中不可重复删除（节点已摘除，子进程仍在后台退出）
    if (slotBusyState[slotIndex] == PluginSlotBusyState::removing)
    {
        statusLabel.setText (TRANS ("Slot ") + juce::String (slotIndex + 1)
                             + TRANS (" is still unloading"),
                             juce::dontSendNotification);
        return;
    }

    const auto removedName = slotStates[slotIndex].pluginName;

    // 标记为用户直接发起的卸载：子进程退出后据此回写状态栏
    slotRemovalStatusPending[slotIndex] = true;

    removePluginFromSlot (slotIndex, true);

    // 卸载是异步的（后台等待子进程退出），状态栏先反映“正在卸载”，
    // 待子进程退出后由 onSlotNodeFullyShutDown() 更新为最终结果。
    statusLabel.setText (removedName.isNotEmpty()
                             ? TRANS ("Removing ") + removedName
                                   + TRANS (" from slot ") + juce::String (slotIndex + 1) + TRANS ("...")
                             : juce::String::formatted (TRANS ("Slot %d cleared"), slotIndex + 1),
                         juce::dontSendNotification);
}

//==============================================================================
void MainComponent::pluginSlotCopyRequested (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    if (isSlotBusy (slotIndex))
    {
        statusLabel.setText (TRANS ("Slot ") + juce::String (slotIndex + 1)
                             + (slotBusyState[slotIndex] == PluginSlotBusyState::removing
                                    ? TRANS (" is still unloading")
                                    : TRANS (" is still loading")),
                             juce::dontSendNotification);
        return;
    }

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
    else
    {
        statusLabel.setText (TRANS ("Slot ") + juce::String (slotIndex + 1) + TRANS (" is empty"),
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::pluginSlotPasteRequested (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots) || isSlotBusy (slotIndex))
        return;

    auto text = juce::SystemClipboard::getTextFromClipboard();

    if (text.startsWith ("MinixerPlugin:"))
    {
        // 剪贴板格式为 "<插件名>:<bypass 标记>"；插件名本身可能包含 ':'，
        // 因此按最后一个 ':' 拆分，避免名称被截断。
        const auto payload = text.substring (14);
        const auto separator = payload.lastIndexOfChar (':');

        auto name = separator >= 0 ? payload.substring (0, separator) : payload;
        auto bypassed = separator >= 0 && payload.substring (separator + 1).getIntValue() != 0;

        if (name.isNotEmpty())
        {
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
    // 槽位内容整体移位后，落在移位区间内的在途加载会失去位置语义：
    // 其完成结果会写回错位槽位，覆盖移位后的插件并留下孤立节点。
    // 因此统一取消区间内所有在途加载（槽位原有插件保持不变）。
    const auto firstIndex = juce::jmin (fromIndex, toIndex);
    const auto lastIndex  = juce::jmax (fromIndex, toIndex);

    for (int i = firstIndex; i <= lastIndex; ++i)
        cancelSlotLoadsForSlot (i);

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
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots) || isSlotBusy (slotIndex))
        return;

    auto types = PluginRegistry::getInstance().getKnownPluginList().getTypes();

    if (types.isEmpty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                                TRANS ("No Plugins"),
                                                TRANS ("No VST3 plugins found. Please scan for plugins in Settings."));
        return;
    }

    auto* selector = new PluginSelectorComponent (
        std::move (types),
        slotStates[slotIndex].pluginIdentifier,
        [this, slotIndex] (juce::PluginDescription desc)
        {
            loadPluginIntoSlot (slotIndex, desc);
        },
        &mixerLookAndFeel);

    juce::DialogWindow::LaunchOptions opts;
    opts.dialogTitle = TRANS ("Select Plugin");
    opts.content.setOwned (selector);
    opts.componentToCentreAround = this;
    opts.dialogBackgroundColour = MixerLookAndFeel::getBackgroundColour();
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = true;
    opts.launchAsync();
}

//==============================================================================
void MainComponent::loadPluginIntoSlot (int slotIndex, const juce::PluginDescription& description,
                                        const std::optional<PluginSlotState>& stateToRestore)
{
    if (audioGraph == nullptr)
        return;

    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    auto sampleRate = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                          : 44100.0;

    auto bufferSize = audioDeviceManager.getCurrentAudioDevice() != nullptr
                          ? audioDeviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                          : 512;

    const auto arch = detectPluginArchitecture (juce::File (description.fileOrIdentifier));

    // 所有架构（x86 / x64 / 未知）统一通过 PluginHost 子进程桥接加载：
    // 插件 DLL 的加载、实例化、license 校验全部在子进程内完成，与主进程 UI
    // 完全隔离，加载期间主界面保持可交互、音频通路保持连贯。
    //
    // 注意：此处不再立即移除槽位已有插件。加载期间槽位进入“加载中”状态
    // （显示提示、屏蔽交互），旧插件继续工作；待新插件在后台就绪后再替换。
    if (! startSlotLoad (slotIndex, description, arch, sampleRate, bufferSize, stateToRestore))
        return;

    setSlotBusyState (slotIndex, PluginSlotBusyState::loading, description.name);

    statusLabel.setText (TRANS ("Loading ") + description.name + TRANS ("..."),
                         juce::dontSendNotification);
}

//==============================================================================
bool MainComponent::startSlotLoad (int slotIndex, const juce::PluginDescription& description,
                                   PluginArchitecture arch, double sampleRate, int bufferSize,
                                   const std::optional<PluginSlotState>& stateToRestore)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return false;

    auto slot = pluginLoadSlots[slotIndex];

    if (slot == nullptr || pluginLoaderPool == nullptr)
    {
        statusLabel.setText (TRANS ("Failed to load plugin: internal error"),
                             juce::dontSendNotification);
        return false;
    }

    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock (slot->mutex);

        ++slot->generation;
        generation = slot->generation;

        // 丢弃尚未被消息线程取走的旧结果，避免新旧加载混淆
        slot->outcome.reset();
        slot->pending = false;
    }

    // 后台线程执行桥接初始化（子进程启动 + 插件加载 + license 校验等耗时操作）。
    // 期间 UI 不做任何同步等待；结果写入共享槽位，由消息线程定时器取回。
    // 显式指定 JobStatus 返回类型，避免与 addJob(std::function<void()>) 重载歧义。
    auto loadTask = [slot, generation, description, arch, sampleRate, bufferSize, stateToRestore]() -> juce::ThreadPoolJob::JobStatus
    {
        auto outcome = std::make_unique<PluginLoadOutcome>();
        outcome->generation     = generation;
        outcome->description    = description;
        outcome->stateToRestore = stateToRestore;

        auto bridge = std::make_unique<PluginBridgeNode> (description, arch);
        juce::String error;

        if (! bridge->initialize (sampleRate, bufferSize, error))
        {
            outcome->error = error;
            bridge->shutdown();
            bridge.reset();
        }

        outcome->bridge = std::move (bridge);
        outcome->ok = (outcome->bridge != nullptr);

        {
            std::lock_guard<std::mutex> lock (slot->mutex);
            slot->outcome = std::move (outcome);
            slot->pending = true;
        }

        return juce::ThreadPoolJob::jobHasFinished;
    };

    pluginLoaderPool->addJob (std::function<juce::ThreadPoolJob::JobStatus()> (std::move (loadTask)));

    return true;
}

//==============================================================================
void MainComponent::setSlotBusyState (int slotIndex, PluginSlotBusyState busyState, const juce::String& pluginName)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    if (slotBusyState[slotIndex] == busyState
        && (busyState == PluginSlotBusyState::none || slotBusyPluginName[slotIndex] == pluginName))
        return;

    slotBusyState[slotIndex] = busyState;
    slotBusyPluginName[slotIndex] = busyState == PluginSlotBusyState::none ? juce::String() : pluginName;

    channelStrip.setPluginSlotBusyState (slotIndex, busyState, pluginName);
}

//==============================================================================
bool MainComponent::isSlotBusy (int slotIndex) const noexcept
{
    return juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots)
           && slotBusyState[slotIndex] != PluginSlotBusyState::none;
}

//==============================================================================
juce::AudioProcessorGraph::Node::Ptr MainComponent::detachSlotNode (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return {};

    auto node = pluginSlotNodes[slotIndex];

    if (node == nullptr)
        return {};

    if (auto* bridge = dynamic_cast<PluginBridgeNode*> (node->getProcessor()))
    {
        bridge->removeListener (this);

        // 子进程完全退出后回调消息线程，用于解除槽位“卸载中”状态。
        // 必须在这里设置：节点可能在 AudioProcessorGraph 回收渲染序列时才析构。
        juce::Component::SafePointer<MainComponent> safeThis (this);
        bridge->setShutdownCompletionCallback ([safeThis, slotIndex]()
        {
            if (safeThis != nullptr)
                safeThis->onSlotNodeFullyShutDown (slotIndex);
        });
    }

    closePluginEditorForProcessor (node->getProcessor());
    audioGraph->removeNode (node->nodeID);
    pluginSlotNodes[slotIndex] = nullptr;

    // 返回节点引用：调用方若不再需要应立即释放；节点析构本身不会阻塞
    //（子进程由 PluginHostProcessReaper 在后台等待退出）
    return node;
}

//==============================================================================
void MainComponent::onSlotNodeFullyShutDown (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    // 用户直接发起的卸载：卸载完成后把状态栏从“Removing ...”更新为最终结果
    const bool reportToStatusBar = slotRemovalStatusPending[slotIndex];
    slotRemovalStatusPending[slotIndex] = false;

    // 期间槽位可能已被重新加载/重新填充，此时不应再改动其状态
    if (slotBusyState[slotIndex] != PluginSlotBusyState::removing)
        return;

    setSlotBusyState (slotIndex, PluginSlotBusyState::none);

    // 仅在槽位确实为空时才刷新为空槽显示（否则保留新填入的插件信息）
    if (slotStates[slotIndex].pluginIdentifier.isEmpty())
    {
        channelStrip.setPluginSlotInfo (slotIndex, {}, false);

        if (reportToStatusBar)
            statusLabel.setText (juce::String::formatted (TRANS ("Slot %d cleared"), slotIndex + 1),
                                 juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::cancelSlotLoadsForSlot (int slotIndex)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    auto slot = pluginLoadSlots[slotIndex];
    if (slot == nullptr)
        return;

    {
        std::lock_guard<std::mutex> lock (slot->mutex);

        // 递增代数：任何在途加载完成时与当前代数不符，将被丢弃并关闭其子进程
        ++slot->generation;

        // 丢弃尚未被消息线程取走的结果
        slot->outcome.reset();
        slot->pending = false;
    }

    // 仅解除“加载中”状态；卸载中由后台销毁完成后自行解除，不能在此提前清掉
    if (slotBusyState[slotIndex] == PluginSlotBusyState::loading)
        setSlotBusyState (slotIndex, PluginSlotBusyState::none);
}

//==============================================================================
void MainComponent::processSlotLoadResults()
{
    for (int slotIndex = 0; slotIndex < defaultNumPluginSlots; ++slotIndex)
    {
        auto slot = pluginLoadSlots[slotIndex];
        if (slot == nullptr)
            continue;

        std::unique_ptr<PluginLoadOutcome> outcome;
        uint64_t currentGeneration = 0;

        {
            std::lock_guard<std::mutex> lock (slot->mutex);

            if (! slot->pending)
                continue;

            outcome = std::move (slot->outcome);
            slot->outcome.reset();
            slot->pending = false;
            currentGeneration = slot->generation;
        }

        if (outcome == nullptr)
            continue;

        // 该槽位已发生新的加载请求或已被移除 → 作废本次结果（关闭其子进程）
        if (outcome->generation != currentGeneration)
        {
            if (outcome->bridge != nullptr)
                outcome->bridge->shutdown();
            continue;
        }

        if (! outcome->ok)
        {
            // 加载失败：解除加载态，槽位原有插件（若有）保持不变
            setSlotBusyState (slotIndex, PluginSlotBusyState::none);

            statusLabel.setText (TRANS ("Failed to load bridged plugin: ") + outcome->error,
                                 juce::dontSendNotification);
            continue;
        }

        auto* bridge = outcome->bridge.get();

        bridge->addListener (this);

        const auto sampleRate = audioDeviceManager.getCurrentAudioDevice() != nullptr
                                    ? audioDeviceManager.getCurrentAudioDevice()->getCurrentSampleRate()
                                    : 44100.0;

        const auto bufferSize = audioDeviceManager.getCurrentAudioDevice() != nullptr
                                    ? audioDeviceManager.getCurrentAudioDevice()->getCurrentBufferSizeSamples()
                                    : 512;

        bridge->prepareToPlay (sampleRate, bufferSize);

        auto node = audioGraph->addNode (std::move (outcome->bridge));

        if (node == nullptr)
        {
            // 添加失败：解除加载态，槽位原有插件（若有）保持不变
            setSlotBusyState (slotIndex, PluginSlotBusyState::none);

            statusLabel.setText (TRANS ("Failed to add plugin to graph"),
                                 juce::dontSendNotification);
            continue;
        }

        // 替换场景：新插件已就绪，此时才移除槽位中的旧插件节点。
        // 旧节点的子进程由 PluginHostProcessReaper 在后台收割，不阻塞消息线程。
        if (auto oldNode = detachSlotNode (slotIndex))
            oldNode.reset();

        pluginSlotNodes[slotIndex] = node;

        auto* processor = node->getProcessor();

        slotStates[slotIndex].pluginIdentifier = outcome->description.createIdentifierString();
        slotStates[slotIndex].pluginName       = outcome->description.name.isEmpty() ? processor->getName()
                                                                                     : outcome->description.name;

        // 若需要恢复预设/剪贴板状态，则先恢复插件参数
        if (outcome->stateToRestore.has_value()
            && outcome->stateToRestore->pluginIdentifier == slotStates[slotIndex].pluginIdentifier)
        {
            if (outcome->stateToRestore->pluginState.getSize() > 0)
            {
                processor->setStateInformation (outcome->stateToRestore->pluginState.getData(),
                                                static_cast<int> (outcome->stateToRestore->pluginState.getSize()));
            }
        }

        // 先解除加载态，再按“全局旁通 + 槽位快捷键默认值”刷新 bypass（会一并刷新槽位显示）
        setSlotBusyState (slotIndex, PluginSlotBusyState::none);
        applySlotBypassDefault (slotIndex);

        rebuildPluginChain();

        statusLabel.setText (TRANS ("Loaded ") + slotStates[slotIndex].pluginName + TRANS (" in slot ") + juce::String (slotIndex + 1),
                             juce::dontSendNotification);
    }
}

//==============================================================================
void MainComponent::removePluginFromSlot (int slotIndex, bool rebuildChain)
{
    if (! juce::isPositiveAndBelow (slotIndex, defaultNumPluginSlots))
        return;

    // 作废该槽位一切在途加载（防止旧加载完成后又把节点塞回已移除的槽位）
    cancelSlotLoadsForSlot (slotIndex);

    // 记录被移除的插件名用于“卸载中”提示（slotStates 随后会被清空）
    const auto removedName = slotStates[slotIndex].pluginName;

    // 从音频图中摘除节点（很快）；节点析构时其子进程会转交后台收割
    auto node = detachSlotNode (slotIndex);

    slotStates[slotIndex].clear();

    if (node != nullptr)
    {
        // 子进程仍在后台退出：槽位进入“卸载中”状态并屏蔽交互，
        // 子进程退出后由 onSlotNodeFullyShutDown() 解除。
        setSlotBusyState (slotIndex, PluginSlotBusyState::removing, removedName);
        node.reset();
    }
    else
    {
        channelStrip.setPluginSlotInfo (slotIndex, {}, false);
    }

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
