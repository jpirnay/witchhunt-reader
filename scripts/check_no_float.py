"""Fails the firmware build when a source listed in FLOAT_FREE uses floating point.

The C3 has no FPU: float or double maths pulls in libgcc's slow soft-float routines. The TTF engine was
designed without them (spec 5.2), and the integer-maths work moves the rest of the firmware off them one
area at a time (docs/future_work/integer-math/): each finished area adds its sources to FLOAT_FREE, so the
float cannot come back unnoticed.

The linker map cannot show this. It names only the first object that pulled in each archive member, and
other code (newlib's printf and strtod, ArduinoJson, libstdc++) keeps the double routines in the image
anyway. So this script reads the listed sources' own objects: any undefined soft-float, libm or
float-parsing symbol in one of them fails the build. Code inlined from a header is checked in the objects
that include it.

PlatformIO runs it after every link (platformio.ini: extra_scripts = post:scripts/check_no_float.py).
By hand:
    python scripts/check_no_float.py <path to nm> <build dir>
"""

import os
import re
import subprocess
import sys

# libgcc's soft-float entry points: __addsf3, __muldf3, __eqsf2, __fixdfsi, __floatsisf, __extendsfdf2, ...
SOFT_FLOAT = re.compile(
    r"^__(?:(?:add|sub|mul|div|neg|eq|ne|ge|gt|le|lt|unord|cmp)(?:sf|df)\d"
    r"|(?:fix|fixuns)(?:sf|df)(?:si|di)"
    r"|float(?:un)?(?:si|di)(?:sf|df)"
    r"|extendsfdf2|truncdfsf2)$"
)
LIBM = re.compile(
    r"^(?:sqrt|cbrt|hypot|pow|exp|exp2|log|log2|log10|sin|cos|tan|asin|acos|atan|atan2"
    r"|floor|ceil|round|lround|trunc|fmod|ldexp|frexp|modf)f?$"
    r"|^(?:strtof|strtod|atof)$"
)

# Sources, from the repository root, that must build without floating point. A path ending in "/" takes the
# folder and everything below it.
FLOAT_FREE = [
    "lib/EpdFont/ttf/",
    # Image scaling (integer-maths area 1): exact ratios (lib/GfxRenderer/ImageScale.h).
    "lib/Epub/Epub/converters/",
    "lib/GfxRenderer/Bitmap.cpp",
    "lib/GfxRenderer/BitmapHelpers.cpp",
    "lib/JpegToBmpConverter/",
    "lib/PngToBmpConverter/",
    "lib/Txt/",
    "lib/Xtc/",
    "src/activities/home/FileBrowserActivity.cpp",
    "src/activities/util/BmpViewerActivity.cpp",
    "src/components/themes/lyra/Lyra3CoversTheme.cpp",
    "src/components/themes/lyra/LyraCarouselTheme.cpp",
    "src/components/themes/lyra/LyraTheme.cpp",
    # Progress and sync (integer-maths area 4): progress in billionths (lib/Epub/Epub/ProgressPpb.h).
    # KOReaderSyncClient.cpp and KOReaderAutoSync.cpp stay out: ArduinoJson's float parsing and writing is inlined
    # into them.
    "lib/Epub/Epub.cpp",
    "lib/KOReaderSync/ChapterXPathForwardMapper.cpp",
    "lib/KOReaderSync/ChapterXPathIndexer.cpp",
    "lib/KOReaderSync/ChapterXPathReverseMapper.cpp",
    "lib/KOReaderSync/ProgressComparison.cpp",
    "lib/KOReaderSync/ProgressMapper.cpp",
    "lib/LibraryIndex/LibraryOrder.cpp",
    "src/ReadingStats.cpp",
    "src/StarAnchoring.cpp",
    "src/activities/boot_sleep/SleepActivity.cpp",
    "src/activities/reader/BookSearchActivity.cpp",
    "src/activities/reader/EpubNavigationTarget.cpp",
    "src/activities/reader/EpubReaderAutoSync.cpp",
    "src/activities/reader/FinishedBookActivity.cpp",
    "src/activities/reader/KOReaderSyncActivity.cpp",
    "src/activities/reader/MdReaderActivity.cpp",
    "src/activities/reader/TxtReaderActivity.cpp",
    "src/activities/settings/ReadingStatsActivity.cpp",
    "src/activities/settings/ReadingStatsBookDetailActivity.cpp",
    "src/components/BookProgressPresentation.cpp",
    # UI screens (area 6): sizes in tenths (src/util/ByteSize.h), the weather in integer units
    # (lib/Weather/WeatherUnits.h). WeatherClient, WeatherSettingsStore, PluginEventCore and the web server read JSON:
    # their objects hold ArduinoJson's number conversions, float included.
    "lib/Serialization/FixedDecimal.cpp",
    "lib/Weather/WeatherIcons.cpp",
    "src/FlashLayoutNotice.cpp",
    "src/activities/home/BookInfoActivity.cpp",
    "src/activities/settings/FontDownloadActivity.cpp",
    "src/activities/settings/OtaUpdateActivity.cpp",
    "src/activities/settings/SystemInformationActivity.cpp",
    "src/activities/weather/",
    "src/components/themes/BaseTheme.cpp",
    "src/util/ByteSize.cpp",
    # Clock and sensors (area 5): drift in ppb and millionths, temperatures in hundredths of a degree
    # (lib/hal/ClockDrift.h), IMU samples in mg and mdps, the battery divider in thousandths.
    "lib/hal/HalClock.cpp",
    "lib/hal/HalTiltSensor.cpp",
    "lib/hal/OrientationTracker.cpp",
    "src/SerialTestApi.cpp",
    "src/activities/settings/SyncTimeActivity.cpp",
    "src/util/DecimalFormat.cpp",
    "freeink-sdk/libs/hardware/BatteryMonitor/src/BatteryMonitor.cpp",
    "freeink-sdk/libs/hardware/Imu/src/Imu.cpp",
    # Text scale (integer-maths area 2): TextScale, 2^-20ths in fixed point (lib/EpdFont/TextScale.h).
    "lib/EpdFont/EpdFont.cpp",
    "lib/EpdFont/FontDecompressor.cpp",
    "lib/EpdFont/SdCardFontManager.cpp",
    "lib/Epub/Epub/ParsedText.cpp",
    "lib/Epub/Epub/VerticalDraw.cpp",
    "lib/Epub/Epub/blocks/TextBlock.cpp",
    "lib/GfxRenderer/GfxRenderer.cpp",
    "lib/GfxRenderer/UiShapedText.cpp",
    "lib/OtShaper/",
    "src/activities/reader/DictionaryWordSelectActivity.cpp",
    "src/activities/settings/FontScalingTestActivity.cpp",
    "src/main.cpp",
]


