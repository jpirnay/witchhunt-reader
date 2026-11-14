#!/usr/bin/env python3
"""Fixed webcam for the gray lab: locked capture, panel mapping and level measurement.

Levels are reported per region against the same region in a clean white and a clean black shot, so
uneven light and lens falloff cancel. 0 % is black, 100 % is white. The camera's gamma stays on, so
the scale is close to perceived lightness rather than to linear reflectance.

Only needs GStreamer and Pillow. Controls go straight to V4L2, as v4l2-ctl is not installed.
"""
import argparse
import fcntl
import json
import os
import statistics
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

def _camera_name(device="/dev/video0"):
    try:
        return Path(f"/sys/class/video4linux/{Path(device).name}/name").read_text().strip()
    except OSError:
        return ""


# The XIFT AC310 (on the stand since 2026-11-12) gives raw YUY2 only up to 1280x720, so it is read as
# MJPEG at 4K; region means average the compression away. The earlier webcam gave YUY2 at 1080p.
MJPEG = "AC310" in _camera_name()
CAM_W, CAM_H = (3840, 2160) if MJPEG else (1920, 1080)
PANEL_W, PANEL_H = {"x3": (792, 528), "x4": (800, 480), "picco": (800, 480)}.get(os.environ.get("LAB_BOARD", "murphy"), (240, 416))
SCALE = 2  # rectified shots are this many pixels per panel pixel

VIDIOC_S_CTRL = 0xC008561C
VIDIOC_G_CTRL = 0xC008561B
CTRL_WB_AUTO = 0x0098090C
CTRL_WB_TEMP = 0x0098091A
CTRL_GAIN = 0x00980913
CTRL_POWER_LINE = 0x00980918
CTRL_SHARPNESS = 0x0098091B
CTRL_BACKLIGHT = 0x0098091C
CTRL_AUTO_EXPOSURE = 0x009A0901
CTRL_EXPOSURE = 0x009A0902
CTRL_DYNAMIC_RATE = 0x009A0903
AUTO_EXPOSURE_MANUAL = 1
AUTO_EXPOSURE_APERTURE = 3
POWER_LINE_50HZ = 1

EXPOSURE = int(os.environ.get("MURPHY_EXPOSURE", "1000"))
STATE = Path(os.path.expanduser("~/.local/state/murphy-gray-lab"))
# Each board sits on the stand in its own place, so it keeps its own calibration and shots.
_SUFFIX = "" if os.environ.get("LAB_BOARD", "murphy") == "murphy" else "-" + os.environ["LAB_BOARD"]
SHOTS = STATE / ("shots" + _SUFFIX)
CAMERA_FILE = STATE / ("camera" + _SUFFIX + ".json")


def set_control(fd, control, value, required=True):
    try:
        fcntl.ioctl(fd, VIDIOC_S_CTRL, bytearray(struct.pack("<Ii", control, value)))
    except OSError:
        if required:
            raise  # a camera without this control (the AC310 has no auto exposure, backlight or flicker setting)


def get_control(fd, control):
    buffer = bytearray(struct.pack("<Ii", control, 0))
    fcntl.ioctl(fd, VIDIOC_G_CTRL, buffer)
    return struct.unpack_from("<i", buffer, 4)[0]


def lock(device, exposure):
    """Everything automatic off. The exposure unit is 100 us: multiples of 100 sit on the 100 Hz
    flicker of mains lighting. Order matters, because manual values are refused while auto is on. After
    the camera powers up it ignores the exposure value until the mode has been switched away from manual
    and back (seen after a reboot, 2026-11-08), so the mode is toggled first."""
    fd = os.open(device, os.O_RDWR)
    try:
        set_control(fd, CTRL_AUTO_EXPOSURE, AUTO_EXPOSURE_APERTURE, required=False)
        set_control(fd, CTRL_AUTO_EXPOSURE, AUTO_EXPOSURE_MANUAL)
        set_control(fd, CTRL_DYNAMIC_RATE, 0, required=False)
        set_control(fd, CTRL_EXPOSURE, exposure)
        set_control(fd, CTRL_WB_AUTO, 0)
        set_control(fd, CTRL_WB_TEMP, 4 if MJPEG else 4600)  # the AC310 takes a preset, 1-5
        set_control(fd, CTRL_GAIN, 0)
        set_control(fd, CTRL_SHARPNESS, 0)
        set_control(fd, CTRL_BACKLIGHT, 0, required=False)
        set_control(fd, CTRL_POWER_LINE, POWER_LINE_50HZ, required=False)
        return get_control(fd, CTRL_EXPOSURE)
    finally:
        os.close(fd)


