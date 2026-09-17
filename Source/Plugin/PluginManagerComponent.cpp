/*
  ==============================================================================

    PluginManagerComponent.cpp

  ==============================================================================
*/

#include "PluginManagerComponent.h"
#include "PluginRegistry.h"
#include "../Settings/AppSettings.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

#include <algorithm>

namespace minixer
{

namespace
{

//==============================================================================
/** 比较两个 PluginDescription 以按指定列排序。 */
struct PluginSorter
{
    int  column;
    bool forwards;

    bool operator() (const juce::PluginDescription& a,
                     const juce::PluginDescription& b) const
    {
        int cmp = 0;

        switch (column)
        {
            case PluginManagerComponent::colName:         cmp = a.name.compare (b.name); break;
            case PluginManagerComponent::colManufacturer: cmp = a.manufacturerName.compare (b.manufacturerName); break;
            case PluginManagerComponent::colChannels:     cmp = a.numInputChannels - b.numInputChannels; break;
            default:                                      cmp = a.name.compare (b.name); break;
        }

        if (cmp == 0)
            cmp = a.name.compare (b.name);

        return forwards ? (cmp < 0) : (cmp > 0);
    }
};

//==============================================================================
/** 扫描目录编辑器：列出内置默认目录（只读）+ 用户自定义目录（可增删）。 */
class ScanPathsEditorContent final : public juce::Component,
                                     private juce::ListBoxModel
{
public:
    ScanPathsEditorContent()
    {
        addButton.setButtonText (TRANS ("Add folder..."));
        addButton.onClick = [this] { addFolder(); };
        addAndMakeVisible (addButton);

        removeButton.setButtonText (TRANS ("Remove selected"));
        removeButton.onClick = [this] { removeSelected(); };
        removeButton.setEnabled (false);
        addAndMakeVisible (removeButton);

        list.setModel (this);
        addAndMakeVisible (list);

        refresh();
        setSize (520, 360);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (MixerLookAndFeel::getSurfaceColour());
    }

    void resized() override
    {
        auto bounds = getLocalBounds().reduced (8);

        auto top = bounds.removeFromTop (28);
        addButton.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (8);
        removeButton.setBounds (top.removeFromLeft (150));

        bounds.removeFromTop (8);
        list.setBounds (bounds);
    }

    //==========================================================================
    // ListBoxModel
    int getNumRows() override { return items.size(); }

    void paintListBoxItem (int rowNumber, juce::Graphics& g,
                           int width, int height, bool rowIsSelected) override
    {
        if (rowIsSelected)
            g.fillAll (MixerLookAndFeel::getAccentColour().withAlpha (0.25f));

        if (! juce::isPositiveAndBelow (rowNumber, items.size()))
            return;

        g.setColour (MixerLookAndFeel::getTextColour());
        g.setFont (juce::Font (juce::FontOptions (13.0f)));

        juce::String text = items[rowNumber].path;
        if (! items[rowNumber].removable)
            text += "  (" + TRANS ("default") + ")";

        g.drawText (text, 4, 0, width - 8, height,
                    juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override { refreshRemoveButton(); }

private:
    struct Item
    {
        juce::String path;
        bool         removable;   // 仅自定义目录可移除，默认目录为只读内置
    };

    //==========================================================================
    // 依据 PluginRegistry 当前的自定义目录重建显示列表。
    void refresh()
    {
        const auto defaultPaths = PluginRegistry::getInstance().getVST3DefaultSearchPath();
        const auto customPaths  = PluginRegistry::getInstance().getCustomScanPaths();

        items.clearQuick();

        for (int i = 0; i < defaultPaths.getNumPaths(); ++i)
            items.add ({ defaultPaths[i].getFullPathName(), false });

        for (int i = 0; i < customPaths.getNumPaths(); ++i)
        {
            bool isDefault = false;

            for (int j = 0; j < defaultPaths.getNumPaths(); ++j)
                if (defaultPaths[j].getFullPathName() == customPaths[i].getFullPathName())
                { isDefault = true; break; }

            if (! isDefault)
                items.add ({ customPaths[i].getFullPathName(), true });
        }

        list.updateContent();
        list.deselectAllRows();
        refreshRemoveButton();
    }

    void refreshRemoveButton()
    {
        const int row = list.getSelectedRow();
        removeButton.setEnabled (juce::isPositiveAndBelow (row, items.size()) && items[row].removable);
    }

    void addFolder()
    {
        chooser = std::make_shared<juce::FileChooser> (
            TRANS ("Add VST3 scan folder"),
            juce::File::getSpecialLocation (juce::File::userHomeDirectory),
            juce::String());

        auto* rawPtr = chooser.get();

        chooser->launchAsync (
            juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
            [this, rawPtr] (const juce::FileChooser&)
            {
                const auto dir = rawPtr->getResult();

                if (dir.isDirectory())
                {
                    PluginRegistry::getInstance().addScanPath (dir);
                    refresh();
                }
            });
    }

    void removeSelected()
    {
        const int row = list.getSelectedRow();

        if (juce::isPositiveAndBelow (row, items.size()) && items[row].removable)
        {
            PluginRegistry::getInstance().removeScanPath (juce::File (items[row].path));
            refresh();
        }
    }

    //==========================================================================
    juce::Array<Item>                items;
    juce::ListBox                    list;
    juce::TextButton                 addButton;
    juce::TextButton                 removeButton;
    std::shared_ptr<juce::FileChooser> chooser;
};

//==============================================================================
/** 扫描目录编辑窗口：关闭时仅隐藏，便于再次打开。 */
class ScanPathsEditorWindow final : public juce::DocumentWindow
{
public:
    using juce::DocumentWindow::DocumentWindow;

    void closeButtonPressed() override
    {
        setVisible (false);
    }
};

} // anonymous namespace

//==============================================================================
PluginManagerComponent::PluginManagerComponent()
    : table ("plugins")
{
    table.onContextMenu = [this] (int row, juce::Point<int> screenPos)
    {
        showRowContextMenu (row, screenPos);
    };

    // —— 工具栏 ——
    optionsButton.setButtonText (TRANS ("Options"));
    optionsButton.onClick = [this] { showOptionsMenu(); };
    addAndMakeVisible (optionsButton);

    rescanFailedPluginsButton.setButtonText (TRANS ("Rescan previously failed plugins"));
    rescanFailedPluginsButton.setTooltip (TRANS ("When checked, plugins that crashed or failed in "
                                                 "the last scan will be scanned again."));
    rescanFailedPluginsButton.onClick = [this]
    {
        PluginRegistry::getInstance().setRescanFailedPlugins (rescanFailedPluginsButton.getToggleState());
    };
    addAndMakeVisible (rescanFailedPluginsButton);

    cancelButton.setButtonText (TRANS ("Cancel"));
    cancelButton.onClick = [this] { cancelScan(); };
    cancelButton.setVisible (false);
    addAndMakeVisible (cancelButton);

    filterLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    filterLabel.setText (TRANS ("Filter:"), juce::dontSendNotification);
    addAndMakeVisible (filterLabel);

    filterEditor.setTextToShowWhenEmpty (TRANS ("Search plugins..."),
                                         MixerLookAndFeel::getTextColour().withAlpha (0.5f));
    filterEditor.onTextChange = [this] { refreshList(); };
    addAndMakeVisible (filterEditor);

    // —— 插件表格 ——
    table.getHeader().setColour (juce::TableHeaderComponent::textColourId, MixerLookAndFeel::getTextColour());
    table.getHeader().setColour (juce::TableHeaderComponent::highlightColourId, MixerLookAndFeel::getAccentColour());

    table.getHeader().addColumn (juce::String(),                PluginManagerComponent::colEnabled,
                                 28, 24, 40, juce::TableHeaderComponent::notResizable);
    table.getHeader().addColumn (TRANS ("Name"),                PluginManagerComponent::colName,
                                 260, 120, 900, juce::TableHeaderComponent::defaultFlags);
    table.getHeader().addColumn (TRANS ("Manufacturer"),        PluginManagerComponent::colManufacturer,
                                 160,  80, 600, 0);
    table.getHeader().addColumn (TRANS ("In/Out"),              PluginManagerComponent::colChannels,
                                 80,  60, 200, juce::TableHeaderComponent::notResizable);
    table.getHeader().setSortColumnId (PluginManagerComponent::colName, true);

    table.setModel (this);
    table.setMultipleSelectionEnabled (true);
    addAndMakeVisible (table);

    // —— 状态栏 ——
    scanStatusLabel.setFont (juce::Font (juce::FontOptions (12.0f)));
    scanStatusLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    scanStatusLabel.setJustificationType (juce::Justification::centredLeft);
    scanStatusLabel.setText (TRANS ("Ready"), juce::dontSendNotification);
    addAndMakeVisible (scanStatusLabel);

    PluginRegistry::getInstance().getKnownPluginList().addChangeListener (this);

    refreshList();
    startTimer (scanStatusTimerMs);
    setSize (760, 520);
}

//==============================================================================
PluginManagerComponent::~PluginManagerComponent()
{
    PluginRegistry::getInstance().getKnownPluginList().removeChangeListener (this);
    stopTimer();
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

    auto toolbar = bounds.removeFromTop (28);
    const int gap = 8;

    optionsButton.setBounds (toolbar.removeFromLeft (100));
    toolbar.removeFromLeft (gap);
    cancelButton.setBounds (toolbar.removeFromLeft (80));
    toolbar.removeFromLeft (gap);

    auto rescanArea = toolbar.removeFromLeft (juce::jmin (220, toolbar.getWidth() / 2));
    rescanFailedPluginsButton.setBounds (rescanArea);

    toolbar.removeFromLeft (gap);
    filterLabel.setBounds (toolbar.removeFromLeft (50));
    toolbar.removeFromLeft (4);
    filterEditor.setBounds (toolbar);

    bounds.removeFromTop (8);

    scanStatusLabel.setBounds (bounds.removeFromBottom (22));
    bounds.removeFromBottom (4);

    table.setBounds (bounds);
}

//==============================================================================
int PluginManagerComponent::getNumRows()
{
    return cachedTypes.size();
}

//==============================================================================
void PluginManagerComponent::paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                                 bool rowIsSelected)
{
    if (rowIsSelected)
        g.fillAll (MixerLookAndFeel::getAccentColour().withAlpha (0.25f));
    else if (rowNumber % 2 == 1)
        g.fillAll (MixerLookAndFeel::getBackgroundColour().brighter (0.04f));
}

//==============================================================================
void PluginManagerComponent::paintCell (juce::Graphics& g, int rowNumber, int columnId,
                                        int width, int height, bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (rowNumber, cachedTypes.size()))
        return;