def object_suffix(source):
    """How the object built from `source` ends: PlatformIO builds lib/<Name>/x.cpp to lib<hash>/<Name>/x.cpp.o
    and src/x.cpp to src/x.cpp.o under the build directory. An SDK library, freeink-sdk/libs/<group>/<Name>/src/x.cpp,
    builds like one of lib/: to lib<hash>/<Name>/x.cpp.o, without its group or src/ folder."""
    if source.startswith("freeink-sdk/libs/"):
        name, _, rest = source[len("freeink-sdk/libs/") :].partition("/")[2].partition("/")
        return "/" + name + "/" + (rest[len("src/") :] if rest.startswith("src/") else rest)
    return "/" + (source[len("lib/") :] if source.startswith("lib/") else source)


def matches(path, source):
    suffix = object_suffix(source)
    return suffix in path if source.endswith("/") else path.endswith(suffix + ".o")


def float_free_objects(build_dir):
    """(object, source entry) for every object built from a FLOAT_FREE source."""
    for root, _, files in os.walk(build_dir):
        for name in files:
            if not name.endswith(".o"):
                continue
            path = os.path.join(root, name).replace("\\", "/")
            for source in FLOAT_FREE:
                if matches(path, source):
                    yield path, source
                    break


def float_symbols(nm, obj, env=None):
    out = subprocess.run([nm, "-u", obj], capture_output=True, text=True, check=True, env=env).stdout
    symbols = {line.split()[-1] for line in out.splitlines() if line.strip()}
    return sorted(s for s in symbols if SOFT_FLOAT.match(s) or LIBM.match(s))


def check(nm, build_dir, env=None):
    """Prints the verdict; returns the number of offending objects."""
    found_objects = sorted(float_free_objects(build_dir))
    bad = 0
    for obj, _ in found_objects:
        found = float_symbols(nm, obj, env)
        if found:
            bad += 1
            print(f"check_no_float: {os.path.relpath(obj, build_dir)} uses floating point: {', '.join(found)}")
    # A listed source that built nothing here is compiled out of this board, or renamed: say so, as a renamed one
    # would otherwise go unchecked.
    covered = {source for _, source in found_objects}
    for source in FLOAT_FREE:
        if source not in covered:
            print(f"check_no_float: no object from {source} in this build")
    if not bad:
        print(f"check_no_float: {len(found_objects)} objects from float-free sources, no floating point")
    return bad


try:
    Import("env")  # noqa: F821 -- provided by PlatformIO when it runs this as an extra script
except NameError:
    env = None

if env is not None:

    def _after_link(target, source, env):  # noqa: ARG001 -- SCons action signature
        nm = env.WhereIs(env.subst("$CC").replace("gcc", "nm"))
        if not nm:
            print("check_no_float: cannot find the toolchain's nm")
            return 1
        return 1 if check(nm, env.subst("$BUILD_DIR"), env["ENV"]) else 0

    env.AddPostAction(  # noqa: F821
        "$BUILD_DIR/${PROGNAME}.elf", env.VerboseAction(_after_link, "Checking float-free sources for floating point")
    )
elif __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(1 if check(sys.argv[1], sys.argv[2]) else 0)