def grab(device, frames=6, warmup=4):
    """Luma of `frames` frames, after dropping the first few while the sensor settles."""
    if MJPEG:
        with tempfile.TemporaryDirectory() as tmp:
            subprocess.run(
                ["gst-launch-1.0", "-q", "v4l2src", f"device={device}", f"num-buffers={frames + warmup}", "!",
                 f"image/jpeg,width={CAM_W},height={CAM_H}", "!", "multifilesink", f"location={tmp}/f%03d.jpg"],
                check=True, timeout=90, stdout=subprocess.DEVNULL,
            )  # fmt: skip
            names = sorted(Path(tmp).glob("f*.jpg"))
            return [Image.open(name).convert("L") for name in names[warmup:]]
    with tempfile.TemporaryDirectory() as tmp:
        path = Path(tmp) / "raw.yuy2"
        subprocess.run(
            ["gst-launch-1.0", "-q", "v4l2src", f"device={device}", f"num-buffers={frames + warmup}", "!",
             f"video/x-raw,format=YUY2,width={CAM_W},height={CAM_H}", "!", "filesink", f"location={path}"],
            check=True, timeout=90, stdout=subprocess.DEVNULL,
        )  # fmt: skip
        data = path.read_bytes()
    size = CAM_W * CAM_H * 2
    count = len(data) // size
    return [Image.frombytes("L", (CAM_W, CAM_H), data[i * size : (i + 1) * size][0::2]) for i in range(warmup, count)]


def average(images):
    """A running blend. Rounding is well under what region means can see."""
    result = images[0]
    for index, image in enumerate(images[1:], start=2):
        result = Image.blend(result, image, 1 / index)
    return result


def shot(device, name, exposure):
    lock(device, exposure)
    SHOTS.mkdir(parents=True, exist_ok=True)
    image = average(grab(device))
    path = SHOTS / f"{name}.png"
    image.save(path)
    return image, path


def solve(matrix, vector):
    """Gaussian elimination with partial pivoting, for the 8x8 perspective system."""
    size = len(vector)
    rows = [row[:] + [value] for row, value in zip(matrix, vector)]
    for column in range(size):
        pivot = max(range(column, size), key=lambda r: abs(rows[r][column]))
        rows[column], rows[pivot] = rows[pivot], rows[column]
        for r in range(size):
            if r != column:
                factor = rows[r][column] / rows[column][column]
                rows[r] = [a - factor * b for a, b in zip(rows[r], rows[column])]
    return [rows[i][size] / rows[i][i] for i in range(size)]


def perspective_coefficients(panel_points, camera_points):
    """Pillow's PERSPECTIVE maps output (panel) coordinates back to input (camera) coordinates."""
    matrix, vector = [], []
    for (x, y), (u, v) in zip(panel_points, camera_points):
        matrix.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        matrix.append([0, 0, 0, x, y, 1, -v * x, -v * y])
        vector += [u, v]
    return solve(matrix, vector)


