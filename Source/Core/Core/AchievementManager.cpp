// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef USE_RETRO_ACHIEVEMENTS

#include "Core/AchievementManager.h"

#include <cctype>
#include <memory>

#include <fmt/format.h>

#include <rcheevos/include/rc_api_info.h>

#include "Common/Assert.h"
#include "Common/BitUtils.h"
#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Image.h"
#include "Common/Logging/Log.h"
#include "Common/ScopeGuard.h"
#include "Common/StringUtil.h"
#include "Common/Version.h"
#include "Common/WorkQueueThread.h"
#include "Core/ActionReplay.h"
#include "Core/Config/AchievementSettings.h"
#include "Core/Config/FreeLookSettings.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/GameConfigLoader.h"
#include "Core/Core.h"
#include "Core/GeckoCode.h"
#include "Core/HW/Memmap.h"
#include "Core/HW/VideoInterface.h"
#include "Core/Host.h"
#include "Core/PatchEngine.h"
#include "Core/PowerPC/MMU.h"
#include "Core/System.h"
#include "DiscIO/Blob.h"
#include "UICommon/DiscordPresence.h"
#include "VideoCommon/Assets/CustomTextureData.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoEvents.h"

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
#include <libloaderapi.h>
#include <rcheevos/include/rc_client_raintegration.h>
#include <shlwapi.h>
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

static const Common::HttpRequest::Headers USER_AGENT_HEADER = {
    {"User-Agent", Common::GetUserAgentStr()}};

AchievementManager& AchievementManager::GetInstance()
{
  static AchievementManager s_instance;
  return s_instance;
}

void AchievementManager::Init(void* hwnd)
{
  LoadDefaultBadges();
  if (!m_client && Config::Get(Config::RA_ENABLED))
  {
    {
      std::lock_guard lg{m_lock};
      m_client = rc_client_create(MemoryVerifier, Request);
    }
    std::string host_url = Config::Get(Config::RA_HOST_URL);
    if (!host_url.empty())
      rc_client_set_host(m_client, host_url.c_str());
    rc_client_set_event_handler(m_client, EventHandler);
    rc_client_enable_logging(m_client, RC_CLIENT_LOG_LEVEL_VERBOSE,
                             [](const char* message, const rc_client_t* client) {
                               INFO_LOG_FMT(ACHIEVEMENTS, "{}", message);
                             });
    m_config_changed_callback_id = Config::AddConfigChangedCallback([this] { SetHardcoreMode(); });
    SetHardcoreMode();
    m_queue.Reset("AchievementManagerQueue");
    m_image_queue.Reset("AchievementManagerImageQueue");

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    // Attempt to load the integration DLL from the directory containing the main client executable.
    // In x64 build, will look for RA_Integration-x64.dll, then RA_Integration.dll.
    // In non-x64 build, will only look for RA_Integration.dll.
    rc_client_begin_load_raintegration(
        m_client, UTF8ToWString(File::GetExeDirectory()).c_str(), reinterpret_cast<HWND>(hwnd),
        "Dolphin", Common::GetScmDescStr().c_str(), LoadIntegrationCallback, NULL);
#else   // RC_CLIENT_SUPPORTS_RAINTEGRATION
    if (HasAPIToken())
      Login("");
    INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager Initialized");
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  }
}

picojson::value AchievementManager::LoadApprovedList()
{
  picojson::value temp;
  std::string error;
  if (!JsonFromFile(fmt::format("{}{}{}", File::GetSysDirectory(), DIR_SEP, APPROVED_LIST_FILENAME),
                    &temp, &error))
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load approved game settings list {}",
                 APPROVED_LIST_FILENAME);
    WARN_LOG_FMT(ACHIEVEMENTS, "Error: {}", error);
    return {};
  }
  auto context = Common::SHA1::CreateContext();
  context->Update(temp.serialize());
  auto digest = context->Finish();
  if (digest != APPROVED_LIST_HASH)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to verify approved game settings list {}",
                 APPROVED_LIST_FILENAME);
    WARN_LOG_FMT(ACHIEVEMENTS, "Expected hash {}, found hash {}",
                 Common::SHA1::DigestToString(APPROVED_LIST_HASH),
                 Common::SHA1::DigestToString(digest));
    return {};
  }
  return temp;
}

void AchievementManager::SetUpdateCallback(UpdateCallback callback)
{
  m_update_callback = std::move(callback);

  if (!m_update_callback)
    m_update_callback = [](UpdatedItems) {};

  m_update_callback(UpdatedItems{.all = true});
}

void AchievementManager::Login(const std::string& password)
{
  if (!m_client)
  {
    ERROR_LOG_FMT(
        ACHIEVEMENTS,
        "Attempted login to RetroAchievements server without achievement client initialized.");
    return;
  }
  if (password.empty())
  {
    rc_client_begin_login_with_token(m_client, Config::Get(Config::RA_USERNAME).c_str(),
                                     Config::Get(Config::RA_API_TOKEN).c_str(), LoginCallback,
                                     nullptr);
  }
  else
  {
    rc_client_begin_login_with_password(m_client, Config::Get(Config::RA_USERNAME).c_str(),
                                        password.c_str(), LoginCallback, nullptr);
  }
}

bool AchievementManager::HasAPIToken() const
{
  return !Config::Get(Config::RA_API_TOKEN).empty();
}

void AchievementManager::LoadGame(const DiscIO::Volume* volume)
{
  if (!Config::Get(Config::RA_ENABLED) || !HasAPIToken())
  {
    return;
  }
  if (!m_client)
  {
    ERROR_LOG_FMT(ACHIEVEMENTS,
                  "Attempted to load game achievements without achievement client initialized.");
    return;
  }
  if (volume == nullptr)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Software format unsupported by AchievementManager.");
    if (rc_client_get_game_info(m_client))
    {
      rc_client_begin_change_media_from_hash(m_client, "", ChangeMediaCallback, NULL);
    }
    else
    {
      rc_client_set_read_memory_function(m_client, MemoryVerifier);
      rc_client_begin_load_game(m_client, "", LoadGameCallback, NULL);
    }
    return;
  }
  rc_client_set_unofficial_enabled(m_client, Config::Get(Config::RA_UNOFFICIAL_ENABLED));
  rc_client_set_encore_mode_enabled(m_client, Config::Get(Config::RA_ENCORE_ENABLED));
  rc_client_set_spectator_mode_enabled(m_client, Config::Get(Config::RA_SPECTATOR_ENABLED));
  {
    std::lock_guard lg{m_lock};
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    const auto& names = volume->GetLongNames();
    if (const auto it = names.find(DiscIO::Language::English); it != names.end())
      m_title_estimate = it->second;
    else if (!names.empty())
      m_title_estimate = names.begin()->second;
    else
      m_title_estimate = "";
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    if (!m_loading_volume)
    {
      m_loading_volume = DiscIO::CreateVolume(volume->GetBlobReader().CopyReader());
    }
  }
  std::lock_guard lg{m_filereader_lock};
  rc_hash_filereader volume_reader{
      .open = &AchievementManager::FilereaderOpen,
      .seek = &AchievementManager::FilereaderSeek,
      .tell = &AchievementManager::FilereaderTell,
      .read = &AchievementManager::FilereaderRead,
      .close = &AchievementManager::FilereaderClose,
  };
  rc_hash_init_custom_filereader(&volume_reader);
  if (rc_client_get_game_info(m_client))
  {
    rc_client_begin_identify_and_change_media(m_client, "", NULL, 0, ChangeMediaCallback, NULL);
  }
  else
  {
    u32 console_id = FindConsoleID(volume->GetVolumeType());
    rc_client_set_read_memory_function(m_client, MemoryVerifier);
    rc_client_begin_identify_and_load_game(m_client, console_id, "", NULL, 0, LoadGameCallback,
                                           NULL);
  }
}

