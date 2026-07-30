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

        g.setColour (textColor);
        g.setFont (juce::Font (juce::FontOptions (14.0f, juce::Font::bold)));
        g.drawText ("B", bounds, juce::Justification::centred);
    }

private:
    bool bypassed = false;
};

//==============================================================================
/** 插件槽位组件（参考 FL Studio 混音台机架风格）。

    布局：
    - 左侧：独立的旁通开关（始终可见，一键启用/禁用）
    - 中间：槽位编号 + 插件名（空槽显示 Empty）
    - 右侧：空槽时显示 "+" 用于加载插件，有插件时显示 "X" 用于删除

    交互：
    - 左键点击槽位：打开插件编辑器（有插件）或加载插件（空槽）
    - 右键菜单：Load / Replace / Bypass / Delete / Copy / Paste
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
    };

    //==============================================================================
    PluginSlotComponent (int slotIndex);
    ~PluginSlotComponent() override = default;

    //==============================================================================
    int getSlotIndex() const noexcept { return slotIndex; }

    /** 设置槽位显示的插件信息。传入空字符串表示空槽位。 */
    void setPluginInfo (const juce::String& pluginName, bool isBypassed);

    const juce::String& getPluginName() const noexcept { return pluginName; }
    bool hasPlugin() const noexcept { return pluginName.isNotEmpty(); }
    bool isBypassed() const noexcept { return bypassed; }

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

    //==============================================================================
    int slotIndex;
    juce::String pluginName;
    bool bypassed = false;

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