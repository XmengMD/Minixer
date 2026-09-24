#pragma once

#include <JuceHeader.h>
#include <memory>
#include <optional>
#include "LookAndFeel/MixerLookAndFeel.h"
#include "Components/ChannelStripComponent.h"
#include "Components/SettingsComponent.h"
#include "Components/PresetBarComponent.h"
#include "Components/LevelMeterProcessor.h"
#include "Plugin/PluginSlotState.h"
#include "Plugin/PluginEditorWindow.h"
#include "Plugin/PluginBridgeNode.h"
#include "Settings/KeyboardShortcut.h"
#include "Settings/MidiShortcutInputManager.h"
#include "Settings/HidShortcutInputManager.h"

namespace minixer
{

//==============================================================================
/** 将单声道输入上混为立体声的简单处理器。

    用于解决硬件麦克风输入通常为单声道、输出目标为立体声的问题。
*/
class MonoToStereoProcessor  : public juce::AudioProcessor
{
public:
    MonoToStereoProcessor();

    const juce::String getName() const override { return "Mono To Stereo"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MonoToStereoProcessor)
};

//==============================================================================
/** 将立体声输入下混为单声道的简单处理器（L+R 求和）。

    用于输出设备为单声道时避免静默丢弃 R 声道（与 MonoToStereoProcessor 对称）。
*/
class StereoToMonoProcessor  : public juce::AudioProcessor
{
public:
    StereoToMonoProcessor();

    const juce::String getName() const override { return "Stereo To Mono"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (StereoToMonoProcessor)
};

//==============================================================================
/** 输入增益微调处理器。 */
class InputTrimProcessor  : public juce::AudioProcessor
{
public:
    InputTrimProcessor();

