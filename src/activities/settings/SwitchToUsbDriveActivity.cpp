#include "SwitchToUsbDriveActivity.h"

#include <Arduino.h>
#include <I18n.h>
#include <Logging.h>
#include <esp_partition.h>

#include "activities/util/ConfirmationActivity.h"
#include "network/OtaBootSwitch.h"

void SwitchToUsbDriveActivity::onEnter() {
  Activity::onEnter();

  const esp_partition_t* app1 =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
  if (!app1) {
    LOG_ERR("USB", "app1 partition not found — USB drive firmware not installed");
    finish();
    return;
  }

  startActivityForResult(std::make_unique<ConfirmationActivity>(renderer, mappedInput, tr(STR_SWITCH_TO_USB_DRIVE),
                                                                tr(STR_USB_DRIVE_CONFIRM_BODY)),
                         [this, app1](const ActivityResult& result) {
                           if (result.isCancelled) {
                             finish();
                             return;
                           }
                           if (!ota_boot::switchTo(app1)) {
                             LOG_ERR("USB", "Failed to switch boot partition to app1");
                             finish();
                             return;
                           }
                           ESP.restart();
                         });
}
