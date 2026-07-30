/*
  ==============================================================================

    PluginManagerComponent.cpp

  ==============================================================================
*/

#include "PluginManagerComponent.h"
#include "PluginRegistry.h"
#include "../Settings/AppSettings.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

//==============================================================================
PluginManagerComponent::PluginManagerComponent()
    : pluginListComponent (PluginRegistry::getInstance().getFormatManager(),
                           PluginRegistry::getInstance().getKnownPluginList(),
                           AppSettings::getInstance().getDeadMansPedalFile(),
                           AppSettings::getInstance().getPropertiesFile(),
                           false)
{
    pluginListComponent.setLookAndFeel (&getLookAndFeel());

    // 使用 JUCE 原生的扫描进度对话框，并配置同步扫描。
    // 说明：VST3 插件的实例化与总线信息查询（如 juce_VST3PluginFormat.cpp:301
    // 的 JUCE_ASSERT_MESSAGE_THREAD）必须在消息线程执行。将扫描线程数设为 0，
    // PluginListComponent 会改为在消息线程上通过 Timer 轮询同步扫描；进度对话
    // 框仍会正常显示，并在每个插件扫描间隙刷新进度。
    pluginListComponent.setScanDialogText (TRANS ("Scanning for VST3 plugins"),
                                           TRANS ("Please wait while the plugins are scanned."));
    pluginListComponent.setNumberOfThreadsForScanning (0);

    rescanFailedPluginsButton.setButtonText (TRANS ("Rescan previously failed plugins"));
    rescanFailedPluginsButton.setTooltip (TRANS ("When checked, plugins that crashed or failed in the last scan will be scanned again."));
    rescanFailedPluginsButton.onClick = [this] { onRescanFailedPluginsToggled(); };
    addAndMakeVisible (rescanFailedPluginsButton);

    addAndMakeVisible (pluginListComponent);

    scanStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    scanStatusLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    scanStatusLabel.setJustificationType (juce::Justification::centredLeft);
    scanStatusLabel.setText (TRANS ("Ready"), juce::dontSendNotification);
    addAndMakeVisible (scanStatusLabel);

    // 监听 KnownPluginList 变化，在扫描结束后展示扫描报告。
    PluginRegistry::getInstance().getKnownPluginList().addChangeListener (this);

    // 启动状态轮询定时器，用于在扫描期间更新 scanStatusLabel。
    startTimer (scanStatusTimerMs);

    setSize (600, 500);
}

//==============================================================================
PluginManagerComponent::~PluginManagerComponent()
{
    PluginRegistry::getInstance().getKnownPluginList().removeChangeListener (this);
    stopTimer();

    // 如果用户勾选了复选框但未实际触发扫描，确保不遗留重试标志。
    PluginRegistry::getInstance().setRescanFailedPlugins (false);
}

//==============================================================================
void PluginManagerComponent::paint (juce::Graphics& g)
{
    g.fillAll (MixerLookAndFeel::getBackgroundColour());
}

//==============================================================================
void PluginManagerComponent::resized()
{
    auto bounds = getLocalBounds().reduced (8);

    auto buttonHeight = 28;
    auto statusHeight = 22;

    rescanFailedPluginsButton.setBounds (bounds.removeFromTop (buttonHeight));
    bounds.removeFromTop (8);

    scanStatusLabel.setBounds (bounds.removeFromBottom (statusHeight));
    bounds.removeFromBottom (4);

    pluginListComponent.setBounds (bounds);
}

//==============================================================================
void PluginManagerComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source != &PluginRegistry::getInstance().getKnownPluginList())
        return;

    // 每次列表变化都重置防抖计数器，扫描真正结束后再展示报告。
    scanReportDelayCounter = 5;
}