    const auto& desc = cachedTypes.getReference (rowNumber);
    const bool enabled = isPluginEnabled (rowNumber);

    g.setColour (enabled ? MixerLookAndFeel::getTextColour()
                         : MixerLookAndFeel::getTextColour().withAlpha (0.35f));
    g.setFont (juce::Font (juce::FontOptions (13.0f)));

    juce::String text;

    switch (columnId)
    {
        case PluginManagerComponent::colName:         text = desc.name; break;
        case PluginManagerComponent::colManufacturer: text = desc.manufacturerName; break;
        case PluginManagerComponent::colChannels:     text = juce::String (desc.numInputChannels)
                                                            + " -> "
                                                            + juce::String (desc.numOutputChannels);
            break;
        default: return;
    }

    g.drawText (text, 4, 0, width - 8, height,
                juce::Justification::centredLeft, true);
}

//==============================================================================
void PluginManagerComponent::cellClicked (int rowNumber, int columnId, const juce::MouseEvent&)
{
    juce::ignoreUnused (rowNumber, columnId);
}

//==============================================================================
juce::Component* PluginManagerComponent::refreshComponentForCell (int rowNumber, int columnId,
                                                                 bool isRowSelected,
                                                                 juce::Component* existingComponentToUpdate)
{
    if (columnId != PluginManagerComponent::colEnabled)
        return nullptr;

    auto* toggle = dynamic_cast<juce::ToggleButton*> (existingComponentToUpdate);

    if (toggle == nullptr)
        toggle = new juce::ToggleButton();

    toggle->setToggleState (isPluginEnabled (rowNumber), juce::dontSendNotification);
    toggle->setTooltip (TRANS ("Enable or disable this plugin"));
    toggle->onClick = [this, rowNumber, toggle]
    {
        setPluginEnabled (rowNumber, toggle->getToggleState());
    };

    return toggle;
}

//==============================================================================
int PluginManagerComponent::getColumnAutoSizeWidth (int columnId)
{
    switch (columnId)
    {
        case PluginManagerComponent::colEnabled:      return 28;
        case PluginManagerComponent::colChannels:     return 80;
        case PluginManagerComponent::colManufacturer: return 160;
        case PluginManagerComponent::colName:
        default:
        {
            int widest = 120;

            for (const auto& d : cachedTypes)
            {
                const auto textWidth = juce::GlyphArrangement::getStringWidthInt (
                    juce::Font (juce::FontOptions (13.0f)), d.name);
                widest = juce::jmax (widest, textWidth + 48);
            }

            return widest;
        }
    }
}

//==============================================================================
void PluginManagerComponent::sortOrderChanged (int newSortColumnId, bool isForwards)
{
    currentSortColumn   = newSortColumnId;
    currentSortForwards = isForwards;
    table.getHeader().setSortColumnId (newSortColumnId, isForwards);

    resortCachedTypes();
    table.updateContent();
}

//==============================================================================
void PluginManagerComponent::backgroundClicked (const juce::MouseEvent&)
{
    table.deselectAllRows();
}

//==============================================================================
void PluginManagerComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source != &PluginRegistry::getInstance().getKnownPluginList())
        return;

    refreshList();
}

