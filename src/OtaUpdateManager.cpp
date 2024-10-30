#include "OtaUpdateManager.h"

const char* const TAG = "OtaUpdateManager";

#include "CaptivePortal.h"
#include "Common.h"
#include "config/Config.h"
#include "GatewayConnectionManager.h"
#include "Hashing.h"
#include "http/HTTPRequestManager.h"
#include "Logging.h"
#include "SemVer.h"
#include "serialization/WSGateway.h"
#include "SimpleMutex.h"
#include "Time.h"
#include "util/HexUtils.h"
#include "util/PartitionUtils.h"
#include "util/StringUtils.h"
#include "util/TaskUtils.h"
#include "wifi/WiFiManager.h"

#include <esp_ota_ops.h>
#include <esp_task_wdt.h>

#include <LittleFS.h>
#include <WiFi.h>

#include <sstream>
#include <string_view>

using namespace std::string_view_literals;

#define OPENSHOCK_FW_CDN_CHANNEL_URL(ch) OPENSHOCK_FW_CDN_URL("/version-" ch ".txt")

#define OPENSHOCK_FW_CDN_STABLE_URL  OPENSHOCK_FW_CDN_CHANNEL_URL("stable")
#define OPENSHOCK_FW_CDN_BETA_URL    OPENSHOCK_FW_CDN_CHANNEL_URL("beta")
#define OPENSHOCK_FW_CDN_DEVELOP_URL OPENSHOCK_FW_CDN_CHANNEL_URL("develop")

#define OPENSHOCK_FW_CDN_BOARDS_BASE_URL_FORMAT  OPENSHOCK_FW_CDN_URL("/%s")
#define OPENSHOCK_FW_CDN_BOARDS_INDEX_URL_FORMAT OPENSHOCK_FW_CDN_BOARDS_BASE_URL_FORMAT "/boards.txt"

#define OPENSHOCK_FW_CDN_VERSION_BASE_URL_FORMAT OPENSHOCK_FW_CDN_BOARDS_BASE_URL_FORMAT "/" OPENSHOCK_FW_BOARD

#define OPENSHOCK_FW_CDN_APP_URL_FORMAT           OPENSHOCK_FW_CDN_VERSION_BASE_URL_FORMAT "/app.bin"
#define OPENSHOCK_FW_CDN_FILESYSTEM_URL_FORMAT    OPENSHOCK_FW_CDN_VERSION_BASE_URL_FORMAT "/staticfs.bin"
#define OPENSHOCK_FW_CDN_SHA256_HASHES_URL_FORMAT OPENSHOCK_FW_CDN_VERSION_BASE_URL_FORMAT "/hashes.sha256.txt"

/// @brief Stops initArduino() from handling OTA rollbacks
/// @todo Get rid of Arduino entirely. >:(
///
/// @see .platformio/packages/framework-arduinoespressif32/cores/esp32/esp32-hal-misc.c
/// @return true
bool verifyRollbackLater()
{
  return true;
}

enum OtaTaskEventFlag : uint32_t {
  OTA_TASK_EVENT_UPDATE_REQUESTED  = 1 << 0,
  OTA_TASK_EVENT_WIFI_DISCONNECTED = 1 << 1,  // If both connected and disconnected are set, disconnected takes priority.
  OTA_TASK_EVENT_WIFI_CONNECTED    = 1 << 2,
};

static esp_ota_img_states_t _otaImageState;
static OpenShock::FirmwareBootType _bootType;
static TaskHandle_t _taskHandle;
static OpenShock::SemVer _requestedVersion;
static OpenShock::SimpleMutex _requestedVersionMutex = {};

using namespace OpenShock;

bool _tryQueueUpdateRequest(const OpenShock::SemVer& version)
{
  if (!_requestedVersionMutex.lock(pdMS_TO_TICKS(1000))) {
    OS_LOGE(TAG, "Failed to take requested version mutex");
    return false;
  }

  _requestedVersion = version;

  _requestedVersionMutex.unlock();

  xTaskNotify(_taskHandle, OTA_TASK_EVENT_UPDATE_REQUESTED, eSetBits);

  return true;
}

