/*
  ==============================================================================

    VST3Scanner.cpp
    自主 VST3 扫描引擎实现（移植自 ScannerTest 工程）。

    说明：
      - 直接使用 JUCE 8 内附的 VST3 SDK（Declare-Only），全部实现已由
        juce_audio_processors 模块编译进目标文件，本文件只引用声明，避免重复编译/重复符号。
      - 遵循官方修复（juce-framework/JUCE commit 3fa28d0a）：
        在查找/枚举类索引之前先对工厂调用 setHostContext(factory)。
      - 插件类实例化一律使用完整 128 位 CID（createInstance(cid, ...)），
        从不用索引定位（WaveShell 等在外壳加载后会整体重排类索引）。
      - prewarmForLoad：为加载侧修复提供“先 setHostContext”的预加载，
        解决 JUCE 自带加载路径“先解析索引、后 setHostContext”导致的外壳错位问题。

  ==============================================================================
*/

#include "VST3Scanner.h"

#include <cstring>

//------------------------------------------------------------------------------
// VST3 SDK —— 仅声明（实现由 juce_audio_processors 模块提供，勿重复编译）
//------------------------------------------------------------------------------
#define JUCE_VST3HEADERS_INCLUDE_HEADERS_ONLY 1
#include <juce_audio_processors/format_types/juce_VST3Headers.h>

#include <pluginterfaces/base/funknown.h>
#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/base/smartpointer.h>
#include <pluginterfaces/vst/vsttypes.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <public.sdk/source/vst/hosting/hostclasses.h>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

#if defined (min)
 #undef min
#endif
#if defined (max)
 #undef max
#endif

namespace minixer
{
namespace vst3scan
{

//------------------------------------------------------------------------------
namespace
{

juce::String winError (DWORD code) noexcept
{
    if (code == 0)
        return {};

    LPWSTR buffer = nullptr;
    const DWORD len = FormatMessageW (FORMAT_MESSAGE_ALLOCATE_BUFFER
                                      | FORMAT_MESSAGE_FROM_SYSTEM
                                      | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr, code, 0,
                                      reinterpret_cast<LPWSTR> (&buffer), 0, nullptr);

    if (len == 0 || buffer == nullptr)
        return L"错误代码 " + juce::String::toHexString ((juce::int64) code);

    const juce::String result (juce::CharPointer_UTF16 (buffer), (size_t) len);
    LocalFree (buffer);
    return result.trim();
}

juce::String tresultName (Steinberg::tresult r) noexcept
{
    switch (r)
    {
        case Steinberg::kResultOk:          return "kResultOk";   // kResultOk == kResultTrue == 1
        case Steinberg::kResultFalse:       return "kResultFalse";
        case Steinberg::kInvalidArgument:   return "kInvalidArgument";
        case Steinberg::kNotImplemented:    return "kNotImplemented";
        case Steinberg::kNoInterface:       return "kNoInterface";
        case Steinberg::kInternalError:     return "kInternalError";
        case Steinberg::kOutOfMemory:       return "kOutOfMemory";
        case Steinberg::kNotInitialized:    return "kNotInitialized";
        default: break;
    }
    return juce::String::toHexString ((juce::int64) r);
}

//------------------------------------------------------------------------------
// 其他操作系统/解压工具残留的判别：
//   - __MACOSX/ 目录：macOS 归档工具解压 zip 时产生的"垃圾"目录；
//   - ._* 文件：AppleDouble 元数据（存扩展属性），如 ._ReLife.vst3；
//   - .DS_Store / .Spotlight-V100 / .Trashes：macOS 文件系统索引残留。
// 市面宿主与安装程序普遍将其视为不属于当前系统的残留垃圾，直接跳过，不当作插件扫描。
bool isOsResidue (const juce::File& f)
{
    const auto name = f.getFileName();
    if (name.startsWith ("._"))
        return true;

    const juce::StringArray parts = juce::StringArray::fromTokens (f.getFullPathName(), "\\/", "");

    for (const auto& p : parts)
        if (p == "__MACOSX" || p == ".DS_Store" || p == ".Spotlight-V100" || p == ".Trashes")
            return true;

    return false;
}

//------------------------------------------------------------------------------
// 判断文件是否为有效的 Windows PE（DLL）镜像：检查 "MZ" 头与 "PE\0\0" 签名。
// 防止 AppleDouble/改名文件等"非模块"进入 LoadLibrary（也可给出更清晰的错误）。
bool isLikelyPe (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk() || in.getTotalLength() < 0x40)
        return false;