//==============================================================================
void PluginManagerComponent::refreshList()
{
    const auto all    = PluginRegistry::getInstance().getKnownPluginList().getTypes();
    const auto filter = filterEditor.getText().trim().toLowerCase();

    cachedTypes.clearQuick();

    for (const auto& d : all)
    {
        if (filter.isNotEmpty() && ! d.name.toLowerCase().contains (filter))
            continue;

        cachedTypes.add (d);
    }

    resortCachedTypes();
    table.updateContent();
}

//==============================================================================
void PluginManagerComponent::resortCachedTypes()
{
    if (cachedTypes.isEmpty())
        return;

    PluginSorter sorter { currentSortColumn, currentSortForwards };
    std::sort (cachedTypes.begin(), cachedTypes.end(), sorter);
}

//==============================================================================
void PluginManagerComponent::showOptionsMenu()
{
    if (PluginRegistry::getInstance().isScanThreadRunning())
        return;

    juce::PopupMenu menu;

    menu.addItem (1, TRANS ("Edit scan directories..."));
    menu.addItem (2, TRANS ("Scan"));
    menu.addItem (3, TRANS ("Fully rescan"));
    menu.addSeparator();

    const bool anySelected = table.getSelectedRows().size() > 0;
    const bool anyRows     = cachedTypes.size() > 0;

    menu.addItem (4, TRANS ("Remove selected plug-ins"), anySelected);
    menu.addItem (5, TRANS ("Remove all plug-ins"), anyRows);
    menu.addSeparator();

    bool applyPedal = true;

    if (auto* props = AppSettings::getInstance().getPropertiesFile())
        applyPedal = props->getBoolValue ("minixer_applyDeadMansPedal", true);

    menu.addItem (6, TRANS ("Apply dead-man's pedal"), true, applyPedal);
    menu.addItem (7, TRANS ("Don't apply dead-man's pedal"), true, ! applyPedal);

    // 独立应用中 JUCE 关闭了模态循环，须使用异步菜单 API。
    menu.showMenuAsync (juce::PopupMenu::Options(), [this] (int chosen)
    {
        auto setPedal = [] (bool apply)
        {
            if (auto* props = AppSettings::getInstance().getPropertiesFile())
                props->setValue ("minixer_applyDeadMansPedal", apply);
        };

        switch (chosen)
        {
            case 1: showScanPathsEditor();   break;
            case 2: PluginRegistry::getInstance().startAsyncScan (true, false); break; // 按已管理目录扫描
            case 3: startFullRescan();       break;
            case 4: removeSelectedPlugins(); break;
            case 5: removeAllPlugins();      break;
            case 6: setPedal (true);  break;
            case 7: setPedal (false); break;
            default: break;
        }
    });
}

