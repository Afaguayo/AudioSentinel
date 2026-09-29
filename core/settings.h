// Settings and today's counters, stored in a small INI file.
#pragma once

#include "exposure.h"

#include <filesystem>
#include <map>
#include <string>

namespace as
{

class IniFile
{
public:
    explicit IniFile(std::filesystem::path path) : path_(std::move(path)) {}

    bool Load();
    // Writes a temporary file and renames it, so a crash never leaves half a file.
    bool Save() const;

    std::string Get(const std::string& section, const std::string& key, const std::string& def = "") const;
    double GetDouble(const std::string& section, const std::string& key, double def) const;
    int GetInt(const std::string& section, const std::string& key, int def) const;
    void Set(const std::string& section, const std::string& key, const std::string& value);

    const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
    std::map<std::string, std::map<std::string, std::string>> data_;
};

struct Settings
{
    bool notifications = true;
    bool topmost = false;
    bool spotify = true;  // show Spotify's track and count its share
    int opacity = 96;     // percent
    int loudness = 1;     // index into LOUDNESS_OFFSETS
};

// Reads settings, and today's counters if they belong to `today`.
void LoadState(const IniFile& ini, const std::string& today, Settings& settings, DayStats& stats);
void StoreState(IniFile& ini, const Settings& settings, const DayStats& stats);

} // namespace as
