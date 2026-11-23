# How to Capture Serial Logs

When submitting an issue or asking for debugging help, capturing and attaching the serial console logs from your device is often essential.

## Which port you get

Every supported board logs over the chip's own USB port, not through a separate USB-to-serial chip (the firmware sets `ARDUINO_USB_CDC_ON_BOOT=1` in `platformio.ini`).

| Board                        | Chip     | USB serial                                             |
| ---------------------------- | -------- | ------------------------------------------------------ |
| Xteink X3, X4                | ESP32-C3 | Built-in USB Serial/JTAG                               |
| Xteink X4 Pro, LilyGo T5 S3  | ESP32-S3 | Built-in USB Serial/JTAG (native USB CDC on pins 19/20) |
| Murphy M3, BOOX Picco        | ESP32-S3 | Built-in USB Serial/JTAG                               |

What that means in practice:

* **The baud rate is ignored.** The data travels as USB packets. `115200` is the conventional value to type in; any value works.
* **No CH340 or CP210x driver is involved.** Windows lists the device as `USB Serial Device` or `USB JTAG/serial debug unit`; Linux as `/dev/ttyACM*`; macOS as `/dev/tty.usbmodem*`. If a port named `CH340` or `CP210x` shows up, it is not this device.
* **The port disappears and returns when the chip resets.** Re-select it in your terminal after a reboot if the session drops. To catch the boot log, open the port first, then press Reset.
* **The USB transfer screens keep the log off the port.** On the X3 and X4, **USB Transfer** holds log lines back from the port for the whole session so they cannot corrupt the transfer. The X4 Pro, T5 S3, Murphy M3 and BOOX Picco hand the USB port to **USB Drive** while it runs. Either way, leaving the screen restarts the reader; reopen the port to pick the log up again.
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

## For developers: timing and profiling a chapter build

These notes serve work on layout and shaping speed. [The serial test API](serial-test-api.md) drives the device. The
timing books come from `tools/writing-modes-bench/` (its README says how to fetch their sources) and are never
committed.

### Timing a chapter build

* **Compare `parse=`**, or the per-page rate (`[SCT] Page N processed`), not `total=`. The logged
  `createSectionFile ... total=` includes 0.7 to 1.4 s in which the build waits for the first page's render and
  refresh, the same for every script.
* **A cold build:** delete the book's `spines/*/*.bin` in its cache folder (`/.crosspoint/epub_<hash of its path>/`).
* **Run one book's builds in a row.** A book whose language picks another family (since #665 a Hebrew book picks
  NotoSansHebrew) reinstalls the glyph store when it opens after a book in another family: 7.5-10.5 s, and an empty
  store. Alternating a book with its Latin twin gives cold-store figures.
* **Instrumented builds**, throwaway and never merged, on origin: `wip/layout-profile` (build with
  `-DLAYOUT_PROFILE=1`; prints `LAYOUTPROF`: the layout pass's steps and the renderer's kerning and glyph lookups) and
  `wip/shaper-profile` (`OT_PROFILE`, with a host profile test).

### Sampling the C3 with OpenOCD

The X3 and X4 carry JTAG on their USB port, so a sampling profiler needs no extra hardware:
1. Use Espressif's OpenOCD, the version that `tool-openocd-esp32`'s `tools.json` names (download it and check its
   sha256), with `set ESP_RTOS none`, `set ESP_FLASH_SIZE 0` and `board/esp32c3-builtin.cfg`.
2. Build with frame pointers in the libraries to profile (a `library.json` with `build.flags` in each of `Epub`,
   `EpdFont`, `GfxRenderer`, `OtShaper`, `MiniBidi`, `TextShaping` and `Utf8`) and in `src/` (`build_src_flags`).
   All of `lib/` with frame pointers does not fit the 0x640000 app slot. Check the `.bin` against the slot before
   flashing: PlatformIO's "used" figure is about 14 KB under the image.
3. Run `reset run` once, so the firmware boots with the debugger attached. Otherwise its memory protection faults on
   the first stack read and the chip panics.
4. A Tcl procedure in OpenOCD halts the core, reads `pc`, `ra`, `sp` and `fp`, walks the frame pointers (reading
   aligned SRAM only) and resumes, about 40 times a second. GDB's `bt` at each halt gave stale frames, so its
   inclusive figures were wrong.

### Traps

* **No literal bidi controls or joiners in sources.** U+200E and U+200F, U+202A-202E, U+2066-2069, U+061C, ZWJ and
  ZWNJ are invisible and reorder or join the text around them. Write them as code points in sources, commit messages
  and generated data. The exceptions are goldens, which hold laid-out bytes, and the translations, which write U+200C,
  U+200E and U+200F as the characters themselves (`docs/i18n.md`, "Hebrew, Arabic and Persian").
* **A `std::string_view` bound to a temporary** (`IndicShaper::Request::before` and `after` are views) can pass one
  host's suite and fail only in Linux CI, where glibc overwrites the first 16 bytes of a freed chunk.
