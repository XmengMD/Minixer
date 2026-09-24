#include "ChannelStripComponent.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

//==============================================================================
ChannelStripComponent::ChannelStripComponent (const juce::String& /*channelName*/)
{
    addAndMakeVisible (inputTrim);
    inputTrim.addListener (this);

    addAndMakeVisible (inputMeter);

    for (int i = 0; i < defaultNumPluginSlots; ++i)
    {
        auto slot = std::make_unique<PluginSlotComponent> (i, defaultNumPluginSlots);
        slot->addListener (this);
        addAndMakeVisible (*slot);
        pluginSlots.push_back (std::move (slot));
    }

    addAndMakeVisible (panKnob);
    panKnob.addListener (this);
    panKnob.setValueFormatter ([] (double value)
    {
        if (value <= -0.01)
            return juce::String::formatted (TRANS ("L %.0f%%"), -value * 100.0);
        if (value >= 0.01)
            return juce::String::formatted (TRANS ("R %.0f%%"), value * 100.0);

        return TRANS ("C");
    });

    addAndMakeVisible (stereoSeparation);
    stereoSeparation.addListener (this);
    stereoSeparation.setValueFormatter ([] (double value)
    {
        if (value < -0.05)
            return juce::String::formatted (TRANS ("separated %.0f%%"), -value);
        if (value > 0.05)
            return juce::String::formatted (TRANS ("merged %.0f%%"), value);

        return TRANS ("off");
    });

    addAndMakeVisible (outputFader);
    outputFader.addListener (this);

    addAndMakeVisible (outputMeter);
}

//==============================================================================
PluginSlotComponent& ChannelStripComponent::getPluginSlot (int index)
{
    jassert (juce::isPositiveAndBelow (index, pluginSlots.size()));
    return *pluginSlots[static_cast<size_t> (index)];
}

//==============================================================================
void ChannelStripComponent::setPluginSlotInfo (int slotIndex, const juce::String& pluginName, bool isBypassed)
{
    if (juce::isPositiveAndBelow (slotIndex, pluginSlots.size()))
        pluginSlots[static_cast<size_t> (slotIndex)]->setPluginInfo (pluginName, isBypassed);
}

//==============================================================================
void ChannelStripComponent::setPluginSlotBusyState (int slotIndex, PluginSlotBusyState busyState, const juce::String& busyPluginName)
{
    if (juce::isPositiveAndBelow (slotIndex, pluginSlots.size()))
        pluginSlots[static_cast<size_t> (slotIndex)]->setPluginBusyState (busyState, busyPluginName);
}

//==============================================================================
void ChannelStripComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    g.setColour (MixerLookAndFeel::getElevatedColour().withAlpha (0.3f));
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (MixerLookAndFeel::getBorderColour());
    g.drawRoundedRectangle (bounds, 6.0f, 1.0f);
}

//==============================================================================
void ChannelStripComponent::resized()
{
    auto bounds = getLocalBounds().reduced (6, 4);

    // 左侧：插件机架（12 个插槽，每个固定 40 像素高）
    const auto rackWidth = 180;
    const auto slotHeight = 40;
    const auto rackHeight = slotHeight * defaultNumPluginSlots;
    auto rackArea = bounds.removeFromLeft (rackWidth).withHeight (rackHeight);

    for (auto& slot : pluginSlots)
        slot->setBounds (rackArea.removeFromTop (slotHeight).reduced (1, 1));

    bounds.removeFromLeft (8);

    // 右侧：控制区（Input/Pan/Output + 电平表），高度与插件机架对齐
    auto controlArea = bounds.withHeight (rackHeight);
    const auto sectionHeight = controlArea.getHeight() / 3;

    // 上部：Input Trim 旋钮 + 输入电平表
    auto inputSection = controlArea.removeFromTop (sectionHeight);
    auto inputMeterWidth = inputSection.getWidth() / 3;
    inputMeter.setBounds (inputSection.removeFromRight (inputMeterWidth).reduced (2, 4));
    inputTrim.setBounds (inputSection.reduced (2, 4));

    // 中部：Pan 旋钮 + Stereo separation 旋钮
    auto panSection = controlArea.removeFromTop (sectionHeight);
    auto panKnobWidth = panSection.getWidth() / 2;
    panKnob.setBounds (panSection.removeFromLeft (panKnobWidth).reduced (2, 4));
    stereoSeparation.setBounds (panSection.reduced (2, 4));

    // 下部：Output 推子 + 输出电平表
    auto outputSection = controlArea;
    auto outputMeterWidth = outputSection.getWidth() / 3;
    outputMeter.setBounds (outputSection.removeFromRight (outputMeterWidth).reduced (2, 4));
    outputFader.setBounds (outputSection.reduced (2, 4));
}

//==============================================================================
void ChannelStripComponent::pluginSlotClicked (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotClicked (slotIndex); });
}

//==============================================================================
void ChannelStripComponent::pluginSlotReplaceRequested (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotReplaceRequested (slotIndex); });
}

//==============================================================================
void ChannelStripComponent::pluginSlotBypassToggled (int slotIndex, bool shouldBypass)
{
    listeners.call ([slotIndex, shouldBypass] (Listener& l)
    {
        l.pluginSlotBypassToggled (slotIndex, shouldBypass);
    });
}

