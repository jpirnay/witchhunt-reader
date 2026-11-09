# exp/iram-budget: what may leave IRAM, and why it is safe

On the single-core ESP32-C3 every flash erase or write (FlashGlyphStore installs and fills, OTA, NVS and settings)
runs with the flash cache disabled. While it is, only code in IRAM can run: the flash driver itself, and any
interrupt registered with `ESP_INTR_FLAG_IRAM`, with everything it calls. Every other interrupt is masked
(`esp_intr_noniram_disable`) and the scheduler is suspended (`vTaskSuspendAll`) before the cache goes off.

`options.txt` flips three option groups, each of which changes ESP-IDF v5.5.5's linker fragments only (no compiled
source reads the symbols). IDF itself keeps in IRAM what its own cache-disabled paths and ISRs need:

| Option | Moved to flash (map) | Kept in IRAM |
|---|---|---|
| `FREERTOS_PLACE_FUNCTIONS_INTO_FLASH` | 91 sections, 11,008 B (tasks, queues, timers, event groups) | FromISR functions, tick and context switch, critical sections, lists, `xTaskGetSchedulerState` (3,308 B) |
| `RINGBUF_PLACE_FUNCTIONS_INTO_FLASH` | 15 sections, 1,758 B (create, send, receive, delete) | every FromISR function and its helpers (3,026 B) |
| `RMT_{TX,RX}_ISR_HANDLER_IN_IRAM` unset | 10 sections, 2,338 B | `rmt_encoder_copy` (its attribute is compiled in) |

## The interrupts of this firmware

None of the firmware's is registered with `ESP_INTR_FLAG_IRAM`, so none runs while the cache is off (the SDK's are
`IRAM_ATTR` all the same, Arduino's HWCDC handler is in flash); every FreeRTOS or ring buffer function they call stays
in IRAM (checked in the linked ELF):

| ISR | Registered by | IRAM flag | Calls (placement after the change) |
|---|---|---|---|
| `epdBusyIsr` (display BUSY wait) | `attachInterrupt` (Arduino GPIO ISR service, `ARDUINO_ISR_FLAG` = 0: `CONFIG_ARDUINO_ISR_IRAM` unset) | no | `xQueueGiveFromISR` (IRAM), `vPortYieldFromISR` (IRAM) |
| `chsc6xTouchInterrupt`, `cstTouchInterrupt` (touch boards; not linked for the C3) | `attachInterrupt` | no | none: a volatile flag |
| `hw_cdc_isr_handler` (USB-CDC Serial; itself in flash) | `esp_intr_alloc(..., 0, ...)` in Arduino's HWCDC | no | `xRingbufferReceiveUpToFromISR`, `vRingbufferReturnItemFromISR`, `xRingbufferSendFromISR`, `xQueueGenericSendFromISR` (all IRAM) |
| `onUsbSerialJtagReset` (USB-OTG handoff; not linked for the C3) | `esp_intr_alloc(..., 0, ...)` | no | `xQueueGiveFromISR`, `vPortYieldFromISR` (IRAM) |

Buttons are polled, not interrupt driven. `esp_timer` callbacks (HapticManager) run in the esp_timer task; its alarm
ISR stays in IRAM and wakes the task with `vTaskGenericNotifyGiveFromISR` (IRAM). The interrupts that are
IRAM-flagged are IDF's own: the FreeRTOS tick (`SysTickIsrHandler`, `xPortSysTickHandler`, `xTaskIncrementTick`,
`vTaskSwitchContext`, all kept), the esp_timer alarm, the watchdogs, cache errors and panic.

## IRAM code that now calls into flash

A disassembly of the new `.iram0.text` lists every call from IRAM to a function that left it (11 targets):

- `uxTaskGetNumberOfTasks` from `xQueueGenericSendFromISR`, `xQueueGiveFromISR`, `xQueueReceiveFromISR` and
  `prvNotifyQueueSetContainer`: only through `prvIncrementQueueTxLock`/`RxLock`, when a task holds the queue locked
  inside a queue call. With one core and the scheduler suspended for a flash operation, no task is inside one.
- `xTaskPriorityDisinherit` from `prvCopyDataToQueue`: mutexes only, which no ISR may give.
- `vTaskDelay` from `spi_flash_os_yield`: called by the erase loop before it takes the bus again (cache on).
  `vTaskSuspendAll` and `xTaskResumeAll` (now in flash too) are called by the flash driver before it disables the
  cache and after it enables it again.
- `vQueueDelete`, `xQueueGetMutexHolder`, `xQueueCreateMutex`, `xQueueSemaphoreTake`, `xQueueTakeMutexRecursive`
  from newlib's and esp_log's locks, `vRingbufferDelete` from `vRingbufferDeleteWithCaps`: task context.
- `xTaskGetCurrentTaskHandleForCore` from `esp_backtrace_print`, `xPortCheckValidTCBMem` from `vTaskGetSnapshot`:
  the panic and core dump paths. Residual risk: a panic raised while the cache is off loses its backtrace or core
  dump, not more.

The device check for the experiment: flash-writing paths (glyph store installs and fills, settings saves) while
buttons, the display BUSY wait and USB serial are active.