def blobs(mask, step=4, minimum=6):
    """Centres of bright regions in a binary image, on a coarse grid. Returns (x, y, size)."""
    small = mask.resize((mask.width // step, mask.height // step), Image.Resampling.BOX)
    width, height = small.size
    pixels = bytearray(small.tobytes())
    found = []
    for start in range(width * height):
        if pixels[start] < 128:
            continue
        stack, sum_x, sum_y, count = [start], 0, 0, 0
        pixels[start] = 0
        while stack:
            index = stack.pop()
            y, x = divmod(index, width)
            sum_x, sum_y, count = sum_x + x, sum_y + y, count + 1
            for neighbour, ok in ((index - 1, x > 0), (index + 1, x < width - 1),
                                  (index - width, y > 0), (index + width, y < height - 1)):  # fmt: skip
                if ok and pixels[neighbour] >= 128:
                    pixels[neighbour] = 0
                    stack.append(neighbour)
        if count >= minimum:
            found.append(((sum_x / count + 0.5) * step, (sum_y / count + 0.5) * step, count))
    return found


# Marks drawn by `lab.py calib`, in panel pixels: (x, y) of each 20 px square's corner.
CALIB_SIZE = 20 if PANEL_W < 400 else 40
CALIB_MARKS = ((12, 12), (PANEL_W - 12 - CALIB_SIZE, 12), (12, PANEL_H - 12 - CALIB_SIZE),
               (PANEL_W - 12 - CALIB_SIZE, PANEL_H - 12 - CALIB_SIZE))
CALIB_ORIENT = (12 + CALIB_SIZE + 16, 12)  # an extra square beside the first, so a turned image cannot pass


def centre(mark):
    return (mark[0] + CALIB_SIZE / 2, mark[1] + CALIB_SIZE / 2)


def calibrate(white, marks):
    """Find the panel in the camera from a frame of marks and a clean white shot."""
    difference = ImageChops.subtract(white, marks)
    found = blobs(difference.point(lambda value: 255 if value > 40 else 0))
    found.sort(key=lambda blob: -blob[2])
    if len(found) < 5:
        raise SystemExit(f"expected 5 marks, found {len(found)}: {found}")
    found = found[:5]
    points = [(x, y) for x, y, _ in found]
    # The orientation mark is the one closest to another mark, and that other is the first corner.
    pairs = [(((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2), i, j)
             for i, a in enumerate(points) for j, b in enumerate(points) if i < j]  # fmt: skip
    _, i, j = min(pairs)
    others = [p for k, p in enumerate(points) if k not in (i, j)]
    far = max(others, key=lambda p: min((p[0] - points[i][0]) ** 2 + (p[1] - points[i][1]) ** 2,
                                        (p[0] - points[j][0]) ** 2 + (p[1] - points[j][1]) ** 2))  # fmt: skip
    # Of the pair, the corner mark is further from the diagonal corner than the orientation mark is.
    first, orient = (points[i], points[j])
    if (first[0] - far[0]) ** 2 + (first[1] - far[1]) ** 2 < (orient[0] - far[0]) ** 2 + (orient[1] - far[1]) ** 2:
        first, orient = orient, first
    rest = [p for p in others if p != far]
    # Panel top right lies on the same side as the orientation mark, seen from the first corner.
    along = (orient[0] - first[0], orient[1] - first[1])
    rest.sort(key=lambda p: -((p[0] - first[0]) * along[0] + (p[1] - first[1]) * along[1]))
    top_right, bottom_left = rest
    camera = [first, top_right, bottom_left, far]
    panel = [centre(mark) for mark in CALIB_MARKS]
    return {"coefficients": perspective_coefficients(panel, camera), "marks": camera}


def load_camera():
    return json.loads(CAMERA_FILE.read_text())


def rectify(image, camera):
    """The camera image in panel coordinates, SCALE pixels to a panel pixel."""
    a, b, c, d, e, f, g, h = camera["coefficients"]
    k = 1 / SCALE
    coefficients = (a * k, b * k, c, d * k, e * k, f, g * k, h * k)
    return image.transform((PANEL_W * SCALE, PANEL_H * SCALE), Image.Transform.PERSPECTIVE, coefficients,
                           Image.Resampling.BICUBIC)  # fmt: skip


def region_mean(image, box):
    x, y, w, h = (value * SCALE for value in box)
    return ImageStat.Stat(image.crop((x, y, x + w, y + h))).mean[0]


def level(image, white, black, box):
    """Where a region sits between its own black and white, in percent."""
    low, high = region_mean(black, box), region_mean(white, box)
    return 100 * (region_mean(image, box) - low) / (high - low)


def load_rectified(name, camera):
    return rectify(Image.open(SHOTS / f"{name}.png").convert("L"), camera)


def inset(box, margin):
    x, y, w, h = box
    return (x + margin, y + margin, w - 2 * margin, h - 2 * margin)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--device", default="/dev/video0")
    parser.add_argument("--exposure", type=int, default=EXPOSURE)
    steps = parser.add_subparsers(dest="step", required=True)
    steps.add_parser("sweep", help="panel brightness at several exposures")
    shoot = steps.add_parser("shot", help="capture and save a shot")
    shoot.add_argument("name")
    steps.add_parser("calibrate", help="find the panel, from shots baseline-white and marks")
    still = steps.add_parser("still", help="has the scene moved since a shot? Exits 1 if it has")
    still.add_argument("reference")
    still.add_argument("name")
    fine = steps.add_parser("fine", help="mean level of each pattern in a `lab.py fine` shot")
    fine.add_argument("name")
    patches = steps.add_parser("patches", help="each `lab.py t4` patch against its clean strip")
    patches.add_argument("name")
    strip = steps.add_parser("strip", help="the patch against the strip beside it, in a `lab.py t6` shot")
    strip.add_argument("name")
    ghost = steps.add_parser("ghost", help="how far a blank-page shot is from even")
    ghost.add_argument("name")
    ghost.add_argument("--against", default="baseline-white", help="the clean shot to compare with")
    bands = steps.add_parser("bands", help="level of each horizontal band in a shot")
    bands.add_argument("name")
    bands.add_argument("--count", type=int, default=4)
    bands.add_argument("--width", type=int, default=PANEL_W, help="only the left part of the panel")
    bands.add_argument("--margin", type=int, default=12, help="panel pixels left out at each band edge")
    args = parser.parse_args()

    if args.step == "sweep":
        for exposure in (100, 200, 300, 400, 500, 600, 800, 1000):
            got = lock(args.device, exposure)
            frame = average(grab(args.device, frames=2, warmup=4))
            box = (CAM_W // 2 - 150, CAM_H // 2 - 300, CAM_W // 2 + 100, CAM_H // 2 + 300)
            stat = ImageStat.Stat(frame.crop(box))
            print(f"exposure {exposure} (reads {got}): panel mean {stat.mean[0]:.1f}, max {stat.extrema[0][1]}")
    elif args.step == "shot":
        image, path = shot(args.device, args.name, args.exposure)
        print(f"{path}: mean {ImageStat.Stat(image).mean[0]:.1f}")
    elif args.step == "calibrate":
        white = Image.open(SHOTS / "baseline-white.png").convert("L")
        marks = Image.open(SHOTS / "marks.png").convert("L")
        camera = calibrate(white, marks)
        CAMERA_FILE.write_text(json.dumps(camera))
        print("marks at", [tuple(round(v) for v in point) for point in camera["marks"]])
    elif args.step == "fine":
        camera = load_camera()
        white, black = load_rectified("baseline-white", camera), load_rectified("baseline-black", camera)
        image = load_rectified(args.name, camera)
        for label, top in (("on white", 0), ("on black", PANEL_H // 2)):
            for index, scale in enumerate((1, 2, 4)):
                row = top + index * 64 + 8
                checker = level(image, white, black, inset((8, row, 104, 48), 8))
                lines = level(image, white, black, inset((128, row, 104, 48), 8))
                print(f"{label}, {scale} px: checker {checker:5.1f}  lines {lines:5.1f}")
    elif args.step == "patches":
        camera = load_camera()
        white, black = load_rectified("baseline-white", camera), load_rectified("baseline-black", camera)
        image = load_rectified(args.name, camera)
        names = ("light to white", "light to black", "dark to white", "dark to black")
        for row, name in enumerate(names):
            patch = level(image, white, black, inset((0, row * 104, 200, 104), 14))
            strip = level(image, white, black, inset((200, row * 104, 40, 104), 12))
            print(f"{name:15} patch {patch:6.1f}  clean strip {strip:6.1f}  ghost {patch - strip:+6.1f}")
    elif args.step == "still":
        # Everything outside the panel should be identical. A bump shows up along every edge in the
        # frame, and it silently turns each later number into a reading of the wrong region.
        one = Image.open(SHOTS / f"{args.reference}.png").convert("L")
        two = Image.open(SHOTS / f"{args.name}.png").convert("L")
        k = CAM_W // 1920
        frame = [(0, 0, CAM_W, 90 * k), (0, 90 * k, 560 * k, CAM_H), (1360 * k, 90 * k, CAM_W, CAM_H),
                 (560 * k, 950 * k, 1360 * k, CAM_H)]  # fmt: skip
        moved = max(ImageStat.Stat(ImageChops.difference(one.crop(b), two.crop(b))).mean[0] for b in frame)
        print(f"scene difference outside the panel: {moved:.2f} ({'moved' if moved > 1.5 else 'still'})")
        return 1 if moved > 1.5 else 0
    elif args.step == "strip":
        camera = load_camera()
        white, black = load_rectified("baseline-white", camera), load_rectified("baseline-black", camera)
        image = load_rectified(args.name, camera)
        rows = range(0, PANEL_H, 104)
        patch = [level(image, white, black, inset((120, y, 72, 104), 12)) for y in rows]
        beside = [level(image, white, black, inset((204, y, 32, 104), 8)) for y in rows]
        gaps = [a - b for a, b in zip(patch, beside)]
        print(f"{args.name}: patch {statistics.mean(patch):.1f}  strip {statistics.mean(beside):.1f}  "
              f"ghost {statistics.mean(gaps):+.1f}  rows {[round(g, 1) for g in gaps]}")
    elif args.step == "ghost":
        # Text rows against the gaps between them: blur cannot hide a whole line of old text.
        camera = load_camera()
        image, clean = load_rectified(args.name, camera), load_rectified(args.against, camera)
        black = load_rectified("baseline-black", camera)
        rows = [level(image, clean, black, (20, y, 200, 2)) for y in range(16, 400, 2)]
        # Lines of text are about 20 panel rows apart: take out anything slower, such as room light.
        window = 24
        fine = [v - statistics.mean(rows[max(0, i - window) : i + window + 1]) for i, v in enumerate(rows)]
        print(f"{args.name}: mean {statistics.mean(rows):.1f}, line-scale stddev {statistics.pstdev(fine):.2f}, "
              f"peak {max(map(abs, fine)):.1f}")
    elif args.step == "bands":
        camera = load_camera()
        white, black = load_rectified("baseline-white", camera), load_rectified("baseline-black", camera)
        image = load_rectified(args.name, camera)
        height = PANEL_H // args.count
        boxes = [inset((0, index * height, args.width, height), args.margin) for index in range(args.count)]
        print(args.name, [round(level(image, white, black, box), 1) for box in boxes])


if __name__ == "__main__":
    sys.exit(main())
