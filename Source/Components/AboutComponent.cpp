#include "AboutComponent.h"
#include "../LookAndFeel/MixerLookAndFeel.h"

namespace minixer
{

//==============================================================================
AboutComponent::AboutComponent()
{
    titleLabel.setText (TRANS("Minixer"), juce::dontSendNotification);
    titleLabel.setFont (juce::Font (juce::FontOptions (24.0f)).boldened());
    titleLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getAccentColour());
    titleLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (titleLabel);

    versionLabel.setText (TRANS("Version 1.0.0"), juce::dontSendNotification);
    versionLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
    versionLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getMutedTextColour());
    versionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (versionLabel);

    descriptionLabel.setText (TRANS("A lightweight single-channel standalone audio mixer"),
                              juce::dontSendNotification);
    descriptionLabel.setFont (juce::Font (juce::FontOptions (14.0f)));
    descriptionLabel.setColour (juce::Label::textColourId, MixerLookAndFeel::getTextColour());
    descriptionLabel.setJustificationType (juce::Justification::centred);
    addAndMakeVisible (descriptionLabel);

    githubLink.setButtonText (TRANS("GitHub: github.com/XmengMD/Minixer"));
    githubLink.setURL (juce::URL ("https://github.com/XmengMD/Minixer"));
    githubLink.setColour (juce::HyperlinkButton::textColourId, MixerLookAndFeel::getAccentColour());
    githubLink.setFont (juce::Font (juce::FontOptions (14.0f)), false);
    addAndMakeVisible (githubLink);

    noticeEditor.setMultiLine (true);
    noticeEditor.setReadOnly (true);
    noticeEditor.setCaretVisible (false);
    noticeEditor.setFont (juce::Font (juce::FontOptions (12.0f)));
    noticeEditor.setColour (juce::TextEditor::backgroundColourId, MixerLookAndFeel::getSurfaceColour());
    noticeEditor.setColour (juce::TextEditor::textColourId, MixerLookAndFeel::getTextColour());
    noticeEditor.setColour (juce::TextEditor::outlineColourId, MixerLookAndFeel::getBorderColour());
    noticeEditor.setColour (juce::TextEditor::focusedOutlineColourId, MixerLookAndFeel::getBorderColour());

    noticeEditor.setText (
        TRANS("License\n")
        + TRANS("-------\n")
        + TRANS("Minixer is licensed under the GNU Affero General Public License v3.0 (AGPLv3).\n")
        + TRANS("Source code is available at the GitHub repository above.\n\n")
        + TRANS("Third-Party Notices\n")
        + TRANS("-------------------\n")
        + TRANS("JUCE framework: Copyright Raw Material Software Limited. "
                "Dual-licensed under AGPLv3 and the JUCE commercial licence.\n\n")
        + TRANS("ASIO: ASIO is a trademark and software of Steinberg Media Technologies GmbH. "
                "This application uses the Steinberg ASIO SDK under the terms of the "
                "Steinberg ASIO Licensing Agreement. Redistribution of the ASIO SDK is not permitted.\n\n")
        + TRANS("VST3: VST is a trademark of Steinberg Media Technologies GmbH. "
                "This application uses the Steinberg VST3 SDK under the terms of the "
                "Steinberg VST3 License or GPLv3.\n\n")
        + TRANS("This software is provided \"as is\", without warranty of any kind.\n"));
    addAndMakeVisible (noticeEditor);

    closeButton.setLookAndFeel (&getLookAndFeel());
    closeButton.setColour (juce::TextButton::buttonColourId, MixerLookAndFeel::getSurfaceColour());
    closeButton.setColour (juce::TextButton::textColourOffId, MixerLookAndFeel::getTextColour());
    closeButton.onClick = [this]
    {
        if (onClose != nullptr)
            onClose();
    };
    addAndMakeVisible (closeButton);
}

//==============================================================================
void AboutComponent::resized()
{
    auto bounds = getLocalBounds().reduced (20, 20);

    titleLabel.setBounds (bounds.removeFromTop (34));
    bounds.removeFromTop (4);
    versionLabel.setBounds (bounds.removeFromTop (18));
    bounds.removeFromTop (8);
    descriptionLabel.setBounds (bounds.removeFromTop (20));
    bounds.removeFromTop (12);

    auto linkBounds = bounds.removeFromTop (24);
    githubLink.setBounds (linkBounds.withSizeKeepingCentre (
        juce::jmin (linkBounds.getWidth(), 320), 24));
    bounds.removeFromTop (12);

    closeButton.setBounds (bounds.removeFromBottom (30).withSizeKeepingCentre (100, 30));
    bounds.removeFromBottom (12);

    noticeEditor.setBounds (bounds);
}

} // namespace minixer
