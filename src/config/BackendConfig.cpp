#include "config/BackendConfig.h"

const char* const TAG = "Config::BackendConfig";

#include "config/internal/utils.h"
#include "Logging.h"

using namespace OpenShock::Config;

BackendConfig::BackendConfig()
  : domain(OPENSHOCK_API_DOMAIN)
  , authToken()
  , lcgOverride()
{
}

BackendConfig::BackendConfig(std::string_view domain, std::string_view authToken, std::string_view lcgOverride)
  : domain(domain)
  , authToken(authToken)
  , lcgOverride(lcgOverride)
{
}

void BackendConfig::ToDefault() {
  domain = OPENSHOCK_API_DOMAIN;
  authToken.clear();
  lcgOverride.clear();
}

bool BackendConfig::FromFlatbuffers(const Serialization::Configuration::BackendConfig* config) {
  if (config == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  Internal::Utils::FromFbsStr(domain, config->domain(), OPENSHOCK_API_DOMAIN);
  Internal::Utils::FromFbsStr(authToken, config->auth_token(), "");
  Internal::Utils::FromFbsStr(lcgOverride, config->lcg_override(), "");

  return true;
}

flatbuffers::Offset<OpenShock::Serialization::Configuration::BackendConfig> BackendConfig::ToFlatbuffers(flatbuffers::FlatBufferBuilder& builder, bool withSensitiveData) const {
  auto domainOffset = builder.CreateString(domain);

  flatbuffers::Offset<flatbuffers::String> authTokenOffset;
  if (withSensitiveData) {
    authTokenOffset = builder.CreateString(authToken);
  } else {
    authTokenOffset = 0;
  }

  auto lcgOverrideOffset = builder.CreateString(lcgOverride);

  return Serialization::Configuration::CreateBackendConfig(builder, domainOffset, authTokenOffset, lcgOverrideOffset);
}

bool BackendConfig::FromJSON(const cJSON* json) {
  if (json == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  if (cJSON_IsObject(json) == 0) {
    OS_LOGE(TAG, "json is not an object");
    return false;
  }

  Internal::Utils::FromJsonStr(domain, json, "domain", OPENSHOCK_API_DOMAIN);
  Internal::Utils::FromJsonStr(authToken, json, "authToken", "");
  Internal::Utils::FromJsonStr(lcgOverride, json, "lcgOverride", "");

  return true;
}

cJSON* BackendConfig::ToJSON(bool withSensitiveData) const {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddStringToObject(root, "domain", domain.c_str());

  if (withSensitiveData) {
    cJSON_AddStringToObject(root, "authToken", authToken.c_str());
  }

  cJSON_AddStringToObject(root, "lcgOverride", lcgOverride.c_str());

  return root;
}
