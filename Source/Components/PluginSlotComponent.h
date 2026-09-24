#pragma once

#include <JuceHeader.h>
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

//==============================================================================
/** 自定义 Bypass 按钮，根据 bypass 状态显示不同的颜色。

    - 正常状态（未 bypass）：蓝色背景，白色文字
    - bypass 状态：灰色背景，红色文字
    - 文字始终显示 "B"，不改变大小
*/
class BypassButton : public juce::Button
{
public:
    BypassButton() : juce::Button ("BypassButton") {}

    void setBypassState (bool isBypassed)
    {
        bypassed = isBypassed;
        repaint();
    }

    bool getBypassState() const noexcept { return bypassed; }

protected:
    void paintButton (juce::Graphics& g, bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override
    {
        auto bounds = getLocalBounds().toFloat().reduced (1.0f);

        // 背景颜色用灰色
        juce::Colour bgColor;
        bgColor = MixerLookAndFeel::getSurfaceColour();

        // 高亮和按下状态调整
        if (shouldDrawButtonAsDown)
            bgColor = bgColor.darker (0.2f);
        else if (shouldDrawButtonAsHighlighted)
            bgColor = bgColor.brighter (0.1f);

        g.setColour (bgColor);
        g.fillRoundedRectangle (bounds, 4.0f);

        // 边框
        g.setColour (MixerLookAndFeel::getBorderColour());
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        // 文字颜色：bypassed 时用红色，否则用白色
        auto textColor = bypassed ? juce::Colour (0xFFE74C3C) : juce::Colours::white;

        // 不可用时（空槽 / 加载中）降低不透明度，明确表示当前不可操作
        if (! isEnabled())
            textColor = textColor.withAlpha (0.35f);

        g.setColour (textColor);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawText ("B", bounds, juce::Justification::centred);
    }

private:
    bool bypassed = false;
};

//==============================================================================
/** 插件槽位的“进行中”状态。

    槽位在加载/卸载这类需要等待 PluginHost 子进程的耗时操作期间进入对应状态：
    显示进度提示与动态进度条，并屏蔽加载/打开编辑器/拖拽等交互，避免出现
    “正在加载却还能再次点开加载”“正在卸载却还能操作”这类交互逻辑问题。
*/
enum class PluginSlotBusyState
{
    none,       /**< 空闲：正常显示空槽或插件名。 */
    loading,    /**< 正在加载插件：可取消。 */
    removing    /**< 正在卸载插件（后台等待子进程退出）：不可取消。 */
};

//==============================================================================
/** 插件槽位组件（参考 FL Studio 混音台机架风格）。

    布局：
    - 左侧：独立的旁通开关（始终可见，一键启用/禁用）
    - 中间：槽位编号 + 插件名（空槽显示 Empty，加载/卸载中显示进行中提示）
    - 右侧：空槽时显示 "+" 用于加载插件，有插件时显示 "X" 用于删除，
             加载中显示 "X" 用于取消本次加载，卸载中禁用

    交互：
    - 左键点击槽位：打开插件编辑器（有插件）或加载插件（空槽）
    - 右键菜单：Load / Replace / Bypass / Delete / Copy / Paste
    - 加载中：屏蔽上述交互，仅允许取消加载；卸载中：屏蔽全部交互
    - 全局快捷键 deleteFocusedSlot：删除当前焦点槽位
*/
class PluginSlotComponent  : public juce::Component,
                             public juce::Button::Listener
{
public:
    //==============================================================================
    class Listener
    {
    public:
        virtual ~Listener() = default;

        virtual void pluginSlotClicked (int slotIndex) = 0;
        virtual void pluginSlotReplaceRequested (int slotIndex) = 0;
        virtual void pluginSlotBypassToggled (int slotIndex, bool shouldBypass) = 0;
        virtual void pluginSlotDeleteRequested (int slotIndex) = 0;
        virtual void pluginSlotCopyRequested (int slotIndex) = 0;
        virtual void pluginSlotPasteRequested (int slotIndex) = 0;
        virtual void pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex) = 0;

        /** 加载中点击“取消”按钮或右键“Cancel loading”时调用。 */
        virtual void pluginSlotLoadCancelRequested (int slotIndex) = 0;
    };

    //==============================================================================
    PluginSlotComponent (int slotIndex, int totalNumSlots);
    ~PluginSlotComponent() override = default;

    //==============================================================================
    int getSlotIndex() const noexcept { return slotIndex; }

    /** 设置槽位显示的插件信息。传入空字符串表示空槽位。 */
    void setPluginInfo (const juce::String& pluginName, bool isBypassed);

    /** 设置槽位的进行中状态（空闲 / 加载中 / 卸载中）。

        进入进行中状态时槽位会显示对应提示与动态进度条，并屏蔽交互；加载中只保留
        “取消加载”，卸载中不可取消。进行中期间槽位原有的插件信息仍会保留，
        供替换场景（旧插件继续工作到新插件就绪）使用。
    */
    void setPluginBusyState (PluginSlotBusyState newState, const juce::String& busyPluginName = {});

    const juce::String& getPluginName() const noexcept { return pluginName; }
    bool hasPlugin() const noexcept { return pluginName.isNotEmpty(); }
    bool isBypassed() const noexcept { return bypassed; }

    PluginSlotBusyState getBusyState() const noexcept { return busyState; }
    bool isLoading() const noexcept { return busyState == PluginSlotBusyState::loading; }
    bool isRemoving() const noexcept { return busyState == PluginSlotBusyState::removing; }
    bool isBusy() const noexcept { return busyState != PluginSlotBusyState::none; }

    /** 设置旁通状态，并通知 listener。 */
    void setBypassed (bool shouldBypass);

    /** 设置当前槽位是否作为拖拽放置目标高亮显示。 */
    void setDropTargetHighlighted (bool shouldHighlight);

    //==============================================================================
    void addListener (Listener* listener) { listeners.add (listener); }
    void removeListener (Listener* listener) { listeners.remove (listener); }

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;
    void buttonClicked (juce::Button* button) override;
    void mouseDown (const juce::MouseEvent& event) override;
    void mouseDrag (const juce::MouseEvent& event) override;
    void mouseUp (const juce::MouseEvent& event) override;
    void mouseDoubleClick (const juce::MouseEvent& event) override;
    void focusOfChildComponentChanged (FocusChangeType cause) override;

private:
    //==============================================================================
    enum class HitArea { none, bypass, slot, action };

    HitArea getHitArea (juce::Point<int> pos) const;
    void showContextMenu (juce::Point<int> clickPos);
    void toggleBypass();

    /** 依据 busyState / pluginName / bypassed 统一刷新文本与可用状态。 */
    void updateDisplay();

    //==============================================================================
    int slotIndex;
    int totalNumSlots;
    juce::String pluginName;
    bool bypassed = false;

    PluginSlotBusyState busyState = PluginSlotBusyState::none;
    juce::String busyPluginName;

    juce::TextButton slotButton;
    BypassButton bypassButton;
    juce::TextButton actionButton { "+" };

    bool dropTargetHighlighted = false;
    bool isDragging = false;
    HitArea mouseDownHitArea = HitArea::none;
    juce::Point<int> mouseDownPos;
    static constexpr int dragThresholdPixels = 6;

    juce::ListenerList<Listener> listeners;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginSlotComponent)
};

} // namespace minixer