bool AchievementManager::IsGameLoaded() const
{
  auto* game_info = rc_client_get_game_info(m_client);
  return game_info && game_info->id != 0;
}

void AchievementManager::SetBackgroundExecutionAllowed(bool allowed)
{
  m_background_execution_allowed = allowed;

  Core::System* system = m_system.load(std::memory_order_acquire);
  if (!system)
    return;

  if (allowed && Core::GetState(*system) == Core::State::Paused)
    DoIdle();
}

std::string AchievementManager::CalculateHash(const std::string& file_path)
{
  char hash_result[33] = "0";
  GetInstance().m_loading_volume = DiscIO::CreateVolume(file_path);
  rc_hash_filereader volume_reader{
      .open = &AchievementManager::FilereaderOpen,
      .seek = &AchievementManager::FilereaderSeek,
      .tell = &AchievementManager::FilereaderTell,
      .read = &AchievementManager::FilereaderRead,
      .close = &AchievementManager::FilereaderClose,
  };
  rc_hash_init_custom_filereader(&volume_reader);
  u32 console_id = FindConsoleID(GetInstance().m_loading_volume->GetVolumeType());
  rc_hash_generate_from_file(hash_result, console_id, file_path.c_str());

  return std::string(hash_result);
}

void AchievementManager::FetchPlayerBadge()
{
  FetchBadge(&m_player_badge, RC_IMAGE_TYPE_USER,
             [](const AchievementManager& manager) {
               auto* user_info = rc_client_get_user_info(manager.m_client);
               if (!user_info)
                 return std::string("");
               return std::string(user_info->display_name);
             },
             {.player_icon = true});
}

void AchievementManager::FetchGameBadges()
{
  FetchBadge(&m_game_badge, RC_IMAGE_TYPE_GAME,
             [](const AchievementManager& manager) {
               auto* game_info = rc_client_get_game_info(manager.m_client);
               if (!game_info)
                 return std::string("");
               return std::string(game_info->badge_name);
             },
             {.game_icon = true});

  if (!rc_client_has_achievements(m_client))
    return;

  rc_client_achievement_list_t* achievement_list;
  {
    std::lock_guard lg{m_lock};
    achievement_list = rc_client_create_achievement_list(
        m_client, RC_CLIENT_ACHIEVEMENT_CATEGORY_CORE_AND_UNOFFICIAL,
        RC_CLIENT_ACHIEVEMENT_LIST_GROUPING_PROGRESS);
  }
  for (u32 bx = 0; bx < achievement_list->num_buckets; bx++)
  {
    auto& bucket = achievement_list->buckets[bx];
    for (u32 achievement = 0; achievement < bucket.num_achievements; achievement++)
    {
      u32 achievement_id = bucket.achievements[achievement]->id;

      FetchBadge(
          &m_unlocked_badges[achievement_id], RC_IMAGE_TYPE_ACHIEVEMENT,
          [achievement_id](const AchievementManager& manager) {
            if (!rc_client_get_achievement_info(manager.m_client, achievement_id))
              return std::string("");
            return std::string(
                rc_client_get_achievement_info(manager.m_client, achievement_id)->badge_name);
          },
          {.achievements = {achievement_id}});
      FetchBadge(
          &m_locked_badges[achievement_id], RC_IMAGE_TYPE_ACHIEVEMENT_LOCKED,
          [achievement_id](const AchievementManager& manager) {
            if (!rc_client_get_achievement_info(manager.m_client, achievement_id))
              return std::string("");
            return std::string(
                rc_client_get_achievement_info(manager.m_client, achievement_id)->badge_name);
          },
          {.achievements = {achievement_id}});
    }
  }
  rc_client_destroy_achievement_list(achievement_list);
}

void AchievementManager::DoFrame()
{
  if (!(IsGameLoaded() || m_dll_found) || !Core::IsCPUThread())
    return;
  {
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
    if (m_dll_found)
    {
      std::lock_guard lg{m_memory_lock};
      Core::System* system = m_system.load(std::memory_order_acquire);
      if (!system)
        return;
      Core::CPUThreadGuard thread_guard(*system);
      u32 mem2_size = (system->IsWii()) ? system->GetMemory().GetExRamSizeReal() : 0;
      if (m_cloned_memory.size() != MEM1_SIZE + mem2_size)
        m_cloned_memory.resize(MEM1_SIZE + mem2_size);
      system->GetMemory().CopyFromEmu(m_cloned_memory.data(), 0, MEM1_SIZE);
      if (mem2_size > 0)
        system->GetMemory().CopyFromEmu(m_cloned_memory.data() + MEM1_SIZE, MEM2_START, mem2_size);
    }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
    std::lock_guard lg{m_lock};
    rc_client_do_frame(m_client);
  }
  auto current_time = std::chrono::steady_clock::now();
  if (current_time - m_last_rp_time > std::chrono::seconds{10})
  {
    m_last_rp_time = current_time;
    rc_client_get_rich_presence_message(m_client, m_rich_presence.data(), RP_SIZE);
    m_update_callback(UpdatedItems{.rich_presence = true});
    if (Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED))
      Discord::UpdateDiscordPresence();
  }
}

bool AchievementManager::CanPause()
{
  if (!IsGameLoaded())
    return true;
  u32 frames_to_next_pause = 0;
  bool can_pause = rc_client_can_pause(m_client, &frames_to_next_pause);
  if (!can_pause)
  {
    OSD::AddMessage(
        fmt::format("RetroAchievements Hardcore Mode:\n"
                    "Cannot pause until another {:.2f} seconds have passed.",
                    static_cast<float>(frames_to_next_pause) /
                        Core::System::GetInstance().GetVideoInterface().GetTargetRefreshRate()),
        OSD::Duration::VERY_LONG, OSD::Color::RED);
  }
  return can_pause;
}

void AchievementManager::DoIdle()
{
  std::thread([this] {
    while (true)
    {
      Common::SleepCurrentThread(1000);
      {
        std::lock_guard lg{m_lock};
        Core::System* system = m_system.load(std::memory_order_acquire);
        if (!system || Core::GetState(*system) != Core::State::Paused)
          return;
        if (!m_background_execution_allowed)
          return;
        if (!m_client || !IsGameLoaded())
          return;
      }
      // rc_client_idle peeks at memory to recalculate rich presence and therefore
      // needs to be on host or CPU thread to access memory.
      Core::QueueHostJob([this](Core::System& system) {
        std::lock_guard lg{m_lock};
        if (Core::GetState(system) != Core::State::Paused)
          return;
        if (!m_background_execution_allowed)
          return;
        if (!m_client || !IsGameLoaded())
          return;
        rc_client_idle(m_client);
      });
    }
  }).detach();
}

std::recursive_mutex& AchievementManager::GetLock()
{
  return m_lock;
}

