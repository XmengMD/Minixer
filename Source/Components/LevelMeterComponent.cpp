#include "LevelMeterComponent.h"

namespace minixer
{

//==============================================================================
LevelMeterComponent::LevelMeterComponent()
{
    startTimer (timerIntervalMs);
}

LevelMeterComponent::~LevelMeterComponent()
{
    stopTimer();
}

//==============================================================================
void LevelMeterComponent::setLevel (int channel, float levelDb)
{
    jassert (juce::isPositiveAndBelow (channel, numChannels));
    currentLevelDb[channel].set (levelDb);
}

//==============================================================================
void LevelMeterComponent::setLevels (float leftDb, float rightDb)
{
    setLevel (0, leftDb);
    setLevel (1, rightDb);
}

//==============================================================================
void LevelMeterComponent::reset()
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        currentLevelDb[ch].set (minDb);
        displayedLevelDb[ch] = minDb;
        peakDb[ch] = minDb;
        isClipping[ch] = false;
    }

    repaint();
}

//==============================================================================
void LevelMeterComponent::paint (juce::Graphics& g)
{
    auto bounds = getLocalBounds().toFloat().reduced (2.0f);

    // 底部留出标签高度，剩余区域用于电平条与刻度
    bounds.removeFromBottom (labelHeight);

    // 右侧留出标准 dBFS 刻度区域，宽度按实际最宽标签计算，避免右侧被遮挡
    g.setFont (juce::Font (juce::FontOptions (9.0f)));
    const auto font = g.getCurrentFont();
    const auto tickLength = 4.0f;
    float maxLabelWidth = 0.0f;

    // 使用完整标准刻度计算最宽标签，确保右侧刻度区域足够容纳
    for (auto db : scaleMarksDb)
    {
        juce::String label = (db == 0.0f) ? juce::String ("0")
                             : (db > 0.0f) ? ("+" + juce::String (db, 0))
                             : juce::String (db, 0);
        // 使用 GlyphArrangement 计算文本宽度（替代已弃用的 getStringWidthFloat）
        juce::GlyphArrangement ga;
        ga.addLineOfText (font, label, 0.0f, 0.0f);
        maxLabelWidth = juce::jmax (maxLabelWidth, ga.getBoundingBox (0, -1, true).getWidth());
    }

    const auto scaleWidth = juce::jmin (bounds.getWidth() * 0.55f,
                                        juce::jmax (22.0f, maxLabelWidth + tickLength + 4.0f));
    auto meterBounds = bounds.withTrimmedRight (scaleWidth);

    // 为顶部 +6 与底部 -60 的刻度标签留出垂直空间，避免文字被边界裁切。
    // 刻度与电平条使用同一份内缩后的区域，保证三者对齐。
    constexpr auto scaleLabelHeight = 10.0f;
    auto meterDisplayBounds = meterBounds.reduced (0.0f, scaleLabelHeight * 0.5f);

    const auto barWidth = (meterDisplayBounds.getWidth() - meterGap) * 0.5f;
    auto leftBounds  = meterDisplayBounds.withWidth (barWidth);
    auto rightBounds = meterDisplayBounds.withLeft (meterDisplayBounds.getRight() - barWidth);

    MixerLookAndFeel::drawMeterBar (g, leftBounds,  displayedLevelDb[0], peakDb[0], isClipping[0], maxDb);
    MixerLookAndFeel::drawMeterBar (g, rightBounds, displayedLevelDb[1], peakDb[1], isClipping[1], maxDb);

    // 标准刻度
    drawScale (g, bounds.removeFromRight (scaleWidth), meterDisplayBounds);

    // 过载标签：任一通道削波即显示
    if (isClipping[0] || isClipping[1])
    {
        g.setColour (MixerLookAndFeel::getClipColour());
        g.setFont (juce::Font (juce::FontOptions (8.0f)).boldened());
        g.drawText ("CLIP", meterBounds.reduced (2.0f), juce::Justification::centredTop, false);
    }

    // 底部显示当前计量标准，仅居中于两根电平条宽度内
    auto labelArea = meterBounds.withHeight (labelHeight).withY (meterBounds.getBottom());
    g.setColour (MixerLookAndFeel::getMutedTextColour());
    g.setFont (juce::Font (juce::FontOptions (7.0f)));
    g.drawText (getMeterStandardName (currentStandard), labelArea, juce::Justification::centred, false);
}

//==============================================================================
void LevelMeterComponent::resized()
{
    repaint();
}

//==============================================================================
void LevelMeterComponent::setCurrentStandard (MeterStandard newStandard)
{
    if (currentStandard == newStandard)
        return;

    currentStandard = newStandard;
    reset();

    if (onStandardChanged != nullptr)
        onStandardChanged (currentStandard);
}

//==============================================================================
void LevelMeterComponent::mouseDown (const juce::MouseEvent& event)
{
    if (event.mods.isPopupMenu())
    {
        showStandardMenu();
        return;
    }

    Component::mouseDown (event);
}

