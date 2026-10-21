#pragma once

#include <juce_core/juce_core.h>

#include <cstdlib>

namespace cuesampler
{
// Offline validation can redirect all per-user files into its scratch folder.
// An absolute path is required so a host's working directory cannot redirect
// ordinary settings or update-cache writes unexpectedly.
inline juce::File userDataDirectory (const juce::File& defaultDirectory)
{
    if (const auto* overridePath = std::getenv ("CUE_USER_DATA_DIR"))
    {
        const auto path = juce::String::fromUTF8 (overridePath);
        if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
            return juce::File (path);
    }

    return defaultDirectory;
}
}
