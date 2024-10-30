#include <freertos/FreeRTOS.h>

#include "CommandHandler.h"

const char* const TAG = "CommandHandler";

#include "Chipset.h"
#include "Common.h"
#include "config/Config.h"
#include "EStopManager.h"
#include "Logging.h"
#include "radio/RFTransmitter.h"
#include "ReadWriteMutex.h"
#include "SimpleMutex.h"
#include "Time.h"
#include "util/TaskUtils.h"

#include <freertos/queue.h>

#include <memory>
#include <unordered_map>

const int64_t KEEP_ALIVE_INTERVAL  = 60'000;
const uint16_t KEEP_ALIVE_DURATION = 300;

uint32_t calculateEepyTime(int64_t timeToKeepAlive)
{
  int64_t now = OpenShock::millis();
  return static_cast<uint32_t>(std::clamp(timeToKeepAlive - now, 0LL, KEEP_ALIVE_INTERVAL));
}

struct KnownShocker {
  bool killTask;
  OpenShock::ShockerModelType model;
  uint16_t shockerId;
  int64_t lastActivityTimestamp;
};

static OpenShock::ReadWriteMutex s_rfTransmitterMutex            = {};
static std::unique_ptr<OpenShock::RFTransmitter> s_rfTransmitter = nullptr;

static OpenShock::SimpleMutex s_estopManagerMutex = {};

static OpenShock::ReadWriteMutex s_keepAliveMutex = {};
static QueueHandle_t s_keepAliveQueue             = nullptr;
static TaskHandle_t s_keepAliveTaskHandle         = nullptr;

using namespace OpenShock;

void _keepAliveTask(void* arg)
{
  (void)arg;

  int64_t timeToKeepAlive = KEEP_ALIVE_INTERVAL;

  // Map of shocker IDs to time of next keep-alive
  std::unordered_map<uint16_t, KnownShocker> activityMap;

  while (true) {
    // Calculate eepyTime based on the timeToKeepAlive
    uint32_t eepyTime = calculateEepyTime(timeToKeepAlive);

    KnownShocker cmd;
    while (xQueueReceive(s_keepAliveQueue, &cmd, pdMS_TO_TICKS(eepyTime)) == pdTRUE) {
      if (cmd.killTask) {
        OS_LOGI(TAG, "Received kill command, exiting keep-alive task");
        vTaskDelete(nullptr);
        break;  // This should never be reached
      }

      activityMap[cmd.shockerId] = cmd;

      eepyTime = calculateEepyTime(std::min(timeToKeepAlive, cmd.lastActivityTimestamp + KEEP_ALIVE_INTERVAL));
    }

    // Update the time to now
    int64_t now = OpenShock::millis();

    // Keep track of the minimum activity time, so we know when to wake up
    timeToKeepAlive = now + KEEP_ALIVE_INTERVAL;

    // For every entry that has a keep-alive time less than now, send a keep-alive
    for (auto it = activityMap.begin(); it != activityMap.end(); ++it) {
      auto& cmdRef = it->second;

      if (cmdRef.lastActivityTimestamp + KEEP_ALIVE_INTERVAL < now) {
        OS_LOGV(TAG, "Sending keep-alive for shocker %u", cmdRef.shockerId);

        if (s_rfTransmitter == nullptr) {
          OS_LOGW(TAG, "RF Transmitter is not initialized, ignoring keep-alive");
          break;
        }

        if (!s_rfTransmitter->SendCommand(cmdRef.model, cmdRef.shockerId, ShockerCommandType::Vibrate, 0, KEEP_ALIVE_DURATION, false)) {
          OS_LOGW(TAG, "Failed to send keep-alive for shocker %u", cmdRef.shockerId);
        }

        cmdRef.lastActivityTimestamp = now;
      }

      timeToKeepAlive = std::min(timeToKeepAlive, cmdRef.lastActivityTimestamp + KEEP_ALIVE_INTERVAL);
    }
  }
}

