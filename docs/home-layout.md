# Lyra Tiles' Home layout file

With **Settings → Display → Home screen → Layout** on **Custom**, Lyra Tiles draws Home from
`/.crosspoint/home.json`. The web interface's Home screen page writes it; it can also be written by
hand. A missing or unreadable file gives the **Reader** layout, and the serial log says why
(`Custom layout: ...`).

```json
{"version": 1,
 "rows": [[{"widget": "book"}],
          [{"widget": "clock"}, {"widget": "picture", "path": "/home"}],
          [{"widget": "week"}, {"widget": "shortcut", "to": "library"}]]}
```

- `version` is 1. A file of another version is not read.
- `rows`: up to six, top to bottom. A row holds one widget across the screen or two side by side;
  `book` and `also` take a row alone. A third widget in a row is left out.
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
| `picture` | A picture. A picture across the screen takes the height the other rows leave. | `"path"`: a JPEG, PNG or BMP, or a folder of them (one per start); `/home` when left out. At most two different pictures per layout. |

The rules Home lays the rows out by:

- More and Settings always close Home in a row of their own. More holds every place the layout's
  shortcuts do not open.
- A shortcut to a place with nothing behind it (Bookmarks before the first bookmark, Weather before it
  is set up) is left out; its neighbour then takes the row.
- Where the screen cannot hold every row with the book card at its smallest readable size, rows are
  left out from the bottom up; the book card's row never is.
- Held sideways, the rows with the book card take the left column and the others the right one.
- A JPEG or PNG is cut to its place and converted to black and white once, by Home's cover pass, and
  kept in `/.crosspoint/home`. A BMP is drawn as it is: make one at its place's size.

What the firmware does not know (a widget or a place a later version adds) is left out, not refused.
The geometry is `src/components/themes/lyra/HomeTilesLayout.h`, the parser
`src/components/themes/lyra/HomeLayoutFile.cpp`.