void AchievementManager::SetHardcoreMode()
{
  rc_client_set_hardcore_enabled(m_client, Config::Get(Config::RA_HARDCORE_ENABLED));
  if (Config::Get(Config::RA_HARDCORE_ENABLED))
  {
    if (Config::Get(Config::MAIN_EMULATION_SPEED) < 1.0f)
      Config::SetBaseOrCurrent(Config::MAIN_EMULATION_SPEED, 1.0f);
    Config::SetBaseOrCurrent(Config::FREE_LOOK_ENABLED, false);
  }
}

bool AchievementManager::IsHardcoreModeActive() const
{
  std::lock_guard lg{m_lock};
  if (!rc_client_get_hardcore_enabled(m_client))
    return false;
  if (!rc_client_get_game_info(m_client))
    return true;
  return rc_client_is_processing_required(m_client);
}

template <typename T>
void AchievementManager::FilterApprovedIni(std::vector<T>& codes, const std::string& game_id,
                                           u16 revision) const
{
  if (codes.empty())
  {
    // There's nothing to verify, so let's save ourselves some work
    return;
  }

  std::lock_guard lg{m_lock};

  if (!IsHardcoreModeActive())
    return;

  // Approved codes list failed to hash
  if (!m_ini_root->is<picojson::value::object>())
  {
    codes.clear();
    return;
  }

  for (auto& code : codes)
  {
    if (code.enabled && !CheckApprovedCode(code, game_id, revision))
      code.enabled = false;
  }
}

template <typename T>
bool AchievementManager::CheckApprovedCode(const T& code, const std::string& game_id,
                                           u16 revision) const
{
  if (!IsHardcoreModeActive())
    return true;

  // Approved codes list failed to hash
  if (!m_ini_root->is<picojson::value::object>())
    return false;

  INFO_LOG_FMT(ACHIEVEMENTS, "Verifying code {}", code.name);

  bool verified = false;

  auto hash = Common::SHA1::DigestToString(GetCodeHash(code));

  for (const std::string& filename : ConfigLoaders::GetGameIniFilenames(game_id, revision))
  {
    auto config = filename.substr(0, filename.length() - 4);
    if (m_ini_root->contains(config) && m_ini_root->get(config).contains(hash))
      verified = true;
  }

  if (!verified)
  {
    OSD::AddMessage(fmt::format("Failed to verify code {} for game ID {}.", code.name, game_id),
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    OSD::AddMessage("Disable hardcore mode to enable this code.", OSD::Duration::VERY_LONG,
                    OSD::Color::RED);
  }
  return verified;
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const PatchEngine::Patch& patch) const
{
  auto context = Common::SHA1::CreateContext();
  context->Update(Common::BitCastToArray<u8>(static_cast<u64>(patch.entries.size())));
  for (const auto& entry : patch.entries)
  {
    context->Update(Common::BitCastToArray<u8>(entry.type));
    context->Update(Common::BitCastToArray<u8>(entry.address));
    context->Update(Common::BitCastToArray<u8>(entry.value));
    context->Update(Common::BitCastToArray<u8>(entry.comparand));
    context->Update(Common::BitCastToArray<u8>(entry.conditional));
  }
  return context->Finish();
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const Gecko::GeckoCode& code) const
{
  auto context = Common::SHA1::CreateContext();
  context->Update(Common::BitCastToArray<u8>(static_cast<u64>(code.codes.size())));
  for (const auto& entry : code.codes)
  {
    context->Update(Common::BitCastToArray<u8>(entry.address));
    context->Update(Common::BitCastToArray<u8>(entry.data));
  }
  return context->Finish();
}

Common::SHA1::Digest AchievementManager::GetCodeHash(const ActionReplay::ARCode& code) const
{
  auto context = Common::SHA1::CreateContext();
  context->Update(Common::BitCastToArray<u8>(static_cast<u64>(code.ops.size())));
  for (const auto& entry : code.ops)
  {
    context->Update(Common::BitCastToArray<u8>(entry.cmd_addr));
    context->Update(Common::BitCastToArray<u8>(entry.value));
  }
  return context->Finish();
}

void AchievementManager::FilterApprovedPatches(std::vector<PatchEngine::Patch>& patches,
                                               const std::string& game_id, u16 revision) const
{
  FilterApprovedIni(patches, game_id, revision);
}

void AchievementManager::FilterApprovedGeckoCodes(std::vector<Gecko::GeckoCode>& codes,
                                                  const std::string& game_id, u16 revision) const
{
  FilterApprovedIni(codes, game_id, revision);
}

void AchievementManager::FilterApprovedARCodes(std::vector<ActionReplay::ARCode>& codes,
                                               const std::string& game_id, u16 revision) const
{
  FilterApprovedIni(codes, game_id, revision);
}

bool AchievementManager::CheckApprovedGeckoCode(const Gecko::GeckoCode& code,
                                                const std::string& game_id, u16 revision) const
{
  return CheckApprovedCode(code, game_id, revision);
}

bool AchievementManager::CheckApprovedARCode(const ActionReplay::ARCode& code,
                                             const std::string& game_id, u16 revision) const
{
  return CheckApprovedCode(code, game_id, revision);
}

void AchievementManager::SetSpectatorMode()
{
  rc_client_set_spectator_mode_enabled(m_client, Config::Get(Config::RA_SPECTATOR_ENABLED));
}

std::string_view AchievementManager::GetPlayerDisplayName() const
{
  if (!HasAPIToken())
    return "";
  auto* user = rc_client_get_user_info(m_client);
  if (!user)
    return "";
  return std::string_view(user->display_name);
}

u32 AchievementManager::GetPlayerScore() const
{
  if (!HasAPIToken())
    return 0;
  auto* user = rc_client_get_user_info(m_client);
  if (!user)
    return 0;
  return user->score;
}

const AchievementManager::Badge& AchievementManager::GetPlayerBadge() const
{
  return m_player_badge.data.empty() ? m_default_player_badge : m_player_badge;
}

std::string_view AchievementManager::GetGameDisplayName() const
{
  return IsGameLoaded() ? std::string_view(rc_client_get_game_info(m_client)->title) : "";
}

rc_client_t* AchievementManager::GetClient()
{
  return m_client;
}

const AchievementManager::Badge& AchievementManager::GetGameBadge() const
{
  return m_game_badge.data.empty() ? m_default_game_badge : m_game_badge;
}

const AchievementManager::Badge& AchievementManager::GetAchievementBadge(AchievementId id,
                                                                         bool locked) const
{
  auto& badge_list = locked ? m_locked_badges : m_unlocked_badges;
  auto itr = badge_list.find(id);
  return (itr != badge_list.end() && itr->second.data.size() > 0) ?
             itr->second :
             (locked ? m_default_locked_badge : m_default_unlocked_badge);
}

const AchievementManager::LeaderboardStatus*
AchievementManager::GetLeaderboardInfo(AchievementManager::AchievementId leaderboard_id)
{
  if (const auto leaderboard_iter = m_leaderboard_map.find(leaderboard_id);
      leaderboard_iter != m_leaderboard_map.end())
  {
    return &leaderboard_iter->second;
  }

  return nullptr;
}

AchievementManager::RichPresence AchievementManager::GetRichPresence() const
{
  return m_rich_presence;
}

bool AchievementManager::AreChallengesUpdated() const
{
  return m_challenges_updated;
}

void AchievementManager::ResetChallengesUpdated()
{
  m_challenges_updated = false;
}

const std::unordered_set<AchievementManager::AchievementId>&
AchievementManager::GetActiveChallenges() const
{
  return m_active_challenges;
}

std::vector<std::string> AchievementManager::GetActiveLeaderboards() const
{
  std::vector<std::string> display_values;
  for (u32 ix = 0; ix < MAX_DISPLAYED_LBOARDS && ix < m_active_leaderboards.size(); ix++)
  {
    display_values.push_back(std::string(m_active_leaderboards[ix].display));
  }
  return display_values;
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
const rc_client_raintegration_menu_t* AchievementManager::GetDevelopmentMenu()
{
  if (!m_dll_found)
    return nullptr;
  return rc_client_raintegration_get_menu(m_client);
}

u32 AchievementManager::ActivateDevMenuItem(u32 menu_item_id)
{
  if (!m_dll_found)
    return 0;
  return rc_client_raintegration_activate_menu_item(m_client, menu_item_id);
}
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

void AchievementManager::DoState(PointerWrap& p)
{
  if (!m_client || !Config::Get(Config::RA_ENABLED))
    return;
  size_t size = 0;
  if (!p.IsReadMode())
    size = rc_client_progress_size(m_client);
  p.Do(size);
  auto buffer = std::make_unique<u8[]>(size);
  if (!p.IsReadMode())
  {
    int result = rc_client_serialize_progress_sized(m_client, buffer.get(), size);
    if (result != RC_OK)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Failed serializing achievement client with error code {}",
                    result);
      return;
    }
  }
  p.DoArray(buffer.get(), (u32)size);
  if (p.IsReadMode())
  {
    int result = rc_client_deserialize_progress_sized(m_client, buffer.get(), size);
    if (result != RC_OK)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Failed deserializing achievement client with error code {}",
                    result);
      return;
    }
    size_t new_size = rc_client_progress_size(m_client);
    if (size != new_size)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Loaded client size {} does not match size in state {}", new_size,
                    size);
      return;
    }
  }
  p.DoMarker("AchievementManager");
}

