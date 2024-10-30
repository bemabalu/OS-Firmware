#include <freertos/FreeRTOS.h>

#include "config/Config.h"

const char* const TAG = "Config";

#include "Chipset.h"
#include "Common.h"
#include "config/RootConfig.h"
#include "Logging.h"
#include "ReadWriteMutex.h"

#include <FS.h>
#include <LittleFS.h>

#include <cJSON.h>

#include <bitset>

using namespace OpenShock;

static fs::LittleFSFS _configFS;
static Config::RootConfig _configData;
static ReadWriteMutex _configMutex;

#define CONFIG_LOCK_READ_ACTION(retval, action)  \
  ScopedReadLock lock__(&_configMutex);          \
  if (!lock__.isLocked()) {                      \
    OS_LOGE(TAG, "Failed to acquire read lock"); \
    action;                                      \
    return retval;                               \
  }

#define CONFIG_LOCK_WRITE_ACTION(retval, action)  \
  ScopedWriteLock lock__(&_configMutex);          \
  if (!lock__.isLocked()) {                       \
    OS_LOGE(TAG, "Failed to acquire write lock"); \
    action;                                       \
    return retval;                                \
  }

#define CONFIG_LOCK_READ(retval)  CONFIG_LOCK_READ_ACTION(retval, {})
#define CONFIG_LOCK_WRITE(retval) CONFIG_LOCK_WRITE_ACTION(retval, {})

bool _tryDeserializeConfig(const uint8_t* buffer, std::size_t bufferLen, OpenShock::Config::RootConfig& config)
{
  if (buffer == nullptr || bufferLen == 0) {
    OS_LOGE(TAG, "Buffer is null or empty");
    return false;
  }

  // Deserialize
  auto fbsConfig = flatbuffers::GetRoot<Serialization::Configuration::HubConfig>(buffer);
  if (fbsConfig == nullptr) {
    OS_LOGE(TAG, "Failed to get deserialization root for config file");
    return false;
  }

  // Validate buffer
  flatbuffers::Verifier::Options verifierOptions {
    .max_size = 4096,  // Should be enough
  };
  flatbuffers::Verifier verifier(buffer, bufferLen, verifierOptions);
  if (!fbsConfig->Verify(verifier)) {
    OS_LOGE(TAG, "Failed to verify config file integrity");
    return false;
  }

  // Read config
  if (!config.FromFlatbuffers(fbsConfig)) {
    OS_LOGE(TAG, "Failed to read config file");
    return false;
  }

  return true;
}
bool _tryLoadConfig(std::vector<uint8_t>& buffer)
{
  File file = _configFS.open("/config", "rb");
  if (!file) {
    OS_LOGE(TAG, "Failed to open config file for reading");
    return false;
  }

  // Get file size
  std::size_t size = file.size();

  // Resize buffer
  buffer.resize(size);

  // Read file
  if (file.read(buffer.data(), buffer.size()) != buffer.size()) {
    OS_LOGE(TAG, "Failed to read config file, size mismatch");
    return false;
  }

  file.close();

  return true;
}
bool _tryLoadConfig()
{
  std::vector<uint8_t> buffer;
  if (!_tryLoadConfig(buffer)) {
    return false;
  }

  return _tryDeserializeConfig(buffer.data(), buffer.size(), _configData);
}
bool _trySaveConfig(const uint8_t* data, std::size_t dataLen)
{
  File file = _configFS.open("/config", "wb");
  if (!file) {
    OS_LOGE(TAG, "Failed to open config file for writing");
    return false;
  }

  // Write file
  if (file.write(data, dataLen) != dataLen) {
    OS_LOGE(TAG, "Failed to write config file");
    return false;
  }

  file.close();

  return true;
}
bool _trySaveConfig()
{
  flatbuffers::FlatBufferBuilder builder;

  auto fbsConfig = _configData.ToFlatbuffers(builder, true);

  Serialization::Configuration::FinishHubConfigBuffer(builder, fbsConfig);

  return _trySaveConfig(builder.GetBufferPointer(), builder.GetSize());
}

