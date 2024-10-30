#include "config/WiFiConfig.h"

const char* const TAG = "Config::WiFiConfig";

#include "config/internal/utils.h"
#include "Logging.h"

using namespace OpenShock::Config;

WiFiConfig::WiFiConfig()
  : accessPointSSID(OPENSHOCK_FW_AP_PREFIX)
  , hostname(OPENSHOCK_FW_HOSTNAME)
  , credentialsList()
{
}

WiFiConfig::WiFiConfig(std::string_view accessPointSSID, std::string_view hostname, const std::vector<WiFiCredentials>& credentialsList)
  : accessPointSSID(accessPointSSID)
  , hostname(hostname)
  , credentialsList(credentialsList)
{
}

void WiFiConfig::ToDefault() {
  accessPointSSID = OPENSHOCK_FW_AP_PREFIX;
  hostname        = OPENSHOCK_FW_HOSTNAME;
  credentialsList.clear();
}

bool WiFiConfig::FromFlatbuffers(const Serialization::Configuration::WiFiConfig* config) {
  if (config == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  Internal::Utils::FromFbsStr(accessPointSSID, config->ap_ssid(), OPENSHOCK_FW_AP_PREFIX);
  Internal::Utils::FromFbsStr(hostname, config->hostname(), OPENSHOCK_FW_HOSTNAME);
  Internal::Utils::FromFbsVec(credentialsList, config->credentials());

  return true;
}

flatbuffers::Offset<OpenShock::Serialization::Configuration::WiFiConfig> WiFiConfig::ToFlatbuffers(flatbuffers::FlatBufferBuilder& builder, bool withSensitiveData) const {
  std::vector<flatbuffers::Offset<OpenShock::Serialization::Configuration::WiFiCredentials>> fbsCredentialsList;
  fbsCredentialsList.reserve(credentialsList.size());

  for (auto& credentials : credentialsList) {
    fbsCredentialsList.emplace_back(credentials.ToFlatbuffers(builder, withSensitiveData));
  }

  return Serialization::Configuration::CreateWiFiConfig(builder, builder.CreateString(accessPointSSID), builder.CreateString(hostname), builder.CreateVector(fbsCredentialsList));
}

bool WiFiConfig::FromJSON(const cJSON* json) {
  if (json == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  if (cJSON_IsObject(json) == 0) {
    OS_LOGE(TAG, "json is not an object");
    return false;
  }

  Internal::Utils::FromJsonStr(accessPointSSID, json, "accessPointSSID", OPENSHOCK_FW_AP_PREFIX);
  Internal::Utils::FromJsonStr(hostname, json, "hostname", OPENSHOCK_FW_HOSTNAME);

  const cJSON* credentialsListJson = cJSON_GetObjectItemCaseSensitive(json, "credentials");
  if (credentialsListJson == nullptr) {
    OS_LOGE(TAG, "credentials is null");
    return false;
  }

  if (cJSON_IsArray(credentialsListJson) == 0) {
    OS_LOGE(TAG, "credentials is not an array");
    return false;
  }

  Internal::Utils::FromJsonArray(credentialsList, credentialsListJson);

  return true;
}

cJSON* WiFiConfig::ToJSON(bool withSensitiveData) const {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddStringToObject(root, "accessPointSSID", accessPointSSID.c_str());
  cJSON_AddStringToObject(root, "hostname", hostname.c_str());

  cJSON* credentialsListJson = cJSON_CreateArray();

  for (auto& credentials : credentialsList) {
    cJSON_AddItemToArray(credentialsListJson, credentials.ToJSON(withSensitiveData));
  }

  cJSON_AddItemToObject(root, "credentials", credentialsListJson);

  return root;
}