//==============================================================================
void LevelMeterComponent::showStandardMenu()
{
    juce::PopupMenu menu;

    const auto addStandardItem = [&menu, this] (MeterStandard standard)
    {
        menu.addItem (static_cast<int> (standard) + 1,
                      getMeterStandardName (standard),
                      true,
                      standard == currentStandard);
    };

    addStandardItem (MeterStandard::dBFS);
    addStandardItem (MeterStandard::RMS);
    addStandardItem (MeterStandard::LUFS_Momentary);
    addStandardItem (MeterStandard::LUFS_ShortTerm);

    menu.addSeparator();
    menu.addItem (resetStandardMenuId,
                  TRANS("Reset to Default (dBFS)"));
    
    menu.showMenuAsync (juce::PopupMenu::Options(),
                        [this] (int result)
    {
        if (result == 0)
            return;

        if (result == resetStandardMenuId)
        {
            setCurrentStandard (getDefaultMeterStandard());
            return;
        }

        auto standard = static_cast<MeterStandard> (result - 1);

        if (standard >= MeterStandard::dBFS && standard <= MeterStandard::LUFS_ShortTerm)
            setCurrentStandard (standard);
    });
}

//==============================================================================
void LevelMeterComponent::timerCallback()
{
    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto targetDb = currentLevelDb[ch].get();

        // 峰值检测
        if (targetDb > peakDb[ch])
        {
            peakDb[ch] = targetDb;
            isClipping[ch] = peakDb[ch] >= clipThresholdDb;
        }
        else
        {
            // 峰值衰减
            auto decayPerFrame = (minDb - peakDb[ch]) * (static_cast<float> (timerIntervalMs) / holdDecayMs);
            peakDb[ch] = juce::jmax (minDb, peakDb[ch] + decayPerFrame);

            if (peakDb[ch] < clipThresholdDb - 0.5f)
                isClipping[ch] = false;
        }

        // 显示电平滑顺衰减
        if (targetDb > displayedLevelDb[ch])
        {
            displayedLevelDb[ch] = targetDb;
        }
        else
        {
            auto decayPerFrame = (minDb - displayedLevelDb[ch]) * (static_cast<float> (timerIntervalMs) / 300.0f);
            displayedLevelDb[ch] = juce::jmax (minDb, displayedLevelDb[ch] + decayPerFrame);
        }
    }

    repaint();
}

//==============================================================================
void LevelMeterComponent::drawScale (juce::Graphics& g, const juce::Rectangle<float>& scaleBounds,
                                     const juce::Rectangle<float>& meterBounds) const
{
    g.setColour (MixerLookAndFeel::getMutedTextColour());
    g.setFont (juce::Font (juce::FontOptions (9.0f)));

    const auto tickLength = 4.0f;
    const auto textX = scaleBounds.getX() + tickLength + 2.0f;
    const auto textWidth = juce::jmax (1.0f, scaleBounds.getRight() - textX);

    // 根据电表高度选择标准刻度子集，0 dBFS 与 +6 dBFS 始终保留
    const float* marksData = nullptr;
    std::size_t marksCount = 0;

    if (meterBounds.getHeight() >= 120.0f)
    {
        marksData = scaleMarksDb.data();
        marksCount = scaleMarksDb.size();
    }
    else if (meterBounds.getHeight() >= 80.0f)
    {
        marksData = scaleMarksDbMedium.data();
        marksCount = scaleMarksDbMedium.size();
    }
    else if (meterBounds.getHeight() >= 50.0f)
    {
        marksData = scaleMarksDbSmall.data();
        marksCount = scaleMarksDbSmall.size();
    }
    else
    {
        marksData = scaleMarksDbTiny.data();
        marksCount = scaleMarksDbTiny.size();
    }

    for (std::size_t i = 0; i < marksCount; ++i)
    {
        auto db = marksData[i];
        auto y = meterBounds.getBottom() - juce::jmap (db, minDb, maxDb, 0.0f, meterBounds.getHeight());

        // 短刻度线
        g.drawHorizontalLine (juce::roundToInt (y), scaleBounds.getX(), scaleBounds.getX() + tickLength);

        // 刻度值
        juce::String label;
        if (db == 0.0f)
            label = "0";
        else if (db > 0.0f)
            label = "+" + juce::String (db, 0);
        else
            label = juce::String (db, 0);

        // 标签垂直居中于刻度线，但限制在刻度区域（scaleBounds）内，防止最底端 -60 被夹到 -54 位置。
        auto labelTop = juce::jlimit (scaleBounds.getY(), scaleBounds.getBottom() - 10.0f, y - 5.0f);
        // 使用 Rectangle<float> 版本的 drawText 避免类型转换警告
        g.drawText (label, juce::Rectangle<float> (textX, labelTop, textWidth, 10.0f),
                    juce::Justification::centredLeft, false);
    }
}

} // namespace minixer