    char mz[2] = {};
    if (in.read (mz, 2) != 2 || mz[0] != 'M' || mz[1] != 'Z')
        return false;

    in.setPosition (0x3C);
    char lfanew[4] = {};
    if (in.read (lfanew, 4) != 4)
        return false;

    const auto peOff = (juce::uint32) (unsigned char) lfanew[0]
                     | ((juce::uint32) (unsigned char) lfanew[1] << 8)
                     | ((juce::uint32) (unsigned char) lfanew[2] << 16)
                     | ((juce::uint32) (unsigned char) lfanew[3] << 24);

    if ((juce::int64) peOff + 4 > in.getTotalLength())
        return false;

    in.setPosition (peOff);
    char sig[4] = {};
    return in.read (sig, 4) == 4 && sig[0] == 'P' && sig[1] == 'E' && sig[2] == 0 && sig[3] == 0;
}

// VST3 规范要求加载模块前把进程工作目录切到插件目录（很多插件按相对路径找附属 DLL）
struct ScopedWorkingDir
{
    const juce::File original;

    explicit ScopedWorkingDir (const juce::File& dir)
        : original (juce::File::getCurrentWorkingDirectory())
    {
        if (dir.isDirectory() && original != dir)
            dir.setAsCurrentWorkingDirectory();
    }

    ~ScopedWorkingDir()
    {
        original.setAsCurrentWorkingDirectory();
    }

    JUCE_DECLARE_NON_COPYABLE (ScopedWorkingDir)
};

//------------------------------------------------------------------------------
// 宿主上下文对象（加载侧预加热用）：进程内全局单例，生命周期覆盖整个进程。
//------------------------------------------------------------------------------
class PrewarmHost final : public Steinberg::Vst::HostApplication
{
public:
    PrewarmHost() = default;

    Steinberg::tresult PLUGIN_API getName (Steinberg::Vst::String128 name) override
    {
        const char* const hostName = "Minixer";
        int i = 0;
        for (; i < 127 && hostName[i] != 0; ++i)
            name[i] = static_cast<Steinberg::char8> (hostName[i]);
        name[i] = 0;
        return Steinberg::kResultTrue;
    }
};

// 预加热宿主：静态对象，故意不释放（模块生命周期 = 进程生命周期）
PrewarmHost& getPrewarmHost() noexcept
{
    static PrewarmHost host;
    return host;
}

} // namespace

//==============================================================================
// 宿主上下文对象（扫描用）：实现 IHostApplication（宿主名），并通过 SDK
// 的 HostApplication 基类附带 IPlugInterfaceSupport 等标准宿主接口。
//==============================================================================
class VST3Scanner::HostContext final : public Steinberg::Vst::HostApplication
{
public:
    HostContext() = default;

    Steinberg::tresult PLUGIN_API getName (Steinberg::Vst::String128 name) override
    {
        // 手动将 ASCII 宿主名写入 UTF-16 字符串并保证终止（避免依赖 StringConvert）
        const char* const hostName = "Minixer";
        int i = 0;
        for (; i < 127 && hostName[i] != 0; ++i)
            name[i] = static_cast<Steinberg::char8> (hostName[i]);
        name[i] = 0;
        return Steinberg::kResultTrue;
    }
};

//==============================================================================
// 模块句柄：Windows DLL 加载 + 工厂获取/释放。
//==============================================================================
struct VST3Scanner::LoadedModule
{
    juce::File file;
    void* handle = nullptr;                        // HMODULE
    Steinberg::IPluginFactory* factory = nullptr;
    juce::String error;

