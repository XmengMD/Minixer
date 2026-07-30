#include "PluginSlotComponent.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

namespace
{
    static constexpr const char* pluginClipboardPrefix = "MinixerPlugin:";
}

//==============================================================================
PluginSlotComponent::PluginSlotComponent (int index)
    : slotIndex (index)
{
    setWantsKeyboardFocus (true);

    slotButton.setClickingTogglesState (false);
    slotButton.setLookAndFeel (&getLookAndFeel());
    slotButton.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (slotButton);

    // 左侧旁通开关：始终可见，toggle 状态
    bypassButton.setTooltip (TRANS("Bypass this plugin"));
    bypassButton.setConnectedEdges (juce::Button::ConnectedOnRight);
    bypassButton.setClickingTogglesState (true);
    bypassButton.addListener (this);
    addAndMakeVisible (bypassButton);

    // 右侧操作按钮：空槽为 "+"（加载），有插件为 "X"（删除）
    actionButton.setTooltip (TRANS("Load or remove this plugin"));
    actionButton.setConnectedEdges (juce::Button::ConnectedOnLeft);
    actionButton.setLookAndFeel (&getLookAndFeel());
    actionButton.addListener (this);
    addAndMakeVisible (actionButton);

    setPluginInfo ({}, false);
}

//==============================================================================
void PluginSlotComponent::setPluginInfo (const juce::String& name, bool isBypassed)
{
    pluginName = name;
    bypassed = isBypassed;

    if (pluginName.isEmpty())
    {
        slotButton.setButtonText (juce::String (slotIndex + 1) + ": " + TRANS("Empty"));
        bypassButton.setBypassState (false);
        bypassButton.setEnabled (false);
        actionButton.setButtonText ("+");
        actionButton.setTooltip (TRANS("Load plugin"));
    }
    else
    {
        slotButton.setButtonText (juce::String (slotIndex + 1) + ": " + pluginName);
        bypassButton.setBypassState (bypassed);
        bypassButton.setEnabled (true);
        actionButton.setButtonText ("X");
        actionButton.setTooltip (TRANS("Remove this plugin"));
    }

    repaint();
}

//==============================================================================
void PluginSlotComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::transparentBlack);

    auto bounds = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (MixerLookAndFeel::getSurfaceColour());
    g.fillRoundedRectangle (bounds, 4.0f);

    g.setColour (MixerLookAndFeel::getBorderColour());
    g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

    if (bypassed && pluginName.isNotEmpty())
    {
        g.setColour (juce::Colours::black.withAlpha (0.3f));
        g.fillRoundedRectangle (bounds, 4.0f);
    }

    if (hasKeyboardFocus (true))
    {
        g.setColour (MixerLookAndFeel::getAccentColour().withAlpha (0.5f));
        g.drawRoundedRectangle (bounds, 4.0f, 1.5f);
    }

    if (dropTargetHighlighted)
    {
        g.setColour (MixerLookAndFeel::getAccentColour().withAlpha (0.35f));
        g.fillRoundedRectangle (bounds, 4.0f);

        g.setColour (MixerLookAndFeel::getAccentColour());
        g.drawRoundedRectangle (bounds, 4.0f, 2.0f);
    }
}

//==============================================================================
void PluginSlotComponent::resized()
{
    auto bounds = getLocalBounds().reduced (2);
    const auto sideButtonWidth = 24;

    bypassButton.setBounds (bounds.removeFromLeft (sideButtonWidth));
    actionButton.setBounds (bounds.removeFromRight (sideButtonWidth));
    slotButton.setBounds (bounds.reduced (2, 0));
}

//==============================================================================
void PluginSlotComponent::buttonClicked (juce::Button* button)
{
    // 点击子按钮时把焦点归到槽位，保证键盘导航和“当前焦点槽位”判断一致。
    grabKeyboardFocus();

    if (button == &bypassButton)
    {
        if (pluginName.isNotEmpty())
            toggleBypass();
    }
    else if (button == &actionButton)
    {
        if (pluginName.isEmpty())
            listeners.call ([this] (Listener& l) { l.pluginSlotClicked (slotIndex); });
        else
            listeners.call ([this] (Listener& l) { l.pluginSlotDeleteRequested (slotIndex); });
    }
}

//==============================================================================
void PluginSlotComponent::mouseDown (const juce::MouseEvent& event)
{
    mouseDownPos = event.getPosition();
    isDragging = false;
    mouseDownHitArea = getHitArea (event.getPosition());

    if (event.mods.isPopupMenu())
    {
        showContextMenu (event.getPosition());
        return;
    }

    grabKeyboardFocus();
}

//==============================================================================
void PluginSlotComponent::mouseDrag (const juce::MouseEvent& event)
{
    if (isDragging || mouseDownHitArea != HitArea::slot)
        return;

    if (event.getPosition().getDistanceFrom (mouseDownPos) >= dragThresholdPixels)
    {
        if (auto* dragContainer = juce::DragAndDropContainer::findParentDragContainerFor (this))
        {
            isDragging = true;
            dragContainer->startDragging (juce::var (slotIndex), this);
        }
    }
}