void AchievementManager::CloseGame()
{
  m_queue.Cancel();
  m_image_queue.Cancel();
  {
    std::lock_guard lg{m_lock};
    m_active_challenges.clear();
    m_active_leaderboards.clear();
    m_game_badge.width = 0;
    m_game_badge.height = 0;
    m_game_badge.data.clear();
    m_unlocked_badges.clear();
    m_locked_badges.clear();
    m_leaderboard_map.clear();
    m_rich_presence.fill('\0');
    m_system.store(nullptr, std::memory_order_release);
    if (Config::Get(Config::RA_DISCORD_PRESENCE_ENABLED))
      Discord::UpdateDiscordPresence();
    if (rc_client_get_game_info(m_client))
      rc_client_unload_game(m_client);
    INFO_LOG_FMT(ACHIEVEMENTS, "Game closed.");
  }

  m_update_callback(UpdatedItems{.all = true});
}

void AchievementManager::Logout()
{
  {
    CloseGame();
    std::lock_guard lg{m_lock};
    m_player_badge.width = 0;
    m_player_badge.height = 0;
    m_player_badge.data.clear();
    Config::SetBaseOrCurrent(Config::RA_API_TOKEN, "");
  }

  m_update_callback(UpdatedItems{.all = true});
  INFO_LOG_FMT(ACHIEVEMENTS, "Logged out from server.");
}

void AchievementManager::Shutdown()
{
  if (m_client)
  {
    CloseGame();
    m_queue.Shutdown();
    Config::RemoveConfigChangedCallback(m_config_changed_callback_id);
    std::lock_guard lg{m_lock};
    // DON'T log out - keep those credentials for next run.
    rc_client_destroy(m_client);
    m_client = nullptr;
    m_dll_found = false;
    INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager shut down.");
  }
}

void* AchievementManager::FilereaderOpen(const char* path_utf8)
{
  auto state = std::make_unique<FilereaderState>();
  {
    auto& instance = GetInstance();
    std::lock_guard lg{instance.GetLock()};
    state->volume = std::move(instance.GetLoadingVolume());
  }
  if (!state->volume)
    return nullptr;
  return state.release();
}

void AchievementManager::FilereaderSeek(void* file_handle, int64_t offset, int origin)
{
  switch (origin)
  {
  case SEEK_SET:
    static_cast<FilereaderState*>(file_handle)->position = offset;
    break;
  case SEEK_CUR:
    static_cast<FilereaderState*>(file_handle)->position += offset;
    break;
  case SEEK_END:
    // Unused
    break;
  }
}

int64_t AchievementManager::FilereaderTell(void* file_handle)
{
  return static_cast<FilereaderState*>(file_handle)->position;
}

size_t AchievementManager::FilereaderRead(void* file_handle, void* buffer, size_t requested_bytes)
{
  FilereaderState* filereader_state = static_cast<FilereaderState*>(file_handle);
  bool success = (filereader_state->volume->Read(filereader_state->position, requested_bytes,
                                                 static_cast<u8*>(buffer), DiscIO::PARTITION_NONE));
  if (success)
  {
    filereader_state->position += requested_bytes;
    return requested_bytes;
  }
  else
  {
    return 0;
  }
}

void AchievementManager::FilereaderClose(void* file_handle)
{
  delete static_cast<FilereaderState*>(file_handle);
}

u32 AchievementManager::FindConsoleID(const DiscIO::Platform& platform)
{
  switch (platform)
  {
  case DiscIO::Platform::GameCubeDisc:
    return RC_CONSOLE_GAMECUBE;
  case DiscIO::Platform::WiiDisc:
  case DiscIO::Platform::WiiWAD:
    return RC_CONSOLE_WII;
  default:
    return RC_CONSOLE_UNKNOWN;
  }
}

void AchievementManager::LoadDefaultBadges()
{
  std::lock_guard lg{m_lock};

  std::string directory = File::GetSysDirectory() + DIR_SEP + RESOURCES_DIR + DIR_SEP;

  if (m_default_player_badge.data.empty())
  {
    if (!LoadPNGTexture(&m_default_player_badge,
                        fmt::format("{}{}", directory, DEFAULT_PLAYER_BADGE_FILENAME)))
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Default player badge '{}' failed to load",
                    DEFAULT_PLAYER_BADGE_FILENAME);
    }
  }

  if (m_default_game_badge.data.empty())
  {
    if (!LoadPNGTexture(&m_default_game_badge,
                        fmt::format("{}{}", directory, DEFAULT_GAME_BADGE_FILENAME)))
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Default game badge '{}' failed to load",
                    DEFAULT_GAME_BADGE_FILENAME);
    }
  }

  if (m_default_unlocked_badge.data.empty())
  {
    if (!LoadPNGTexture(&m_default_unlocked_badge,
                        fmt::format("{}{}", directory, DEFAULT_UNLOCKED_BADGE_FILENAME)))
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Default unlocked achievement badge '{}' failed to load",
                    DEFAULT_UNLOCKED_BADGE_FILENAME);
    }
  }

  if (m_default_locked_badge.data.empty())
  {
    if (!LoadPNGTexture(&m_default_locked_badge,
                        fmt::format("{}{}", directory, DEFAULT_LOCKED_BADGE_FILENAME)))
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Default locked achievement badge '{}' failed to load",
                    DEFAULT_LOCKED_BADGE_FILENAME);
    }
  }
}

