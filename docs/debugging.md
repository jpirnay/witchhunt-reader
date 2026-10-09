# How to Capture Serial Logs

When submitting an issue or asking for debugging help, capturing and attaching the serial console logs from your device is often essential.

## Which port you get

Every supported board logs over the chip's own USB port, not through a separate USB-to-serial chip (the firmware sets `ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`).

| Board                        | Chip     | USB serial                                             |
| ---------------------------- | -------- | ------------------------------------------------------ |
| Xteink X3, X4                | ESP32-C3 | Built-in USB Serial/JTAG                               |
| Xteink X4 Pro, LilyGo T5 S3  | ESP32-S3 | Built-in USB Serial/JTAG (native USB CDC on pins 19/20) |

What that means in practice:

* **The baud rate is ignored.** The data travels as USB packets. `115200` is the conventional value to type in; any value works.
* **No CH340 or CP210x driver is involved.** Windows lists the device as `USB Serial Device` or `USB JTAG/serial debug unit`; Linux as `/dev/ttyACM*`; macOS as `/dev/tty.usbmodem*`. If a port named `CH340` or `CP210x` shows up, it is not this device.
* **The port disappears and returns when the chip resets.** Re-select it in your terminal after a reboot if the session drops. To catch the boot log, open the port first, then press Reset.
* **The USB transfer screens keep the log off the port.** On the X3 and X4, **USB Transfer** holds log lines back from the port for the whole session so they cannot corrupt the transfer. The X4 Pro and T5 S3 hand the USB port to **USB Drive** while it runs. Either way, leaving the screen restarts the reader; reopen the port to pick the log up again.
* **Only builds with serial logging print anything.** The release and development builds do; the `slim` build does not.

Below are step-by-step instructions for Windows, macOS, Linux, and Web Browsers. Choose the method that best matches your setup.

## 🌐 Method 1: Web Browser (Chrome / Edge) — *Easiest / All Operating Systems*

If you use **Google Chrome** or **Microsoft Edge**, you do not need to install any software or use command-line terminals. You can use Web Serial directly in your browser.

1. **Connect Device:** Plug the reader into your computer using a USB **data** cable (ensure it is not a power-only charging cable).

2. **Open Web Serial Monitor:** Go to a Web Serial tool in Chrome or Edge (e.g., [https://esp.nonet.eu/](https://esp.nonet.eu/) or [https://serial.dazzler.app/](https://serial.dazzler.app/)).

3. **Configure Settings:**

   * **Baud Rate:** Set to `115200`

4. **Connect:** Click **Connect** (or **Pair**), select your reader from the browser pop-up window, and confirm.

5. **Capture Output:** Press the **Reset** button on the reader to capture the boot sequence and logs. Copy the output text and paste it into your GitHub issue.

## 🪟 Method 2: Windows

### Using PuTTY (Recommended)

1. **Find COM Port:**

   * Right-click the Windows Start menu and select **Device Manager**.

   * Expand **Ports (COM & LPT)**.

   * Look for `USB Serial Device` or `USB JTAG/serial debug unit` and note the COM number (e.g., `COM3`).

2. **Download PuTTY:** Download standalone `putty.exe` from [putty.org](https://www.putty.org/).

3. **Configure PuTTY:**

   * Connection type: Select **Serial**.

   * **Serial line:** Enter your COM port (e.g., `COM3`).

   * **Speed:** Enter `115200`.

4. **Capture Logs:**

   * Click **Open**.

   * Press the **Reset** button on the reader device.

   * Right-click the PuTTY title bar and select **Copy All to Clipboard** to copy your log messages.

## 🍎 Method 3: macOS

1. **Open Terminal:** Press `Cmd + Space`, type `Terminal`, and press `Enter`.

2. **Identify Port:** Run the following command to list connected USB serial devices:

   ```
   ls /dev/tty.usbmodem*
   
   ```

   *Note the device path returned (e.g., `/dev/tty.usbmodem14101`).*

3. **Start Serial Session:**
   Run `screen` with your device path and baud rate:

   ```
   screen /dev/tty.usbmodem14101 115200
   
   ```

4. **Capture Output:** Press the **Reset** button on the reader to view boot logs. Select and copy the text output.

5. **To Exit `screen`:** Press `Ctrl + A`, then type `:quit` and press `Enter`.

## 🐧 Method 4: Linux

1. **Identify Port:** Open a terminal and run:

   ```
   ls /dev/ttyACM*
   
   ```

   *(Usually `/dev/ttyACM0`)*

2. **Check Port Permissions:**
   If you get a permission error when opening the port, add your user to the `dialout` group:

   ```
   sudo usermod -a -G dialout $USER
   
   ```

   *(You may need to log out and log back in for changes to take effect).*

3. **Monitor Output:**
   Use `screen`:

   ```
   screen /dev/ttyACM0 115200
   
   ```

   Or use direct stream reading:

   ```
   stty -F /dev/ttyACM0 115200 raw -echo && cat /dev/ttyACM0
   
   ```

## 🛠️ Troubleshooting Checklist

* **Garbage Characters / Gibberish Text:** The baud rate cannot cause this on these boards (see [Which port you get](#which-port-you-get)). Check that you opened the reader's port and not another device's.

* **No Device Found:** Ensure your USB cable supports **data transmission** and isn't a power-only charging cable.

* **Unknown device (Windows):** If Device Manager shows an unknown USB device with a yellow icon, try another cable or port, then reinstall the Espressif USB JTAG/serial driver. No CH340 or CP210x driver is needed.