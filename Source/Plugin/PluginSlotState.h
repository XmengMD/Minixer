/*
  ==============================================================================

    PluginSlotState.h
    描述单个插件槽位的可持久化状态。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

namespace minixer
{

//==============================================================================
/** 单个插件槽位的持久化状态。

    用于预设保存/加载与剪贴板：保存插件唯一标识、显示名称以及
    AudioProcessor::getStateInformation() 得到二进制状态。
    bypassed 仅用于运行时状态与剪贴板标记，不写入预设文件。
*/
struct PluginSlotState
{
    /** 插件唯一标识（PluginDescription::createIdentifierString()）。 */
    juce::String pluginIdentifier;

    /** 插件显示名称。 */
    juce::String pluginName;

    /** 槽位是否被旁通（仅运行时/剪贴板使用，不写入预设 XML）。 */
    bool bypassed = false;

    /** 插件私有状态（Base64 编码后写入 XML）。 */
    juce::MemoryBlock pluginState;

    //==============================================================================
    bool isValid() const noexcept { return pluginIdentifier.isNotEmpty(); }

    void clear()
    {
        pluginIdentifier.clear();
        pluginName.clear();
        bypassed = false;
        pluginState.reset();
    }

    //==============================================================================
    /** 序列化为 <Slot> XML 元素。调用方负责设置 index 等附加属性。 */
    std::unique_ptr<juce::XmlElement> toXml() const
    {
        auto xml = std::make_unique<juce::XmlElement> ("Slot");
        xml->setAttribute ("identifier", pluginIdentifier);
        xml->setAttribute ("name",       pluginName);

        if (pluginState.getSize() > 0)
            xml->setAttribute ("state", pluginState.toBase64Encoding());

        return xml;
    }

    //==============================================================================
    /** 从 <Slot> XML 元素反序列化。 */
    bool fromXml (const juce::XmlElement& xml)
    {
        clear();

        if (! xml.hasAttribute ("identifier"))
            return false;

        pluginIdentifier = xml.getStringAttribute ("identifier");
        pluginName       = xml.getStringAttribute ("name");

        auto stateString = xml.getStringAttribute ("state");
        if (stateString.isNotEmpty())
            pluginState.fromBase64Encoding (stateString);

        return isValid();
    }
};

} // namespace minixer