void AchievementManager::LoginCallback(int result, const char* error_message, rc_client_t* client,
                                       void* userdata)
{
  if (result != RC_OK)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to login {} to RetroAchievements server.",
                 Config::Get(Config::RA_USERNAME));
    AchievementManager::GetInstance().m_update_callback({.failed_login_code = result});
    return;
  }

  const rc_client_user_t* user;
  {
    std::lock_guard lg{AchievementManager::GetInstance().GetLock()};
    user = rc_client_get_user_info(client);
  }
  if (!user)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to retrieve user information from client.");
    AchievementManager::GetInstance().m_update_callback({.failed_login_code = RC_INVALID_STATE});
    return;
  }

  std::string config_username = Config::Get(Config::RA_USERNAME);
  if (config_username != user->username)
  {
    if (Common::CaseInsensitiveEquals(config_username, user->username))
    {
      INFO_LOG_FMT(ACHIEVEMENTS,
                   "Case mismatch between site {} and local {}; updating local config.",
                   user->username, Config::Get(Config::RA_USERNAME));
      Config::SetBaseOrCurrent(Config::RA_USERNAME, user->username);
    }
    else
    {
      INFO_LOG_FMT(ACHIEVEMENTS, "Attempted to login prior user {}; current user is {}.",
                   user->username, Config::Get(Config::RA_USERNAME));
      rc_client_logout(client);
      AchievementManager::GetInstance().m_update_callback({.failed_login_code = RC_INVALID_STATE});
      return;
    }
  }
  INFO_LOG_FMT(ACHIEVEMENTS, "Successfully logged in {} to RetroAchievements server.",
               user->username);
  std::lock_guard lg{AchievementManager::GetInstance().GetLock()};
  Config::SetBaseOrCurrent(Config::RA_API_TOKEN, user->token);
  AchievementManager::GetInstance().FetchPlayerBadge();
}

void AchievementManager::FetchBoardInfo(AchievementId leaderboard_id)
{
  u32* callback_data_1 = new u32(leaderboard_id);
  u32* callback_data_2 = new u32(leaderboard_id);
  rc_client_begin_fetch_leaderboard_entries(m_client, leaderboard_id, 1, 4,
                                            LeaderboardEntriesCallback, callback_data_1);
  rc_client_begin_fetch_leaderboard_entries_around_user(
      m_client, leaderboard_id, 4, LeaderboardEntriesCallback, callback_data_2);
}

void AchievementManager::LeaderboardEntriesCallback(int result, const char* error_message,
                                                    rc_client_leaderboard_entry_list_t* list,
                                                    rc_client_t* client, void* userdata)
{
  u32* leaderboard_id = static_cast<u32*>(userdata);
  Common::ScopeGuard on_end_scope([&] { delete leaderboard_id; });

  if (result != RC_OK)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to fetch leaderboard entries.");
    return;
  }

  auto& leaderboard = AchievementManager::GetInstance().m_leaderboard_map[*leaderboard_id];
  for (size_t ix = 0; ix < list->num_entries; ix++)
  {
    std::lock_guard lg{AchievementManager::GetInstance().GetLock()};
    const auto& response_entry = list->entries[ix];
    auto& map_entry = leaderboard.entries[response_entry.index];
    map_entry.username.assign(response_entry.user);
    memcpy(map_entry.score.data(), response_entry.display, FORMAT_SIZE);
    map_entry.rank = response_entry.rank;
    if (static_cast<int32_t>(ix) == list->user_index)
      leaderboard.player_index = response_entry.rank;
  }
  AchievementManager::GetInstance().m_update_callback({.leaderboards = {*leaderboard_id}});
}

void AchievementManager::LoadGameCallback(int result, const char* error_message,
                                          rc_client_t* client, void* userdata)
{
  auto& instance = AchievementManager::GetInstance();
  instance.m_loading_volume.reset(nullptr);
  if (result == RC_API_FAILURE)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Load data request rejected for old Dolphin version.");
    OSD::AddMessage("RetroAchievements no longer supports this version of Dolphin.",
                    OSD::Duration::VERY_LONG, OSD::Color::RED);
    OSD::AddMessage("Please update Dolphin to a newer version.", OSD::Duration::VERY_LONG,
                    OSD::Color::RED);
    return;
  }

  auto* game = rc_client_get_game_info(client);
  if (result == RC_OK)
  {
    if (!game)
    {
      ERROR_LOG_FMT(ACHIEVEMENTS, "Failed to retrieve game information from client.");
      OSD::AddMessage("Failed to load achievements for this title.", OSD::Duration::VERY_LONG,
                      OSD::Color::RED);
    }
    else
    {
      INFO_LOG_FMT(ACHIEVEMENTS, "Loaded data for game ID {}.", game->id);
      instance.m_display_welcome_message = true;
    }
  }
  else
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load data for current game.");
    OSD::AddMessage("Achievements are not supported for this title.", OSD::Duration::VERY_LONG,
                    OSD::Color::RED);
  }

  rc_client_set_read_memory_function(instance.m_client, MemoryPeeker);
  instance.FetchGameBadges();
  instance.m_system.store(&Core::System::GetInstance(), std::memory_order_release);
  instance.m_update_callback({.all = true});
  // Set this to a value that will immediately trigger RP
  instance.m_last_rp_time = std::chrono::steady_clock::now() - std::chrono::minutes{2};

  std::lock_guard lg{instance.GetLock()};
  auto* leaderboard_list =
      rc_client_create_leaderboard_list(client, RC_CLIENT_LEADERBOARD_LIST_GROUPING_NONE);
  for (u32 bucket = 0; bucket < leaderboard_list->num_buckets; bucket++)
  {
    const auto& leaderboard_bucket = leaderboard_list->buckets[bucket];
    for (u32 board = 0; board < leaderboard_bucket.num_leaderboards; board++)
    {
      const auto& leaderboard = leaderboard_bucket.leaderboards[board];
      instance.m_leaderboard_map.insert(
          std::pair(leaderboard->id, LeaderboardStatus{.name = leaderboard->title,
                                                       .description = leaderboard->description}));
    }
  }
  rc_client_destroy_leaderboard_list(leaderboard_list);
}

void AchievementManager::ChangeMediaCallback(int result, const char* error_message,
                                             rc_client_t* client, void* userdata)
{
  AchievementManager::GetInstance().m_loading_volume.reset(nullptr);
  if (result == RC_OK)
  {
    return;
  }

  if (result == RC_HARDCORE_DISABLED)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Hardcore disabled. Unrecognized media inserted.");
  }
  else
  {
    if (!error_message)
      error_message = rc_error_str(result);

    ERROR_LOG_FMT(ACHIEVEMENTS, "RetroAchievements media change failed: {}", error_message);
  }
}

