/*
  ==============================================================================

    ScannerData.h
    自主 VST3 扫描器的数据模型与持久化（移植自 ScannerTest 工程）。

    设计要点（针对 WaveShell / IKM 等外壳(shell)插件因仅用 32 位哈希/索引
    而加载错位的问题）：
      * CID 一律以完整的 128 位（16 字节）为唯一标识，绝不使用索引或截断值；
      * 字符串转换全部按固定长度缓冲做有界拷贝，规避宽字符截断/非空终止风险；
      * 枚举索引同时记录 setHostContext 之前 / 之后两个状态，用于诊断外壳插件
        在设置宿主上下文后发生的类索引移位。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>

#include <array>
#include <map>
#include <utility>
#include <vector>

namespace minixer
{
namespace vst3scan
{

//==============================================================================

struct CID16
{
    std::array<juce::uint8, 16> bytes{};

    bool isValid() const noexcept  { return bytes != std::array<juce::uint8, 16>{}; }

    static CID16 fromRaw (const void* tuid) noexcept
    {
        CID16 c;
        if (tuid != nullptr)
            std::memcpy (c.bytes.data(), tuid, c.bytes.size());
        return c;
    }

    static CID16 fromHex (const juce::String& hex) noexcept
    {
        CID16 c;
        juce::String h = hex.trim();
        if (h.length() != 32)
            return c;

        for (int i = 0; i < 16; ++i)
        {
            const int hi = hexDigitValue (h[i * 2]);
            const int lo = hexDigitValue (h[i * 2 + 1]);
            if (hi < 0 || lo < 0)
                return CID16{};
            c.bytes[(size_t) i] = (juce::uint8) ((hi << 4) | lo);
        }
        return c;
    }

    juce::String toHex() const
    {
        juce::String result;
        result.preallocateBytes (32);
        for (size_t i = 0; i < bytes.size(); ++i)
            result += juce::String::toHexString (bytes[i]).paddedLeft ('0', 2).toUpperCase();
        return result;
    }

    bool operator== (const CID16& o) const noexcept { return bytes == o.bytes; }
    bool operator!= (const CID16& o) const noexcept { return bytes != o.bytes; }
    bool operator<  (const CID16& o) const noexcept { return bytes <  o.bytes; }

private:
    static int hexDigitValue (juce::juce_wchar d) noexcept
    {
        if (d >= '0' && d <= '9') return d - '0';
        if (d >= 'a' && d <= 'f') return d - 'a' + 10;
        if (d >= 'A' && d <= 'F') return d - 'A' + 10;
        return -1;
    }
};

//==============================================================================
namespace ScannerUtil
{
    inline juce::String asciiBounded (const char* src, int maxLen) noexcept
    {
        if (src == nullptr || maxLen <= 0)
            return {};
        int n = 0;
        while (n < maxLen && src[n] != 0)
            ++n;
        // 按 UTF-8 解释：字节 >0x7F 不触发 juce::String 的 ASCII 断言，也避免宽字符截断
        return juce::String (juce::CharPointer_UTF8 (src), (size_t) n);
    }

    inline juce::String utf16Bounded (const juce::uint16* src, int maxUnits) noexcept
    {
        if (src == nullptr || maxUnits <= 0)
            return {};
        int n = 0;
        while (n < maxUnits && src[n] != 0)
            ++n;
        return juce::String (juce::CharPointer_UTF16 (reinterpret_cast<const juce::CharPointer_UTF16::CharType*> (src)),
                             (size_t) n);
    }

    inline juce::String hex32 (juce::uint32 v) noexcept
    {
        return ("0x" + juce::String::toHexString (v).paddedLeft ('0', 8)).toUpperCase();
    }
}

//==============================================================================

struct PluginClassRecord
{
    CID16    cid;                       // 完整 128 位 CID —— 唯一标识
    juce::String category;              // 如 "Audio Module Class" / "Component Controller Class"
    juce::String name;                  // PClassInfo 的 ASCII 名称
    juce::String nameUnicode;           // PClassInfoW 的 UTF-16 名称（显示优先级更高）
    juce::int32 cardinality  = 0;       // kManyInstances 等
    juce::uint32 classFlags  = 0;       // PClassInfo2 的 classFlags
    juce::String subCategories;         // PClassInfo2 的子类别（OR 文本）
    juce::String vendor;                // 类级厂商（可覆盖工厂级）
    juce::String version;               // 类版本
    juce::String sdkVersion;            // 构建所用 SDK 版本
    juce::int32 indexPreContext  = -1;  // setHostContext 之前的类索引（JUCE 旧扫描路径的“失效索引”）
    juce::int32 indexPostContext = -1;  // setHostContext 之后的类索引（本工具的规范索引）

    juce::String displayName() const
    {
        auto n = nameUnicode.trim();
        if (n.isNotEmpty())
            return n;
        return name.trim();
    }

    void toXml (juce::XmlElement& parent) const;
    void readXml  (const juce::XmlElement& el);
};

//==============================================================================

struct PluginModuleRecord
{
    juce::String filePath;
    juce::String fileName;
    juce::String fileModified;          // ISO 8601
    juce::int64  fileSize  = 0;

    bool      loaded      = false;      // 是否成功加载并枚举
    juce::String loadError;             // 非空表示模块加载/枚举失败

    juce::String factoryVendor;
    juce::String factoryUrl;
    juce::String factoryEmail;
    juce::int32  factoryFlags = 0;

    juce::int32 classCountPre  = -1;    // 设置宿主上下文前的类总数（外壳插件常变大）
    juce::int32 classCountPost = -1;    // 设置宿主上下文后的类总数
    juce::int32 moduleInfoClassCount = -1;  // moduleinfo.json 快照类数（-1 表示无快照/未解析）
    bool supportsFactory2 = false;
    bool supportsFactory3 = false;
    bool hostContextSet   = false;      // 是否按“修复”在枚举前调用了 setHostContext

    std::vector<PluginClassRecord> classes;

    void toXml (juce::XmlElement& parent) const;
    bool readXml (const juce::XmlElement& el);
};

//==============================================================================

struct ScannerDatabase
{
    juce::StringArray scanDirectories;      // 用户添加的扫描根目录
    std::vector<PluginModuleRecord> modules;

    const PluginModuleRecord* findModule (const juce::String& path) const;
    juce::StringArray         allFiles()   const;  // 所有已记录模块的路径

    bool saveToFile (const juce::File& file, juce::String& errorOut) const;
    bool loadFromFile (const juce::File& file, juce::String& errorOut);
};

//==============================================================================
// 内联实现
//==============================================================================
inline void PluginClassRecord::toXml (juce::XmlElement& parent) const
{
    auto* el = new juce::XmlElement ("Class");
    el->setAttribute ("cid",           cid.toHex());
    el->setAttribute ("category",      category);
    el->setAttribute ("name",          name);
    el->setAttribute ("nameUnicode",   nameUnicode);
    el->setAttribute ("cardinality",   cardinality);
    el->setAttribute ("classFlags",    juce::String::toHexString (classFlags));
    el->setAttribute ("subCategories", subCategories);
    el->setAttribute ("vendor",        vendor);
    el->setAttribute ("version",       version);
    el->setAttribute ("sdkVersion",    sdkVersion);
    el->setAttribute ("indexPre",      indexPreContext);
    el->setAttribute ("indexPost",     indexPostContext);
    parent.addChildElement (el);
}

inline void PluginClassRecord::readXml (const juce::XmlElement& el)
{
    cid = CID16::fromHex (el.getStringAttribute ("cid"));
    category      = el.getStringAttribute ("category");
    name          = el.getStringAttribute ("name");
    nameUnicode   = el.getStringAttribute ("nameUnicode");
    cardinality   = el.getIntAttribute  ("cardinality");
    classFlags    = (juce::uint32) el.getStringAttribute ("classFlags").getHexValue32();
    subCategories = el.getStringAttribute ("subCategories");
    vendor        = el.getStringAttribute ("vendor");
    version       = el.getStringAttribute ("version");
    sdkVersion    = el.getStringAttribute ("sdkVersion");
    indexPreContext  = el.getIntAttribute ("indexPre", -1);
    indexPostContext = el.getIntAttribute ("indexPost", -1);
}

inline void PluginModuleRecord::toXml (juce::XmlElement& parent) const
{
    auto* el = new juce::XmlElement ("Module");
    el->setAttribute ("filePath",       filePath);
    el->setAttribute ("fileName",       fileName);
    el->setAttribute ("fileModified",   fileModified);
    el->setAttribute ("fileSize",       juce::String ((juce::int64) fileSize));
    el->setAttribute ("loaded",         loaded);
    el->setAttribute ("loadError",      loadError);
    el->setAttribute ("factoryVendor",  factoryVendor);
    el->setAttribute ("factoryUrl",     factoryUrl);
    el->setAttribute ("factoryEmail",   factoryEmail);
    el->setAttribute ("factoryFlags",   factoryFlags);
    el->setAttribute ("classCountPre",  classCountPre);
    el->setAttribute ("classCountPost", classCountPost);
    el->setAttribute ("moduleInfoClasses", moduleInfoClassCount);
    el->setAttribute ("supportsFactory2", supportsFactory2);
    el->setAttribute ("supportsFactory3", supportsFactory3);
    el->setAttribute ("hostContextSet",   hostContextSet);
    for (const auto& c : classes)
        c.toXml (*el);
    parent.addChildElement (el);
}

inline bool PluginModuleRecord::readXml (const juce::XmlElement& el)
{
    filePath    = el.getStringAttribute ("filePath");
    if (filePath.isEmpty())
        return false;
    fileName      = el.getStringAttribute ("fileName");
    fileModified  = el.getStringAttribute ("fileModified");
    fileSize      = (juce::int64) el.getDoubleAttribute ("fileSize");
    loaded        = el.getBoolAttribute   ("loaded");
    loadError     = el.getStringAttribute ("loadError");
    factoryVendor = el.getStringAttribute ("factoryVendor");
    factoryUrl    = el.getStringAttribute ("factoryUrl");
    factoryEmail  = el.getStringAttribute ("factoryEmail");
    factoryFlags  = el.getIntAttribute    ("factoryFlags");
    classCountPre  = el.getIntAttribute ("classCountPre", -1);
    classCountPost = el.getIntAttribute ("classCountPost", -1);
    moduleInfoClassCount = el.getIntAttribute ("moduleInfoClasses", -1);
    supportsFactory2 = el.getBoolAttribute ("supportsFactory2");
    supportsFactory3 = el.getBoolAttribute ("supportsFactory3");
    hostContextSet   = el.getBoolAttribute ("hostContextSet");

    classes.clear();
    for (auto* child : el.getChildIterator())
        if (child->hasTagName ("Class"))
        {
            PluginClassRecord cr;
            cr.readXml (*child);
            if (cr.cid.isValid())           // 过滤损坏/截断的 CID 记录
                classes.push_back (std::move (cr));
        }
    return true;
}

inline const PluginModuleRecord* ScannerDatabase::findModule (const juce::String& path) const
{
    for (const auto& m : modules)
        if (m.filePath == path)
            return &m;
    return nullptr;
}

inline juce::StringArray ScannerDatabase::allFiles() const
{
    juce::StringArray files;
    for (const auto& m : modules)
        if (m.fileSize >= 0 && ! m.filePath.isEmpty())
            files.add (m.filePath);
    return files;
}

inline bool ScannerDatabase::saveToFile (const juce::File& file, juce::String& errorOut) const
{
    juce::XmlElement root ("ScannerDatabase");
    root.setAttribute ("version", 1);
    root.setAttribute ("note", L"VST3 扫描数据库：类以完整 128 位 CID 为唯一标识。");

    {
        auto* dirs = new juce::XmlElement ("ScanDirectories");
        for (const auto& d : scanDirectories)
        {
            auto* e = new juce::XmlElement ("Directory");
            e->setAttribute ("path", d);
            dirs->addChildElement (e);
        }
        root.addChildElement (dirs);
    }

    {
        auto* mods = new juce::XmlElement ("Modules");
        for (const auto& m : modules)
            m.toXml (*mods);
        root.addChildElement (mods);
    }

    if (root.writeTo (file))
        return true;

    errorOut = L"无法写入文件: " + file.getFullPathName();
    return false;
}

inline bool ScannerDatabase::loadFromFile (const juce::File& file, juce::String& errorOut)
{
    std::unique_ptr<juce::XmlElement> root (juce::XmlDocument::parse (file));
    if (root == nullptr || ! root->hasTagName ("ScannerDatabase"))
    {
        errorOut = L"不是有效的扫描数据库 XML: " + file.getFullPathName();
        return false;
    }

    ScannerDatabase loaded;
    if (auto* dirs = root->getChildByName ("ScanDirectories"))
        for (auto* e : dirs->getChildIterator())
            if (e->hasTagName ("Directory"))
            {
                const auto p = e->getStringAttribute ("path").trim();
                if (p.isNotEmpty())
                    loaded.scanDirectories.addIfNotAlreadyThere (p);
            }

    if (auto* mods = root->getChildByName ("Modules"))
    {
        int bad = 0;
        for (auto* mEl : mods->getChildIterator())
            if (mEl->hasTagName ("Module"))
            {
                PluginModuleRecord rec;
                if (rec.readXml (*mEl))
                    loaded.modules.push_back (std::move (rec));
                else
                    ++bad;
            }
        if (bad > 0)
            errorOut = juce::String (bad) + L" 个模块记录因路径缺失被跳过。";
    }

    *this = std::move (loaded);
    return true;
}

} // namespace vst3scan
} // namespace minixer