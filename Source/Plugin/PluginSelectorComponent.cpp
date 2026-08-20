/*
  ==============================================================================

    PluginSelectorComponent.cpp
    带搜索和清空搜索按钮的插件选择对话框内容组件。

  ==============================================================================
*/

#include "PluginSelectorComponent.h"
#include "LookAndFeel/MixerLookAndFeel.h"
#include "Settings/AppSettings.h"

namespace minixer
{

//==============================================================================
namespace
{
    struct PluginNameComparator
    {
        static int compareElements (const juce::PluginDescription& a,
                                    const juce::PluginDescription& b)
        {
            return a.name.compareNatural (b.name);
        }
    };

    constexpr int columnCount             = 3;
    constexpr int nameColumnIndex         = 0;
    constexpr int manufacturerColumnIndex = 1;
    constexpr int categoryColumnIndex     = 2;
    constexpr int defaultNameColumnWidth         = 280;
    constexpr int defaultManufacturerColumnWidth = 180;
    constexpr int defaultCategoryColumnWidth     = 140;
    constexpr int columnPadding                  = 8;
    constexpr int minColumnWidth                 = 40;
    constexpr int maxColumnWidth                 = 800;
    constexpr int headerHeight                   = 24;
    constexpr int dividerHitRadius               = 4;

    juce::Array<int> makeDefaultColumnWidths()
    {
        return { defaultNameColumnWidth, defaultManufacturerColumnWidth, defaultCategoryColumnWidth };
    }

    int sumColumnWidths (const juce::Array<int>& widths)
    {
        int total = 0;
        for (auto w : widths)
            total += w;
        return total;
    }
}

//==============================================================================
/** 可拖拽调整列宽的表头组件。 */
class PluginSelectorHeaderComponent  : public juce::Component
{
    public:
        using OnWidthsChanged = std::function<void()>;
        using OnDragFinished  = std::function<void()>;

        PluginSelectorHeaderComponent (juce::Array<int>& widthsToControl,
                                       OnWidthsChanged onChanged,
                                       OnDragFinished onFinished)
            : columnWidths (widthsToControl),
              onWidthsChanged (std::move (onChanged)),
              onDragFinished (std::move (onFinished))
        {
            setInterceptsMouseClicks (true, true);
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (MixerLookAndFeel::getSurfaceColour().brighter (0.05f));

            g.setColour (MixerLookAndFeel::getBorderColour());
            g.drawHorizontalLine (getHeight() - 1, 0.0f, static_cast<float> (getWidth()));

            g.setColour (MixerLookAndFeel::getTextColour());
            g.setFont (juce::Font (juce::FontOptions (static_cast<float> (getHeight()) * 0.55f)).boldened());

            const juce::String labels[columnCount] = { TRANS ("Name"), TRANS ("Manufacturer"), TRANS ("Category") };
            int x = 0;

            for (int i = 0; i < columnCount; ++i)
            {
                auto bounds = juce::Rectangle<int> (x + columnPadding, 0,
                                                    columnWidths.getReference (i) - columnPadding * 2, getHeight());
                g.drawText (labels[i], bounds, juce::Justification::centredLeft, true);

                if (i < columnCount - 1)
                {
                    auto dividerX = x + columnWidths.getReference (i);
                    g.setColour (MixerLookAndFeel::getBorderColour());
                    g.drawVerticalLine (dividerX, 0.0f, static_cast<float> (getHeight()));
                    g.setColour (MixerLookAndFeel::getTextColour());
                }

                x += columnWidths.getReference (i);
            }
        }

        void mouseMove (const juce::MouseEvent& e) override
        {
            updateMouseCursor (e.position.x);
        }

        void mouseExit (const juce::MouseEvent&) override
        {
            setMouseCursor (juce::MouseCursor::NormalCursor);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            draggingDivider = findDividerAt (e.position.x);

            if (draggingDivider >= 0)
            {
                dragStartX = e.position.x;
                dragStartWidths = columnWidths;
            }
        }

