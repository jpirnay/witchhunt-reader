---
title: Touch Gestures
---

# Touch Gestures

For readers with a touchscreen — the **Xteink X4 Pro** and the **LilyGo T5S3**. The
X3 and X4 have no digitiser and none of this applies to them.

Everything here can be changed. The zones are fixed, but what each one *does* is a
setting: **Settings → Controls → Gesture actions**. To see the zones and your own
current assignments on the device itself, open **Settings → Controls → Gesture
overview** — that page is generated from your live settings, so it is always right,
and it is the authority if this document and your device ever disagree.

---

## 1. The reading screen

### Tap and hold zones

The page is divided into three columns. The outer columns turn pages over their whole
height; the middle column is split into three. Four small corner squares sit on top of
the grid and answer only to a **hold**, never a tap — so no tap changes meaning.

```
 +-----+---------------------+-----+
 |[TL] |        Top          |[TR] |
 +-----+---------------------+-----+
 |     |                     |     |
 | Prev|       Centre        | Next|
 | page|                     | page|
 |     |                     |     |
 +-----+---------------------+-----+
 |[BL] |       Bottom        |[BR] |
 +-----+---------------------+-----+
```

| Gesture | Default |
| --- | --- |
| Tap left column | Previous page |
| Tap right column | Next page |
| Tap centre | Reader menu |
| Tap top / bottom | *(nothing — reserved for vertical gestures)* |
| Hold left / right column | Previous / Next chapter |
| Hold centre | Dictionary |
| Hold bottom | Star page |
| Hold top | *(nothing)* |
| **Hold top-left corner** | **Toggle reading light** |
| Hold other three corners | *(nothing — free for you to assign)* |

The corner squares are one eighth of the screen's shorter side, so they are square and
the same physical size whichever way you hold the device.

> The reading light is the one control the brightness swipes cannot reach. Swiping down
> only dims to a minimum, and swiping up on an unlit screen turns the light *on* — so
> turning it **off** needs its own gesture, and a corner hold is deliberate enough not
> to fire while you shift your grip.

### Swipe zones

Vertical swipes are decided by **where your finger starts**, not where it ends. Put
your finger on the control you want, then move — you never have to judge distance.

```
 +---------------------------------+
 |    top edge: reading light      |
 +-----+---------------------+-----+
 |     |                     |     |
 |Bright|   < 10 pages >     |Warmth|
 | ness |                    |      |
 |     |                     |     |
 +-----+---------------------+-----+
 |   bottom edge: reader menu      |
 +---------------------------------+
```

| Gesture | Default |
| --- | --- |
| Swipe **down** from the top edge | Reading light panel |
| Swipe **up** from the bottom edge | Reader menu |
| Swipe up / down in the **left edge** column | Light brighter / dimmer |
| Swipe up / down in the **right edge** column | Light warmer / cooler |
| Swipe **left** inside the left (back) column | Skip 10 pages back |
| Swipe **right** inside the right (forward) column | Skip 10 pages forward |
| Swipe left / right elsewhere | Next / Previous page *(in Swipe reading mode)* |

The left and right columns stop short of the top and bottom bands, so a swipe starting
in a corner cannot mean two things at once.

A vertical swipe down the **middle** of the page does nothing on purpose — it stops a
thumb resting mid-page from dimming the screen.

The ten-page skips read as one rule with the taps beside them: the left column already
means *back*, so a tap there goes back one page and a flick the same way goes back ten.

### Two fingers

Only on devices whose touch controller reports more than one finger (the GT911, in both
the X4 Pro and the T5S3).

| Gesture | Default |
| --- | --- |
| Pinch in / out | Smaller / Larger text |
| Rotate clockwise / anticlockwise | Change orientation forward / back |

---

## 2. Everywhere else

Outside a book, **taps belong to whatever is on screen** — a row, a cover, a keyboard
key, a button hint. Only swipes are interpreted as gestures, which is why the reading
light is reachable from the home screen in the dark and the corner hold is not.

| Gesture | What it does |
| --- | --- |
| Tap a row, cover, folder or button hint | Select it; tap again to activate |
| Hold a button hint | The same as holding that button |
| Swipe up / down over a list | Page the list |
| **Tap the scroll bar** above / below the thumb | Page back / forward |
| Swipe **right from the left edge** | Back |
| Swipe **down from the top edge** | Reading light panel |

The scroll-bar strip is wider than the thin bar you can see, so you do not have to hit
it precisely. Tapping the thumb itself does nothing.

> Both touch boards have a **Down** key but no **Up** key, so paging a list *backward*
> has no physical button. The scroll bar and the swipe are how you do it.

---

## 3. Per-board differences

| | X4 Pro | T5S3 |
| --- | --- | --- |
| Top edge, swipe down | Reading light | Reading light |
| Bottom edge, swipe up | Reader menu | Reader menu |
| Right edge, swipe up/down | Light warmer / cooler | *(free — single-channel light)* |
| Physical keys | Up, Down, Power | Down, Power |
| Back / Confirm | Home key: hold / tap | Home key: hold / tap |

On a touchscreen board **without** a reading light the two vertical edges swap roles:
the top edge becomes the reader menu (there being no light panel to put there) and the
bottom edge returns Home.

---

## 4. Turning it off

- **Settings → Controls → Touch Page Turn** chooses how touch turns pages: *Off*, *Tap*,
  *Swipe*, or *Inverted tap*. It governs page turns only — the reader menu and the light
  stay reachable, so switching page turns off while reading with a palm on the glass does
  not strand you.
- **Settings → Controls → Tap Centre for Menu** switches the centre tap off on its own.
- **Settings → Controls → Touch Navigation** silences touch outside the reader.
- Any single gesture can be set to **Ignore (do nothing)** in *Gesture actions* to switch
  just that one off.

---

## 5. If a gesture does nothing

1. Open **Settings → Controls → Gesture overview** and check what that zone is actually
   assigned to. A gesture showing *Built-in* does whatever the reader would have done
   anyway, which for most zones is nothing.
2. A vertical swipe must **start** in the edge column or band it belongs to. Starting
   mid-page is unassigned by design.
3. A swipe needs about 60 px of travel to register as a swipe rather than a tap.
4. Light and warmth actions disappear on boards without that hardware, and two-finger
   actions on controllers that report a single finger. The overview page only ever shows
   what your board can do.

---

## See also

- [Touch input migration](touch-input-migration-2026-08-14.md) — the developer-facing
  record of how this was built and why each default was chosen.