bool _tryGetRequestedVersion(OpenShock::SemVer& version)
{
  if (!_requestedVersionMutex.lock(pdMS_TO_TICKS(1000))) {
    OS_LOGE(TAG, "Failed to take requested version mutex");
    return false;
  }

  version = _requestedVersion;

  _requestedVersionMutex.unlock();

  return true;
}

void _otaEvGotIPHandler(arduino_event_t* event)
{
  (void)event;
  xTaskNotify(_taskHandle, OTA_TASK_EVENT_WIFI_CONNECTED, eSetBits);
}
void _otaEvWiFiDisconnectedHandler(arduino_event_t* event)
{
  (void)event;
  xTaskNotify(_taskHandle, OTA_TASK_EVENT_WIFI_DISCONNECTED, eSetBits);
}

bool _sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask task, float progress)
{
  int32_t updateId;
  if (!Config::GetOtaUpdateId(updateId)) {
    OS_LOGE(TAG, "Failed to get OTA update ID");
    return false;
  }

  if (!Serialization::Gateway::SerializeOtaInstallProgressMessage(updateId, task, progress, GatewayConnectionManager::SendMessageBIN)) {
    OS_LOGE(TAG, "Failed to send OTA install progress message");
    return false;
  }

  return true;
}
bool _sendFailureMessage(std::string_view message, bool fatal = false)
{
  int32_t updateId;
  if (!Config::GetOtaUpdateId(updateId)) {
    OS_LOGE(TAG, "Failed to get OTA update ID");
    return false;
  }

  if (!Serialization::Gateway::SerializeOtaInstallFailedMessage(updateId, message, fatal, GatewayConnectionManager::SendMessageBIN)) {
    OS_LOGE(TAG, "Failed to send OTA install failed message");
    return false;
  }

  return true;
}

bool _flashAppPartition(const esp_partition_t* partition, std::string_view remoteUrl, const uint8_t (&remoteHash)[32])
{
  OS_LOGD(TAG, "Flashing app partition");

  if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::FlashingApplication, 0.0f)) {
    return false;
  }

  auto onProgress = [](std::size_t current, std::size_t total, float progress) -> bool {
    OS_LOGD(TAG, "Flashing app partition: %u / %u (%.2f%%)", current, total, progress * 100.0f);

    _sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::FlashingApplication, progress);

    return true;
  };

  if (!OpenShock::FlashPartitionFromUrl(partition, remoteUrl, remoteHash, onProgress)) {
    OS_LOGE(TAG, "Failed to flash app partition");
    _sendFailureMessage("Failed to flash app partition"sv);
    return false;
  }

  if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::MarkingApplicationBootable, 0.0f)) {
    return false;
  }

  // Set app partition bootable.
  if (esp_ota_set_boot_partition(partition) != ESP_OK) {
    OS_LOGE(TAG, "Failed to set app partition bootable");
    _sendFailureMessage("Failed to set app partition bootable"sv);
    return false;
  }

  return true;
}

bool _flashFilesystemPartition(const esp_partition_t* parition, std::string_view remoteUrl, const uint8_t (&remoteHash)[32])
{
  if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::PreparingForInstall, 0.0f)) {
    return false;
  }

  // Make sure captive portal is stopped, timeout after 5 seconds.
  if (!CaptivePortal::ForceClose(5000U)) {
    OS_LOGE(TAG, "Failed to force close captive portal (timed out)");
    _sendFailureMessage("Failed to force close captive portal (timed out)"sv);
    return false;
  }

  OS_LOGD(TAG, "Flashing filesystem partition");

  if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::FlashingFilesystem, 0.0f)) {
    return false;
  }

  auto onProgress = [](std::size_t current, std::size_t total, float progress) -> bool {
    OS_LOGD(TAG, "Flashing filesystem partition: %u / %u (%.2f%%)", current, total, progress * 100.0f);

    _sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::FlashingFilesystem, progress);

    return true;
  };

  if (!OpenShock::FlashPartitionFromUrl(parition, remoteUrl, remoteHash, onProgress)) {
    OS_LOGE(TAG, "Failed to flash filesystem partition");
    _sendFailureMessage("Failed to flash filesystem partition"sv);
    return false;
  }

  if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::VerifyingFilesystem, 0.0f)) {
    return false;
  }

  // Attempt to mount filesystem.
  fs::LittleFSFS test;
  if (!test.begin(false, "/static", 10, "static0")) {
    OS_LOGE(TAG, "Failed to mount filesystem");
    _sendFailureMessage("Failed to mount filesystem"sv);
    return false;
  }
  test.end();

  OpenShock::CaptivePortal::ForceClose(false);

  return true;
}

