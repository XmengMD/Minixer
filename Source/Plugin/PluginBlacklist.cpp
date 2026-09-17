/*
  ==============================================================================

    PluginBlacklist.cpp

  ==============================================================================
*/

#include "PluginBlacklist.h"
#include "../Settings/AppSettings.h"

namespace minixer
{

namespace
{

//==============================================================================
/** 连续崩溃多少次后标记为永久跳过。 */
constexpr int permanentlySkippedThreshold = 3;

/** 冷静期天数：崩溃后 N 天内不自动重试。 */
constexpr int cooldownDays = 7;

} // anonymous namespace

//==============================================================================
bool BlacklistEntry::isInCooldownPeriod() const noexcept
{
    if (lastCrashTime.toMilliseconds() == 0)
        return false;

    return (juce::Time::getCurrentTime() - lastCrashTime) < juce::RelativeTime::days (cooldownDays);
}

bool BlacklistEntry::shouldSkip() const noexcept
{
    return permanentlySkipped || isInCooldownPeriod();
}

bool BlacklistEntry::canRetryNow() const noexcept
{
    return ! permanentlySkipped;
}

//==============================================================================
PluginBlacklist& PluginBlacklist::getInstance()
{
    static PluginBlacklist instance;
    return instance;
}

PluginBlacklist::PluginBlacklist()
{
    load();
}

//==============================================================================
void PluginBlacklist::load()
{
    entries.clear();

    auto file = getBlacklistFile();

    if (! file.existsAsFile())
        return;

    auto json = juce::JSON::parse (file.loadFileAsString());

    if (! json.isObject())
        return;

    auto* root = json.getDynamicObject();

    if (root == nullptr)
        return;

    auto entriesVar = root->getProperty ("entries");

    if (! entriesVar.isArray())
        return;

    for (const auto& item : *entriesVar.getArray())
    {
        if (! item.isObject())
            continue;

        auto* obj = item.getDynamicObject();

        if (obj == nullptr)
            continue;

        BlacklistEntry entry;
        entry.filePath           = obj->getProperty ("filePath").toString();
        entry.reason             = obj->getProperty ("reason").toString();
        entry.crashCount         = obj->getProperty ("crashCount");
        entry.lastExitCode       = obj->getProperty ("lastExitCode");
        entry.permanentlySkipped = obj->getProperty ("permanentlySkipped");

        auto timeMs = static_cast<juce::int64> (obj->getProperty ("lastCrashTimeMs"));
        entry.lastCrashTime = juce::Time (timeMs);

        if (entry.filePath.isNotEmpty())
            entries.add (std::move (entry));
    }
}

//==============================================================================
void PluginBlacklist::save() const
{
    auto root = std::make_unique<juce::DynamicObject>();
    juce::Array<juce::var> entriesArray;

    for (const auto& entry : entries)
    {
        auto obj = std::make_unique<juce::DynamicObject>();
        obj->setProperty ("filePath",           entry.filePath);
        obj->setProperty ("reason",             entry.reason);
        obj->setProperty ("crashCount",         entry.crashCount);
        obj->setProperty ("lastExitCode",       entry.lastExitCode);
        obj->setProperty ("permanentlySkipped", entry.permanentlySkipped);
        obj->setProperty ("lastCrashTimeMs",    static_cast<juce::int64> (entry.lastCrashTime.toMilliseconds()));

        entriesArray.add (juce::var (obj.release()));
    }

    root->setProperty ("entries", entriesArray);

    auto file = getBlacklistFile();
    file.replaceWithText (juce::JSON::toString (juce::var (root.release()), true));
}

//==============================================================================
void PluginBlacklist::recordCrash (const juce::String& filePath, int exitCode)
{
    std::lock_guard<std::mutex> guard (lock);

    BlacklistEntry entry;

    if (auto* existing = findEntry (filePath))
        entry = *existing;
    else
        entry.filePath = filePath;

    entry.reason = "crash";
    ++entry.crashCount;
    entry.lastCrashTime = juce::Time::getCurrentTime();
    entry.lastExitCode  = exitCode;

    if (entry.crashCount >= permanentlySkippedThreshold)
        entry.permanentlySkipped = true;

    updateEntry (entry);
    save();
}

//==============================================================================
void PluginBlacklist::recordScanFailure (const juce::String& filePath, const juce::String& reason)
{
    std::lock_guard<std::mutex> guard (lock);

    BlacklistEntry entry;

    if (auto* existing = findEntry (filePath))
        entry = *existing;
    else
        entry.filePath = filePath;

    entry.reason = reason.isEmpty() ? "scanFailure" : reason;
    ++entry.crashCount;
    entry.lastCrashTime = juce::Time::getCurrentTime();

    if (entry.crashCount >= permanentlySkippedThreshold)
        entry.permanentlySkipped = true;

    updateEntry (entry);
    save();
}

//==============================================================================
void PluginBlacklist::clearEntry (const juce::String& filePath)
{
    std::lock_guard<std::mutex> guard (lock);

    for (int i = entries.size(); --i >= 0;)
    {
        if (entries.getReference (i).filePath == filePath)
        {
            entries.remove (i);
            save();
            return;
        }
    }
}

//==============================================================================
void PluginBlacklist::clearAll()
{
    std::lock_guard<std::mutex> guard (lock);

    if (entries.isEmpty())
        return;

    entries.clear();
    save();
}

//==============================================================================
bool PluginBlacklist::isBlacklisted (const juce::String& filePath) const
{
    std::lock_guard<std::mutex> guard (lock);

    auto* entry = findEntry (filePath);
    return entry != nullptr && entry->shouldSkip();
}

//==============================================================================
bool PluginBlacklist::canRetry (const juce::String& filePath) const
{
    std::lock_guard<std::mutex> guard (lock);

    auto* entry = findEntry (filePath);
    return entry == nullptr || entry->canRetryNow();
}

//==============================================================================
juce::Array<BlacklistEntry> PluginBlacklist::getEntries() const
{
    std::lock_guard<std::mutex> guard (lock);

    return entries;
}

//==============================================================================
int PluginBlacklist::getNumEntries() const noexcept
{
    std::lock_guard<std::mutex> guard (lock);

    return entries.size();
}

//==============================================================================
BlacklistEntry* PluginBlacklist::findEntry (const juce::String& filePath)
{
    for (auto& entry : entries)
    {
        if (entry.filePath == filePath)
            return &entry;
    }

    return nullptr;
}

const BlacklistEntry* PluginBlacklist::findEntry (const juce::String& filePath) const
{
    return const_cast<PluginBlacklist*> (this)->findEntry (filePath);
}

//==============================================================================
void PluginBlacklist::updateEntry (const BlacklistEntry& entry)
{
    if (auto* existing = findEntry (entry.filePath))
    {
        *existing = entry;
    }
    else
    {
        entries.add (entry);
    }
}

//==============================================================================
juce::File PluginBlacklist::getBlacklistFile() const
{
    return AppSettings::getInstance().getAppDataDirectory().getChildFile ("PluginBlacklist.json");
}

} // namespace minixer
