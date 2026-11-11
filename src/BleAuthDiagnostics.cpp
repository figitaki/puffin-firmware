#include "BleAuthDiagnostics.h"
#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_gap_ble_api.h>
#include <esp_system.h>

void logBleBondCount(const char *stage) {
  Serial.printf("[ble-diag] %s bonds=%d uptime_ms=%lu\n", stage,
                esp_ble_get_bond_device_num(), millis());
}

static void observeGap(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  // Never dump GAP parameters: some events contain keys or passkeys.
  if (event == ESP_GAP_BLE_AUTH_CMPL_EVT) {
    const auto &auth = param->ble_security.auth_cmpl;
    Serial.printf("[ble-diag] auth success=%u reason=0x%02x auth_mode=0x%02x uptime_ms=%lu\n",
                  auth.success, auth.success ? 0 : auth.fail_reason, auth.auth_mode, millis());
    logBleBondCount("after-auth");
  } else if (event == ESP_GAP_BLE_REMOVE_BOND_DEV_COMPLETE_EVT) {
    Serial.printf("[ble-diag] bond-remove status=%u\n", param->remove_bond_dev_cmpl.status);
    logBleBondCount("after-bond-remove");
  }
}

void beginBleAuthDiagnostics() {
  // BLEDevice invokes this observer after its normal GAP/security handlers.
  BLEDevice::setCustomGapHandler(observeGap);
  Serial.printf("[ble-diag] boot reset_reason=%d\n", esp_reset_reason());
}

// Capture the library's existing request, once, with identical arguments/result.
// No second request, callback replacement, or security policy changes.
extern "C" esp_err_t __real_esp_ble_set_encryption(esp_bd_addr_t, esp_ble_sec_act_t);
extern "C" esp_err_t __wrap_esp_ble_set_encryption(esp_bd_addr_t address, esp_ble_sec_act_t action) {
  logBleBondCount("before-encryption");
  const esp_err_t result = __real_esp_ble_set_encryption(address, action);
  Serial.printf("[ble-diag] encryption-request action=%d result=%d uptime_ms=%lu\n",
                action, result, millis());
  return result;
}