void Config::Init()
{
  CONFIG_LOCK_WRITE();

  if (!_configFS.begin(true, "/config", 3, "config")) {
    OS_PANIC(TAG, "Unable to mount config LittleFS partition!");
  }

  if (_tryLoadConfig()) {
    return;
  }

  OS_LOGW(TAG, "Failed to load config, writing default config");

  _configData.ToDefault();

  if (!_trySaveConfig()) {
    OS_PANIC(TAG, "Failed to save default config. Recommend formatting microcontroller and re-flashing firmware");
  }
}

cJSON* _getAsCJSON(bool withSensitiveData)
{
  CONFIG_LOCK_READ(nullptr);

  return _configData.ToJSON(withSensitiveData);
}

std::string Config::GetAsJSON(bool withSensitiveData)
{
  cJSON* root = _getAsCJSON(withSensitiveData);

  char* json = cJSON_PrintUnformatted(root);

  std::string result(json);

  free(json);

  cJSON_Delete(root);

  return result;
}
bool Config::SaveFromJSON(std::string_view json)
{
  cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
  if (root == nullptr) {
    OS_LOGE(TAG, "Failed to parse JSON: %s", cJSON_GetErrorPtr());
    return false;
  }

  CONFIG_LOCK_WRITE_ACTION(false, cJSON_Delete(root));

  bool result = _configData.FromJSON(root);

  cJSON_Delete(root);

  if (!result) {
    OS_LOGE(TAG, "Failed to read JSON");
    return false;
  }

  return _trySaveConfig();
}

flatbuffers::Offset<Serialization::Configuration::HubConfig> Config::GetAsFlatBuffer(flatbuffers::FlatBufferBuilder& builder, bool withSensitiveData)
{
  CONFIG_LOCK_READ(0);

  return _configData.ToFlatbuffers(builder, withSensitiveData);
}

bool Config::SaveFromFlatBuffer(const Serialization::Configuration::HubConfig* config)
{
  CONFIG_LOCK_WRITE(false);

  if (!_configData.FromFlatbuffers(config)) {
    OS_LOGE(TAG, "Failed to read config file");
    return false;
  }

  return _trySaveConfig();
}

bool Config::GetRaw(std::vector<uint8_t>& buffer)
{
  CONFIG_LOCK_READ(false);

  return _tryLoadConfig(buffer);
}

bool Config::SetRaw(const uint8_t* buffer, std::size_t size)
{
  CONFIG_LOCK_WRITE(false);

  OpenShock::Config::RootConfig config;
  if (!_tryDeserializeConfig(buffer, size, config)) {
    OS_LOGE(TAG, "Failed to deserialize config");
    return false;
  }

  return _trySaveConfig(buffer, size);
}

void Config::FactoryReset()
{
  CONFIG_LOCK_WRITE();

  _configData.ToDefault();

  if (!_configFS.remove("/config") && _configFS.exists("/config")) {
    OS_PANIC(TAG, "Failed to remove existing config file for factory reset. Reccomend formatting microcontroller and re-flashing firmware");
  }

  if (!_trySaveConfig()) {
    OS_PANIC(TAG, "Failed to save default config. Recommend formatting microcontroller and re-flashing firmware");
  }

  OS_LOGI(TAG, "Factory reset complete");
}

bool Config::GetRFConfig(Config::RFConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf;

  return true;
}

bool Config::GetWiFiConfig(Config::WiFiConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi;

  return true;
}

bool Config::GetCaptivePortalConfig(Config::CaptivePortalConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.captivePortal;

  return true;
}

bool Config::GetBackendConfig(Config::BackendConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend;

  return true;
}

bool Config::GetSerialInputConfig(Config::SerialInputConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.serialInput;

  return true;
}

bool Config::GetOtaUpdateConfig(Config::OtaUpdateConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate;

  return true;
}

bool Config::GetEStop(Config::EStopConfig& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop;

  return true;
}

bool Config::SetRFConfig(const Config::RFConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf = config;
  return _trySaveConfig();
}

bool Config::SetWiFiConfig(const Config::WiFiConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi = config;
  return _trySaveConfig();
}

bool Config::SetCaptivePortalConfig(const Config::CaptivePortalConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.captivePortal = config;
  return _trySaveConfig();
}