void AchievementManager::DisplayWelcomeMessage()
{
  std::lock_guard lg{m_lock};
  m_display_welcome_message = false;
  const u32 color =
      rc_client_get_hardcore_enabled(m_client) ? OSD::Color::YELLOW : OSD::Color::CYAN;

  OSD::AddMessage("", OSD::Duration::VERY_LONG, OSD::Color::GREEN, &GetGameBadge());
  auto info = rc_client_get_game_info(m_client);
  if (!info)
  {
    ERROR_LOG_FMT(ACHIEVEMENTS, "Attempting to welcome player to game not running.");
    return;
  }
  OSD::AddMessage(info->title, OSD::Duration::VERY_LONG, OSD::Color::GREEN);
  rc_client_user_game_summary_t summary;
  rc_client_get_user_game_summary(m_client, &summary);
  OSD::AddMessage(fmt::format("You have {}/{} achievements worth {}/{} points",
                              summary.num_unlocked_achievements, summary.num_core_achievements,
                              summary.points_unlocked, summary.points_core),
                  OSD::Duration::VERY_LONG, color);
  if (summary.num_unsupported_achievements > 0)
  {
    OSD::AddMessage(
        fmt::format("{} achievements unsupported", summary.num_unsupported_achievements),
        OSD::Duration::VERY_LONG, OSD::Color::RED);
  }
  OSD::AddMessage(
      fmt::format("Hardcore mode is {}", rc_client_get_hardcore_enabled(m_client) ? "ON" : "OFF"),
      OSD::Duration::VERY_LONG, color);
  OSD::AddMessage(fmt::format("Leaderboard submissions are {}",
                              rc_client_get_hardcore_enabled(m_client) ? "ON" : "OFF"),
                  OSD::Duration::VERY_LONG, color);
}

void AchievementManager::HandleAchievementTriggeredEvent(const rc_client_event_t* client_event)
{
  const auto& instance = AchievementManager::GetInstance();

  OSD::AddMessage(fmt::format("Unlocked: {} ({})", client_event->achievement->title,
                              client_event->achievement->points),
                  OSD::Duration::VERY_LONG,
                  (rc_client_get_hardcore_enabled(instance.m_client)) ? OSD::Color::YELLOW :
                                                                        OSD::Color::CYAN,
                  &instance.GetAchievementBadge(client_event->achievement->id, false));
  AchievementManager::GetInstance().m_update_callback(
      UpdatedItems{.achievements = {client_event->achievement->id}});
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  switch (rc_client_raintegration_get_achievement_state(instance.m_client,
                                                        client_event->achievement->id))
  {
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_LOCAL:
    // Achievement only exists locally and has not been uploaded.
    OSD::AddMessage("Local achievement; not submitted to site.", OSD::Duration::VERY_LONG,
                    OSD::Color::GREEN);
    break;
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_MODIFIED:
    // Achievement has been modified locally and differs from the one on the site.
    OSD::AddMessage("Modified achievement; not submitted to site.", OSD::Duration::VERY_LONG,
                    OSD::Color::GREEN);
    break;
  case RC_CLIENT_RAINTEGRATION_ACHIEVEMENT_STATE_INSECURE:
    // The player has done something that we consider cheating like modifying the RAM while playing.
    // Just indicate that the achievement was only unlocked locally, but don't clarify why.
    OSD::AddMessage("Achievement not submitted to site.", OSD::Duration::VERY_LONG,
                    OSD::Color::GREEN);
    break;
  default:
    break;
  }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
}

void AchievementManager::HandleLeaderboardStartedEvent(const rc_client_event_t* client_event)
{
  OSD::AddMessage(fmt::format("Attempting leaderboard: {} - {}", client_event->leaderboard->title,
                              client_event->leaderboard->description),
                  OSD::Duration::VERY_LONG, OSD::Color::GREEN);
  AchievementManager::GetInstance().FetchBoardInfo(client_event->leaderboard->id);
}

void AchievementManager::HandleLeaderboardFailedEvent(const rc_client_event_t* client_event)
{
  OSD::AddMessage(fmt::format("Failed leaderboard: {}", client_event->leaderboard->title),
                  OSD::Duration::VERY_LONG, OSD::Color::RED);
  AchievementManager::GetInstance().FetchBoardInfo(client_event->leaderboard->id);
}

void AchievementManager::HandleLeaderboardSubmittedEvent(const rc_client_event_t* client_event)
{
  OSD::AddMessage(fmt::format("Scored {} on leaderboard: {}",
                              client_event->leaderboard->tracker_value,
                              client_event->leaderboard->title),
                  OSD::Duration::VERY_LONG, OSD::Color::YELLOW);
  AchievementManager::GetInstance().FetchBoardInfo(client_event->leaderboard->id);
  AchievementManager::GetInstance().m_update_callback(
      UpdatedItems{.leaderboards = {client_event->leaderboard->id}});
}

void AchievementManager::HandleLeaderboardTrackerUpdateEvent(const rc_client_event_t* client_event)
{
  auto& active_leaderboards = AchievementManager::GetInstance().m_active_leaderboards;
  for (auto& leaderboard : active_leaderboards)
  {
    if (leaderboard.id == client_event->leaderboard_tracker->id)
    {
      strncpy(leaderboard.display, client_event->leaderboard_tracker->display,
              RC_CLIENT_LEADERBOARD_DISPLAY_SIZE);
    }
  }
}

void AchievementManager::HandleLeaderboardTrackerShowEvent(const rc_client_event_t* client_event)
{
  AchievementManager::GetInstance().m_active_leaderboards.push_back(
      *client_event->leaderboard_tracker);
}

void AchievementManager::HandleLeaderboardTrackerHideEvent(const rc_client_event_t* client_event)
{
  auto& active_leaderboards = AchievementManager::GetInstance().m_active_leaderboards;
  std::erase_if(active_leaderboards, [client_event](const auto& leaderboard) {
    return leaderboard.id == client_event->leaderboard_tracker->id;
  });
}

void AchievementManager::HandleAchievementChallengeIndicatorShowEvent(
    const rc_client_event_t* client_event)
{
  auto& instance = AchievementManager::GetInstance();
  const auto [iter, inserted] = instance.m_active_challenges.insert(client_event->achievement->id);
  if (inserted)
    instance.m_challenges_updated = true;
  AchievementManager::GetInstance().m_update_callback(UpdatedItems{.rich_presence = true});
}

void AchievementManager::HandleAchievementChallengeIndicatorHideEvent(
    const rc_client_event_t* client_event)
{
  auto& instance = AchievementManager::GetInstance();
  const auto removed = instance.m_active_challenges.erase(client_event->achievement->id);
  if (removed > 0)
    instance.m_challenges_updated = true;
  AchievementManager::GetInstance().m_update_callback(UpdatedItems{.rich_presence = true});
}

void AchievementManager::HandleAchievementProgressIndicatorShowEvent(
    const rc_client_event_t* client_event)
{
  auto& instance = AchievementManager::GetInstance();
  auto current_time = std::chrono::steady_clock::now();
  const auto message_wait_time = std::chrono::milliseconds{OSD::Duration::SHORT};
  if (current_time - instance.m_last_progress_message < message_wait_time)
    return;
  OSD::AddMessage(fmt::format("{} {}", client_event->achievement->title,
                              client_event->achievement->measured_progress),
                  OSD::Duration::SHORT, OSD::Color::GREEN,
                  &instance.GetAchievementBadge(client_event->achievement->id, false));
  instance.m_last_progress_message = current_time;
  AchievementManager::GetInstance().m_update_callback(
      UpdatedItems{.achievements = {client_event->achievement->id}});
}

void AchievementManager::HandleGameCompletedEvent(const rc_client_event_t* client_event,
                                                  rc_client_t* client)
{
  auto* user_info = rc_client_get_user_info(client);
  auto* game_info = rc_client_get_game_info(client);
  if (!user_info || !game_info)
  {
    WARN_LOG_FMT(ACHIEVEMENTS, "Received Game Completed event when game not running.");
    return;
  }
  bool hardcore = rc_client_get_hardcore_enabled(client);
  OSD::AddMessage(fmt::format("Congratulations! {} has {} {}", user_info->display_name,
                              hardcore ? "mastered" : "completed", game_info->title),
                  OSD::Duration::VERY_LONG, hardcore ? OSD::Color::YELLOW : OSD::Color::CYAN,
                  &AchievementManager::GetInstance().GetGameBadge());
}

