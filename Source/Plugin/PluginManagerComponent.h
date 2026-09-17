/*
  ==============================================================================

    PluginManagerComponent.h
    插件扫描与管理界面（自建 UI，替代 JUCE 原生 PluginListComponent）。

    原有的 JUCE PluginListComponent 会在消息线程上同步扫描，导致扫描期间
    整个软件界面卡死。本自建组件改为：
      - 前端完全自建：插件表格（启用/名称/厂商/通道）、筛选、表头排序、
        行右键菜单、Options 菜单、扫描状态与扫描报告。
      - 扫描由 PluginRegistry::startAsyncScan 驱动的“专用后台线程 + 逐文件
        PluginHost 子进程”完成；本组件仅通过 100ms 定时器调用
        PluginRegistry::pumpScanResults() 把结果应用到 KnownPluginList 并刷新，
        从而彻底不阻塞消息线程。

  ==============================================================================
*/

#pragma once

#include <JuceHeader.h>
#include "PluginRegistry.h"

#include <functional>

namespace minixer
{

//==============================================================================
/** 支持行右键弹出菜单的插件表格（在 TableListBox 之上拦截鼠标右键）。 */
class PluginContextMenuTable final : public juce::TableListBox
{
public:
    using juce::TableListBox::TableListBox;

    /** (右键所在行号, 屏幕坐标) 回调；点击非行空白时不会调用。 */
    std::function<void (int, juce::Point<int>)> onContextMenu;

private:
    void mouseDown (const juce::MouseEvent& e) override
    {
        juce::TableListBox::mouseDown (e);

        if (e.mods.isPopupMenu())
        {
            const int row = getRowContainingPosition (e.getPosition().getX(),
                                                      e.getPosition().getY());

            if (row >= 0 && onContextMenu != nullptr)
                onContextMenu (row, e.getScreenPosition());
        }
    }
};

//==============================================================================
class PluginManagerComponent  : public juce::Component,
                                public juce::TableListBoxModel,
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

    //==============================================================================
    // 表格列 ID（公开，供排序比较器引用）
    enum ColumnIds
    {
        colEnabled      = 1,   /**< 启用复选框 */
        colName         = 2,   /**< 插件名称 */
        colManufacturer = 3,   /**< 厂商 */
        colChannels     = 4    /**< 输入/输出通道 */
    };

    //==============================================================================
    // TableListBoxModel
    int getNumRows() override;
    void paintRowBackground (juce::Graphics&, int rowNumber, int width, int height,
                             bool rowIsSelected) override;
    void paintCell (juce::Graphics&, int rowNumber, int columnId, int width, int height,
                    bool rowIsSelected) override;
    void cellClicked (int rowNumber, int columnId, const juce::MouseEvent&) override;
    juce::Component* refreshComponentForCell (int rowNumber, int columnId,
                                              bool isRowSelected,
                                              juce::Component* existingComponentToUpdate) override;
    int getColumnAutoSizeWidth (int columnId) override;
    void sortOrderChanged (int newSortColumnId, bool isForwards) override;
    void backgroundClicked (const juce::MouseEvent&) override;

private:
    //==============================================================================
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;
    void timerCallback() override;

    //==============================================================================
    void showOptionsMenu();
    void startFullRescan();
    void cancelScan();
    void showScanPathsEditor();
    void showRowContextMenu (int rowNumber, juce::Point<int> screenPos);
    void removeSelectedPlugins();
    void removeAllPlugins();

    bool isPluginEnabled (int rowNumber) const;
    void setPluginEnabled (int rowNumber, bool enabled);

    /** 依据 KnownPluginList 重建缓存列表并刷新表格（消息线程）。 */
    void refreshList();

    /** 依据当前表头排序设置，对 cachedTypes 重新排序。 */
    void resortCachedTypes();

    /** 将最近一次扫描报告格式化为可读文本。 */
    static juce::String formatScanReport (const PluginScanReport& report);

    /** 展示最近一次扫描报告（若有尚未展示的报告）。 */
    void showScanReportIfNeeded();

    //==============================================================================
    // 工具栏与状态组件

    //==============================================================================
    juce::TextButton    optionsButton;
    juce::ToggleButton  rescanFailedPluginsButton;
    juce::TextButton    cancelButton;
    juce::Label         filterLabel;
    juce::TextEditor    filterEditor;
    PluginContextMenuTable table;
    juce::Label         scanStatusLabel;

    /** 扫描目录编辑窗口（复用，仅隐藏不销毁）。 */
    std::unique_ptr<juce::DocumentWindow> scanPathsWindow;

    /** KnownPluginList 的快照副本（消息线程维护，表格模型只读它）。 */
    juce::Array<juce::PluginDescription> cachedTypes;

    /** 表头排序状态（refreshList / resortCachedTypes 共用）。 */
    int  currentSortColumn = ColumnIds::colName;
    bool currentSortForwards = true;

    /** 是否在扫描中（用于捕获"刚结束"这一边沿以弹出报告）。 */
    bool wasScanning = false;

    /** 扫描报告防抖计数器（以 timer tick 为单位，5 ticks × 100ms = 500ms）。 */
    int scanReportDelayCounter = 0;

    /** 扫描状态轮询与报告防抖定时器间隔（ms）。 */
    static constexpr int scanStatusTimerMs = 100;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginManagerComponent)
};

} // namespace minixer