//==============================================================================
void PluginManagerComponent::showScanPathsEditor()
{
    if (scanPathsWindow == nullptr)
    {
        auto* content = new ScanPathsEditorContent();

        scanPathsWindow.reset (new ScanPathsEditorWindow (TRANS ("Scan directories"),
                                                          MixerLookAndFeel::getBackgroundColour(),
                                                          juce::DocumentWindow::closeButton));
        scanPathsWindow->setContentOwned (content, true);
        scanPathsWindow->setUsingNativeTitleBar (true);
        scanPathsWindow->setResizable (true, true);
        scanPathsWindow->centreWithSize (content->getWidth(), content->getHeight());
    }

    scanPathsWindow->setVisible (true);
    scanPathsWindow->toFront (true);
}

//==============================================================================
void PluginManagerComponent::startFullRescan()
{
    PluginRegistry::getInstance().startAsyncScan (true, true);
}

//==============================================================================
void PluginManagerComponent::cancelScan()
{
    PluginRegistry::getInstance().cancelAsyncScan();
}

//==============================================================================
void PluginManagerComponent::showRowContextMenu (int rowNumber, juce::Point<int> screenPos)
{
    if (! juce::isPositiveAndBelow (rowNumber, cachedTypes.size()))
        return;

    const bool enabled = isPluginEnabled (rowNumber);

    juce::PopupMenu menu;
    menu.addItem (1, enabled ? TRANS ("Disable") : TRANS ("Enable"));
    menu.addItem (2, TRANS ("Remove plug-in"));
    menu.addItem (3, TRANS ("Show folder in file manager"));

    // 独立应用中 JUCE 关闭了模态循环，须使用异步菜单 API。
    menu.showMenuAsync (juce::PopupMenu::Options ()
                            .withMinimumWidth (150)
                            .withTargetScreenArea (juce::Rectangle<int> (screenPos, juce::Point<int> (0, 0))),
                        [this, rowNumber, enabled] (int chosen)
    {
        if (! juce::isPositiveAndBelow (rowNumber, cachedTypes.size()))
            return;

        if (chosen == 1)
        {
            setPluginEnabled (rowNumber, ! enabled);
        }
        else if (chosen == 2)
        {
            const auto id = cachedTypes.getReference (rowNumber).createIdentifierString();
            auto& known = PluginRegistry::getInstance().getKnownPluginList();

            for (auto& d : known.getTypes())
                if (d.createIdentifierString() == id)
                    known.removeType (d);

            known.sendChangeMessage(); // 触发 refreshList
            PluginRegistry::getInstance().saveList();
        }
        else if (chosen == 3)
        {
            juce::File (cachedTypes.getReference (rowNumber).fileOrIdentifier)
                .getParentDirectory()
                .startAsProcess();
        }
    });
}

