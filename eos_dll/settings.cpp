/*
 * Copyright (C) 2020 Nemirtingas
 * This file is part of the Nemirtingas's Epic Emulator
 *
 * The Nemirtingas's Epic Emulator is free software; you can redistribute it
 * and/or modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 * 
 * The Nemirtingas's Epic Emulator is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with the Nemirtingas's Epic Emulator; if not, see
 * <http://www.gnu.org/licenses/>.
 */

#include "settings.h"
#include "eos_client_api.h"
#include <filesystem>
#include <cctype>

static std::filesystem::path utf8_to_fs_path(std::string const& utf8_path)
{
#ifdef __WINDOWS__
    std::wstring wide;
    if (utf8::is_valid(utf8_path.begin(), utf8_path.end()))
    {
        utf8::utf8to16(utf8_path.begin(), utf8_path.end(), std::back_inserter(wide));
    }
    else
    {
        for (unsigned char c : utf8_path)
            wide.push_back(static_cast<wchar_t>(c));
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(utf8_path);
#endif
}

template<typename T>
T get_setting(nlohmann::json& settings, std::string const& key, T default_val)
{
    T val;
    try
    {
        val = settings[key].get<T>();
    }
    catch (...)
    {
        val = default_val;
        settings[key] = default_val;
    }
    return val;
}

Settings::Settings()
{
    load_settings();
}

Settings::~Settings()
{

}

static bool load_json(std::string const& file_path, nlohmann::json& json)
{
    std::ifstream file(utf8_to_fs_path(file_path), std::ios::in | std::ios::binary);
    if (file)
    {
        file.seekg(0, std::ios::end);
        size_t size = static_cast<size_t>(file.tellg());
        file.seekg(0, std::ios::beg);

        std::string buffer(size, '\0');

        file.read(&buffer[0], size);
        file.close();

        try
        {
            json = std::move(nlohmann::json::parse(buffer));

            return true;
        }
        catch (std::exception& e)
        {
            APP_LOG(Log::LogLevel::ERR, "Error while parsing JSON %s: %s", file_path.c_str(), e.what());
        }
    }
    else
    {
        APP_LOG(Log::LogLevel::WARN, "File not found: %s", file_path.c_str());
    }
    return false;
}

static bool save_json(std::string const& file_path, nlohmann::json const& json)
{
    std::ofstream file(utf8_to_fs_path(file_path), std::ios::trunc | std::ios::out | std::ios::binary);
    if (!file)
    {
        APP_LOG(Log::LogLevel::ERR, "Failed to save: %s", file_path.c_str());
        return false;
    }
    file << std::setw(2) << json;
    return true;
}

Settings& Settings::Inst()
{
    static Settings inst;
    return inst;
}

void Settings::load_settings()
{
    bool default_config = false;

    GLOBAL_LOCK();

    nlohmann::json settings;
    config_path = std::move(FileManager::dirname(get_executable_path()) + settings_file_name);

    if (!load_json(config_path, settings))
    {
        std::string dll_cfg = FileManager::dirname(get_module_path()) + settings_file_name;
        if (dll_cfg != config_path && load_json(dll_cfg, settings))
        {
            config_path = std::move(dll_cfg);
        }
        else
        {
            default_config = true;
        }
    }

#ifndef DISABLE_LOG
    Log::LogLevel llvl;
    switchstr(get_setting(settings, "log_level", std::string("OFF")))
    {
        casestr("TRACE"): llvl = Log::LogLevel::TRACE; break;
        casestr("DEBUG"): llvl = Log::LogLevel::DEBUG; break;
        casestr("INFO") : llvl = Log::LogLevel::INFO ; break;
        casestr("WARN") : llvl = Log::LogLevel::WARN ; break;
        casestr("ERR")  : llvl = Log::LogLevel::ERR  ; break;
        casestr("FATAL"): llvl = Log::LogLevel::FATAL; break;
        casestr("OFF")  :
        default         : llvl = Log::LogLevel::OFF;
    }
    Log::set_loglevel(llvl);
    APP_LOG(Log::LogLevel::INFO, "Setting log level to: %s", Log::loglevel_to_str(llvl));
#endif

    auto has_non_placeholder = [&](const char* key, const char* placeholder) -> bool
    {
        if (!settings.contains(key) || !settings[key].is_string())
            return false;
        std::string val = settings[key].get<std::string>();
        return !val.empty() && val != placeholder;
    };
    _has_explicit_gamename      = has_non_placeholder("gamename", "DefaultGameName");
    _has_explicit_appid         = has_non_placeholder("appid", "InvalidAppid");
    _has_explicit_productuserid = has_non_placeholder("productuserid", "");

    APP_LOG(Log::LogLevel::INFO, "Configuration Path: %s", config_path.c_str());
    if (default_config)
    {
        APP_LOG(Log::LogLevel::WARN, "Error while loading settings, building a default one");
    }

    APP_LOG(Log::LogLevel::INFO, "Emulator version %s", _EMU_VERSION_);

    auto is_auto_placeholder = [](std::string const& s) -> bool
    {
        if (s.empty())
            return true;
        if (s.size() != 4)
            return false;
        return std::tolower(static_cast<unsigned char>(s[0])) == 'a' &&
               std::tolower(static_cast<unsigned char>(s[1])) == 'u' &&
               std::tolower(static_cast<unsigned char>(s[2])) == 't' &&
               std::tolower(static_cast<unsigned char>(s[3])) == 'o';
    };

    std::string raw_username = get_setting(settings, "username", std::string(""));
    if (is_auto_placeholder(raw_username))
    {
        raw_username = get_env_var("USERNAME");
        if (raw_username.empty())
            raw_username = get_env_var("USER");
        if (raw_username.empty())
            raw_username = get_env_var("COMPUTERNAME");
        if (raw_username.empty())
            raw_username = "Player";
    }
    username = std::move(raw_username);
    if (username.empty() || !utf8::is_valid(username.begin(), username.end()))
    {
        APP_LOG(Log::LogLevel::WARN, "Invalid username, resetting to default name.");
        username = "Player";
    }

    settings["username"] = username;

    std::string raw_epicid = get_setting(settings, "epicid", std::string(""));
    if (is_auto_placeholder(raw_epicid))
        raw_epicid.clear();
    userid = GetEpicUserId(raw_epicid);
    if (!userid->IsValid())
    {
        APP_LOG(Log::LogLevel::INFO, "Generating unique random epicid for this machine");
        userid = GetEpicUserId(generate_epic_id_user());
    }

    language                          = get_setting(settings, "language", std::string("en"));
    gamename                          = get_setting(settings, "gamename", std::string("DefaultGameName"));
    appid                             = get_setting(settings, "appid", std::string("InvalidAppid"));
    unlock_dlcs                       = get_setting(settings, "unlock_dlcs", bool(true));
    enable_overlay                    = get_setting(settings, "enable_overlay", bool(true));
    disable_online_networking         = get_setting(settings, "disable_online_networking", bool(false));
    enable_lan_broadcast              = get_setting(settings, "enable_lan_broadcast", bool(true));
    keep_lobby_on_destroy             = get_setting(settings, "keep_lobby_on_destroy", bool(false));
    suppress_member_disconnect_notify = get_setting(settings, "suppress_member_disconnect_notify", bool(false));

    int dp = get_setting(settings, "discovery_port", int(55789));
    if (dp < 1024 || dp > 65520)
    {
        APP_LOG(Log::LogLevel::WARN, "discovery_port %d out of range [1024, 65520], resetting to 55789", dp);
        dp = 55789;
    }
    discovery_port = static_cast<uint16_t>(dp);

    int tp = get_setting(settings, "tcp_port", int(0));
    if (tp < 0 || tp > 65535)
    {
        APP_LOG(Log::LogLevel::WARN, "tcp_port %d out of range [0, 65535], resetting to 0", tp);
        tp = 0;
    }
    tcp_port = static_cast<uint16_t>(tp);

    if (settings.contains("custom_broadcast") && settings["custom_broadcast"].is_array())
    {
        custom_broadcast.clear();
        for (auto const& item : settings["custom_broadcast"])
        {
            if (item.is_string())
            {
                std::string val = item.get<std::string>();
                if (!val.empty())
                {
                    if (!custom_broadcast.empty())
                        custom_broadcast += ", ";
                    custom_broadcast += val;
                }
            }
        }
        settings["custom_broadcast"] = custom_broadcast;
    }
    else
    {
        custom_broadcast = get_setting(settings, "custom_broadcast", std::string(""));
    }

    savepath                          = get_setting(settings, "savepath", std::string("appdata"));

    std::string productuserid = get_setting(settings, "productuserid", generate_account_id_from_name(appid + userid->to_string()));
    this->productuserid = GetProductUserId(productuserid);

    std::string settings_dir;
    if (savepath == "appdata")
    {
        settings_dir = get_userdata_path();
    }
    else if (FileManager::is_absolute(savepath))
    {
        settings_dir = savepath;
    }
    else
    {
        settings_dir = FileManager::join(FileManager::dirname(get_executable_path()), savepath);
    }

    FileManager::set_root_dir(settings_dir);
    settings_dir = FileManager::root_dir();
    settings_dir = FileManager::join(settings_dir, emu_savepath, userid->to_string());
    _base_save_root = settings_dir;

    //load_avatar(settings_dir);

    FileManager::set_root_dir(FileManager::join(settings_dir, appid));
    
    save_settings();
}

void Settings::apply_runtime_game_info(std::string const& product_id, std::string const& deployment_id, std::string const& product_name)
{
    GLOBAL_LOCK();

    bool updated = false;
    if (!_has_explicit_gamename && !product_name.empty() && product_name != gamename)
    {
        gamename = product_name;
        updated = true;
    }

    if (!_has_explicit_appid)
    {
        std::string const& candidate = !product_id.empty() ? product_id : deployment_id;
        if (!candidate.empty() && candidate != appid)
        {
            appid = candidate;
            if (!_has_explicit_productuserid)
            {
                this->productuserid = GetProductUserId(generate_account_id_from_name(appid + userid->to_string()));
            }
            if (!_base_save_root.empty())
            {
                FileManager::set_root_dir(FileManager::join(_base_save_root, appid));
            }
            updated = true;
        }
    }

    if (updated)
    {
        APP_LOG(Log::LogLevel::INFO, "[SETTINGS] Auto-derived appid='%s' gamename='%s' from EOS SDK options",
            appid.c_str(), gamename.c_str());
        save_settings();
    }
}

void Settings::save_settings()
{
    nlohmann::json settings;
    APP_LOG(Log::LogLevel::INFO, "Saving emu settings: %s", config_path.c_str());

    settings["appid"]                             = appid;
    settings["username"]                          = username;
    settings["epicid"]                            = userid->to_string();
    settings["productuserid"]                     = productuserid->to_string();
    settings["language"]                          = language;
    settings["gamename"]                          = gamename;
    settings["unlock_dlcs"]                       = unlock_dlcs;
    settings["enable_overlay"]                    = enable_overlay;
    settings["disable_online_networking"]         = disable_online_networking;
    settings["enable_lan_broadcast"]              = enable_lan_broadcast;
    settings["discovery_port"]                    = discovery_port;
    settings["tcp_port"]                          = tcp_port;
    settings["keep_lobby_on_destroy"]             = keep_lobby_on_destroy;
    settings["suppress_member_disconnect_notify"] = suppress_member_disconnect_notify;
#ifndef DISABLE_LOG
    settings["log_level"]                         = Log::loglevel_to_str();
#endif
    settings["custom_broadcast"]                  = custom_broadcast;
    settings["savepath"]                          = savepath;

    save_json(config_path, settings);
}
