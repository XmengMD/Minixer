#pragma once

#include <JuceHeader.h>

namespace minixer
{

//==============================================================================
/** 插件选择对话框内容组件。

    提供搜索框、可滚动插件列表以及清空搜索按钮，用于替代会撑满屏幕的 PopupMenu。
*/
class PluginSelectorComponent  : public juce::Component,
                                 private juce::TextEditor::Listener,
                                 private juce::ComboBox::Listener,
                                 private juce::ListBoxModel
{
public:
    //==============================================================================
    using ResultCallback = std::function<void (juce::PluginDescription)>;

    enum class SearchField
    {
        name = 0,
        manufacturer,
        category,
        all
    };

    //==============================================================================
    PluginSelectorComponent (juce::Array<juce::PluginDescription> pluginTypes,
                             const juce::String& currentPluginIdentifier,
                             ResultCallback callback,
                             juce::LookAndFeel* lookAndFeelToUse = nullptr);

    ~PluginSelectorComponent() override = default;

    //==============================================================================
    void paint (juce::Graphics& g) override;
    void resized() override;
    void visibilityChanged() override;

private:
    //==============================================================================
    // juce::TextEditor::Listener
    void textEditorTextChanged (juce::TextEditor&) override;

    //==============================================================================
    // juce::ComboBox::Listener
    void comboBoxChanged (juce::ComboBox*) override;

    //==============================================================================
    // juce::ListBoxModel
    int getNumRows() override;
    void paintListBoxItem (int rowNumber, juce::Graphics& g, int width, int height,
                           bool rowIsSelected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    void returnKeyPressed (int lastRowSelected) override;

    //==============================================================================
    void applyFilter();
    void commitSelection (const juce::PluginDescription& desc);
    int findRowForCurrentPlugin() const;
    void closeDialog();

    const juce::PluginDescription* getPluginDescriptionForRow (int row) const;
    void onColumnWidthsChanged();

    //==============================================================================
    juce::Array<int> columnWidths;

    juce::TextEditor searchEditor;
    juce::ComboBox criteriaBox { "SearchCriteria" };
    juce::TextButton clearSearchButton { "AC" };  // 清空搜索
    juce::ListBox listBox { "PluginSelectorListBox", this };
    juce::Label emptyLabel;

    juce::Array<juce::PluginDescription> allTypes;
    juce::Array<int> filteredTypes;
    juce::String currentIdentifier;
    ResultCallback onResult;
    SearchField searchField = SearchField::name;

    bool hasFinished = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginSelectorComponent)
};

} // namespace minixer