        void mouseDrag (const juce::MouseEvent& e) override
        {
            if (draggingDivider < 0)
                return;

            auto delta = static_cast<int> (e.position.x - dragStartX);

            auto newLeft  = juce::jlimit (minColumnWidth, maxColumnWidth, dragStartWidths[draggingDivider]     + delta);
            auto newRight = juce::jlimit (minColumnWidth, maxColumnWidth, dragStartWidths[draggingDivider + 1] - delta);

            columnWidths.getReference (draggingDivider)     = newLeft;
            columnWidths.getReference (draggingDivider + 1) = newRight;

            if (onWidthsChanged != nullptr)
                onWidthsChanged();

            repaint();
        }

        void mouseUp (const juce::MouseEvent&) override
        {
            if (draggingDivider >= 0 && onDragFinished != nullptr)
                onDragFinished();

            draggingDivider = -1;
            setMouseCursor (juce::MouseCursor::NormalCursor);
        }

    private:
        int findDividerAt (float x) const
        {
            int pos = 0;

            for (int i = 0; i < columnCount - 1; ++i)
            {
                pos += columnWidths.getReference (i);

                if (std::abs (x - pos) <= dividerHitRadius)
                    return i;
            }

            return -1;
        }

        void updateMouseCursor (float x)
        {
            setMouseCursor (findDividerAt (x) >= 0
                                ? juce::MouseCursor (juce::MouseCursor::LeftRightResizeCursor)
                                : juce::MouseCursor (juce::MouseCursor::NormalCursor));
        }

        juce::Array<int>& columnWidths;
        OnWidthsChanged onWidthsChanged;
        OnDragFinished  onDragFinished;

        int draggingDivider = -1;
        float dragStartX = 0.0f;
        juce::Array<int> dragStartWidths;
    };

//==============================================================================
PluginSelectorComponent::PluginSelectorComponent (juce::Array<juce::PluginDescription> pluginTypes,
                                                  const juce::String& currentPluginIdentifier,
                                                  ResultCallback callback,
                                                  juce::LookAndFeel* lookAndFeelToUse)
    : allTypes (std::move (pluginTypes)),
      currentIdentifier (currentPluginIdentifier),
      onResult (std::move (callback)),
      columnWidths (AppSettings::getInstance().getPluginSelectorColumnWidths())
{
    if (lookAndFeelToUse != nullptr)
        setLookAndFeel (lookAndFeelToUse);

    PluginNameComparator comparator;
    allTypes.sort (comparator);

    addAndMakeVisible (searchEditor);
    addAndMakeVisible (criteriaBox);
    addAndMakeVisible (clearSearchButton);
    addAndMakeVisible (listBox);
    addChildComponent (emptyLabel);

    auto header = std::make_unique<PluginSelectorHeaderComponent> (
        columnWidths,
        [this] { onColumnWidthsChanged(); },
        [this] { AppSettings::getInstance().setPluginSelectorColumnWidths (columnWidths); });
    header->setSize (sumColumnWidths (columnWidths), headerHeight);
    listBox.setHeaderComponent (std::move (header));

    searchField = static_cast<SearchField> (
        juce::jlimit (0, 3, AppSettings::getInstance().getPluginSelectorSearchMode()));

    criteriaBox.addItem (TRANS("Name"),         1);
    criteriaBox.addItem (TRANS("Manufacturer"),  2);
    criteriaBox.addItem (TRANS("Category"),      3);
    criteriaBox.addItem (TRANS("All"),           4);
    criteriaBox.setSelectedId (static_cast<int> (searchField) + 1, juce::dontSendNotification);
    criteriaBox.setTooltip (TRANS("Choose which fields the search text should match."));
    criteriaBox.addListener (this);
    criteriaBox.setColour (juce::ComboBox::outlineColourId, MixerLookAndFeel::getBorderColour());

    searchEditor.setTextToShowWhenEmpty (TRANS("Search plugins..."),
                                         MixerLookAndFeel::getMutedTextColour());
    searchEditor.setCaretVisible (true);
    searchEditor.setSelectAllWhenFocused (true);
    searchEditor.addListener (this);

    clearSearchButton.onClick = [this]
    {
        searchEditor.clear();
        applyFilter();
        searchEditor.grabKeyboardFocus();
    };
    clearSearchButton.setTooltip (TRANS("Clear search"));

    listBox.setRowHeight (28);
    listBox.setMultipleSelectionEnabled (false);
    listBox.setMinimumContentWidth (sumColumnWidths (columnWidths));
    listBox.setColour (juce::ListBox::backgroundColourId, MixerLookAndFeel::getSurfaceColour());
    listBox.setColour (juce::ListBox::outlineColourId,   MixerLookAndFeel::getBorderColour());
    listBox.setColour (juce::ListBox::textColourId,      MixerLookAndFeel::getTextColour());

    emptyLabel.setJustificationType (juce::Justification::centred);
    emptyLabel.setText (TRANS("No matching plugins"), juce::dontSendNotification);
    emptyLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getMutedTextColour());
    emptyLabel.setInterceptsMouseClicks (false, false);

