/*
  ==============================================================================

    PluginManagerComponent.h
    插件扫描与管理界面（自建 UI，替代 JUCE 原生 PluginListComponent）。

    原有的 JUCE PluginListComponent 会在消息线程上同步扫描，导致扫描期间
    整个软件界面卡死。本自建组件改为：
      - 前端完全自建：插件表格（启用/名称/厂商/类别）、筛选（可切换
        Name / Manufacturer / Category / All）、表头排序、批量启用/禁用、
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
    explicit PluginContextMenuTable (const juce::String& componentName)
        : juce::TableListBox (componentName)
    {
        addMouseListener (&rowMouseWatcher, true);
    }

    /** (右键所在行号, 屏幕坐标) 回调；点击非行空白时不会调用。 */
    std::function<void (int, juce::Point<int>)> onContextMenu;

private:
    /** 行与单元格都是子组件，鼠标事件不会到达表格本身，因此需要一个
        “接收全部嵌套子组件事件”的监听器来响应行上的右键。

        注意：不能把表格自身（this）注册为该监听器 —— 那样嵌套事件的
        mouseUp 会二次派发到 ListBox::mouseUp，从而每次松开鼠标都触发
        backgroundClicked → 清空多选，导致 Ctrl/Shift 多选在松开后失效。
    */
    struct RowMouseWatcher final : public juce::MouseListener
    {
        explicit RowMouseWatcher (PluginContextMenuTable& tableToWatch) : owner (tableToWatch) {}

        ~RowMouseWatcher() override
        {
            owner.removeMouseListener (this);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.handleRowMouseDown (e);
        }

        PluginContextMenuTable& owner;
    };

    void handleRowMouseDown (const juce::MouseEvent& e)
    {
        if (! e.mods.isPopupMenu())
            return;

        // 事件来自行/单元格等子组件：换算到表格坐标后再判定行号。
        const auto pos = e.getEventRelativeTo (this).getPosition();

        // 仅当右键落在滚动区域（行区域）内时才响应；表头与边框不弹菜单。
        auto* viewport = getViewport();

        if (viewport == nullptr || ! viewport->getBounds().contains (pos))
            return;

        const int row = getRowContainingPosition (pos.getX(), pos.getY());

        if (row >= 0 && onContextMenu != nullptr)
            onContextMenu (row, e.getScreenPosition());
    }

    RowMouseWatcher rowMouseWatcher { *this };
};

//==============================================================================
class PluginManagerComponent  : public juce::Component,
                                public juce::TableListBoxModel,
                                private juce::ChangeListener,
                                private juce::Timer,
                                private juce::TableHeaderComponent::Listener
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
        colCategory     = 4    /**< 类别（与插件选择对话框一致） */
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
    // juce::TableHeaderComponent::Listener
    // （表头的“全部启用/全部禁用”复选框需跟随列拖动/调整宽度重新定位）
    void tableColumnsChanged (juce::TableHeaderComponent* header) override;
    void tableColumnsResized (juce::TableHeaderComponent* header) override;
    void tableSortOrderChanged (juce::TableHeaderComponent* header) override;

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

    //==============================================================================
    // 批量启用 / 禁用

    /** 批量设置一组插件描述的启用状态（写入 PluginRegistry 并刷新表格）。 */
    void setPluginsEnabled (const juce::Array<juce::PluginDescription>& descriptions, bool enabled);

    /** 批量设置当前选中行。 */
    void setSelectedPluginsEnabled (bool enabled);

    /** 批量设置全部已扫描插件（不受 Filter 影响）。 */
    void setAllPluginsEnabled (bool enabled);

    /** 批量设置当前 Filter 过滤后的插件。 */
    void setFilteredPluginsEnabled (bool enabled);

    /** 按厂商或类别批量设置（作用于整个已扫描列表，含被 Filter 隐藏的同类插件）。
        columnId 只能是 colManufacturer 或 colCategory。 */
    void setPluginsEnabledMatchingField (bool enabled, int columnId, const juce::String& value);

    /** 是否所有已扫描插件都已启用（空列表返回 false）。 */
    bool areAllPluginsEnabled() const;

    /** 是否至少有一个已扫描插件已启用。 */
    bool isAnyPluginEnabled() const;

    /** 把表头“全选”复选框移动到“启用”列的位置（列拖动/改宽后调用）。 */
    void layoutSelectAllToggle();

    /** 启用状态变化后刷新表头复选框的显示。 */
    void updateSelectAllToggle();

    /** 把当前可见行的勾选框状态与 PluginRegistry 的启用状态同步。

        勾选框是单元格里的子组件，只有 refreshComponentForCell 被调用时才会
        重新读取状态；批量修改后直接同步可见组件，避免依赖 updateContent 的
        刷新时机（也避免在勾选框自己的 onClick 回调里重建行组件）。
    */
    void syncEnableToggles();

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
    juce::ComboBox      filterCriteriaBox;
    juce::TextEditor    filterEditor;
    PluginContextMenuTable table;
    juce::Label         scanStatusLabel;

    /** 覆盖在“启用”列表头上的三态复选框（全部启用 / 部分启用 / 全部禁用）。 */
    std::unique_ptr<juce::Component> selectAllToggle;

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