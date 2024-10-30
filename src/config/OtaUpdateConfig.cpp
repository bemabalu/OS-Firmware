#include "config/OtaUpdateConfig.h"

const char* const TAG = "Config::OtaUpdateConfig";

#include "config/internal/utils.h"
#include "Logging.h"

using namespace OpenShock::Config;

OtaUpdateConfig::OtaUpdateConfig()
  : isEnabled(true)
  , cdnDomain(OPENSHOCK_FW_CDN_DOMAIN)
  , updateChannel(OtaUpdateChannel::Stable)
  , checkOnStartup(false)
  , checkPeriodically(false)
  , checkInterval(30)
  , allowBackendManagement(true)
  , requireManualApproval(false)
  , updateId(0)
  , updateStep(OtaUpdateStep::None)
{
}

OtaUpdateConfig::OtaUpdateConfig(
    bool isEnabled,
    std::string cdnDomain,
    OtaUpdateChannel updateChannel,
    bool checkOnStartup,
    bool checkPeriodically,
    uint16_t checkInterval,
    bool allowBackendManagement,
    bool requireManualApproval,
    int32_t updateId,
    OtaUpdateStep updateStep
  )
  : isEnabled(isEnabled)
  , cdnDomain(std::move(cdnDomain))
  , updateChannel(updateChannel)
  , checkOnStartup(checkOnStartup)
  , checkPeriodically(checkPeriodically)
  , checkInterval(checkInterval)
  , allowBackendManagement(allowBackendManagement)
  , requireManualApproval(requireManualApproval)
  , updateId(updateId)
  , updateStep(updateStep)
{
}

void OtaUpdateConfig::ToDefault() {
  isEnabled              = true;
  cdnDomain              = OPENSHOCK_FW_CDN_DOMAIN;
  updateChannel          = OtaUpdateChannel::Stable;
  checkOnStartup         = false;
  checkPeriodically      = false;
  checkInterval          = 30;  // 30 minutes
  allowBackendManagement = true;
  requireManualApproval  = false;
  updateId               = 0;
  updateStep             = OtaUpdateStep::None;
}

bool OtaUpdateConfig::FromFlatbuffers(const Serialization::Configuration::OtaUpdateConfig* config) {
  if (config == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  isEnabled = config->is_enabled();
  Internal::Utils::FromFbsStr(cdnDomain, config->cdn_domain(), OPENSHOCK_FW_CDN_DOMAIN);
  updateChannel          = config->update_channel();
  checkOnStartup         = config->check_on_startup();
  checkPeriodically      = config->check_periodically();
  checkInterval          = config->check_interval();
  allowBackendManagement = config->allow_backend_management();
  requireManualApproval  = config->require_manual_approval();
  updateId               = config->update_id();
  updateStep             = config->update_step();

  return true;
}

flatbuffers::Offset<OpenShock::Serialization::Configuration::OtaUpdateConfig> OtaUpdateConfig::ToFlatbuffers(flatbuffers::FlatBufferBuilder& builder, bool withSensitiveData) const {
  return Serialization::Configuration::CreateOtaUpdateConfig(builder, isEnabled, builder.CreateString(cdnDomain), updateChannel, checkOnStartup, checkPeriodically, checkInterval, allowBackendManagement, requireManualApproval, updateId, updateStep);
}

bool OtaUpdateConfig::FromJSON(const cJSON* json) {
  if (json == nullptr) {
    OS_LOGW(TAG, "Config is null, setting to default");
    ToDefault();
    return true;
  }

  if (!cJSON_IsObject(json)) {
    OS_LOGE(TAG, "json is not an object");
    return false;
  }

  Internal::Utils::FromJsonBool(isEnabled, json, "isEnabled", true);
  Internal::Utils::FromJsonStr(cdnDomain, json, "cdnDomain", OPENSHOCK_FW_CDN_DOMAIN);
  Internal::Utils::FromJsonStrParsed(updateChannel, json, "updateChannel", OpenShock::TryParseOtaUpdateChannel, OpenShock::OtaUpdateChannel::Stable);
  Internal::Utils::FromJsonBool(checkOnStartup, json, "checkOnStartup", false);
  Internal::Utils::FromJsonBool(checkPeriodically, json, "checkPeriodically", false);
  Internal::Utils::FromJsonU16(checkInterval, json, "checkInterval", 30);
  Internal::Utils::FromJsonBool(allowBackendManagement, json, "allowBackendManagement", true);
  Internal::Utils::FromJsonBool(requireManualApproval, json, "requireManualApproval", false);
  Internal::Utils::FromJsonI32(updateId, json, "updateId", 0);
  Internal::Utils::FromJsonStrParsed(updateStep, json, "updateStep", OpenShock::TryParseOtaUpdateStep, OpenShock::OtaUpdateStep::None);

  return true;
}

cJSON* OtaUpdateConfig::ToJSON(bool withSensitiveData) const {
  cJSON* root = cJSON_CreateObject();

  cJSON_AddBoolToObject(root, "isEnabled", isEnabled);
  cJSON_AddStringToObject(root, "cdnDomain", cdnDomain.c_str());
  cJSON_AddStringToObject(root, "updateChannel", OpenShock::Serialization::Configuration::EnumNameOtaUpdateChannel(updateChannel));
  cJSON_AddBoolToObject(root, "checkOnStartup", checkOnStartup);
  cJSON_AddBoolToObject(root, "checkPeriodically", checkPeriodically);
  cJSON_AddNumberToObject(root, "checkInterval", checkInterval);
  cJSON_AddBoolToObject(root, "allowBackendManagement", allowBackendManagement);
  cJSON_AddBoolToObject(root, "requireManualApproval", requireManualApproval);
  cJSON_AddNumberToObject(root, "updateId", updateId);
  cJSON_AddStringToObject(root, "updateStep", OpenShock::Serialization::Configuration::EnumNameOtaUpdateStep(updateStep));

  return root;
}