void AchievementManager::HandleResetEvent(const rc_client_event_t* client_event)
{
  INFO_LOG_FMT(ACHIEVEMENTS, "Reset requested by Achievement Manager");
  Core::Stop(Core::System::GetInstance());
}

void AchievementManager::HandleServerErrorEvent(const rc_client_event_t* client_event)
{
  ERROR_LOG_FMT(ACHIEVEMENTS, "RetroAchievements server error: {} {}",
                client_event->server_error->api, client_event->server_error->error_message);
}

void AchievementManager::Request(const rc_api_request_t* request,
                                 rc_client_server_callback_t callback, void* callback_data,
                                 rc_client_t* client)
{
  std::string url = request->url;
  std::string post_data = request->post_data;
  AchievementManager::GetInstance().m_queue.Push(
      [url = std::move(url), post_data = std::move(post_data), callback, callback_data] {
        Common::HttpRequest http_request;
        Common::HttpRequest::Response http_response;
        if (!post_data.empty())
        {
          http_response = http_request.Post(url, post_data, USER_AGENT_HEADER,
                                            Common::HttpRequest::AllowedReturnCodes::All);
        }
        else
        {
          http_response = http_request.Get(url, USER_AGENT_HEADER,
                                           Common::HttpRequest::AllowedReturnCodes::All);
        }

        rc_api_server_response_t server_response;
        if (http_response.has_value() && http_response->size() > 0)
        {
          server_response.body = reinterpret_cast<const char*>(http_response->data());
          server_response.body_length = http_response->size();
          server_response.http_status_code = http_request.GetLastResponseCode();
        }
        else
        {
          static constexpr char error_message[] = "Failed HTTP request.";
          server_response.body = error_message;
          server_response.body_length = sizeof(error_message);
          server_response.http_status_code = RC_API_SERVER_RESPONSE_RETRYABLE_CLIENT_ERROR;
        }

        callback(&server_response, callback_data);
      });
}

// Currently, when rc_client calls the memory peek method provided in its constructor (or in
// rc_client_set_read_memory_function) it will do so on the thread that calls DoFrame, which is
// currently the host thread, with one exception: an asynchronous callback in the load game process.
// This is done to validate/invalidate each memory reference in the downloaded assets, mark assets
// as unsupported, and notify the player upon startup that there are unsupported assets and how
// many. As such, all that call needs to do is return the number of bytes that can be read with this
// call. As only the CPU and host threads are allowed to read from memory, I provide a separate
// method for this verification. In lieu of a more convenient set of steps, I provide MemoryVerifier
// to rc_client at construction, and in the Load Game callback, after the verification has been
// complete, I call rc_client_set_read_memory_function to switch to the usual MemoryPeeker for all
// future synchronous calls.
u32 AchievementManager::MemoryVerifier(u32 address, u8* buffer, u32 num_bytes, rc_client_t* client)
{
  auto& system = Core::System::GetInstance();
  u32 mem2_size = system.GetMemory().GetExRamSizeReal();
  if (address < MEM1_SIZE + mem2_size)
    return std::min(MEM1_SIZE + mem2_size - address, num_bytes);
  return 0;
}

u32 AchievementManager::MemoryPeeker(u32 address, u8* buffer, u32 num_bytes, rc_client_t* client)
{
  if (buffer == nullptr)
    return 0u;
#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
  auto& instance = AchievementManager::GetInstance();
  if (instance.m_dll_found)
  {
    std::lock_guard lg{instance.m_memory_lock};
    if (u64(address) + num_bytes > instance.m_cloned_memory.size())
    {
      ERROR_LOG_FMT(ACHIEVEMENTS,
                    "Attempt to read past memory size: size {} address {} write length {}",
                    instance.m_cloned_memory.size(), address, num_bytes);
      return 0;
    }
    std::copy(instance.m_cloned_memory.begin() + address,
              instance.m_cloned_memory.begin() + address + num_bytes, buffer);
    return num_bytes;
  }
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION
  auto& system = Core::System::GetInstance();
  if (!(Core::IsHostThread() || Core::IsCPUThread()))
  {
    ASSERT_MSG(ACHIEVEMENTS, false, "MemoryPeeker called from wrong thread");
    return 0;
  }
  Core::CPUThreadGuard thread_guard(system);
  if (address > MEM1_SIZE)
    address += (MEM2_START - MEM1_SIZE);
  for (u32 num_read = 0; num_read < num_bytes; num_read++)
  {
    auto value = system.GetMMU().HostTryReadU8(thread_guard, address + num_read,
                                               PowerPC::RequestedAddressSpace::Physical);
    if (!value.has_value())
      return num_read;
    buffer[num_read] = value.value().value;
  }
  return num_bytes;
}

void AchievementManager::FetchBadge(AchievementManager::Badge* badge, u32 badge_type,
                                    const AchievementManager::BadgeNameFunction function,
                                    UpdatedItems callback_data)
{
  if (!m_client || !HasAPIToken())
  {
    m_update_callback(callback_data);
    if (m_display_welcome_message && badge_type == RC_IMAGE_TYPE_GAME)
      DisplayWelcomeMessage();
    return;
  }

  m_image_queue.Push([this, badge, badge_type, function = std::move(function),
                      callback_data = std::move(callback_data)] {
    Common::ScopeGuard on_end_scope([&] {
      if (m_display_welcome_message && badge_type == RC_IMAGE_TYPE_GAME)
        DisplayWelcomeMessage();
    });

    std::string name_to_fetch;
    {
      std::lock_guard lg{m_lock};
      name_to_fetch = function(*this);
      if (name_to_fetch.empty())
        return;
    }

    const std::string cache_path = fmt::format(
        "{}/badge-{}-{}.png", File::GetUserPath(D_RETROACHIEVEMENTSCACHE_IDX), badge_type,
        Common::SHA1::DigestToString(Common::SHA1::CalculateDigest(name_to_fetch)));

    AchievementManager::Badge tmp_badge;
    if (!LoadPNGTexture(&tmp_badge, cache_path))
    {
      rc_api_fetch_image_request_t icon_request = {.image_name = name_to_fetch.c_str(),
                                                   .image_type = badge_type};
      Badge fetched_badge;
      rc_api_request_t api_request;
      Common::HttpRequest http_request;
      if (rc_api_init_fetch_image_request(&api_request, &icon_request) != RC_OK)
      {
        ERROR_LOG_FMT(ACHIEVEMENTS, "Invalid request for image {}.", name_to_fetch);
        return;
      }
      auto http_response = http_request.Get(api_request.url, USER_AGENT_HEADER,
                                            Common::HttpRequest::AllowedReturnCodes::All);
      if (!http_response.has_value() || http_response->empty())
      {
        WARN_LOG_FMT(ACHIEVEMENTS,
                     "RetroAchievements connection failed on image request.\n URL: {}",
                     api_request.url);
        rc_api_destroy_request(&api_request);
        m_update_callback(callback_data);
        return;
      }

      rc_api_destroy_request(&api_request);

      INFO_LOG_FMT(ACHIEVEMENTS, "Successfully downloaded badge id {}.", name_to_fetch);

      if (!LoadPNGTexture(&tmp_badge, *http_response))
      {
        ERROR_LOG_FMT(ACHIEVEMENTS, "Badge '{}' failed to load", name_to_fetch);
        return;
      }

      std::string temp_path = fmt::format("{}.tmp", cache_path);
      File::IOFile temp_file(temp_path, "wb");
      if (!temp_file.IsOpen() ||
          !temp_file.WriteBytes(http_response->data(), http_response->size()) ||
          !temp_file.Close() || !File::Rename(temp_path, cache_path))
      {
        File::Delete(temp_path);
        WARN_LOG_FMT(ACHIEVEMENTS, "Failed to store badge '{}' to cache", name_to_fetch);
      }
    }

    std::lock_guard lg{m_lock};
    if (function(*this).empty() || name_to_fetch != function(*this))
    {
      INFO_LOG_FMT(ACHIEVEMENTS, "Requested outdated badge id {}.", name_to_fetch);
      return;
    }

    *badge = std::move(tmp_badge);
    m_update_callback(callback_data);
    if (badge_type == RC_IMAGE_TYPE_ACHIEVEMENT &&
        m_active_challenges.contains(*callback_data.achievements.begin()))
    {
      m_challenges_updated = true;
    }
  });
}