bool Config::SetBackendConfig(const Config::BackendConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend = config;
  return _trySaveConfig();
}

bool Config::SetSerialInputConfig(const Config::SerialInputConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.serialInput = config;
  return _trySaveConfig();
}

bool Config::SetOtaUpdateConfig(const Config::OtaUpdateConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.otaUpdate = config;
  return _trySaveConfig();
}

bool Config::SetEStop(const Config::EStopConfig& config)
{
  CONFIG_LOCK_WRITE(false);

  _configData.estop = config;
  return _trySaveConfig();
}

bool Config::GetWiFiCredentials(std::vector<Config::WiFiCredentials>& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi.credentialsList;

  return true;
}

bool Config::GetWiFiCredentials(cJSON* array, bool withSensitiveData)
{
  CONFIG_LOCK_READ(false);

  for (auto& creds : _configData.wifi.credentialsList) {
    cJSON* jsonCreds = creds.ToJSON(withSensitiveData);

    cJSON_AddItemToArray(array, jsonCreds);
  }

  return true;
}

bool Config::SetWiFiCredentials(const std::vector<Config::WiFiCredentials>& credentials)
{
  bool foundZeroId = std::any_of(credentials.begin(), credentials.end(), [](const Config::WiFiCredentials& creds) { return creds.id == 0; });
  if (foundZeroId) {
    OS_LOGE(TAG, "Cannot set WiFi credentials: credential ID cannot be 0");
    return false;
  }

  CONFIG_LOCK_WRITE(false);

  _configData.wifi.credentialsList = credentials;
  return _trySaveConfig();
}

bool Config::GetRFConfigTxPin(gpio_num_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf.txPin;

  return true;
}

bool Config::SetRFConfigTxPin(gpio_num_t txPin)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf.txPin = txPin;
  return _trySaveConfig();
}

bool Config::GetRFConfigKeepAliveEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.rf.keepAliveEnabled;

  return true;
}

bool Config::SetRFConfigKeepAliveEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.rf.keepAliveEnabled = enabled;
  return _trySaveConfig();
}

bool Config::AnyWiFiCredentials(std::function<bool(const Config::WiFiCredentials&)> predicate)
{
  CONFIG_LOCK_READ(false);

  auto& creds = _configData.wifi.credentialsList;

  return std::any_of(creds.begin(), creds.end(), predicate);
}

uint8_t Config::AddWiFiCredentials(std::string_view ssid, std::string_view password)
{
  CONFIG_LOCK_WRITE(0);

  uint8_t id = 0;

  std::bitset<255> bits;
  for (auto it = _configData.wifi.credentialsList.begin(); it != _configData.wifi.credentialsList.end(); ++it) {
    auto& creds = *it;

    if (std::string_view(creds.ssid) == ssid) {
      creds.password = password;

      id = creds.id;

      break;
    }

    if (creds.id == 0) {
      OS_LOGW(TAG, "Found WiFi credentials with ID 0, removing");
      it = _configData.wifi.credentialsList.erase(it);
      continue;
    }

    // Mark ID as used
    bits[creds.id - 1] = true;
  }

  // Get first available ID
  for (std::size_t i = 0; i < bits.size(); ++i) {
    if (!bits[i]) {
      id = i + 1;
      break;
    }
  }

  if (id == 0) {
    OS_LOGE(TAG, "Failed to add WiFi credentials: no available IDs");
    return 0;
  }

  _configData.wifi.credentialsList.push_back({
    .id       = id,
    .ssid     = std::string(ssid),
    .password = std::string(password),
  });
  _trySaveConfig();

  return id;
}

bool Config::TryGetWiFiCredentialsByID(uint8_t id, Config::WiFiCredentials& credentials)
{
  CONFIG_LOCK_READ(false);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.id == id) {
      credentials = creds;
      return true;
    }
  }

  return false;
}

bool Config::TryGetWiFiCredentialsBySSID(const char* ssid, Config::WiFiCredentials& credentials)
{
  CONFIG_LOCK_READ(false);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.ssid == ssid) {
      credentials = creds;
      return true;
    }
  }

  return false;
}

