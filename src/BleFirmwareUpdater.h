#pragma once

#include "FirmwareUpdate.h"
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLE2902.h>
#include <esp_ota_ops.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <freertos/semphr.h>

class EspFirmwareStorage : public FirmwareStorage {
public:
    uint32_t capacity() const override {
        const esp_partition_t *next = esp_ota_get_next_update_partition(nullptr);
        return next ? next->size : 0;
    }

    bool begin(uint32_t size) override {
        target = esp_ota_get_next_update_partition(nullptr);
        if (!target || esp_ota_begin(target, size, &handle) != ESP_OK) return false;
        writing = true;
        mbedtls_sha256_init(&hash);
        hashing = true;
        return mbedtls_sha256_starts_ret(&hash, 0) == 0;
    }

    bool write(const uint8_t *data, size_t size) override {
        return writing && mbedtls_sha256_update_ret(&hash, data, size) == 0 &&
               esp_ota_write(handle, data, size) == ESP_OK;
    }

    bool finish(const uint8_t expected[32]) override {
        uint8_t actual[32];
        if (!writing || mbedtls_sha256_finish_ret(&hash, actual) || memcmp(actual, expected, 32)) return false;
        mbedtls_sha256_free(&hash);
        hashing = false;
        const esp_err_t valid = esp_ota_end(handle); // Includes ESP chip/image validation.
        writing = false;
        return valid == ESP_OK && esp_ota_set_boot_partition(target) == ESP_OK;
    }

    void abort() override {
        if (writing) esp_ota_abort(handle);
        if (hashing) mbedtls_sha256_free(&hash);
        writing = hashing = false;
    }

private:
    const esp_partition_t *target = nullptr;
    esp_ota_handle_t handle = 0;
    mbedtls_sha256_context hash;
    bool writing = false, hashing = false;
};

// Exclusive boot mode: never initialize this alongside BLE Keyboard or MIDI.
// State and flash operations are serialized across the Arduino/Bluedroid tasks.
class BleFirmwareUpdater : public BLEServerCallbacks, public BLECharacteristicCallbacks {
public:
    bool begin() {
        guard = xSemaphoreCreateMutex();
        if (!guard) return false;
        snprintf(pin, sizeof(pin), "%06lu", static_cast<unsigned long>(esp_random() % 1000000));
        session = new FirmwareUpdate(storage, pin, millis());
        // Keep a unique maintenance identity separate from the keyboard/MIDI
        // identities so hosts do not reuse their cached HID/GATT discovery.
        uint8_t mac[6];
        if (esp_efuse_mac_get_default(mac) != ESP_OK) return false;
        mac[0] = (mac[0] | 2) & 0xFE;
        mac[5] ^= 0x80;
        if (esp_base_mac_addr_set(mac) != ESP_OK) return false;
        BLEDevice::init("Puffin Update");
        BLEDevice::setMTU(185);
        server = BLEDevice::createServer();
        server->setCallbacks(this);
        BLEService *service = server->createService("B9B75000-892A-4B9E-9E34-9F1B8DC7B56A");
        control = service->createCharacteristic("B9B75001-892A-4B9E-9E34-9F1B8DC7B56A", BLECharacteristic::PROPERTY_WRITE);
        data = service->createCharacteristic("B9B75002-892A-4B9E-9E34-9F1B8DC7B56A", BLECharacteristic::PROPERTY_WRITE);
        statusValue = service->createCharacteristic("B9B75003-892A-4B9E-9E34-9F1B8DC7B56A",
                                                    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
        statusValue->addDescriptor(new BLE2902());
        control->setCallbacks(this);
        data->setCallbacks(this);
        publish(false);
        service->start();
        BLEAdvertising *advertising = BLEDevice::getAdvertising();
        advertising->addServiceUUID(service->getUUID());
        advertising->setScanResponse(true);
        advertising->start();
        ready = true;
        return true;
    }

    const char *code() const { return pin; }

    void status(uint8_t bytes[12]) {
        memset(bytes, 0, 12);
        bytes[0] = 1;
        if (!ready) { bytes[1] = FirmwareUpdate::Error; bytes[2] = FirmwareUpdate::Flash; return; }
        xSemaphoreTake(guard, portMAX_DELAY);
        session->status(bytes);
        xSemaphoreGive(guard);
    }

    void poll() {
        if (!ready) return;
        xSemaphoreTake(guard, portMAX_DELAY);
        session->poll(millis());
        publish(false);
        if (!advertisingClosed && (session->error() == FirmwareUpdate::Timeout || session->error() == FirmwareUpdate::Locked)) {
            BLEDevice::getAdvertising()->stop();
            advertisingClosed = true;
        }
        if (session->state() == FirmwareUpdate::Complete && !rebootPending) {
            rebootPending = true;
            completedAt = millis();
        }
        const bool restart = rebootPending && uint32_t(millis() - completedAt) >= 1000;
        xSemaphoreGive(guard);
        if (restart) ESP.restart();
    }

    void onConnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
        xSemaphoreTake(guard, portMAX_DELAY);
        const bool reject = connected;
        if (!reject) { connected = true; connection = param->connect.conn_id; }
        xSemaphoreGive(guard);
        if (reject) server->disconnect(param->connect.conn_id);
    }

    void onDisconnect(BLEServer *, esp_ble_gatts_cb_param_t *param) override {
        xSemaphoreTake(guard, portMAX_DELAY);
        if (!connected || connection != param->disconnect.conn_id) { xSemaphoreGive(guard); return; }
        connected = false;
        session->disconnect(millis());
        publish(false);
        const bool advertise = session->state() != FirmwareUpdate::Complete &&
                               session->error() != FirmwareUpdate::Locked && session->error() != FirmwareUpdate::Timeout;
        if (advertise) advertisingClosed = false;
        xSemaphoreGive(guard);
        if (advertise) BLEDevice::startAdvertising();
    }

    void onWrite(BLECharacteristic *characteristic, esp_ble_gatts_cb_param_t *param) override {
        xSemaphoreTake(guard, portMAX_DELAY);
        if (connected && param->write.conn_id == connection) {
            const std::string value = characteristic->getValue();
            const uint8_t *bytes = reinterpret_cast<const uint8_t *>(value.data());
            if (characteristic == control) session->control(bytes, value.size(), millis());
            else if (characteristic == data) session->data(bytes, value.size(), millis());
            publish(true);
        }
        xSemaphoreGive(guard);
    }

private:
    EspFirmwareStorage storage;
    FirmwareUpdate *session = nullptr;
    SemaphoreHandle_t guard = nullptr;
    BLEServer *server = nullptr;
    BLECharacteristic *control = nullptr, *data = nullptr, *statusValue = nullptr;
    char pin[7] = "------";
    uint8_t previous[12] = {255};
    uint16_t connection = 0;
    bool connected = false, ready = false, rebootPending = false, advertisingClosed = false;
    uint32_t completedAt = 0;

    void publish(bool force) { // Caller holds guard (or is initializing before advertising).
        uint8_t bytes[12];
        session->status(bytes);
        if (force || memcmp(bytes, previous, 12)) {
            statusValue->setValue(bytes, 12);
            memcpy(previous, bytes, 12);
            if (connected) statusValue->notify();
        }
    }
};
