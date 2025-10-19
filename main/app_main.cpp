/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <log_heap_numbers.h>

#include <app_priv.h>
#include <app_reset.h>
#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif


#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
#include <esp_matter_providers.h>
#include <lib/support/Span.h>
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
#include <platform/ESP32/ESP32SecureCertDACProvider.h>
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
#include <platform/ESP32/ESP32FactoryDataProvider.h>
#endif
using namespace chip::DeviceLayer;
#endif
#include <air-quality-sensor-manager.h>

#include "drivers/scd4x_i2c.h"
#include "drivers/sensirion_i2c_hal.h"

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
// using namespace chip::app::Clusters;
using namespace ::chip::app::Clusters;
using namespace ::chip::app::Clusters::AirQuality;

constexpr auto k_timeout_seconds = 300;

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
extern const uint8_t cd_start[] asm("_binary_certification_declaration_der_start");
extern const uint8_t cd_end[] asm("_binary_certification_declaration_der_end");

const chip::ByteSpan cdSpan(cd_start, static_cast<size_t>(cd_end - cd_start));
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

#if CONFIG_ENABLE_ENCRYPTED_OTA
extern const char decryption_key_start[] asm("_binary_esp_image_encryption_key_pem_start");
extern const char decryption_key_end[] asm("_binary_esp_image_encryption_key_pem_end");

static const char *s_decryption_key = decryption_key_start;
static const uint16_t s_decryption_key_len = decryption_key_end - decryption_key_start;
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
        MEMORY_PROFILER_DUMP_HEAP_STAT("commissioning window opened");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        {
            ESP_LOGI(TAG, "Fabric removed successfully");
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0)
            {
                chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
                constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
                if (!commissionMgr.IsCommissioningWindowOpen())
                {
                    /* After removing last fabric, this example does not remove the Wi-Fi credentials
                     * and still has IP connectivity so, only advertising on DNS-SD.
                     */
                    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
                    if (err != CHIP_NO_ERROR)
                    {
                        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                    }
                }
            }
        break;
        }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        MEMORY_PROFILER_DUMP_HEAP_STAT("BLE deinitialized");
        break;

    default:
        break;
    }
}


uint16_t qual_endpoint;
// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        // app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        // err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

