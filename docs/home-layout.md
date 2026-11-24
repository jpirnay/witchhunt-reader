# Lyra Tiles' Home layout file

With **Settings → Display → Home screen → Layout** on **Custom**, Lyra Tiles draws Home from
`/.crosspoint/home.json`. The web interface's **Home Screen** page (`/home-screen`) writes it, with a
preview of Home as the reader lays it out ([the API](webserver-endpoints.md#home-screen)); it can also
be written by hand. A missing or unreadable file gives the **Reader** layout, and the serial log says why
(`Custom layout: ...`).

```json
{"version": 1,
 "rows": [[{"widget": "book"}],
          [{"widget": "clock"}, {"widget": "picture", "path": "/home"}],
          [{"widget": "week"}, {"widget": "shortcut", "to": "library"}]]}
```

- `version` is 1. A file of another version is not read.
- `rows`: up to six, top to bottom. A row holds one widget across the screen or two side by side;
  `book` and `also` take a row alone. A third widget in a row is left out, and so is a second `book`
  or a second `also`: Home has one book card and one Also reading row, the first ones in the file.
- At most 4 KB.

| `widget` | Shows | Options |
|---|---|---|
| `book` | The book read last: cover, title, author, history, progress. Takes the height the other rows leave. | `"size": "compact"`: a fixed, smaller card |
| `also` | The three books after it. Only where the book card keeps 360 px beside it, never on the Murphy M3. | |
| `shortcut` | A tile that opens one place. | `"to"`: `library`, `stats`, `bookmarks`, `transfer`, `weather`, `recent` (the Library's Recent tab) |
| `clock` | The time and date. | |
| `calendar` | The month, the days shaded by the time read on them. Opens Reading Stats. | |
| `week` | The time read this week, by day across the screen; books finished and in progress; the streak. Opens Reading Stats. | |
| `weather` | The last forecast the Weather screen fetched. Opens Weather. | |
| `picture` | A picture. A picture across the screen takes the height the other rows leave. | `"path"`: a JPEG, PNG or BMP, or a folder of them (one per start); `/home` when left out. At most two different pictures per layout. A picture from the web is a file in `/home/web` that a plugin fetches (below). |

The rules Home lays the rows out by:

- More and Settings always close Home in a row of their own. More holds every place the layout's
  shortcuts do not open.
- A shortcut to a place with nothing behind it (Bookmarks before the first bookmark, Weather while it
  is off) is left out; its neighbour then takes the row. The `weather` widget goes with Weather's
  shortcut.
- Where the screen cannot hold every row with the book card at its smallest readable size, rows are
  left out from the bottom up; the book card's row never is.
- Held sideways, the rows with the book card take the left column and the others the right one.
- A JPEG or PNG is cut to its place and converted to black and white once, by Home's cover pass, and
  kept in `/.crosspoint/home`, named by the picture's path and size and the place's size. A picture
  replaced by one of another size is converted again, and the file the old one left is removed. A BMP
  is drawn as it is: make one at its place's size.
- The widget says Loading while its picture is being converted. A press puts the conversion off, and it
  goes on once the press is served. A file not on the card (a picture from the web before its first
  fetch) or one that cannot be converted (damaged, or a PNG over 960,000 pixels) shows "Could not render
  image" and its name. One that could not be converted is tried again at the next visit, twice in all,
  then not until the next start or until the file changes.

What the firmware does not know (a widget or a place a later version adds) is left out, not refused.
The geometry is `src/components/themes/lyra/HomeTilesLayout.h`, the parser
`src/components/themes/lyra/HomeLayoutFile.cpp`.

## Pictures from the web

The Home Screen page's **From the web…** keeps a picture's source in its cell, which the reader
ignores: `"web": {"preset": "calendar", "country": "DE", "weekStart": "monday", "event": "sleep.enter"}`,
or `{"url": "https://...", "event": ...}` for an address of one's own. The file the reader shows is the
cell's `path`, in `/home/web` (`web-<n>.<jpg|png|bmp>`).

| Source (`preset`) | Fetches | Notes |
|---|---|---|
| `photo` | `https://picsum.photos/{width}/{height}?grayscale` | A new photo each fetch (Lorem Picsum, Unsplash's free photos) |
| `calendar` | `https://crosspoint-calendar.marczyk.ovh/current.bmp?w={width}&h={height}&country=…&week_start=…` | A 1-bit BMP at the widget's size, so Home draws it as it is; holidays and language by `country`. The server of Marcin Marczyk's Month Wallpaper plugin |
| `sun` | `https://sdo.gsfc.nasa.gov/assets/img/latest/latest_512_HMIIC.jpg` | NASA SDO, public domain, about every 15 minutes |
| `earth` | `https://cdn.star.nesdis.noaa.gov/GOES19/ABI/FD/GEOCOLOR/339x339.jpg` | NOAA GOES-19, public domain, every 10 minutes, 126 KB |
| `note` | `https://placehold.co/{width}x{height}/FFFFFF/000000/png?text=…` | Made once |
| `qr` | `https://api.qrserver.com/v1/create-qr-code/?size=…&margin=…&data=…` | Made once; a square with a margin that survives Home cutting it to the box |

A source that changes gets a plugin (`home-picture-<n>` in `/.crosspoint/plugins`): a `device.json`
whose one event handler downloads the address to the file
([plugin events](sd-plugins.md#events-the-reader-sends-a-plugins-requests)), and a `manifest.json` that
may reach the address's host and its subdomains and keeps the address and the preset under
`homeScreen`. `{width}` and `{height}` are filled with the widget's size when the page saves, and the
page fetches the picture once through `/api/fetch` as it saves. The reader fills neither, so an address
with them gets no plugin while its row does not fit (the page removes the one it had and says so). A note and a QR code never change: the
browser fetches them once from their service (which allows it) and uploads the file, and no plugin
fetches them again. The page replaces the plugins when the layout changes and removes those of
pictures it no longer shows. A plugin switched off on the Plugins page is switched off again after the
replacement, and one whose host changed is removed first, so the hosts approved for the old address
(kept by the plugin's name) do not carry over. The firmware has no code of its own for any of this: what such a picture
shows is the business of the service behind the address ([SCOPE.md](../SCOPE.md)). Every source was
checked on 2026-11-24; a service may change or stop.
