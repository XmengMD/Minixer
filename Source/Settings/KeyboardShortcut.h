#pragma once

#include <JuceHeader.h>
#include "../Components/ChannelStripComponent.h"
#include "ShortcutInputSource.h"

namespace minixer
{

//==============================================================================
/** 可在设置中配置的全局动作。 */
enum class GlobalShortcutAction
{
    bringWindowToFront,
    toggleAllPluginsBypass,
    toggleSettingsPanel,
    deleteFocusedSlot,

    numActions
};

//==============================================================================
/** 插件槽 bypass 快捷键的触发模式。 */
enum class SlotShortcutMode
{
    cycleToggle,    /**< 点按循环：每次按下在旁通/不旁通之间切换。 */
    holdToggle      /**< 长按切换：按住时临时进入相反状态，释放后恢复默认旁通状态。 */
};

//==============================================================================
/** 单个插件槽的快捷键配置。 */
struct SlotShortcut
{
    ShortcutInputSource inputSource;    /**< 快捷键输入源；isValid() 为 false 表示未设置。 */
    bool defaultBypassed = false;       /**< 默认是否旁通（preset 加载完成/关闭全局旁通时应用；长按模式释放后恢复至此状态）。 */
    SlotShortcutMode mode = SlotShortcutMode::cycleToggle;

    bool isAssigned() const noexcept { return inputSource.isValid(); }
    void clear() { inputSource = {}; defaultBypassed = false; mode = SlotShortcutMode::cycleToggle; }

    /** 如果输入源是键盘按键，则返回该按键；否则返回无效 KeyPress。 */
    juce::KeyPress getKeyPress() const noexcept { return inputSource.isKeyboard() ? inputSource.keyPress : juce::KeyPress(); }

    std::unique_ptr<juce::XmlElement> toXml (const juce::String& tagName) const;
    bool fromXml (const juce::XmlElement& xml);
};

//==============================================================================
/** 应用级快捷键配置，负责持久化与查询。 */
class ShortcutSettings
{
public:
    //==============================================================================
    ShortcutSettings();

    //==============================================================================
    /** 重置为默认快捷键（保留当前未设置项为空）。 */
    void resetToDefaults();

    //==============================================================================
    /** 全局动作对应的快捷键。 */
    juce::KeyPress getGlobalShortcut (GlobalShortcutAction action) const;
    void setGlobalShortcut (GlobalShortcutAction action, const juce::KeyPress& key);

    //==============================================================================
    /** 插槽 bypass 快捷键配置。 */
    const SlotShortcut& getSlotShortcut (int slotIndex) const;
    void setSlotShortcut (int slotIndex, const SlotShortcut& shortcut);

    /** 查找所有绑定到指定输入源的插槽索引（允许多个插槽共享同一来源）。 */
    juce::Array<int> findSlotIndicesForInputSource (const ShortcutInputSource& source) const;

    /** 兼容旧代码：查找所有绑定到指定按键的插槽索引。 */
    juce::Array<int> findSlotIndicesForKey (const juce::KeyPress& key) const;

    //==============================================================================
    /** 将整个配置序列化为 XML。 */
    std::unique_ptr<juce::XmlElement> toXml() const;

    /** 从 XML 反序列化；失败时保持当前值。 */
    bool fromXml (const juce::XmlElement& xml);

    /** 返回适合写入 PropertiesFile 的 XML 字符串。 */
    juce::String toXmlString() const;

    /** 从 PropertiesFile 字符串加载。 */
    bool fromXmlString (const juce::String& text);

    //==============================================================================
    static juce::String getGlobalShortcutActionName (GlobalShortcutAction action);
    static juce::String getGlobalShortcutActionDescription (GlobalShortcutAction action);
    static juce::String slotShortcutModeToString (SlotShortcutMode mode);
    static SlotShortcutMode stringToSlotShortcutMode (const juce::String& text);

private:
    //==============================================================================
    std::array<juce::KeyPress, static_cast<size_t> (GlobalShortcutAction::numActions)> globalShortcuts;
    std::array<SlotShortcut, defaultNumPluginSlots> slotShortcuts;

    JUCE_LEAK_DETECTOR (ShortcutSettings)
};

} // namespace minixer