bool _internalSetKeepAliveEnabled(bool enabled)
{
  bool wasEnabled = s_keepAliveQueue != nullptr && s_keepAliveTaskHandle != nullptr;

  if (enabled == wasEnabled) {
    return true;
  }

  ScopedWriteLock lock__(&s_keepAliveMutex);

  if (enabled) {
    OS_LOGV(TAG, "Enabling keep-alive task");

    s_keepAliveQueue = xQueueCreate(32, sizeof(KnownShocker));
    if (s_keepAliveQueue == nullptr) {
      OS_LOGE(TAG, "Failed to create keep-alive task");
      return false;
    }

    if (TaskUtils::TaskCreateExpensive(_keepAliveTask, "KeepAliveTask", 4096, nullptr, 1, &s_keepAliveTaskHandle) != pdPASS) {  // PROFILED: 1.5KB stack usage
      OS_LOGE(TAG, "Failed to create keep-alive task");

      vQueueDelete(s_keepAliveQueue);
      s_keepAliveQueue = nullptr;

      return false;
    }
  } else {
    OS_LOGV(TAG, "Disabling keep-alive task");
    if (s_keepAliveTaskHandle != nullptr && s_keepAliveQueue != nullptr) {
      // Wait for the task to stop
      KnownShocker cmd {.killTask = true};
      while (eTaskGetState(s_keepAliveTaskHandle) != eDeleted) {
        vTaskDelay(pdMS_TO_TICKS(10));

        // Send nullptr to stop the task gracefully
        xQueueSend(s_keepAliveQueue, &cmd, pdMS_TO_TICKS(10));
      }
      vQueueDelete(s_keepAliveQueue);
      s_keepAliveQueue = nullptr;
    } else {
      OS_LOGW(TAG, "keep-alive task is already disabled? Something might be wrong.");
    }
  }

  return true;
}

bool CommandHandler::Init()
{
  static bool initialized = false;
  if (initialized) {
    OS_LOGW(TAG, "RF Transmitter and EStopManager are already initialized?");
    return true;
  }
  initialized = true;

  Config::RFConfig rfConfig;
  if (!Config::GetRFConfig(rfConfig)) {
    OS_LOGE(TAG, "Failed to get RF config");
    return false;
  }

  gpio_num_t txPin = rfConfig.txPin;
  if (!OpenShock::IsValidOutputPin(txPin)) {
    if (!OpenShock::IsValidOutputPin(OPENSHOCK_RF_TX_GPIO)) {
      OS_LOGE(TAG, "Configured RF TX pin (%hhi) is invalid, and default pin (%hhi) is invalid. Unable to initialize RF transmitter", txPin, OPENSHOCK_RF_TX_GPIO);

      OS_LOGD(TAG, "Setting RF TX pin to GPIO_INVALID");
      return Config::SetRFConfigTxPin(static_cast<gpio_num_t>(OPENSHOCK_GPIO_INVALID));  // This is not a error yet, unless we are unable to save the RF TX Pin as invalid
    }

    OS_LOGW(TAG, "Configured RF TX pin (%hhi) is invalid, using default pin (%hhi)", txPin, OPENSHOCK_RF_TX_GPIO);
    txPin = static_cast<gpio_num_t>(OPENSHOCK_RF_TX_GPIO);
    if (!Config::SetRFConfigTxPin(txPin)) {
      OS_LOGE(TAG, "Failed to set RF TX pin in config");
      return false;
    }
  }

  s_rfTransmitter = std::make_unique<RFTransmitter>(txPin);
  if (!s_rfTransmitter->ok()) {
    OS_LOGE(TAG, "Failed to initialize RF Transmitter");
    s_rfTransmitter = nullptr;
    return false;
  }

  if (rfConfig.keepAliveEnabled) {
    _internalSetKeepAliveEnabled(true);
  }

  Config::EStopConfig estopConfig;
  if (!Config::GetEStop(estopConfig)) {
    OS_LOGE(TAG, "Failed to get EStop config");
    return false;
  }

  // TODO: Implement EStopManager pin change logic

  return true;
}

bool CommandHandler::Ok()
{
  return s_rfTransmitter != nullptr;
}