uint8_t Config::GetWiFiCredentialsIDbySSID(const char* ssid)
{
  CONFIG_LOCK_READ(0);

  for (const auto& creds : _configData.wifi.credentialsList) {
    if (creds.ssid == ssid) {
      return creds.id;
    }
  }

  return 0;
}

bool Config::RemoveWiFiCredentials(uint8_t id)
{
  CONFIG_LOCK_WRITE(false);

  for (auto it = _configData.wifi.credentialsList.begin(); it != _configData.wifi.credentialsList.end(); ++it) {
    if (it->id == id) {
      _configData.wifi.credentialsList.erase(it);
      _trySaveConfig();
      return true;
    }
  }

  return false;
}

bool Config::ClearWiFiCredentials()
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi.credentialsList.clear();

  return _trySaveConfig();
}

bool Config::GetWiFiHostname(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.wifi.hostname;

  return true;
}

bool Config::SetWiFiHostname(std::string_view hostname)
{
  CONFIG_LOCK_WRITE(false);

  _configData.wifi.hostname = std::string(hostname);

  return _trySaveConfig();
}

bool Config::GetBackendDomain(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend.domain;

  return true;
}

bool Config::SetBackendDomain(std::string_view domain)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.domain = std::string(domain);
  return _trySaveConfig();
}

bool Config::HasBackendAuthToken()
{
  CONFIG_LOCK_READ(false);

  return !_configData.backend.authToken.empty();
}

bool Config::GetBackendAuthToken(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend.authToken;

  return true;
}

bool Config::SetBackendAuthToken(std::string_view token)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.authToken = std::string(token);
  return _trySaveConfig();
}

bool Config::ClearBackendAuthToken()
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.authToken.clear();
  return _trySaveConfig();
}

bool Config::HasBackendLCGOverride()
{
  CONFIG_LOCK_READ(false);

  return !_configData.backend.lcgOverride.empty();
}

bool Config::GetBackendLCGOverride(std::string& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.backend.lcgOverride;

  return true;
}

bool Config::SetBackendLCGOverride(std::string_view lcgOverride)
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.lcgOverride = std::string(lcgOverride);
  return _trySaveConfig();
}

bool Config::ClearBackendLCGOverride()
{
  CONFIG_LOCK_WRITE(false);

  _configData.backend.lcgOverride.clear();
  return _trySaveConfig();
}

bool Config::GetSerialInputConfigEchoEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.serialInput.echoEnabled;
  return true;
}

bool Config::SetSerialInputConfigEchoEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.serialInput.echoEnabled = enabled;
  return _trySaveConfig();
}

bool Config::GetOtaUpdateId(int32_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate.updateId;

  return true;
}

bool Config::SetOtaUpdateId(int32_t updateId)
{
  CONFIG_LOCK_WRITE(false);

  if (_configData.otaUpdate.updateId == updateId) {
    return true;
  }

  _configData.otaUpdate.updateId = updateId;
  return _trySaveConfig();
}

bool Config::GetOtaUpdateStep(OtaUpdateStep& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.otaUpdate.updateStep;

  return true;
}

bool Config::SetOtaUpdateStep(OtaUpdateStep updateStep)
{
  CONFIG_LOCK_WRITE(false);

  if (_configData.otaUpdate.updateStep == updateStep) {
    return true;
  }

  _configData.otaUpdate.updateStep = updateStep;
  return _trySaveConfig();
}

bool Config::GetEStopEnabled(bool& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop.enabled;

  return true;
}

bool Config::SetEStopEnabled(bool enabled)
{
  CONFIG_LOCK_WRITE(false);

  _configData.estop.enabled = enabled;
  return _trySaveConfig();
}

bool Config::GetEStopGpioPin(gpio_num_t& out)
{
  CONFIG_LOCK_READ(false);

  out = _configData.estop.gpioPin;

  return true;
}

bool Config::SetEStopGpioPin(gpio_num_t gpioPin)
{
  CONFIG_LOCK_WRITE(false);

  if (!OpenShock::IsValidInputPin(gpioPin)) {
    OS_LOGE(TAG, "Invalid EStop GPIO Pin: %d", gpioPin);
    return false;
  }

  _configData.estop.gpioPin = gpioPin;
  return _trySaveConfig();
}