//==============================================================================
void PluginSlotComponent::mouseUp (const juce::MouseEvent& event)
{
    if (isDragging || event.mods.isPopupMenu())
        return;

    auto hitArea = getHitArea (event.getPosition());

    if (hitArea == HitArea::slot && mouseDownHitArea == HitArea::slot)
        listeners.call ([this] (Listener& l) { l.pluginSlotClicked (slotIndex); });
}

//==============================================================================
void PluginSlotComponent::mouseDoubleClick (const juce::MouseEvent& event)
{
    if (getHitArea (event.getPosition()) == HitArea::slot && pluginName.isEmpty())
        listeners.call ([this] (Listener& l) { l.pluginSlotClicked (slotIndex); });
}

//==============================================================================
void PluginSlotComponent::focusOfChildComponentChanged (FocusChangeType /*cause*/)
{
    // 子按钮获得/失去焦点时，槽位的高亮外框状态可能改变，需要重绘。
    repaint();
}

//==============================================================================
void PluginSlotComponent::showContextMenu (juce::Point<int> clickPos)
{
    juce::PopupMenu menu;

    if (pluginName.isNotEmpty())
    {
        menu.addItem (1, TRANS ("Open plugin editor"), true, false);
        menu.addItem (2, TRANS ("Replace plugin..."), true, false);
        menu.addItem (3, TRANS (bypassed ? "Unbypass" : "Bypass"), true, bypassed);
        menu.addSeparator();
        menu.addItem (4, TRANS ("Copy plugin"), true, false);
        menu.addItem (5, TRANS ("Paste plugin"), true, false);
        menu.addSeparator();
        menu.addItem (7, TRANS ("Move up"), slotIndex > 0, false);
        menu.addItem (8, TRANS ("Move down"), slotIndex < 11, false);
        menu.addSeparator();
        menu.addItem (6, TRANS ("Delete"), true, false);
    }
    else
    {
        menu.addItem (10, TRANS ("Load plugin..."), true, false);
        menu.addItem (11, TRANS ("Paste plugin"), true, false);
    }

    auto screenPos = localPointToGlobal (clickPos);
    auto targetArea = juce::Rectangle<int> (screenPos.x, screenPos.y, 1, 1);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (targetArea)
                                                              .withMinimumWidth (160),
                        [this] (int result)
    {
        switch (result)
        {
            case 1:
                listeners.call ([this] (Listener& l) { l.pluginSlotClicked (slotIndex); });
                break;
            case 2:
                listeners.call ([this] (Listener& l) { l.pluginSlotReplaceRequested (slotIndex); });
                break;
            case 3:
                toggleBypass();
                break;
            case 4:
                listeners.call ([this] (Listener& l) { l.pluginSlotCopyRequested (slotIndex); });
                break;
            case 5:
            case 11:
                listeners.call ([this] (Listener& l) { l.pluginSlotPasteRequested (slotIndex); });
                break;
            case 6:
                listeners.call ([this] (Listener& l) { l.pluginSlotDeleteRequested (slotIndex); });
                break;
            case 7:
                listeners.call ([this] (Listener& l) { l.pluginSlotMoveRequested (slotIndex, slotIndex - 1); });
                break;
            case 8:
                listeners.call ([this] (Listener& l) { l.pluginSlotMoveRequested (slotIndex, slotIndex + 1); });
                break;
            case 10:
                listeners.call ([this] (Listener& l) { l.pluginSlotClicked (slotIndex); });
                break;
            default:
                break;
        }
    });
}

//==============================================================================
void PluginSlotComponent::setBypassed (bool shouldBypass)
{
    if (pluginName.isEmpty() || bypassed == shouldBypass)
        return;

    bypassed = shouldBypass;
    bypassButton.setBypassState (bypassed);
    listeners.call ([this] (Listener& l) { l.pluginSlotBypassToggled (slotIndex, bypassed); });
    repaint();
}

//==============================================================================
void PluginSlotComponent::setDropTargetHighlighted (bool shouldHighlight)
{
    if (dropTargetHighlighted == shouldHighlight)
        return;

    dropTargetHighlighted = shouldHighlight;
    repaint();
}

//==============================================================================
PluginSlotComponent::HitArea PluginSlotComponent::getHitArea (juce::Point<int> pos) const
{
    if (bypassButton.getBounds().contains (pos))
        return HitArea::bypass;

    if (actionButton.getBounds().contains (pos))
        return HitArea::action;

    if (slotButton.getBounds().contains (pos))
        return HitArea::slot;

    return HitArea::none;
}

//==============================================================================
void PluginSlotComponent::toggleBypass()
{
    if (pluginName.isEmpty())
        return;

    setBypassed (! bypassed);
}

} // namespace minixer