//==============================================================================
void PluginManagerComponent::timerCallback()
{
    auto& registry = PluginRegistry::getInstance();

    // 更新扫描状态标签：PluginListComponent 已创建 Scanner 但 PluginRegistry
    // 尚未开始记录单个文件时，说明正处于 JUCE 的目录枚举阶段。
    if (pluginListComponent.isScanning())
    {
        if (registry.isScanInProgress())
        {
            auto currentFile = registry.getCurrentScanningFile();

            if (currentFile.isNotEmpty())
            {
                scanStatusLabel.setText (TRANS ("Scanning: ") + juce::File (currentFile).getFileName(),
                                         juce::dontSendNotification);
            }
            else
            {
                scanStatusLabel.setText (TRANS ("Scanning plugins"),
                                         juce::dontSendNotification);
            }
        }
        else
        {
            scanStatusLabel.setText (TRANS ("Enumerating plugin directories"),
                                     juce::dontSendNotification);
        }
    }
    else
    {
        scanStatusLabel.setText (TRANS ("Ready"), juce::dontSendNotification);
    }

    // 扫描报告防抖：每次 KnownPluginList 变化时计数器重置为 5（约 500ms）。
    // 计数器归零且扫描真正空闲时，弹出扫描报告。
    if (scanReportDelayCounter > 0)
    {
        --scanReportDelayCounter;

        if (scanReportDelayCounter == 0)
        {
            registry.checkAndFinishIdleScan();

            if (! registry.isScanInProgress() && registry.getLastScanReport().hasUnshownReport)
            {
                rescanFailedPluginsButton.setToggleState (false, juce::dontSendNotification);
                showScanReportIfNeeded();
            }
        }
    }
}

//==============================================================================
juce::String PluginManagerComponent::formatScanReport (const PluginScanReport& report)
{
    juce::String text;
    text << TRANS ("Scan completed") << ":\n\n";
    text << TRANS ("Total files scanned") << ": " << report.totalFiles << "\n";
    text << TRANS ("Successful") << ": " << report.successCount << "\n";
    text << TRANS ("New plugins") << ": " << report.newCount << "\n";
    text << TRANS ("Updated plugins") << ": " << report.updatedCount << "\n";
    text << TRANS ("Skipped (unchanged)") << ": " << report.skippedCount << "\n";
    text << TRANS ("Skipped (blacklisted)") << ": " << report.blacklistedCount << "\n";
    text << TRANS ("Failed") << ": " << report.failedCount << "\n";

    if (! report.failedEntries.isEmpty())
    {
        text << "\n" << TRANS ("Failed files") << ":\n";

        constexpr int maxFailedEntriesToShow = 10;
        const int numToShow = juce::jmin (report.failedEntries.size(), maxFailedEntriesToShow);

        for (int i = 0; i < numToShow; ++i)
        {
            const auto& entry = report.failedEntries.getReference (i);
            text << "  " << juce::File (entry.filePath).getFileName()
                 << " - " << entry.reason << "\n";
        }

        if (report.failedEntries.size() > maxFailedEntriesToShow)
        {
            text << "  " << TRANS ("and") << " "
                 << (report.failedEntries.size() - maxFailedEntriesToShow)
                 << " " << TRANS ("more") << "\n";
        }
    }

    if (report.blacklistedCount > 0)
    {
        text << "\n" << TRANS ("Blacklisted plugins skipped") << ": "
             << report.blacklistedCount << "\n";
        text << TRANS ("Check \"Rescan previously failed plugins\" to retry them.") << "\n";
    }

    return text;
}

//==============================================================================
void PluginManagerComponent::showScanReportIfNeeded()
{
    auto report = PluginRegistry::getInstance().getLastScanReport();

    if (! report.hasUnshownReport)
        return;

    PluginRegistry::getInstance().markLastScanReportAsShown();

    juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                            TRANS ("Plugin Scan Report"),
                                            formatScanReport (report),
                                            TRANS ("OK"));
}

//==============================================================================
void PluginManagerComponent::onRescanFailedPluginsToggled()
{
    PluginRegistry::getInstance().setRescanFailedPlugins (rescanFailedPluginsButton.getToggleState());
}

} // namespace minixer
