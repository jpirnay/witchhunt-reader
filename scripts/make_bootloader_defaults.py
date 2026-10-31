"""Write bootloader/esp32c3/sdkconfig.defaults from the Arduino core's own ESP32-C3 sdkconfig.

The bootloader built from it (bootloader/esp32c3, see its README.md) is the core's own but for a few
lines, so it is regenerated whenever the core's ESP-IDF changes:

    python scripts/make_bootloader_defaults.py         <core_dir>/packages/framework-arduinoespressif32-libs/esp32c3/sdkconfig         bootloader/esp32c3/sdkconfig.defaults
"""
import re
import sys

src, out = sys.argv[1], sys.argv[2]
text = open(src, encoding='utf-8').read()
m = re.search(r'ESP-IDF\) (\S+) Project Configuration', text)
idf = m.group(1) if m else '?'

# The flash mode the stock bootloader_dio_80m was cut in (the library's sdkconfig selects QIO, and a QIO
# bootloader sets the flash's quad-enable bit at every boot: bootloader_enable_qio_mode(); the lib-builder
# rebuilds the bootloader once per mode), and the one real change.
overrides = {
    'CONFIG_ESPTOOLPY_FLASHMODE_QIO': None,
    'CONFIG_ESPTOOLPY_FLASHMODE_QOUT': None,
    'CONFIG_ESPTOOLPY_FLASHMODE_DOUT': None,
    'CONFIG_ESPTOOLPY_FLASHMODE_DIO': 'y',
    'CONFIG_ESPTOOLPY_FLASHMODE': '"dio"',
    'CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON': 'y',
}

lines = []
seen = set()
deprecated = False
for line in text.splitlines():
    # The old names the file repeats at its end (CONFIG_FLASHMODE_QIO for CONFIG_ESPTOOLPY_FLASHMODE_QIO, ...)
    # are renamed onto the current ones as they are read, so they would undo an override above: left out.
    if line.startswith('# Deprecated options for backward compatibility'):
        deprecated = True
    elif line.startswith('# End of deprecated options'):
        deprecated = False
        continue
    if deprecated:
        continue
    sym = None
    m1 = re.match(r'(CONFIG_\w+)=', line)
    m2 = re.match(r'# (CONFIG_\w+) is not set', line)
    if m1:
        sym = m1.group(1)
    elif m2:
        sym = m2.group(1)
    else:
        continue  # comments and blank lines of the generated file
    if sym in overrides:
        seen.add(sym)
        v = overrides[sym]
        lines.append(f'# {sym} is not set' if v is None else f'{sym}={v}')
    else:
        lines.append(line)
for sym, v in overrides.items():
    if sym not in seen:
        lines.append(f'# {sym} is not set' if v is None else f'{sym}={v}')

header = f"""# The Arduino core's ESP32-C3 configuration (framework-arduinoespressif32-libs/esp32c3/sdkconfig,
# ESP-IDF {idf}), so the bootloader built here is the one the core ships but for the lines below
# marked CHANGED. Regenerate with scripts/make_bootloader_defaults.py when the core's ESP-IDF changes.
#
# CHANGED: CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON=y (the reason for this bootloader), and the flash
# mode of the image it replaces, bootloader_dio_80m. The deprecated-names block at the end is left out.
"""
open(out, 'w', encoding='utf-8', newline='\n').write(header + '\n'.join(lines) + '\n')
print('idf', idf, 'lines', len(lines), 'overrides seen', sorted(seen))