    bool isValid() const { return handle != nullptr && factory != nullptr; }

    bool open()
    {
        // file 必须已是实际 DLL 文件（bundle 目录由调用方先行
        // resolveLibraryFile 解析；此处拒绝目录，避免模块缓存键不一致）。
        if (file.isDirectory())
        {
            error = L"未找到可加载的 .vst3 库文件（bundle 内无有效 DLL）";
            return false;
        }

        // 非有效 PE（AppleDouble/改名文件等残留）直接拒绝，不进入 LoadLibraryW
        if (! isLikelyPe (file))
        {
            error = L"非有效 DLL/PE 镜像（可能是 macOS/解压残留或损坏文件）";
            return false;
        }

        // 按 VST3 规范：加载前把工作目录切到插件目录，加载完恢复
        ScopedWorkingDir cwdScope (file.getParentDirectory());

        const auto path = file.getFullPathName();  // toUTF16 依赖该 String 存活
        handle = (void*) LoadLibraryW (reinterpret_cast<LPCWSTR> (path.toUTF16().getAddress()));
        if (handle == nullptr)
        {
            error = L"LoadLibraryW 失败: " + winError (GetLastError());
            return false;
        }

        // VST3 模块生命周期：宿主必须先调用 InitDll() 初始化模块，再取 GetPluginFactory()。
        // 跳过 InitDll 会让部分外壳插件（如 WaveShell、Pianoteq）的资源系统未初始化而崩溃。
        using InitDllType = bool (PLUGIN_API*) ();
        if (auto initDll = reinterpret_cast<InitDllType> (GetProcAddress ((HMODULE) handle, "InitDll")))
        {
            if (! initDll())
            {
                error = L"InitDll() 返回失败";
                close();
                return false;
            }
        }

        using GetPluginFactoryType = Steinberg::IPluginFactory* (PLUGIN_API*) ();
        auto getFactory = reinterpret_cast<GetPluginFactoryType> (GetProcAddress ((HMODULE) handle, "GetPluginFactory"));
        if (getFactory == nullptr)
        {
            error = L"模块未导出 GetPluginFactory()";
            close();
            return false;
        }

        factory = getFactory();
        if (factory == nullptr)
        {
            error = L"GetPluginFactory() 返回空指针";
            close();
            return false;
        }
        return true;
    }