SetGPIOResultCode CommandHandler::SetRfTxPin(gpio_num_t txPin)
{
  if (!OpenShock::IsValidOutputPin(txPin)) {
    return SetGPIOResultCode::InvalidPin;
  }

  ScopedWriteLock lock__(&s_rfTransmitterMutex);

  if (s_rfTransmitter != nullptr) {
    OS_LOGV(TAG, "Destroying existing RF transmitter");
    s_rfTransmitter = nullptr;
  }

  OS_LOGV(TAG, "Creating new RF transmitter");
  auto rfxmit = std::make_unique<RFTransmitter>(txPin);
  if (!rfxmit->ok()) {
    OS_LOGE(TAG, "Failed to initialize RF transmitter");
    return SetGPIOResultCode::InternalError;
  }

  if (!Config::SetRFConfigTxPin(txPin)) {
    OS_LOGE(TAG, "Failed to set RF TX pin in config");
    return SetGPIOResultCode::InternalError;
  }

  s_rfTransmitter = std::move(rfxmit);

  return SetGPIOResultCode::Success;
}

SetGPIOResultCode CommandHandler::SetEStopPin(gpio_num_t estopPin)
{
  if (OpenShock::IsValidInputPin(static_cast<int8_t>(estopPin))) {
    ScopedLock lock__(&s_estopManagerMutex);

    if (!EStopManager::SetEStopPin(estopPin)) {
      OS_LOGE(TAG, "Failed to set EStop pin");

      return SetGPIOResultCode::InternalError;
    }

    if (!Config::SetEStopGpioPin(estopPin)) {
      OS_LOGE(TAG, "Failed to set EStop pin in config");

      return SetGPIOResultCode::InternalError;
    }

    return SetGPIOResultCode::Success;
  } else {
    return SetGPIOResultCode::InvalidPin;
  }
}

bool CommandHandler::SetKeepAliveEnabled(bool enabled)
{
  if (!_internalSetKeepAliveEnabled(enabled)) {
    return false;
  }

  if (!Config::SetRFConfigKeepAliveEnabled(enabled)) {
    OS_LOGE(TAG, "Failed to set keep-alive enabled in config");
    return false;
  }

  return true;
}

bool CommandHandler::SetKeepAlivePaused(bool paused)
{
  bool keepAliveEnabled = false;
  if (!Config::GetRFConfigKeepAliveEnabled(keepAliveEnabled)) {
    OS_LOGE(TAG, "Failed to get keep-alive enabled from config");
    return false;
  }

  if (keepAliveEnabled == false && paused == false) {
    OS_LOGW(TAG, "Keep-alive is disabled in config, ignoring unpause command");
    return false;
  }
  if (!_internalSetKeepAliveEnabled(!paused)) {
    return false;
  }

  return true;
}

gpio_num_t CommandHandler::GetRfTxPin()
{
  if (s_rfTransmitter != nullptr) {
    return s_rfTransmitter->GetTxPin();
  }

  gpio_num_t txPin;
  if (!Config::GetRFConfigTxPin(txPin)) {
    OS_LOGE(TAG, "Failed to get RF TX pin from config");
    txPin = static_cast<gpio_num_t>(OPENSHOCK_GPIO_INVALID);
  }

  return txPin;
}

bool CommandHandler::HandleCommand(ShockerModelType model, uint16_t shockerId, ShockerCommandType type, uint8_t intensity, uint16_t durationMs)
{
  ScopedReadLock lock__rf(&s_rfTransmitterMutex);

  if (s_rfTransmitter == nullptr) {
    OS_LOGW(TAG, "RF Transmitter is not initialized, ignoring command");
    return false;
  }

  // Stop logic
  if (type == ShockerCommandType::Stop) {
    OS_LOGV(TAG, "Stop command received, clearing pending commands");

    type       = ShockerCommandType::Vibrate;
    intensity  = 0;
    durationMs = 300;

    s_rfTransmitter->ClearPendingCommands();
  } else {
    OS_LOGD(TAG, "Command received: %u %u %u %u", model, shockerId, type, intensity);
  }

  bool ok = s_rfTransmitter->SendCommand(model, shockerId, type, intensity, durationMs);

  lock__rf.unlock();
  ScopedReadLock lock__ka(&s_keepAliveMutex);

  if (ok && s_keepAliveQueue != nullptr) {
    KnownShocker cmd {.model = model, .shockerId = shockerId, .lastActivityTimestamp = OpenShock::millis() + durationMs};
    if (xQueueSend(s_keepAliveQueue, &cmd, pdMS_TO_TICKS(10)) != pdTRUE) {
      OS_LOGE(TAG, "Failed to send keep-alive command to queue");
    }
  }

  return ok;
}