    const juce::String getName() const override { return "Input Trim"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void setTrimDb (float trimDb);
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    juce::Atomic<float> trimGain { 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (InputTrimProcessor)
};

//==============================================================================
/** 通道条后级处理器：声像、立体声宽度、输出推子。 */
class ChannelStripProcessor  : public juce::AudioProcessor
{
public:
    ChannelStripProcessor();

    const juce::String getName() const override { return "Channel Strip"; }
    void prepareToPlay (double /*sampleRate*/, int /*maximumExpectedSamplesPerBlock*/) override {}
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& /*midiMessages*/) override;
    double getTailLengthSeconds() const override { return 0.0; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

    void setPan (float pan);
    void setStereoSeparation (float separationPercent);
    void setOutputDb (float outputDb);
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

private:
    juce::Atomic<float> panLeft { 1.0f };
    juce::Atomic<float> panRight { 1.0f };
    juce::Atomic<float> sideGain { 1.0f };
    juce::Atomic<float> outputGain { 1.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStripProcessor)
};

//==============================================================================
/** 插件异步加载的共享槽位状态（完整定义见 MainComponent.cpp）。
    后台线程把结果写入槽位，消息线程的定时器统一取回应用；用 shared_ptr 保证
    即使进程退出时仍有后台任务在途，也不会悬空访问。
*/
struct PluginLoadSlot;

//==============================================================================
/** Minixer 主界面组件。

    负责：
    - 音频引擎初始化（AudioDeviceManager + AudioProcessorGraph + AudioProcessorPlayer）
    - UI 布局（预设栏、通道条、设备选择器）
    - 实时电平表数据桥接
    - 插件槽事件处理（第一阶段仅打印日志/弹窗提示）
*/
class MainComponent  : public juce::DragAndDropContainer,
                       public juce::Component,
                       public juce::Timer,
                       public juce::KeyListener,
                       public juce::ChangeListener,
                       public ChannelStripComponent::Listener,
                       public PresetBarComponent::Listener,
                       public SettingsComponent::Listener,
                       public PluginEditorWindow::Listener,
                       public PluginBridgeNode::Listener,
                       public MidiShortcutInputManager::Listener,
                       public HidShortcutInputManager::Listener
{
public:
    //==============================================================================
    MainComponent();
    ~MainComponent() override;

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    // juce::Timer
    void timerCallback() override;

    //==============================================================================
    // juce::KeyListener
    bool keyPressed (const juce::KeyPress& key, juce::Component* originatingComponent) override;
    bool keyStateChanged (bool isKeyDown, juce::Component* originatingComponent) override;

    //==============================================================================
    // juce::ChangeListener
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    //==============================================================================
    // ChannelStripComponent::Listener
    void channelStripParameterChanged() override;
    void pluginSlotClicked (int slotIndex) override;
    void pluginSlotReplaceRequested (int slotIndex) override;
    void pluginSlotBypassToggled (int slotIndex, bool shouldBypass) override;
    void pluginSlotDeleteRequested (int slotIndex) override;
    void pluginSlotCopyRequested (int slotIndex) override;
    void pluginSlotPasteRequested (int slotIndex) override;
    void pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex) override;
    void pluginSlotLoadCancelRequested (int slotIndex) override;

    //==============================================================================
    // PresetBarComponent::Listener
    void savePresetRequested (const juce::File& presetFile) override;
    void loadPresetRequested (const juce::File& presetFile) override;
    void deletePresetRequested (const juce::File& presetFile) override;
    void settingsRequested() override;

    //==============================================================================
    // SettingsComponent::Listener
    void audioSettingsChanged() override;
    void preferencesChanged() override;
    void shortcutsChanged() override;

    //==============================================================================
    // PluginEditorWindow::Listener
    void pluginEditorWindowClosed (PluginEditorWindow* window) override;

    //==============================================================================
    // PluginBridgeNode::Listener
    void pluginBridgeNodeCrashed (PluginBridgeNode* node) override;

    //==============================================================================
    // MidiShortcutInputManager::Listener
    void midiShortcutMessageReceived (const juce::MidiMessage& message) override;

    // HidShortcutInputManager::Listener
    void hidShortcutEventReceived (const HidShortcutEvent& event) override;

private:
    //==============================================================================
    void setupAudioGraph();
    void reconfigureAudioGraphForCurrentDevice();
    void updateMonoDeviceState();
    bool ensureStereoChannelsIfAvailable();
    void toggleSettings();
    void applyStartupSettings();

    //==============================================================================
    // 快捷键处理
    bool handleGlobalShortcut (const juce::KeyPress& key);
    bool handleSlotShortcutPressed (const juce::KeyPress& key);
    bool handleSlotShortcutSourcePressed (const ShortcutInputSource& source);
    void releaseHeldSlotShortcuts();
    void setSlotBypassState (int slotIndex, bool shouldBypass, bool updateStatus = true);
    void applySlotBypassDefault (int slotIndex);
    void applyShortcutDefaults();

    // 系统全局插槽快捷键（通过定时轮询检测键盘；MIDI/HID 为事件驱动）
    void pollGlobalSlotShortcuts();
    void handleGlobalSlotShortcutState (int slotIndex, bool isKeyDown);

    //==============================================================================
    // 插件管理
    void showPluginSelectionMenu (int slotIndex);
    void loadPluginIntoSlot (int slotIndex, const juce::PluginDescription& description,
                             const std::optional<PluginSlotState>& stateToRestore = {});

    // 异步桥接加载（所有架构均通过 PluginHost 子进程，UI 不再被插件加载/校验阻塞）
    bool startSlotLoad (int slotIndex, const juce::PluginDescription& description,
                        PluginArchitecture arch, double sampleRate, int bufferSize,
                        const std::optional<PluginSlotState>& stateToRestore);
    void cancelSlotLoadsForSlot (int slotIndex);
    void processSlotLoadResults();

    /** 设置槽位进行中状态（同时刷新槽位组件的提示显示与交互屏蔽）。 */
    void setSlotBusyState (int slotIndex, PluginSlotBusyState busyState, const juce::String& pluginName = {});

    /** 槽位是否处于进行中状态（加载中 / 卸载中）。 */
    bool isSlotBusy (int slotIndex) const noexcept;

    /** 从音频图中摘除指定槽位当前的插件节点并返回其引用（调用方决定销毁时机）。

        摘除本身很快；节点析构时会把其子进程交给 PluginHostProcessReaper 在后台
        等待退出，因此无论谁在哪个线程析构都不会阻塞。
    */
    juce::AudioProcessorGraph::Node::Ptr detachSlotNode (int slotIndex);

    /** 槽位插件子进程完全退出后（消息线程）解除“卸载中”状态。 */
    void onSlotNodeFullyShutDown (int slotIndex);

    void removePluginFromSlot (int slotIndex, bool rebuildChain);
    void rebuildPluginChain();
    void openPluginEditor (int slotIndex);
    void closePluginEditorForProcessor (juce::AudioProcessor* processor);
    void closeAllPluginEditors();
    void setBypassForAllPlugins (bool shouldBypass);

    //==============================================================================
    // 预设管理
    juce::XmlElement* saveSlotStatesToXml (juce::XmlElement& parent) const;
    void loadSlotStatesFromXml (const juce::XmlElement& parent);
    PluginSlotState getSlotState (int slotIndex) const;
    void applySlotState (int slotIndex, const PluginSlotState& state);

    // 插件槽位移动
    void refreshSlotDisplays();
    void moveSlotContent (int fromIndex, int toIndex);

    // 通道条参数序列化
    void saveChannelStripStateToXml (juce::XmlElement& parent);
    void loadChannelStripStateFromXml (const juce::XmlElement& parent);

    //==============================================================================
    static void connectNodes (juce::AudioProcessorGraph& graph,
                              juce::AudioProcessorGraph::Node::Ptr source,
                              juce::AudioProcessorGraph::Node::Ptr dest);


    //==============================================================================
    MixerLookAndFeel mixerLookAndFeel;

    juce::AudioDeviceManager audioDeviceManager;
    juce::AudioProcessorPlayer processorPlayer;
    std::unique_ptr<juce::AudioProcessorGraph> audioGraph;

    ChannelStripComponent channelStrip;
    SettingsComponent settingsPanel;
    PresetBarComponent presetBar;

    juce::Label statusLabel;
    juce::Label monoDeviceLabel;

    // 电平数据：使用 JUCE 原子浮点避免音频线程与 GUI 线程竞争
    juce::Atomic<float> inputPeakDb[2]  { { -60.0f }, { -60.0f } };
    juce::Atomic<float> inputRmsDb[2]   { { -60.0f }, { -60.0f } };
    juce::Atomic<float> inputLufsM      { -60.0f };
    juce::Atomic<float> inputLufsS      { -60.0f };

    juce::Atomic<float> outputPeakDb[2] { { -60.0f }, { -60.0f } };
    juce::Atomic<float> outputRmsDb[2]  { { -60.0f }, { -60.0f } };
    juce::Atomic<float> outputLufsM     { -60.0f };
    juce::Atomic<float> outputLufsS     { -60.0f };

    /** 当前设备是否为单声道输入。 */
    bool isMonoDevice = false;

    /** 当前设备是否为单声道输出（启用输出通道数为 1）。 */
    bool isMonoOutputDevice = false;

    /** 防止 ensureStereoChannelsIfAvailable() 中 setAudioDeviceSetup 触发递归。 */
    bool isReconfiguringDevice = false;

    // 音频图关键节点
    juce::AudioProcessorGraph::Node::Ptr inputNode;
    juce::AudioProcessorGraph::Node::Ptr inputMeterNode;
    juce::AudioProcessorGraph::Node::Ptr inputTrimNode;
    juce::AudioProcessorGraph::Node::Ptr monoToStereoNode;
    juce::AudioProcessorGraph::Node::Ptr downMixNode;
    juce::AudioProcessorGraph::Node::Ptr outputMeterNode;
    juce::AudioProcessorGraph::Node::Ptr channelStripNode;
    juce::AudioProcessorGraph::Node::Ptr outputNode;

    // 每个槽位对应的插件节点
    std::array<juce::AudioProcessorGraph::Node::Ptr, defaultNumPluginSlots> pluginSlotNodes;

    // 插件后台加载线程池（少量并发，UI 线程不参与）
    std::unique_ptr<juce::ThreadPool> pluginLoaderPool;

    std::array<std::shared_ptr<PluginLoadSlot>, defaultNumPluginSlots> pluginLoadSlots;

    // 每个槽位的持久化状态（用于预设保存/加载）
    std::array<PluginSlotState, defaultNumPluginSlots> slotStates;

    // 每个槽位的进行中状态与目标插件名（消息线程维护，用于槽位状态显示与交互屏蔽）
    std::array<PluginSlotBusyState, defaultNumPluginSlots> slotBusyState {};
    std::array<juce::String, defaultNumPluginSlots> slotBusyPluginName;

    // 该槽位的卸载是否由用户直接发起：卸载完成后据此回写状态栏
    //（预设批量清空不置位，避免刷屏覆盖“Loaded ...”提示）
    std::array<bool, defaultNumPluginSlots> slotRemovalStatusPending {};

    // 已打开的插件编辑器窗口
    std::vector<std::unique_ptr<PluginEditorWindow>> pluginEditorWindows;

    // 应用内插件剪贴板（复制/粘贴）
    PluginSlotState copiedSlotState;

    // 所有插件是否被全局旁通
    bool allPluginsBypassed = false;

    // 当前处于 hold 模式且按键尚未释放的插槽索引
    juce::Array<int> heldSlotShortcutIndices;

    // 上一帧各插槽快捷键的全局按下状态，避免焦点外重复触发
    std::array<bool, defaultNumPluginSlots> previousGlobalShortcutStates {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};

} // namespace minixer