//==============================================================================
void ChannelStripComponent::pluginSlotDeleteRequested (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotDeleteRequested (slotIndex); });
}

//==============================================================================
void ChannelStripComponent::pluginSlotCopyRequested (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotCopyRequested (slotIndex); });
}

//==============================================================================
void ChannelStripComponent::pluginSlotPasteRequested (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotPasteRequested (slotIndex); });
}

//==============================================================================
void ChannelStripComponent::pluginSlotMoveRequested (int fromSlotIndex, int toSlotIndex)
{
    listeners.call ([fromSlotIndex, toSlotIndex] (Listener& l)
    {
        l.pluginSlotMoveRequested (fromSlotIndex, toSlotIndex);
    });
}

//==============================================================================
void ChannelStripComponent::pluginSlotLoadCancelRequested (int slotIndex)
{
    listeners.call ([slotIndex] (Listener& l) { l.pluginSlotLoadCancelRequested (slotIndex); });
}

//==============================================================================
int ChannelStripComponent::getFocusedPluginSlotIndex() const
{
    for (size_t i = 0; i < pluginSlots.size(); ++i)
    {
        if (pluginSlots[i]->hasKeyboardFocus (true))
            return static_cast<int> (i);
    }

    return -1;
}

//==============================================================================
void ChannelStripComponent::setAllPluginsBypassed (bool shouldBypass)
{
    for (auto& slot : pluginSlots)
    {
        if (slot->hasPlugin())
            slot->setBypassed (shouldBypass);
    }
}

//==============================================================================
void ChannelStripComponent::rotaryKnobValueChanged (RotaryKnobComponent* /*knob*/)
{
    notifyParameterChanged();
}

//==============================================================================
void ChannelStripComponent::faderValueChanged (FaderComponent* /*fader*/)
{
    notifyParameterChanged();
}

//==============================================================================
void ChannelStripComponent::notifyParameterChanged()
{
    listeners.call ([] (Listener& l) { l.channelStripParameterChanged(); });
}

//==============================================================================
int ChannelStripComponent::getSlotIndexAtPoint (juce::Point<int> pos) const
{
    for (size_t i = 0; i < pluginSlots.size(); ++i)
    {
        if (pluginSlots[i]->getBounds().contains (pos))
            return static_cast<int> (i);
    }

    return -1;
}

//==============================================================================
void ChannelStripComponent::updateDragHoverSlot (int hoverIndex)
{
    for (size_t i = 0; i < pluginSlots.size(); ++i)
        pluginSlots[i]->setDropTargetHighlighted (static_cast<int> (i) == hoverIndex);
}

//==============================================================================
bool ChannelStripComponent::isInterestedInDragSource (const SourceDetails& dragSourceDetails)
{
    auto* sourceComp = dragSourceDetails.sourceComponent.get();
    if (sourceComp == nullptr)
        return false;

    auto* slotComp = dynamic_cast<PluginSlotComponent*> (sourceComp);
    if (slotComp == nullptr)
        return false;

    // 空槽与进行中的槽位没有可移动的插件
    if (slotComp->isBusy() || ! slotComp->hasPlugin())
        return false;

    if (! dragSourceDetails.description.isInt())
        return false;

    int sourceIndex = dragSourceDetails.description;
    return juce::isPositiveAndBelow (sourceIndex, static_cast<int> (pluginSlots.size()));
}

//==============================================================================
void ChannelStripComponent::itemDragEnter (const SourceDetails& dragSourceDetails)
{
    auto hoverIndex = getSlotIndexAtPoint (dragSourceDetails.localPosition);
    updateDragHoverSlot (hoverIndex);
}

//==============================================================================
void ChannelStripComponent::itemDragMove (const SourceDetails& dragSourceDetails)
{
    auto hoverIndex = getSlotIndexAtPoint (dragSourceDetails.localPosition);
    updateDragHoverSlot (hoverIndex);
}

//==============================================================================
void ChannelStripComponent::itemDragExit (const SourceDetails& /*dragSourceDetails*/)
{
    updateDragHoverSlot (-1);
}

//==============================================================================
void ChannelStripComponent::itemDropped (const SourceDetails& dragSourceDetails)
{
    updateDragHoverSlot (-1);

    int fromIndex = dragSourceDetails.description;
    int toIndex   = getSlotIndexAtPoint (dragSourceDetails.localPosition);

    if (toIndex >= 0 && fromIndex != toIndex)
        pluginSlotMoveRequested (fromIndex, toIndex);
}

//==============================================================================
bool ChannelStripComponent::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::altModifier, 0))
    {
        moveFocusedSlot (-1);
        return true;
    }

    if (key == juce::KeyPress (juce::KeyPress::downKey, juce::ModifierKeys::altModifier, 0))
    {
        moveFocusedSlot (1);
        return true;
    }

    return false;
}

//==============================================================================
void ChannelStripComponent::moveFocusedSlot (int delta)
{
    auto focused = getFocusedPluginSlotIndex();
    if (focused < 0)
        return;

    auto target = focused + delta;
    if (! juce::isPositiveAndBelow (target, static_cast<int> (pluginSlots.size())))
        return;

    pluginSlotMoveRequested (focused, target);

    // 保持焦点跟随被移动的槽位
    pluginSlots[static_cast<size_t> (target)]->grabKeyboardFocus();
}

} // namespace minixer
