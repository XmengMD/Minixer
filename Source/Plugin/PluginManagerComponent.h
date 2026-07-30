/*
  ==============================================================================

    PluginManagerComponent.h
    插件扫描与管理界面（基于 JUCE PluginListComponent）。

    界面完全复用 JUCE 原生的 PluginListComponent：
    - 列表展示、启用/禁用、移除已扫描插件；
    - 通过 Options 菜单的 “scan for new or updated VST3 plug-ins” 选择目录并扫描。

    扫描配置为同步消息线程（threads == 0）：VST3 插件的实例化与总线信息
    查询必须在消息线程执行。JUCE 原生的进度对话框仍会显示，并在每个插件
    扫描间隙刷新进度。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginRegistry.h"

namespace minixer
{

//==============================================================================
class PluginManagerComponent  : public juce::Component,
                                private juce::ChangeListener,
                                private juce::Timer
{
public:
    //==============================================================================
    PluginManagerComponent();
    ~PluginManagerComponent() override;

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    //==============================================================================
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    /** 将最近一次扫描报告格式化为可读的报告文本。 */
    static juce::String formatScanReport (const PluginScanReport& report);

    /** 展示最近一次扫描报告（如果有未展示的报告）。 */
    void showScanReportIfNeeded();

    /** 当“重新扫描上次出错的插件”复选框状态变化时调用。 */
    void onRescanFailedPluginsToggled();

    //==============================================================================
    juce::ToggleButton rescanFailedPluginsButton;
    juce::PluginListComponent pluginListComponent;
    juce::Label scanStatusLabel;

    /** 扫描报告防抖计数器（以 timer tick 为单位，5 ticks × 100ms = 500ms）。 */
    int scanReportDelayCounter = 0;

    /** 扫描状态轮询与报告防抖定时器间隔（ms）。 */
    static constexpr int scanStatusTimerMs = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerComponent)
};

} // namespace minixer