//==============================================================================
void PluginManagerComponent::removeSelectedPlugins()
{
    auto& known = PluginRegistry::getInstance().getKnownPluginList();
    juce::StringArray ids;

    const auto selectedRows = table.getSelectedRows();

    for (int i = 0; i < selectedRows.size(); ++i)
    {
        const int sel = selectedRows[i];

        if (juce::isPositiveAndBelow (sel, cachedTypes.size()))
            ids.add (cachedTypes.getReference (sel).createIdentifierString());
    }

    if (ids.isEmpty())
        return;

    for (auto& d : known.getTypes())
    {
        if (ids.contains (d.createIdentifierString()))
            known.removeType (d);
    }

    known.sendChangeMessage();
    PluginRegistry::getInstance().saveList();
}

//==============================================================================
void PluginManagerComponent::removeAllPlugins()
{
    auto& known = PluginRegistry::getInstance().getKnownPluginList();
    known.clear();
    known.sendChangeMessage();
    PluginRegistry::getInstance().saveList();
}

//==============================================================================
bool PluginManagerComponent::isPluginEnabled (int rowNumber) const
{
    if (! juce::isPositiveAndBelow (rowNumber, cachedTypes.size()))
        return true;

    auto* props = AppSettings::getInstance().getPropertiesFile();

    if (props == nullptr)
        return true;

    return props->getBoolValue ("minixer_pluginEnabled_"
                                + cachedTypes.getReference (rowNumber).createIdentifierString(), true);
}

