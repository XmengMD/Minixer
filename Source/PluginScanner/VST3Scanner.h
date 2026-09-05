/*
  ==============================================================================

    VST3Scanner.h
    自主 VST3 扫描引擎接口（移植自 ScannerTest 工程）。

    与 JUCE 自带扫描器的关键差异（针对 WaveShell / IKM 等外壳插件的错位问题）：
      1. 在第一次枚举类(Class)之前，先对工厂调用 IPluginFactory3::setHostContext ——
         外壳插件会在此后才把全部（含 EditController）类登记进工厂，类索引随之移位；
         先设置上下文再索引，才能保证“记录/查找/实例化”全部基于同一批工厂状态。
      2. 插件类一律以完整 128 位 CID 为唯一键：记录、查找、实例化全部使用
         16 字节 CID，绝不使用索引或 32 位哈希（JUCE PluginDescription::uid 的缺陷）。
      3. 同时记录 setHostContext 前后的类索引与类总数，用于量化展示外壳插件
         的索引移位，并验证按 CID 加载的稳定性。

  ==============================================================================
*/

#pragma once

#include "ScannerData.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>

namespace minixer
{
namespace vst3scan
{

class VST3Scanner
{
public:
    using LogFn = std::function<void (const juce::String&)>;

    explicit VST3Scanner (LogFn log = nullptr);
    ~VST3Scanner();

    //==============================================================================
    /** 递归（或非递归）查找目录下的所有 .vst3（文件或 bundle 目录，自动过滤 __MACOSX / ._* 等残留）。 */
    static juce::Array<juce::File> findVST3Files (const juce::File& directory, bool recursive = true);

    /** 判断是否为其他系统/解压工具遗留的残留文件：
        __MACOSX / .DS_Store / .Spotlight-V100 / .Trashes / AppleDouble ._*。 */
    static bool isOsResidueFile (const juce::File& file);

    /** 将路径解析为实际可加载的 .vst3 库文件（与 JUCE VST3PluginFormat::getLibraryPaths 一致）：
        - 已是 .vst3 普通文件 → 原样返回；
        - bundle 目录（*.vst3 目录）→ 递归查找其中的 .vst3 文件，优先 x86_64-win / win64 架构子目录；
        返回无效 File 表示无法解析（bundle 内无有效 DLL）。 */
    static juce::File resolveLibraryFile (const juce::File& fileOrBundle);

    /** 扫描单个 .vst3 模块：
        加载 DLL → 取工厂 → 记录 setHostContext 前的类状态 → setHostContext（修复点）
        → 枚举全部类并记录（含完整 128 位 CID、名称、类别、厂商/版本/子类别、前后索引）。 */
    PluginModuleRecord scanModule (const juce::File& file, const std::atomic<bool>* cancelled);

    /** 释放全部缓存的模块句柄（下次访问会重新加载 DLL）。 */
    void unloadAllModules();

    //==============================================================================
    /** 预加载 .vst3 模块并为其设置宿主上下文（仅用于 Windows 加载侧修复）。
        关键作用：JUCE 的 VST3PluginFormat 在 createPluginInstance 时会先解析类索引
        再调用 setHostContext（顺序错误），外壳插件（WaveShell 等）在 setHostContext
        后会重排/追加类表，导致此前解析的索引失效、加载错位（点 A 出 B）。
        本函数在 JUCE 加载之前先把模块加载好并调用 setHostContext，使后续 JUCE 的
        findClassMatchingDescription 在“已展开”的类表上按 name+CID 哈希匹配，
        解析出的索引即为规范索引，加载结果正确。
        模块句柄将一直保持到进程退出（有意不卸载，避免个别插件退出时崩溃）。
        @return true 表示预热完成（即使工厂不支持 setHostContext 也返回 true）。 */
    static bool prewarmForLoad (const juce::File& file, juce::String* errorOut = nullptr);

private:
    struct LoadedModule;                          // PIMPL（Windows HMODULE / IPluginFactory）

    std::shared_ptr<LoadedModule> getModule (const juce::File& file);
    void log (const juce::String& msg);

    LogFn                 logFn;
    class HostContext;                           // 宿主对象（IHostApplication）
    std::unique_ptr<HostContext> host;           // 先构造后销毁（存活期覆盖所有模块）
    juce::CriticalSection cacheLock;
    std::map<juce::String, std::shared_ptr<LoadedModule>> loadedModules;
};

} // namespace vst3scan
} // namespace minixer