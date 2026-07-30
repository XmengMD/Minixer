#pragma once

#include <JuceHeader.h>

namespace minixer
{

//==============================================================================
/** 关于页面组件。

    展示软件信息、开源协议、GitHub 地址以及第三方 SDK（ASIO、VST3、JUCE）的许可声明。
*/
class AboutComponent  : public juce::Component
{
public:
    //==============================================================================
    AboutComponent();
    ~AboutComponent() override = default;

    //==============================================================================
    void resized() override;

    //==============================================================================
    std::function<void()> onClose;

private:
    //==============================================================================
    juce::Label titleLabel;
    juce::Label versionLabel;
    juce::Label descriptionLabel;
    juce::HyperlinkButton githubLink;
    juce::TextEditor noticeEditor;
    juce::TextButton closeButton { TRANS("Close") };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AboutComponent)
};

} // namespace minixer