//==============================================================================
void PluginManagerComponent::setPluginEnabled (int rowNumber, bool enabled)
{
    if (! juce::isPositiveAndBelow (rowNumber, cachedTypes.size()))
        return;

    auto* props = AppSettings::getInstance().getPropertiesFile();

    if (props == nullptr)
        return;

    props->setValue ("minixer_pluginEnabled_"
                     + cachedTypes.getReference (rowNumber).createIdentifierString(), enabled);
    table.repaint();
}

//==============================================================================
void PluginManagerComponent::timerCallback()
{
    auto& reg = PluginRegistry::getInstance();

    // 在消息线程把后台扫描结果应用到 KnownPluginList / 元数据 / 报告。
    reg.pumpScanResults();

    const bool scanning = reg.isScanThreadRunning() || reg.isScanInProgress();

    if (scanning)
    {
        const int total = reg.getScanProgressTotal();
        const int done  = reg.getScanProgressCompleted();
        const auto detail = reg.getScanProgressDetail();

        juce::String text = TRANS ("Scanning");

        if (total > 0)
            text << " " << done << "/" << total;

        if (detail.isNotEmpty())
            text << ": " << detail;

        scanStatusLabel.setText (text, juce::dontSendNotification);
        cancelButton.setVisible (true);
    }
    else
    {
        cancelButton.setVisible (false);
        scanStatusLabel.setText (TRANS ("Ready"), juce::dontSendNotification);
    }

    optionsButton.setEnabled             (! scanning);
    rescanFailedPluginsButton.setEnabled (! scanning);
    filterEditor.setEnabled              (! scanning);

    // 扫描刚结束时，等待短暂时间确保结果已应用，再弹出报告。
    if (wasScanning && ! reg.isScanInProgress())
        scanReportDelayCounter = 5;

    wasScanning = reg.isScanInProgress();

    if (scanReportDelayCounter > 0)
    {
        --scanReportDelayCounter;

        if (scanReportDelayCounter == 0)
        {
            if (! reg.isScanInProgress() && reg.getLastScanReport().hasUnshownReport)
                showScanReportIfNeeded();
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
    rescanFailedPluginsButton.setToggleState (false, juce::dontSendNotification);

    juce::AlertWindow::showMessageBoxAsync (juce::AlertWindow::InfoIcon,
                                            TRANS ("Plugin Scan Report"),
                                            formatScanReport (report),
                                            TRANS ("OK"));
}

} // namespace minixer