# The one list of built-in font sizes.
#
# Sourced by convert-builtin-fonts.sh, which generates the faces, and by
# build-font-ids.sh, which hashes each face set into the ID the renderer keys on.
# Keeping it here means a size cannot exist in one and not the other -- the ID
# script used to be unrolled per size by hand and silently fell behind.
#
# Changing a size here is half the job: the point size these map to lives in
# CrossPointSettings::FONT_SIZE_RUNGS, and the generated headers are referenced by
# name from builtinFonts/all.h, main.cpp and CrossPointSettings.cpp.

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
SERIF_FONT_SIZES=(10 12 14 16 18)
NOTOSANS_FONT_SIZES=(10 12 14 16 18)

# Sizes with no face of their own: rendered by SCALING the master below. They still need a font
# ID, because the renderer keys everything on it -- the section cache included, which is what makes
# a scaled size cache separately from its master without any extra plumbing.
#
# The master is the largest size above, and must stay so: CrossPointSettings::BUILTIN_FACE_MAX_POINTS
# mirrors it, and main.cpp registers every rung above it as that size's faces scaled. 20 pt had
# faces of its own until they were dropped for flash (eight faces, 505,656 B of the image); 18 pt
# is the master since, at x1.11 for 20 pt up to x1.44 for 26 pt.
SYNTH_FONT_SIZES=(20 22 24 26)
SYNTH_FONT_MASTER=18

UI_FONT_STYLES=("Regular" "Bold")
UI_FONT_SIZES=(8 10 12 14)
# The UI faces that get a logical ID of their own. 8 pt and 14 pt are deliberately
# absent: Murphy uses Inter 8/10/12 for its Normal roles and 10/12/14 for Large;
# the IDs remain UI_10/UI_12 on every board. Its Noto Sans 6 pt Small face is not an ID.
UI_ID_FONT_SIZES=(10 12)

# The CJK UI faces (build_cjk_ui_fonts.py): one subset face per CJK translation per size,
# holding only the characters that translation uses, attached behind the UI faces above as
# fallback glyphs (src/CjkUiFonts.cpp). A UI face takes the largest CJK size not above its own,
# or the smallest: Murphy's 6/8 pt and all boards' 10 pt UI text draw CJK at 10 pt; 12/14 pt draw
# CJK at 12 pt. A Han or kana character fills its em box where a Latin letter does not, so CJK at
# 12 pt sits well beside Latin at 14 pt; a 14 pt CJK size would cost ~125 KB of flash for Large.
CJK_UI_FONT_SIZES=(10 12)
