#pragma once

#include <JuceHeader.h>
#include <memory>
#include <vector>
#include "LevelMeterComponent.h"
#include "RotaryKnobComponent.h"
#include "FaderComponent.h"
#include "PluginSlotComponent.h"

namespace minixer
{

//==============================================================================
static constexpr int defaultNumPluginSlots = 12;

//==============================================================================
/** 单通道条组件。

    采用左右分栏布局：
    - 左侧：插件机架插槽（默认 12 个）
    - 右侧：独立控制区，从上到下包含 Input Trim 旋钮 + 输入电平表、
            Pan 旋钮 + Stereo separation 旋钮、Output 推子 + 输出电平表
*/
class ChannelStripComponent  : public juce::Component,
                               public juce::DragAndDropTarget,
                               public PluginSlotComponent::Listener,
                               public RotaryKnobComponent::Listener,
                               public FaderComponent::Listener
{
public:
    //==============================================================================
    class Listener
    {
    public:
        virtual ~Listener() = default;

        virtual void channelStripParameterChanged() = 0;
        virtual void pluginSlotClicked (int slotIndex) = 0;
        virtual void pluginSlotReplaceRequested (int slotIndex) = 0;
        virtual void pluginSlotBypassToggled (int slotIndex, bool shouldBypass) = 0;
        virtual void pluginSlotDeleteRequested (int slotIndex) = 0;
        virtual void pluginSlotCopyRequested (int slotIndex) = 0;
        virtual void pluginSlotPasteRequested (int slotIndex) = 0;
        virtual void pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex) = 0;
        virtual void pluginSlotLoadCancelRequested (int slotIndex) = 0;
    };

    //==============================================================================
    ChannelStripComponent (const juce::String& channelName = {});
    ~ChannelStripComponent() override = default;

    //==============================================================================
    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

    //==============================================================================
    RotaryKnobComponent& getInputTrim() noexcept       { return inputTrim; }
    RotaryKnobComponent& getPanKnob() noexcept         { return panKnob; }
    RotaryKnobComponent& getStereoSeparation() noexcept { return stereoSeparation; }
    FaderComponent& getOutputFader() noexcept          { return outputFader; }
    LevelMeterComponent& getInputMeter() noexcept      { return inputMeter; }
    LevelMeterComponent& getOutputMeter() noexcept     { return outputMeter; }

    int getNumPluginSlots() const noexcept { return static_cast<int> (pluginSlots.size()); }
    PluginSlotComponent& getPluginSlot (int index);

    /** 返回当前拥有键盘焦点的插件槽索引；无焦点时返回 -1。 */
    int getFocusedPluginSlotIndex() const;

    /** 一键设置所有已加载插件的旁通状态。 */
    void setAllPluginsBypassed (bool shouldBypass);

    //==============================================================================
    void setPluginSlotInfo (int slotIndex, const juce::String& pluginName, bool isBypassed);

    /** 设置槽位的进行中状态（加载中/卸载中显示提示并屏蔽交互）。 */
    void setPluginSlotBusyState (int slotIndex, PluginSlotBusyState busyState, const juce::String& busyPluginName);

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

    //==============================================================================
    // PluginSlotComponent::Listener
    void pluginSlotClicked (int slotIndex) override;
    void pluginSlotReplaceRequested (int slotIndex) override;
    void pluginSlotBypassToggled (int slotIndex, bool shouldBypass) override;
    void pluginSlotDeleteRequested (int slotIndex) override;
    void pluginSlotCopyRequested (int slotIndex) override;
    void pluginSlotPasteRequested (int slotIndex) override;
    void pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex) override;
    void pluginSlotLoadCancelRequested (int slotIndex) override;

    // RotaryKnobComponent::Listener
    void rotaryKnobValueChanged (RotaryKnobComponent* knob) override;

    // FaderComponent::Listener
    void faderValueChanged (FaderComponent* fader) override;

    // juce::DragAndDropTarget
    bool isInterestedInDragSource (const SourceDetails& dragSourceDetails) override;
    void itemDragEnter (const SourceDetails& dragSourceDetails) override;
    void itemDragMove (const SourceDetails& dragSourceDetails) override;
    void itemDragExit (const SourceDetails& dragSourceDetails) override;
    void itemDropped (const SourceDetails& dragSourceDetails) override;

    // juce::Component
    bool keyPressed (const juce::KeyPress& key) override;

private:
    //==============================================================================
    void notifyParameterChanged();
    int getSlotIndexAtPoint (juce::Point<int> pos) const;
    void updateDragHoverSlot (int hoverIndex);
    void moveFocusedSlot (int delta);

    //==============================================================================
    RotaryKnobComponent inputTrim { TRANS("Input Trim"), -24.0, 24.0, 0.0, 0.1, "dB" };
    LevelMeterComponent inputMeter;

    std::vector<std::unique_ptr<PluginSlotComponent>> pluginSlots;

    RotaryKnobComponent panKnob { TRANS("Pan"), -1.0, 1.0, 0.0, 0.01, "" };
    RotaryKnobComponent stereoSeparation { TRANS("Stereo"), -100.0, 100.0, 0.0, 0.1, "%" };
    FaderComponent outputFader { TRANS("Output"), -60.0, 12.0, 0.0 };
    LevelMeterComponent outputMeter;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChannelStripComponent)
};

} // namespace minixer
