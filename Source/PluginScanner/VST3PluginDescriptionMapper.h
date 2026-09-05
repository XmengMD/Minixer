/*
  ==============================================================================

    VST3PluginDescriptionMapper.h
    自主 VST3 扫描结果 → JUCE PluginDescription 的转换（主程序与 PluginHost 通用）。

    背景：加载期仍由 JUCE VST3PluginFormat 创建实例（按 name +
    uniqueId/deprecatedUid 哈希匹配类）。为了不修改 JUCE 源码，这里必须精确
    复刻 JUCE 的哈希算法（juce_VST3PluginFormat.cpp 的 getHashForRange /
    getNormalisedTUID，FUID 取 COM_COMPATIBLE=1 的 getLong1..4 字节序），
    保证扫描结果在加载期能被匹配到同一类。

  ==============================================================================
*/

#pragma once

#include "ScannerData.h"

namespace minixer
{
namespace vst3scan
{

//==============================================================================
// 复刻 JUCE getHashForRange(TUID)：TUID 是 char[16]，逐字节（有符号扩展）累加。
//==============================================================================
inline juce::int32 vst3DeprecatedUidHash (const CID16& cid) noexcept
{
    juce::uint32 value = 0;

    for (auto b : cid.bytes)
        value = (value * 31u) + (juce::uint32) (juce::int8) b;

    return (juce::int32) value;
}

//==============================================================================
// 复刻 JUCE getHashForRange(getNormalisedTUID())：FUID(COM_COMPATIBLE) 四字序累加。
//==============================================================================
inline juce::int32 vst3UniqueIdHash (const CID16& cid) noexcept
{
    const auto& d = cid.bytes;
    const auto b  = [&d] (size_t i) { return (juce::uint32) d[i]; };

    const juce::uint32 l1 = (b (3) << 24) | (b (2) << 16) | (b (1) << 8) | b (0);
    const juce::uint32 l2 = (b (5) << 24) | (b (4) << 16) | (b (7) << 8) | b (6);
    const juce::uint32 l3 = (b (8) << 24) | (b (9) << 16) | (b (10) << 8) | b (11);
    const juce::uint32 l4 = (b (12) << 24) | (b (13) << 16) | (b (14) << 8) | b (15);

    juce::uint32 value = 0;

    for (auto item : { l1, l2, l3, l4 })
        value = (value * 31u) + item;

    return (juce::int32) value;
}

//==============================================================================
/** 将自主扫描的一个 .vst3 模块记录转换为 PluginDescription 列表。
    仅收录音频效果类（VST3 乐器以 Instrument 子类别注册在 Audio Module Class），
    与 JUCE 自带扫描的 kVstAudioEffectClass 过滤保持一致。 */
//==============================================================================
inline void addDescriptionsFromModule (const PluginModuleRecord& rec,
                                       juce::OwnedArray<juce::PluginDescription>& result)
{
    const juce::File file (rec.filePath);
    const auto modTime = file.getLastModificationTime();
    const auto now     = juce::Time::getCurrentTime();

    for (const auto& cr : rec.classes)
    {
        if (cr.category != "Audio Module Class")
            continue;

        auto desc = std::make_unique<juce::PluginDescription>();

        desc->fileOrIdentifier   = rec.filePath;
        desc->lastFileModTime    = modTime;
        desc->lastInfoUpdateTime = now;
        desc->manufacturerName   = cr.vendor.trim().isNotEmpty() ? cr.vendor.trim()
                                                                 : rec.factoryVendor.trim();
        // name 必须等于 PClassInfo 的 ASCII 名称（加载期 findClassMatchingDescription 按 name 匹配）
        desc->name               = cr.name.trim();
        desc->descriptiveName    = cr.displayName();          // 显示名优先 Unicode（PClassInfoW）
        desc->pluginFormatName   = "VST3";
        desc->numInputChannels   = 0;                          // 加载时按插件实际总线重新配置
        desc->numOutputChannels  = 0;
        desc->version            = cr.version.trim();

        const auto sub = cr.subCategories.trim();
        desc->category     = sub.isNotEmpty() ? sub : cr.category;
        desc->isInstrument = desc->category.containsIgnoreCase ("Instrument");

        desc->deprecatedUid = vst3DeprecatedUidHash (cr.cid);
        desc->uniqueId      = vst3UniqueIdHash (cr.cid);

        result.add (std::move (desc));
    }
}

} // namespace vst3scan
} // namespace minixer