void AchievementManager::EventHandler(const rc_client_event_t* event, rc_client_t* client)
{
  switch (event->type)
  {
  case RC_CLIENT_EVENT_ACHIEVEMENT_TRIGGERED:
    HandleAchievementTriggeredEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_STARTED:
    HandleLeaderboardStartedEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_FAILED:
    HandleLeaderboardFailedEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_SUBMITTED:
    HandleLeaderboardSubmittedEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_UPDATE:
    HandleLeaderboardTrackerUpdateEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_SHOW:
    HandleLeaderboardTrackerShowEvent(event);
    break;
  case RC_CLIENT_EVENT_LEADERBOARD_TRACKER_HIDE:
    HandleLeaderboardTrackerHideEvent(event);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_SHOW:
    HandleAchievementChallengeIndicatorShowEvent(event);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_CHALLENGE_INDICATOR_HIDE:
    HandleAchievementChallengeIndicatorHideEvent(event);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_SHOW:
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_UPDATE:
    HandleAchievementProgressIndicatorShowEvent(event);
    break;
  case RC_CLIENT_EVENT_ACHIEVEMENT_PROGRESS_INDICATOR_HIDE:
    // OnScreenDisplay messages disappear over time, so this is unnecessary
    // unless the display algorithm changes in the future.
    break;
  case RC_CLIENT_EVENT_GAME_COMPLETED:
    HandleGameCompletedEvent(event, client);
    break;
  case RC_CLIENT_EVENT_RESET:
    HandleResetEvent(event);
    break;
  case RC_CLIENT_EVENT_SERVER_ERROR:
    HandleServerErrorEvent(event);
    break;
  default:
    INFO_LOG_FMT(ACHIEVEMENTS, "Event triggered of unhandled type {}", event->type);
    break;
  }
}

#ifdef RC_CLIENT_SUPPORTS_RAINTEGRATION
void AchievementManager::LoadIntegrationCallback(int result, const char* error_message,
                                                 rc_client_t* client, void* userdata)
{
  auto& instance = AchievementManager::GetInstance();
  switch (result)
  {
  case RC_OK:
    INFO_LOG_FMT(ACHIEVEMENTS, "RAIntegration.dll found.");
    instance.m_dll_found = true;
    rc_client_raintegration_set_event_handler(instance.m_client, RAIntegrationEventHandler);
    rc_client_raintegration_set_write_memory_function(instance.m_client, MemoryPoker);
    rc_client_raintegration_set_get_game_name_function(instance.m_client, GameTitleEstimateHandler);
    instance.m_dev_menu_callback();
    // TODO: hook up menu and dll event handlers
    break;

  case RC_MISSING_VALUE:
    INFO_LOG_FMT(ACHIEVEMENTS, "RAIntegration.dll not found.");
    // DLL is not present; do nothing.
    break;

  default:
    WARN_LOG_FMT(ACHIEVEMENTS, "Failed to load RAIntegration.dll. {}", error_message);
    break;
  }

  if (instance.HasAPIToken())
    instance.Login("");
  INFO_LOG_FMT(ACHIEVEMENTS, "Achievement Manager Initialized");
}

void AchievementManager::RAIntegrationEventHandler(const rc_client_raintegration_event_t* event,
                                                   rc_client_t* client)
{
  auto& instance = AchievementManager::GetInstance();
  switch (event->type)
  {
  case RC_CLIENT_RAINTEGRATION_EVENT_MENU_CHANGED:
  case RC_CLIENT_RAINTEGRATION_EVENT_MENUITEM_CHECKED_CHANGED:
    instance.m_dev_menu_callback();
    break;
  case RC_CLIENT_RAINTEGRATION_EVENT_PAUSE:
  {
    Core::QueueHostJob([](Core::System& system) { Core::SetState(system, Core::State::Paused); });
    break;
  }
  case RC_CLIENT_RAINTEGRATION_EVENT_HARDCORE_CHANGED:
    Config::SetBaseOrCurrent(Config::RA_HARDCORE_ENABLED,
                             !Config::Get(Config::RA_HARDCORE_ENABLED));
    break;
  default:
    WARN_LOG_FMT(ACHIEVEMENTS, "Unsupported raintegration event. {}", event->type);
    break;
  }
}

void AchievementManager::MemoryPoker(u32 address, u8* buffer, u32 num_bytes, rc_client_t* client)
{
  if (buffer == nullptr)
    return;
  if (!(Core::IsHostThread() || Core::IsCPUThread()))
  {
    Core::QueueHostJob([address, buffer, num_bytes, client](Core::System& system) {
      MemoryPoker(address, buffer, num_bytes, client);
    });
    return;
  }
  auto& instance = AchievementManager::GetInstance();
  if (u64(address) + num_bytes >= instance.m_cloned_memory.size())
  {
    ERROR_LOG_FMT(ACHIEVEMENTS,
                  "Attempt to write past memory size: size {} address {} write length {}",
                  instance.m_cloned_memory.size(), address, num_bytes);
    return;
  }
  Core::System* system = instance.m_system.load(std::memory_order_acquire);
  if (!system)
    return;
  Core::CPUThreadGuard thread_guard(*system);
  std::lock_guard lg{instance.m_memory_lock};
  if (address < MEM1_SIZE)
    system->GetMemory().CopyToEmu(address, buffer, num_bytes);
  else
    system->GetMemory().CopyToEmu(address - MEM1_SIZE + MEM2_START, buffer, num_bytes);
  std::copy(buffer, buffer + num_bytes, instance.m_cloned_memory.begin() + address);
}
void AchievementManager::GameTitleEstimateHandler(char* buffer, u32 buffer_size,
                                                  rc_client_t* client)
{
  auto& instance = AchievementManager::GetInstance();
  std::lock_guard lg{instance.m_lock};
  strncpy(buffer, instance.m_title_estimate.c_str(), static_cast<size_t>(buffer_size));
}
#endif  // RC_CLIENT_SUPPORTS_RAINTEGRATION

#endif  // USE_RETRO_ACHIEVEMENTS