    void close()
    {
        if (handle != nullptr)
        {
            // VST3 模块结束后应调用 ExitDll()（逆序于 InitDll）
            using ExitDllType = void (PLUGIN_API*) ();
            if (auto exitDll = reinterpret_cast<ExitDllType> (GetProcAddress ((HMODULE) handle, "ExitDll")))
                exitDll();

            if (factory != nullptr)
            {
                factory->release();
                factory = nullptr;
            }
            FreeLibrary ((HMODULE) handle);
            handle = nullptr;
        }
    }
};

//==============================================================================
VST3Scanner::VST3Scanner (LogFn logIn)
    : logFn (std::move (logIn)),
      host (new HostContext ())
{
}

VST3Scanner::~VST3Scanner()
{
    unloadAllModules();
}

void VST3Scanner::log (const juce::String& msg)
{
    if (logFn)
        logFn (msg);
}

//------------------------------------------------------------------------------
juce::Array<juce::File> VST3Scanner::findVST3Files (const juce::File& directory, bool recursive)
{
    // 递归收集 *.vst3（普通文件 + bundle 目录），并过滤其他系统/解压残留
    // （__MACOSX、AppleDouble ._* 等）
    juce::Array<juce::File> result;

    for (const auto& f : directory.findChildFiles (juce::File::findFiles, recursive, "*.vst3"))
        if (! isOsResidue (f))
            result.add (f);

    for (const auto& d : directory.findChildFiles (juce::File::findDirectories, recursive, "*.vst3"))
        if (! isOsResidue (d))
            result.add (d);

    return result;
}

//------------------------------------------------------------------------------
bool VST3Scanner::isOsResidueFile (const juce::File& file)
{
    return isOsResidue (file);
}

//------------------------------------------------------------------------------
juce::File VST3Scanner::resolveLibraryFile (const juce::File& input)
{
    if (input.existsAsFile())
        return input;

    if (input.isDirectory())
    {
        // bundle 目录：递归查找其中的 .vst3 文件（JUCE getLibraryPaths 同逻辑），
        // 优先 x86_64-win / win64 架构子目录（避免跨平台直达包里的 macOS 二进制）。
        juce::File fallback;

        for (const auto& f : input.findChildFiles (juce::File::findFiles, true, "*.vst3"))
        {
            if (isOsResidue (f))
                continue;

            const auto path = f.getFullPathName();

            if (path.containsIgnoreCase ("x86_64-win") || path.containsIgnoreCase ("win64"))
                return f;

            if (! fallback.existsAsFile())
                fallback = f;
        }

        if (fallback.existsAsFile())
            return fallback;
    }

    return {};
}

//------------------------------------------------------------------------------
std::shared_ptr<VST3Scanner::LoadedModule> VST3Scanner::getModule (const juce::File& file)
{
    const auto key = file.getFullPathName();

    {
        const juce::CriticalSection::ScopedLockType lock (cacheLock);
        const auto it = loadedModules.find (key);
        if (it != loadedModules.end())
            return it->second;
    }

    auto mod = std::make_shared<LoadedModule>();
    mod->file = file;

    if (mod->open())
    {
        const juce::CriticalSection::ScopedLockType lock (cacheLock);
        loadedModules[key] = mod;
    }
    return mod;
}

//------------------------------------------------------------------------------
void VST3Scanner::unloadAllModules()
{
    const juce::CriticalSection::ScopedLockType lock (cacheLock);
    for (auto& pair : loadedModules)
        pair.second->close();
    loadedModules.clear();
}

//------------------------------------------------------------------------------
// moduleinfo.json 是 VST3 可选的“快速扫描”快照（宿主可据此不加载 DLL 就列类）。
// 对 shell / bundle 类插件，该快照与运行时工厂状态经常不一致（WaveShell 在
// setHostContext 后类表会重排/追加），因此本扫描器始终以真实加载后的运行时枚举为准，
// 这里仅解析快照做诊断对比。
static int moduleInfoJsonClassCount (const juce::File& moduleFile)
{
    juce::Array<juce::File> candidates;
    candidates.add (moduleFile.getSiblingFile ("moduleinfo.json"));

    const auto bundleRoot = moduleFile.getParentDirectory();
    candidates.add (bundleRoot.getChildFile ("moduleinfo.json"));                     // Contents/x86_64-win/
    candidates.add (bundleRoot.getParentDirectory().getChildFile ("moduleinfo.json")); // Contents/
    candidates.add (bundleRoot.getParentDirectory().getParentDirectory().getChildFile ("moduleinfo.json")); // bundle 根

    for (const auto& f : candidates)
    {
        if (! f.existsAsFile())
            continue;

        juce::var parsed = juce::JSON::parse (f);
        if (auto* obj = parsed.getDynamicObject())
        {
            if (auto* classes = obj->getProperty ("Classes").getArray())
                return (int) classes->size();
        }
        return -1;   // 文件存在但结构非预期
    }
    return -1;       // 无 moduleinfo.json
}

//------------------------------------------------------------------------------
PluginModuleRecord VST3Scanner::scanModule (const juce::File& file, const std::atomic<bool>* cancelled)
{
    PluginModuleRecord rec;

    // 兼容 bundle 目录：解析到实际 DLL 文件再扫描（普通文件原样返回）
    const auto lib = resolveLibraryFile (file);
    if (! lib.existsAsFile())
    {
        rec.filePath = file.getFullPathName();
        rec.fileName = file.getFileName();
        rec.loadError = L"未找到可加载的 .vst3 库文件（bundle 内无有效 DLL）";
        log (juce::String (L"[扫描失败] ") + rec.fileName + L" —— " + rec.loadError);
        return rec;
    }

    rec.filePath   = lib.getFullPathName();
    rec.fileName   = lib.getFileName();
    rec.fileSize   = lib.getSize();
    rec.fileModified = lib.getLastModificationTime().toISO8601 (true);

    auto mod = getModule (lib);
    if (! mod->isValid())
    {
        rec.loadError = mod->error.isNotEmpty() ? mod->error : L"模块加载失败";
        log (juce::String (L"[扫描失败] ") + rec.fileName + L" —— " + rec.loadError);
        return rec;
    }
    rec.loaded = true;

    auto* factory = mod->factory;

    //------------------------------------------------------ 工厂级信息
    Steinberg::PFactoryInfo fi{};
    if (factory->getFactoryInfo (&fi) == Steinberg::kResultOk)
    {
        rec.factoryVendor = ScannerUtil::asciiBounded (fi.vendor, Steinberg::PFactoryInfo::kNameSize).trim();
        rec.factoryUrl    = ScannerUtil::asciiBounded (fi.url,    Steinberg::PFactoryInfo::kURLSize).trim();
        rec.factoryEmail  = ScannerUtil::asciiBounded (fi.email,  Steinberg::PFactoryInfo::kEmailSize).trim();
        rec.factoryFlags  = fi.flags;
    }

    Steinberg::FUnknownPtr<Steinberg::IPluginFactory2> pf2 (factory);
    rec.supportsFactory2 = (pf2 != nullptr);
    Steinberg::FUnknownPtr<Steinberg::IPluginFactory3> pf3 (factory);
    rec.supportsFactory3 = (pf3 != nullptr);

    //------------------------------------------------------ Phase A：设置宿主上下文之前的类状态（JUCE 旧路径的“错位快照”）
    if (cancelled != nullptr && *cancelled)
        return rec;

    rec.classCountPre = factory->countClasses();
    if (rec.classCountPre < 0 || rec.classCountPre > 50000)
    {
        rec.loadError = L"countClasses() 返回异常值: " + juce::String ((juce::int64) rec.classCountPre);
        log (juce::String (L"[扫描失败] ") + rec.fileName + L" —— " + rec.loadError);
        return rec;
    }

    std::map<CID16, Steinberg::int32> preContextIndex;
    for (Steinberg::int32 i = 0; i < rec.classCountPre; ++i)
    {
        Steinberg::PClassInfo info{};
        if (factory->getClassInfo (i, &info) == Steinberg::kResultOk)
            preContextIndex.emplace (CID16::fromRaw (info.cid), i);
    }

    //------------------------------------------------------ Phase B：【修复点】先设置宿主上下文，再枚举/查找/实例化
    if (pf3 != nullptr)
    {
        const auto hr = pf3->setHostContext (host.get());
        rec.hostContextSet = (hr == Steinberg::kResultOk);
        if (! rec.hostContextSet)
            log (juce::String (L"[警告] ") + rec.fileName + L" setHostContext 返回 " + tresultName (hr));
    }

    if (cancelled != nullptr && *cancelled)
        return rec;

    //------------------------------------------------------ Phase C：设置宿主上下文之后的规范枚举
    rec.classCountPost = factory->countClasses();
    if (rec.classCountPost < 0 || rec.classCountPost > 50000)
    {
        rec.loadError = L"countClasses()（上下文设置后）返回异常值: " + juce::String ((juce::int64) rec.classCountPost);
        log (juce::String (L"[扫描失败] ") + rec.fileName + L" —— " + rec.loadError);
        return rec;
    }

    int classShiftCount = 0;
    rec.classes.reserve ((size_t) std::max (rec.classCountPost, 0));

    for (Steinberg::int32 i = 0; i < rec.classCountPost; ++i)
    {
        if (cancelled != nullptr && *cancelled)
            break;

        PluginClassRecord cr;
        Steinberg::PClassInfo info{};
        if (factory->getClassInfo (i, &info) != Steinberg::kResultOk)
            continue;

        cr.cid             = CID16::fromRaw (info.cid);
        cr.category        = ScannerUtil::asciiBounded (info.category, Steinberg::PClassInfo::kCategorySize).trim();
        cr.name            = ScannerUtil::asciiBounded (info.name,    Steinberg::PClassInfo::kNameSize).trim();
        cr.cardinality     = info.cardinality;
        cr.indexPostContext = i;

        // 两种工厂版本的扩展信息
        if (pf2 != nullptr)
        {
            Steinberg::PClassInfo2 info2{};
            if (pf2->getClassInfo2 (i, &info2) == Steinberg::kResultOk)
            {
                cr.classFlags    = info2.classFlags;
                cr.subCategories = ScannerUtil::asciiBounded (info2.subCategories, Steinberg::PClassInfo2::kSubCategoriesSize).trim();
                cr.vendor        = ScannerUtil::asciiBounded (info2.vendor, Steinberg::PClassInfo2::kVendorSize).trim();
                cr.version       = ScannerUtil::asciiBounded (info2.version, Steinberg::PClassInfo2::kVersionSize).trim();
                cr.sdkVersion    = ScannerUtil::asciiBounded (info2.sdkVersion, Steinberg::PClassInfo2::kVersionSize).trim();
            }
        }
        if (pf3 != nullptr)
        {
            Steinberg::PClassInfoW infoW{};
            if (pf3->getClassInfoUnicode (i, &infoW) == Steinberg::kResultOk)
            {
                cr.nameUnicode   = ScannerUtil::utf16Bounded (reinterpret_cast<const juce::uint16*> (infoW.name),
                                                              Steinberg::PClassInfo::kNameSize).trim();
                if (cr.vendor.isEmpty())
                    cr.vendor    = ScannerUtil::utf16Bounded (reinterpret_cast<const juce::uint16*> (infoW.vendor),
                                                              Steinberg::PClassInfo2::kVendorSize).trim();
                if (cr.version.isEmpty())
                    cr.version   = ScannerUtil::utf16Bounded (reinterpret_cast<const juce::uint16*> (infoW.version),
                                                              Steinberg::PClassInfo2::kVersionSize).trim();
            }
        }

        // 诊断：外壳插件在设置宿主上下文后的索引移位
        const auto preIt = preContextIndex.find (cr.cid);
        if (preIt != preContextIndex.end())
        {
            cr.indexPreContext = preIt->second;
            if (preIt->second != i)
            {
                ++classShiftCount;
                juce::String msg;
                msg << L"  [外壳索引移位] 类 '" << cr.displayName() << L"' (" << cr.cid.toHex()
                    << L") 索引 " << preIt->second << L" -> " << i
                    << L"；类总数 " << rec.classCountPre << L" -> " << rec.classCountPost
                    << L"。本扫描器以完整 CID 记录，不受移位影响。";
                log (msg);
            }
        }
        else
        {
            juce::String msg;
            msg << L"  [构造函数延迟] 类 '" << cr.displayName() << L"' (" << cr.cid.toHex()
                << L") 仅在 setHostContext 之后枚举到。";
            log (msg);
        }

        rec.classes.push_back (std::move (cr));
    }

    juce::String completeMsg (L"[扫描完成] ");
    completeMsg << rec.fileName << L"：类 " << rec.classCountPost << L" 个（上下文前 "
                << rec.classCountPre << L" 个）";
    if (classShiftCount > 0)
        completeMsg << L"，检出索引移位类 " << classShiftCount << L" 个";
    log (completeMsg);

    // moduleinfo.json 快照对比诊断（本扫描器不依赖快照，以运行时枚举为准）
    rec.moduleInfoClassCount = moduleInfoJsonClassCount (file);
    if (rec.moduleInfoClassCount >= 0)
    {
        if (rec.moduleInfoClassCount != rec.classCountPost)
            log (juce::String (L"[moduleinfo] ") + rec.fileName
                 + L"：快照 " + juce::String ((juce::int64) rec.moduleInfoClassCount)
                 + L" 类，运行时 " + juce::String ((juce::int64) rec.classCountPost)
                 + L" 类 —— 不一致，已按运行时真实枚举为准（shell/bundle 快照不可信）");
        else
            log (juce::String (L"[moduleinfo] ") + rec.fileName
                 + L"：快照与运行时一致（" + juce::String ((juce::int64) rec.moduleInfoClassCount) + L" 类）");
    }

    return rec;
}

//------------------------------------------------------------------------------
bool VST3Scanner::prewarmForLoad (const juce::File& file, juce::String* errorOut)
{
   #if ! JUCE_WINDOWS
    juce::ignoreUnused (file, errorOut);
    return true;
   #else
    // 兼容 bundle 目录：解析到实际 DLL 文件（普通文件原样返回）
    const auto lib = resolveLibraryFile (file);

    if (! lib.existsAsFile())
    {
        if (errorOut != nullptr)
            *errorOut = L"未找到可加载的 .vst3 库文件: " + file.getFullPathName();
        return false;
    }

    // 静态注册表：已预热的模块路径直接复用；句柄/工厂保留到进程退出（有意不卸载，
    // 避免个别插件在 ExitDll()/FreeLibrary 阶段崩溃影响宿主进程正常退出）。
    static juce::CriticalSection prewarmLock;
    static std::map<juce::String, void*> handleRegistry;   // 值保留 HMODULE 引用计数，进程内不释放

    const auto key = lib.getFullPathName();

    {
        const juce::CriticalSection::ScopedLockType lock (prewarmLock);
        if (handleRegistry.count (key) != 0)
            return true;
    }

    if (! isLikelyPe (lib))
    {
        if (errorOut != nullptr)
            *errorOut = L"非有效 DLL/PE 镜像（可能是 macOS/解压残留或损坏文件）";
        return false;
    }

    // 按 VST3 规范：加载前把工作目录切到插件目录
    ScopedWorkingDir cwdScope (lib.getParentDirectory());

    const auto path = lib.getFullPathName();  // toUTF16 依赖该 String 存活
    auto* handle = (void*) LoadLibraryW (reinterpret_cast<LPCWSTR> (path.toUTF16().getAddress()));
    if (handle == nullptr)
    {
        if (errorOut != nullptr)
            *errorOut = L"LoadLibraryW 失败: " + winError (GetLastError());
        return false;
    }

    using InitDllType = bool (PLUGIN_API*) ();
    if (auto initDll = reinterpret_cast<InitDllType> (GetProcAddress ((HMODULE) handle, "InitDll")))
    {
        if (! initDll())
        {
            if (errorOut != nullptr)
                *errorOut = L"InitDll() 返回失败";
            FreeLibrary ((HMODULE) handle);
            return false;
        }
    }

    using GetPluginFactoryType = Steinberg::IPluginFactory* (PLUGIN_API*) ();
    auto getFactory = reinterpret_cast<GetPluginFactoryType> (GetProcAddress ((HMODULE) handle, "GetPluginFactory"));
    if (getFactory == nullptr)
    {
        if (errorOut != nullptr)
            *errorOut = L"模块未导出 GetPluginFactory()";
        FreeLibrary ((HMODULE) handle);
        return false;
    }

    auto* factory = getFactory();
    if (factory == nullptr)
    {
        if (errorOut != nullptr)
            *errorOut = L"GetPluginFactory() 返回空指针";
        FreeLibrary ((HMODULE) handle);
        return false;
    }

    // 【修复点】先设置宿主上下文：外壳插件（WaveShell 等）会在 setHostContext
    // 后重排/追加工厂类表；此后 JUCE 的 findClassMatchingDescription 才能在该
    // 已展开的类表上按 name + CID 哈希匹配到正确索引，避免“点 A 出 B”。
    // 该模块已由本函数持有一个引用（初始计数 1），保留到进程退出，不额外 addRef。
    Steinberg::FUnknownPtr<Steinberg::IPluginFactory3> pf3 (factory);
    if (pf3 != nullptr)
        pf3->setHostContext (&getPrewarmHost());

    {
        const juce::CriticalSection::ScopedLockType lock (prewarmLock);
        handleRegistry[key] = handle;
    }
    return true;
   #endif
}

} // namespace vst3scan
} // namespace minixer