#include "settings.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace as
{

namespace
{

std::string Trim(const std::string& s)
{
    size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::string FormatDouble(double value, const char* format)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), format, value);
    return buf;
}

} // namespace

bool IniFile::Load()
{
    data_.clear();
    std::ifstream in(path_, std::ios::binary);
    if (!in)
        return false;

    std::string line, section;
    bool first = true;
    while (std::getline(in, line))
    {
        if (first && line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            line.erase(0, 3);  // UTF-8 BOM
        first = false;

        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#')
            continue;
        if (line.front() == '[' && line.back() == ']')
        {
            section = Trim(line.substr(1, line.size() - 2));
            continue;
        }
        size_t eq = line.find('=');
        if (eq != std::string::npos)
            data_[section][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }
    return true;
}

bool IniFile::Save() const
{
    std::error_code ec;
    if (path_.has_parent_path())
        std::filesystem::create_directories(path_.parent_path(), ec);

    std::filesystem::path tmp = path_;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        for (const auto& [section, values] : data_)
        {
            out << '[' << section << "]\n";
            for (const auto& [key, value] : values)
                out << key << '=' << value << '\n';
            out << '\n';
        }
        if (!out)
            return false;
    }
    std::filesystem::rename(tmp, path_, ec);
    return !ec;
}

std::string IniFile::Get(const std::string& section, const std::string& key, const std::string& def) const
{
    auto s = data_.find(section);
    if (s == data_.end())
        return def;
    auto k = s->second.find(key);
    return k == s->second.end() ? def : k->second;
}

double IniFile::GetDouble(const std::string& section, const std::string& key, double def) const
{
    std::string v = Get(section, key);
    if (v.empty())
        return def;
    char* end = nullptr;
    double d = std::strtod(v.c_str(), &end);
    return end == v.c_str() ? def : d;
}

int IniFile::GetInt(const std::string& section, const std::string& key, int def) const
{
    std::string v = Get(section, key);
    if (v.empty())
        return def;
    char* end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    return end == v.c_str() ? def : (int)n;
}

void IniFile::Set(const std::string& section, const std::string& key, const std::string& value)
{
    data_[section][key] = value;
}

void LoadState(const IniFile& ini, const std::string& today, Settings& settings, DayStats& stats)
{
    settings.notifications = ini.GetInt("Settings", "Notifications", 1) != 0;
    settings.topmost = ini.GetInt("Settings", "Topmost", 0) != 0;
    settings.spotify = ini.GetInt("Settings", "Spotify", 1) != 0;
    settings.opacity = std::clamp(ini.GetInt("Settings", "Opacity", 96), 30, 100);
    settings.loudness = std::clamp(ini.GetInt("Settings", "Loudness", 1), 0, LOUDNESS_COUNT - 1);

    stats = DayStats{};
    stats.date = today;
    if (ini.Get("Today", "Date") == today)
    {
        stats.exposure = std::max(0.0, ini.GetDouble("Today", "Exposure", 0.0));
        stats.spotifyExposure = std::max(0.0, ini.GetDouble("Today", "SpotifyExposure", 0.0));
        stats.listenSeconds = std::max(0.0, ini.GetDouble("Today", "ListenSeconds", 0.0));
        stats.peakDb = std::max(0.0, ini.GetDouble("Today", "PeakDb", 0.0));
        stats.alertLevel = std::clamp(ini.GetInt("Today", "Alerts", 0), 0, 3);
    }
}

void StoreState(IniFile& ini, const Settings& settings, const DayStats& stats)
{
    ini.Set("Today", "Date", stats.date);
    ini.Set("Today", "Exposure", FormatDouble(stats.exposure, "%.8f"));
    ini.Set("Today", "SpotifyExposure", FormatDouble(stats.spotifyExposure, "%.8f"));
    ini.Set("Today", "ListenSeconds", FormatDouble(stats.listenSeconds, "%.1f"));
    ini.Set("Today", "PeakDb", FormatDouble(stats.peakDb, "%.1f"));
    ini.Set("Today", "Alerts", std::to_string(stats.alertLevel));

    ini.Set("Settings", "Notifications", settings.notifications ? "1" : "0");
    ini.Set("Settings", "Topmost", settings.topmost ? "1" : "0");
    ini.Set("Settings", "Spotify", settings.spotify ? "1" : "0");
    ini.Set("Settings", "Opacity", std::to_string(settings.opacity));
    ini.Set("Settings", "Loudness", std::to_string(settings.loudness));
}

} // namespace as