    applyFilter();

    setSize (480, 600);
}

//==============================================================================
void PluginSelectorComponent::resized()
{
    auto bounds = getLocalBounds().reduced (8);
    auto searchBar = bounds.removeFromTop (32);

    clearSearchButton.setBounds (searchBar.removeFromRight (32)
                                          .withHeight (24)
                                          .withY (searchBar.getCentreY() - 12));
    criteriaBox.setBounds (searchBar.removeFromRight (100)
                                    .withHeight (24)
                                    .withY (searchBar.getCentreY() - 12)
                                    .reduced (4, 0));
    searchEditor.setBounds (searchBar.reduced (0, 4));

    bounds.removeFromTop (4);
    listBox.setBounds (bounds);
    emptyLabel.setBounds (listBox.getBounds());
}

void PluginSelectorComponent::onColumnWidthsChanged()
{
    listBox.setMinimumContentWidth (sumColumnWidths (columnWidths));
    listBox.repaint();
}

void PluginSelectorComponent::visibilityChanged()
{
    if (isVisible())
        searchEditor.grabKeyboardFocus();
}

//==============================================================================
void PluginSelectorComponent::paint (juce::Graphics& g)
{
    g.fillAll (MixerLookAndFeel::getBackgroundColour());
}

//==============================================================================
void PluginSelectorComponent::textEditorTextChanged (juce::TextEditor&)
{
    applyFilter();
}

void PluginSelectorComponent::comboBoxChanged (juce::ComboBox*)
{
    auto newField = static_cast<SearchField> (
        juce::jlimit (0, 3, criteriaBox.getSelectedId() - 1));

    if (newField == searchField)
        return;

    searchField = newField;
    AppSettings::getInstance().setPluginSelectorSearchMode (static_cast<int> (searchField));
    applyFilter();
}

//==============================================================================
int PluginSelectorComponent::getNumRows()
{
    return filteredTypes.size();
}

const juce::PluginDescription* PluginSelectorComponent::getPluginDescriptionForRow (int row) const
{
    if (! juce::isPositiveAndBelow (row, filteredTypes.size()))
        return nullptr;

    const auto index = filteredTypes[row];
    if (! juce::isPositiveAndBelow (index, allTypes.size()))
        return nullptr;

    return &allTypes.getReference (index);
}

