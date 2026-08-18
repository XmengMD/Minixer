/*
  ==============================================================================

    PluginSelectorComponent.cpp
    带搜索和关闭按钮的插件选择对话框内容组件。

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
}

//==============================================================================
PluginSelectorComponent::PluginSelectorComponent (juce::Array<juce::PluginDescription> pluginTypes,
                                                  const juce::String& currentPluginIdentifier,
                                                  ResultCallback callback,
                                                  juce::LookAndFeel* lookAndFeelToUse)
    : allTypes (std::move (pluginTypes)),
      currentIdentifier (currentPluginIdentifier),
      onResult (std::move (callback))
{
    if (lookAndFeelToUse != nullptr)
        setLookAndFeel (lookAndFeelToUse);

    PluginNameComparator comparator;
    allTypes.sort (comparator);

    addAndMakeVisible (searchEditor);
    addAndMakeVisible (criteriaBox);
    addAndMakeVisible (closeButton);
    addAndMakeVisible (listBox);
    addChildComponent (emptyLabel);

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

    closeButton.onClick = [this] { commitSelection (nullptr); };

    listBox.setRowHeight (28);
    listBox.setMultipleSelectionEnabled (false);
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
    auto header = bounds.removeFromTop (32);

    closeButton.setBounds (header.removeFromRight (32)
                                  .withHeight (24)
                                  .withY (header.getCentreY() - 12));
    criteriaBox.setBounds (header.removeFromRight (100)
                                  .withHeight (24)
                                  .withY (header.getCentreY() - 12)
                                  .reduced (4, 0));
    searchEditor.setBounds (header.reduced (0, 4));

    listBox.setBounds (bounds.withTrimmedTop (8));
    emptyLabel.setBounds (listBox.getBounds());
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

void PluginSelectorComponent::paintListBoxItem (int rowNumber, juce::Graphics& g,
                                                int width, int height, bool rowIsSelected)
{
    if (! juce::isPositiveAndBelow (rowNumber, filteredTypes.size()))
        return;

    const auto& desc = *filteredTypes[rowNumber];
    const bool isCurrent = desc.createIdentifierString() == currentIdentifier;

    const auto backgroundColour = MixerLookAndFeel::getSurfaceColour();
    const auto selectedColour   = MixerLookAndFeel::getAccentColour();
    g.fillAll (rowIsSelected ? selectedColour : backgroundColour);

    auto textColour = rowIsSelected ? juce::Colours::white
                                    : MixerLookAndFeel::getTextColour();
    if (isCurrent && ! rowIsSelected)
        textColour = MixerLookAndFeel::getAccentColour();

    g.setColour (textColour);
    g.setFont (juce::Font (juce::FontOptions ((float) height * 0.6f)));

    const auto textBounds = juce::Rectangle<int> (8, 0, width - 16, height);
    g.drawText (desc.name, textBounds, juce::Justification::centredLeft, true);
}

void PluginSelectorComponent::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (juce::isPositiveAndBelow (row, filteredTypes.size()))
        commitSelection (filteredTypes[row]);
}

void PluginSelectorComponent::returnKeyPressed (int lastRowSelected)
{
    if (juce::isPositiveAndBelow (lastRowSelected, filteredTypes.size()))
        commitSelection (filteredTypes[lastRowSelected]);
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

    for (const auto& desc : allTypes)
    {
        if (matchesQuery (desc))
            filteredTypes.add (&desc);
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

void PluginSelectorComponent::commitSelection (const juce::PluginDescription* desc)
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
        if (filteredTypes[i]->createIdentifierString() == currentIdentifier)
            return i;

    return -1;
}

void PluginSelectorComponent::closeDialog()
{
    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
        dialog->exitModalState (0);
}

} // namespace minixer