void _otaUpdateTask(void* arg)
{
  (void)arg;

  OS_LOGD(TAG, "OTA update task started");

  bool connected          = false;
  bool updateRequested    = false;
  int64_t lastUpdateCheck = 0;

  // Update task loop.
  while (true) {
    // Wait for event.
    uint32_t eventBits = 0;
    xTaskNotifyWait(0, UINT32_MAX, &eventBits, pdMS_TO_TICKS(5000));  // TODO: wait for rest time

    updateRequested |= (eventBits & OTA_TASK_EVENT_UPDATE_REQUESTED) != 0;

    if ((eventBits & OTA_TASK_EVENT_WIFI_DISCONNECTED) != 0) {
      OS_LOGD(TAG, "WiFi disconnected");
      connected = false;
      continue;  // No further processing needed.
    }

    if ((eventBits & OTA_TASK_EVENT_WIFI_CONNECTED) != 0 && !connected) {
      OS_LOGD(TAG, "WiFi connected");
      connected = true;
    }

    // If we're not connected, continue.
    if (!connected) {
      continue;
    }

    int64_t now = OpenShock::millis();

    Config::OtaUpdateConfig config;
    if (!Config::GetOtaUpdateConfig(config)) {
      OS_LOGE(TAG, "Failed to get OTA update config");
      continue;
    }

    if (!config.isEnabled) {
      OS_LOGD(TAG, "OTA updates are disabled, skipping update check");
      continue;
    }

    bool firstCheck  = lastUpdateCheck == 0;
    int64_t diff     = now - lastUpdateCheck;
    int64_t diffMins = diff / 60'000LL;

    bool check = false;
    check |= config.checkOnStartup && firstCheck;                           // On startup
    check |= config.checkPeriodically && diffMins >= config.checkInterval;  // Periodically
    check |= updateRequested && (firstCheck || diffMins >= 1);              // Update requested

    if (!check) {
      continue;
    }

    lastUpdateCheck = now;

    if (config.requireManualApproval) {
      OS_LOGD(TAG, "Manual approval required, skipping update check");
      // TODO: IMPLEMENT
      continue;
    }

    OpenShock::SemVer version;
    if (updateRequested) {
      updateRequested = false;

      if (!_tryGetRequestedVersion(version)) {
        OS_LOGE(TAG, "Failed to get requested version");
        continue;
      }

      OS_LOGD(TAG, "Update requested for version %s", version.toString().c_str());  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this
    } else {
      OS_LOGD(TAG, "Checking for updates");

      // Fetch current version.
      if (!OtaUpdateManager::TryGetFirmwareVersion(config.updateChannel, version)) {
        OS_LOGE(TAG, "Failed to fetch firmware version");
        continue;
      }

      OS_LOGD(TAG, "Remote version: %s", version.toString().c_str());  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this
    }

    if (version.toString() == OPENSHOCK_FW_VERSION) {  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this
      OS_LOGI(TAG, "Requested version is already installed");
      continue;
    }

    // Generate random int32_t for this update.
    int32_t updateId = static_cast<int32_t>(esp_random());
    if (!Config::SetOtaUpdateId(updateId)) {
      OS_LOGE(TAG, "Failed to set OTA update ID");
      continue;
    }
    if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Updating)) {
      OS_LOGE(TAG, "Failed to set OTA update step");
      continue;
    }

    if (!Serialization::Gateway::SerializeOtaInstallStartedMessage(updateId, version, GatewayConnectionManager::SendMessageBIN)) {
      OS_LOGE(TAG, "Failed to serialize OTA install started message");
      continue;
    }

    if (!_sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::FetchingMetadata, 0.0f)) {
      continue;
    }

    // Fetch current release.
    OtaUpdateManager::FirmwareRelease release;
    if (!OtaUpdateManager::TryGetFirmwareRelease(version, release)) {
      OS_LOGE(TAG, "Failed to fetch firmware release");  // TODO: Send error message to server
      _sendFailureMessage("Failed to fetch firmware release"sv);
      continue;
    }

    // Print release.
    OS_LOGD(TAG, "Firmware release:");
    OS_LOGD(TAG, "  Version:                %s", version.toString().c_str());  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this
    OS_LOGD(TAG, "  App binary URL:         %s", release.appBinaryUrl.c_str());
    OS_LOGD(TAG, "  App binary hash:        %s", HexUtils::ToHex<32>(release.appBinaryHash).data());
    OS_LOGD(TAG, "  Filesystem binary URL:  %s", release.filesystemBinaryUrl.c_str());
    OS_LOGD(TAG, "  Filesystem binary hash: %s", HexUtils::ToHex<32>(release.filesystemBinaryHash).data());

    // Get available app update partition.
    const esp_partition_t* appPartition = esp_ota_get_next_update_partition(nullptr);
    if (appPartition == nullptr) {
      OS_LOGE(TAG, "Failed to get app update partition");  // TODO: Send error message to server
      _sendFailureMessage("Failed to get app update partition"sv);
      continue;
    }

    // Get filesystem partition.
    const esp_partition_t* filesystemPartition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "static0");
    if (filesystemPartition == nullptr) {
      OS_LOGE(TAG, "Failed to find filesystem partition");  // TODO: Send error message to server
      _sendFailureMessage("Failed to find filesystem partition"sv);
      continue;
    }

    // Increase task watchdog timeout.
    // Prevents panics on some ESP32s when clearing large partitions.
    esp_task_wdt_init(15, true);

    // Flash app and filesystem partitions.
    if (!_flashFilesystemPartition(filesystemPartition, release.filesystemBinaryUrl, release.filesystemBinaryHash)) continue;
    if (!_flashAppPartition(appPartition, release.appBinaryUrl, release.appBinaryHash)) continue;

    // Set OTA boot type in config.
    if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Updated)) {
      OS_LOGE(TAG, "Failed to set OTA update step");
      _sendFailureMessage("Failed to set OTA update step"sv);
      continue;
    }

    // Set task watchdog timeout back to default.
    esp_task_wdt_init(5, true);

    // Send reboot message.
    _sendProgressMessage(Serialization::Gateway::OtaInstallProgressTask::Rebooting, 0.0f);

    // Reboot into new firmware.
    OS_LOGI(TAG, "Restarting into new firmware...");
    vTaskDelay(pdMS_TO_TICKS(200));
    break;
  }

  // Restart.
  esp_restart();
}