//==============================================================================
void PluginSelectorComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                int width, int height, bool rowIsSelected)
{
    const auto* descPtr = getPluginDescriptionForRow (rowNumber);
    if (descPtr == nullptr)
        return;

    const auto& desc = *descPtr;
    const bool isCurrent = desc.createIdentifierString() == currentIdentifier;

    const auto backgroundColour = MixerLookAndFeel::getSurfaceColour();
    const auto selectedColour   = MixerLookAndFeel::getAccentColour();
    g.fillAll (rowIsSelected ? selectedColour : backgroundColour);

    auto textColour = rowIsSelected ? juce::Colours::white
                                    : MixerLookAndFeel::getTextColour();
    if (isCurrent && ! rowIsSelected)
        textColour = MixerLookAndFeel::getAccentColour();

    g.setColour (textColour);
    g.setFont (juce::Font (juce::FontOptions ((float) height * 0.55f)));

    auto drawColumn = [&g, height] (const juce::String& text, int x, int w)
    {
        auto bounds = juce::Rectangle<int> (x + columnPadding, 0, juce::jmax (0, w - columnPadding * 2), height);
        g.drawText (text, bounds, juce::Justification::centredLeft, true);
    };

    int x = 0;
    drawColumn (desc.name,            x, columnWidths[nameColumnIndex]);         x += columnWidths[nameColumnIndex];
    drawColumn (desc.manufacturerName, x, columnWidths[manufacturerColumnIndex]); x += columnWidths[manufacturerColumnIndex];
    drawColumn (desc.category,         x, columnWidths[categoryColumnIndex]);

    // 列分隔线
    g.setColour (MixerLookAndFeel::getBorderColour());
    x = 0;
    g.drawVerticalLine (x += columnWidths[nameColumnIndex],         0.0f, static_cast<float> (height));
    g.drawVerticalLine (x += columnWidths[manufacturerColumnIndex], 0.0f, static_cast<float> (height));

    juce::ignoreUnused (width);
}

void PluginSelectorComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (auto* desc = getPluginDescriptionForRow (row))
        commitSelection (*desc);
}

void PluginSelectorComponent::returnKeyPressed (int lastRowSelected)
{
    if (auto* desc = getPluginDescriptionForRow (lastRowSelected))
        commitSelection (*desc);
}

//==============================================================================
void PluginSelectorComponent::applyFilter()
{
    filteredTypes.clear();
    const auto query = searchEditor.getText();

    auto matchesQuery = [this, &query] (const juce::PluginDescription& desc) -> bool
    {
        if (query.isEmpty())
            return true;

        switch (searchField)
        {
            case SearchField::name:
                return desc.name.containsIgnoreCase (query);
            case SearchField::manufacturer:
                return desc.manufacturerName.containsIgnoreCase (query);
            case SearchField::category:
                return desc.category.containsIgnoreCase (query);
            case SearchField::all:
                return desc.name.containsIgnoreCase (query)
                    || desc.manufacturerName.containsIgnoreCase (query)
                    || desc.category.containsIgnoreCase (query);
        }

        return false;
    };

    for (int i = 0; i < allTypes.size(); ++i)
    {
        if (matchesQuery (allTypes.getReference (i)))
            filteredTypes.add (i);
    }

    listBox.updateContent();

    if (filteredTypes.isEmpty())
    {
        listBox.deselectAllRows();
        emptyLabel.setVisible (true);
    }
    else
    {
        emptyLabel.setVisible (false);

        auto row = findRowForCurrentPlugin();
        if (row < 0)
            row = 0;

        listBox.selectRow (row);
        listBox.scrollToEnsureRowIsOnscreen (row);
    }
}

void PluginSelectorComponent::commitSelection (const juce::PluginDescription& desc)
{
    if (hasFinished)
        return;

    hasFinished = true;

    if (onResult != nullptr)
    {
        auto cb = std::move (onResult);
        onResult = nullptr;
        cb (desc);
    }

    closeDialog();
}

int PluginSelectorComponent::findRowForCurrentPlugin() const
{
    if (currentIdentifier.isEmpty())
        return -1;

    for (int i = 0; i < filteredTypes.size(); ++i)
    {
        if (auto* desc = getPluginDescriptionForRow (i))
        {
            if (desc->createIdentifierString() == currentIdentifier)
                return i;
        }
    }

    return -1;
}

void PluginSelectorComponent::closeDialog()
{
    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
        dialog->exitModalState (0);
}

} // namespace minixer