static void sensor_update_task(void *pvParameters)
{
    uint16_t endpoint_id = qual_endpoint;
    
    ESP_LOGI(TAG, "Waiting for Matter stack to initialize...");
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    
    while (true) {

        
        // Update CO2 concentration
        
        chip::DeviceLayer::PlatformMgr().LockChipStack();


        uint16_t co2_measurement; 
        int32_t temp_measurement;
        int32_t humidity_measurement;
        scd4x_read_measurement(&co2_measurement, &temp_measurement, &humidity_measurement); 
        uint16_t co2_value = co2_measurement;
        ESP_LOGI(TAG, "MEASUREMENTS: %d, %lu, %lu", co2_measurement, temp_measurement, humidity_measurement);

        //UNCOMMENT THESE IF USING AAI
        // AirQualitySensorManager * mInstance = AirQualitySensorManager::GetInstance();
        // mInstance->OnAirQualityChangeHandler(AirQualityEnum::kGood);
        ESP_LOGI(TAG, "CO2: %d", co2_value);

        // mInstance->OnCarbonDioxideMeasurementChangeHandler(co2_value);
        
        //Note that from what I remember, the feature flags for air quality aren't really set by us. Maybe handled by esp-matter, idk, but 
        //The only flags that exist are all optional and are for supporting fair, moderate, very poor, and extremely poor air quality. 
        //value 1 is good, value 2 is fair, 3 is moderate, 4 is poor. Value 0 is unknown.

        int8_t air_quality_value_int = 0;
        //this is a quick and dirty way of setting this. Consider making it less terrible later. Also, these categories are completely arbitrary.
        if(co2_value <= 1000)
        {
            air_quality_value_int = 1;
        }
        else if(co2_value <= 2500)
        {
            air_quality_value_int = 2;
        }
        else if(co2_value <= 5000)
        {
            air_quality_value_int = 3;
        }
        else
        {
            air_quality_value_int = 4;
        }
        esp_matter_attr_val_t air_qual_val = esp_matter_enum8(air_quality_value_int);

        esp_matter::attribute::update(endpoint_id, AirQuality::Id, AirQuality::Attributes::AirQuality::Id, &air_qual_val);

        esp_matter_attr_val_t co2_val = esp_matter_nullable_float(co2_measurement);
        esp_matter::attribute::update(endpoint_id, 
                                     CarbonDioxideConcentrationMeasurement::Id,
                                     CarbonDioxideConcentrationMeasurement::Attributes::MeasuredValue::Id,
                                     &co2_val);
        chip::DeviceLayer::PlatformMgr().UnlockChipStack();

        vTaskDelay(5000 / portTICK_PERIOD_MS);
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    MEMORY_PROFILER_DUMP_HEAP_STAT("Bootup");

    /* Initialize driver */
    sensirion_i2c_hal_init();
    scd4x_init(0x62);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;

    // node handle can be used to add/modify other endpoints.
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    MEMORY_PROFILER_DUMP_HEAP_STAT("node created");

    air_quality_sensor::config_t air_quality_sensor_config;

    endpoint_t *air_qual_ep = air_quality_sensor::create(node, &air_quality_sensor_config, ENDPOINT_FLAG_NONE, NULL);
    //note that the air quality cluster is automagically created by the air quality endpoint? This part isn't 100% clear to me, and it seems like it's changing on
    //the esp-matter side too. Remember to keep watch on this. 

    //cluster_t *aq_cluster_def = esp_matter::cluster::air_quality::create(air_qual_ep, &air_quality_sensor_config.air_quality, CLUSTER_FLAG_SERVER);

    esp_matter::cluster::concentration_measurement::config_t co2_config;

    // Set measurement medium to Air (typically 0x00)
    co2_config.measurement_medium = 0x00; // Air

    // Configure the mandatory numeric measurement feature for CO2
    co2_config.features.numeric_measurement.min_measured_value = 0.0f;
    co2_config.features.numeric_measurement.max_measured_value = 10000.0f; // Typical CO2 sensors go up to 5000 ppm
    co2_config.features.numeric_measurement.measured_value = 400.0f; // Initialize to typical ambient CO2
    co2_config.features.numeric_measurement.measurement_unit = 0; // Parts per million

    // Set feature flags for numeric measurement
    co2_config.feature_flags = 1;

    // Delegate can be set later if needed
    co2_config.delegate = nullptr;

    // Create the cluster instance
    cluster_t *cluster = esp_matter::cluster::carbon_dioxide_concentration_measurement::create(air_qual_ep, &co2_config, CLUSTER_FLAG_SERVER);

    qual_endpoint = endpoint::get_id(air_qual_ep);

    ABORT_APP_ON_FAILURE(air_qual_ep != nullptr, ESP_LOGE(TAG, "Failed to create air quality sensor endpoint"));
    
    //UNCOMMENT THIS IF USING AAI.
    // AirQualitySensorManager::InitInstance(qual_endpoint);

    //init scd40 here

    sensirion_i2c_hal_init();
    vTaskDelay(100 / portTICK_PERIOD_MS);
    scd4x_start_periodic_measurement();


#if CHIP_DEVICE_CONFIG_ENABLE_THREAD && CHIP_DEVICE_CONFIG_ENABLE_WIFI_STATION
    // Enable secondary network interface
    secondary_network_interface::config_t secondary_network_interface_config;
    endpoint = endpoint::secondary_network_interface::create(node, &secondary_network_interface_config, ENDPOINT_FLAG_NONE, nullptr);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create secondary network interface endpoint"));
#endif


#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

#ifdef CONFIG_ENABLE_SET_CERT_DECLARATION_API
    auto * dac_provider = get_dac_provider();
#ifdef CONFIG_SEC_CERT_DAC_PROVIDER
    static_cast<ESP32SecureCertDACProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#elif defined(CONFIG_FACTORY_PARTITION_DAC_PROVIDER)
    static_cast<ESP32FactoryDataProvider *>(dac_provider)->SetCertificationDeclaration(cdSpan);
#endif
#endif // CONFIG_ENABLE_SET_CERT_DECLARATION_API

    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    MEMORY_PROFILER_DUMP_HEAP_STAT("matter started");

    /* Starting driver with default values */

#if CONFIG_ENABLE_ENCRYPTED_OTA
    err = esp_matter_ota_requestor_encrypted_init(s_decryption_key, s_decryption_key_len);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to initialized the encrypted OTA, err: %d", err));
#endif // CONFIG_ENABLE_ENCRYPTED_OTA

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::attribute_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
    esp_matter::console::init();
#endif
    xTaskCreate(sensor_update_task, "sensor_update", 4096, NULL, 5, NULL);

    while (true) {
        MEMORY_PROFILER_DUMP_HEAP_STAT("Idle");
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }
}