bool _tryGetStringList(std::string_view url, std::vector<std::string>& list)
{
  auto response = OpenShock::HTTP::GetString(
    url,
    {
      {"Accept", "text/plain"}
  },
    {200, 304}
  );
  if (response.result != OpenShock::HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Failed to fetch list: [%u] %s", response.code, response.data.c_str());
    return false;
  }

  list.clear();

  std::string_view data = response.data;

  auto lines = OpenShock::StringSplitNewLines(data);
  list.reserve(lines.size());

  for (auto line : lines) {
    line = OpenShock::StringTrim(line);

    if (line.empty()) {
      continue;
    }

    list.push_back(std::string(line));
  }

  return true;
}

bool OtaUpdateManager::Init()
{
  OS_LOGN(TAG, "Fetching current partition");

  // Fetch current partition info.
  const esp_partition_t* partition = esp_ota_get_running_partition();
  if (partition == nullptr) {
    OS_PANIC(TAG, "Failed to get currently running partition");
    return false;  // This will never be reached, but the compiler doesn't know that.
  }

  OS_LOGD(TAG, "Fetching partition state");

  // Get OTA state for said partition.
  esp_err_t err = esp_ota_get_state_partition(partition, &_otaImageState);
  if (err != ESP_OK) {
    OS_PANIC(TAG, "Failed to get partition state: %s", esp_err_to_name(err));
    return false;  // This will never be reached, but the compiler doesn't know that.
  }

  OS_LOGD(TAG, "Fetching previous update step");
  OtaUpdateStep updateStep;
  if (!Config::GetOtaUpdateStep(updateStep)) {
    OS_LOGE(TAG, "Failed to get OTA update step");
    return false;
  }

  // Infer boot type from update step.
  switch (updateStep) {
    case OtaUpdateStep::Updated:
      _bootType = FirmwareBootType::NewFirmware;
      break;
    case OtaUpdateStep::Validating:  // If the update step is validating, we have failed in the middle of validating the new firmware, meaning this is a rollback.
    case OtaUpdateStep::RollingBack:
      _bootType = FirmwareBootType::Rollback;
      break;
    default:
      _bootType = FirmwareBootType::Normal;
      break;
  }

  if (updateStep == OtaUpdateStep::Updated) {
    if (!Config::SetOtaUpdateStep(OtaUpdateStep::Validating)) {
      OS_PANIC(TAG, "Failed to set OTA update step in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
    }
  }

  WiFi.onEvent(_otaEvGotIPHandler, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  WiFi.onEvent(_otaEvGotIPHandler, ARDUINO_EVENT_WIFI_STA_GOT_IP6);
  WiFi.onEvent(_otaEvWiFiDisconnectedHandler, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Start OTA update task.
  TaskUtils::TaskCreateExpensive(_otaUpdateTask, "OTA Update", 8192, nullptr, 1, &_taskHandle);  // PROFILED: 6.2KB stack usage

  return true;
}

bool OtaUpdateManager::TryGetFirmwareVersion(OtaUpdateChannel channel, OpenShock::SemVer& version)
{
  std::string_view channelIndexUrl;
  switch (channel) {
    case OtaUpdateChannel::Stable:
      channelIndexUrl = std::string_view(OPENSHOCK_FW_CDN_STABLE_URL);
      break;
    case OtaUpdateChannel::Beta:
      channelIndexUrl = std::string_view(OPENSHOCK_FW_CDN_BETA_URL);
      break;
    case OtaUpdateChannel::Develop:
      channelIndexUrl = std::string_view(OPENSHOCK_FW_CDN_DEVELOP_URL);
      break;
    default:
      OS_LOGE(TAG, "Unknown channel: %u", channel);
      return false;
  }

  OS_LOGD(TAG, "Fetching firmware version from %s", channelIndexUrl);

  auto response = OpenShock::HTTP::GetString(
    channelIndexUrl,
    {
      {"Accept", "text/plain"}
  },
    {200, 304}
  );
  if (response.result != OpenShock::HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Failed to fetch firmware version: [%u] %s", response.code, response.data.c_str());
    return false;
  }

  if (!OpenShock::TryParseSemVer(response.data, version)) {
    OS_LOGE(TAG, "Failed to parse firmware version: %.*s", response.data.size(), response.data.data());
    return false;
  }

  return true;
}

bool OtaUpdateManager::TryGetFirmwareBoards(const OpenShock::SemVer& version, std::vector<std::string>& boards)
{
  std::string channelIndexUrl;
  if (!FormatToString(channelIndexUrl, OPENSHOCK_FW_CDN_BOARDS_INDEX_URL_FORMAT, version.toString().c_str())) {  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this
    OS_LOGE(TAG, "Failed to format URL");
    return false;
  }

  OS_LOGD(TAG, "Fetching firmware boards from %s", channelIndexUrl.c_str());

  if (!_tryGetStringList(channelIndexUrl, boards)) {
    OS_LOGE(TAG, "Failed to fetch firmware boards");
    return false;
  }

  return true;
}

bool _tryParseIntoHash(std::string_view hash, uint8_t (&hashBytes)[32])
{
  if (!HexUtils::TryParseHex(hash.data(), hash.size(), hashBytes, 32)) {
    OS_LOGE(TAG, "Failed to parse hash: %.*s", hash.size(), hash.data());
    return false;
  }

  return true;
}

bool OtaUpdateManager::TryGetFirmwareRelease(const OpenShock::SemVer& version, FirmwareRelease& release)
{
  auto versionStr = version.toString();  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this

  if (!FormatToString(release.appBinaryUrl, OPENSHOCK_FW_CDN_APP_URL_FORMAT, versionStr.c_str())) {
    OS_LOGE(TAG, "Failed to format URL");
    return false;
  }

  if (!FormatToString(release.filesystemBinaryUrl, OPENSHOCK_FW_CDN_FILESYSTEM_URL_FORMAT, versionStr.c_str())) {
    OS_LOGE(TAG, "Failed to format URL");
    return false;
  }

  // Construct hash URLs.
  std::string sha256HashesUrl;
  if (!FormatToString(sha256HashesUrl, OPENSHOCK_FW_CDN_SHA256_HASHES_URL_FORMAT, versionStr.c_str())) {
    OS_LOGE(TAG, "Failed to format URL");
    return false;
  }

  // Fetch hashes.
  auto sha256HashesResponse = OpenShock::HTTP::GetString(
    sha256HashesUrl,
    {
      {"Accept", "text/plain"}
  },
    {200, 304}
  );
  if (sha256HashesResponse.result != OpenShock::HTTP::RequestResult::Success) {
    OS_LOGE(TAG, "Failed to fetch hashes: [%u] %s", sha256HashesResponse.code, sha256HashesResponse.data.c_str());
    return false;
  }

  auto hashesLines = OpenShock::StringSplitNewLines(sha256HashesResponse.data);

  // Parse hashes.
  bool foundAppHash = false, foundFilesystemHash = false;
  for (std::string_view line : hashesLines) {
    auto parts = OpenShock::StringSplitWhiteSpace(line);
    if (parts.size() != 2) {
      OS_LOGE(TAG, "Invalid hashes entry: %.*s", line.size(), line.data());
      return false;
    }

    auto hash = OpenShock::StringTrim(parts[0]);
    auto file = OpenShock::StringTrim(parts[1]);

    if (OpenShock::StringStartsWith(file, "./"sv)) {
      file = file.substr(2);
    }

    if (hash.size() != 64) {
      OS_LOGE(TAG, "Invalid hash: %.*s", hash.size(), hash.data());
      return false;
    }

    if (file == "app.bin") {
      if (foundAppHash) {
        OS_LOGE(TAG, "Duplicate hash for app.bin");
        return false;
      }

      if (!_tryParseIntoHash(hash, release.appBinaryHash)) {
        return false;
      }

      foundAppHash = true;
    } else if (file == "staticfs.bin") {
      if (foundFilesystemHash) {
        OS_LOGE(TAG, "Duplicate hash for staticfs.bin");
        return false;
      }

      if (!_tryParseIntoHash(hash, release.filesystemBinaryHash)) {
        return false;
      }

      foundFilesystemHash = true;
    }
  }

  return true;
}

bool OtaUpdateManager::TryStartFirmwareInstallation(const OpenShock::SemVer& version)
{
  OS_LOGD(TAG, "Requesting firmware version %s", version.toString().c_str());  // TODO: This is abusing the SemVer::toString() method causing alot of string copies, fix this

  return _tryQueueUpdateRequest(version);
}

FirmwareBootType OtaUpdateManager::GetFirmwareBootType()
{
  return _bootType;
}

bool OtaUpdateManager::IsValidatingApp()
{
  return _otaImageState == ESP_OTA_IMG_PENDING_VERIFY;
}

void OtaUpdateManager::InvalidateAndRollback()
{
  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::RollingBack)) {
    OS_PANIC(TAG, "Failed to set OTA firmware boot type in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
    return;
  }

  switch (esp_ota_mark_app_invalid_rollback_and_reboot()) {
    case ESP_FAIL:
      OS_LOGE(TAG, "Rollback failed (ESP_FAIL)");
      break;
    case ESP_ERR_OTA_ROLLBACK_FAILED:
      OS_LOGE(TAG, "Rollback failed (ESP_ERR_OTA_ROLLBACK_FAILED)");
      break;
    default:
      OS_LOGE(TAG, "Rollback failed (Unknown)");
      break;
  }

  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::None)) {
    OS_LOGE(TAG, "Failed to set OTA firmware boot type");
  }

  esp_restart();
}

void OtaUpdateManager::ValidateApp()
{
  if (esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
    OS_PANIC(TAG, "Unable to mark app as valid, WTF?");  // TODO: Wtf do we do here?
  }

  // Set OTA boot type in config.
  if (!Config::SetOtaUpdateStep(OpenShock::OtaUpdateStep::Validated)) {
    OS_PANIC(TAG, "Failed to set OTA firmware boot type in critical section");  // TODO: THIS IS A CRITICAL SECTION, WHAT DO WE DO?
  }

  _otaImageState = ESP_OTA_IMG_VALID;
}
