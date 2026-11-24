# Web Interface Guide

Witch Reader has a web interface: while **File Transfer** runs on the reader, any computer, phone or
tablet on the same network can open its pages in a browser. From there you can:

- upload books (and whole folders of them), and organise, rename and delete files on the SD card;
- prepare EPUBs for the reader: shrink their pictures and split overlong chapters;
- correct a book's title, author, series and cover;
- change every setting of the reader, and manage its saved Wi-Fi networks and OPDS catalogs;
- arrange the Home screen: its rows, shortcuts, clock, calendar, weather and pictures, from the card or
  from the web;
- install fonts from the catalog, upload or convert your own, and install language packs;
- install dictionaries for looking words up;
- see your reading statistics;
- add plugins, and download a report for a bug report.

All of this works without an internet connection, except the few things that fetch something from
the internet (noted below).

> **New in 2.40: setup on the first visit.** The web interface's pages are no longer built into the
> firmware; they live on the SD card. The first time you open the reader's address after installing
> 2.40, and after any later update that changes the pages, a setup page puts them there. See
> [The first visit](#3-the-first-visit-setup).

**Contents**

1. [Start File Transfer on the reader](#1-start-file-transfer-on-the-reader)
2. [Open the web interface](#2-open-the-web-interface)
3. [The first visit: setup](#3-the-first-visit-setup)
4. [Finding your way around](#4-finding-your-way-around)
5. [File Manager](#5-file-manager)
6. [Settings](#6-settings)
7. [Home Screen](#7-home-screen)
8. [Font Manager](#8-font-manager)
9. [Dictionaries](#9-dictionaries)
10. [Reading Stats](#10-reading-stats)
11. [Plugins](#11-plugins)
12. [System Info and Diagnostics](#12-system-info-and-diagnostics)
13. [Other ways to reach the card](#13-other-ways-to-reach-the-card)
14. [Leaving File Transfer](#14-leaving-file-transfer)
15. [Troubleshooting](#15-troubleshooting)
16. [Security](#16-security)

---

## 1. Start File Transfer on the reader

Open **File Transfer** from the Home screen (if your Home screen does not show it, it is under
**More**). The screen offers:

| Entry | What it does |
| ----- | ------------ |
| **Join a Network** | The reader joins your Wi-Fi. Your computer or phone must be on the same network. Use this whenever you can: the reader can then download things it needs, such as the web interface itself and fonts. |
| **Calibre Wireless** | Receive books from Calibre's "Connect to device" (see the [User Guide](../USER_GUIDE.md#361-calibre-wireless-transfers)). |
| **OPDS Browser** | Browse and download from an online catalog on the reader itself. |
| **Create Hotspot** | The reader makes its own Wi-Fi network, **CrossPoint-Reader** (open, no password), for when there is no Wi-Fi around. Join it from your phone or computer. The reader has no internet connection in this mode. |
| **USB Drive** / **USB Transfer** | Copy files over the cable instead (depends on the model; see the [User Guide](../USER_GUIDE.md#36-file-transfer-screen)). |

### Joining a network

The reader first tries the networks it has saved and connects by itself when one is in range.
Otherwise it lists the networks it can see: `*` marks a network with a password, `+` one whose
password is saved. Pick yours, enter the password on the on-screen keyboard, and choose whether to
save it. A saved password is used automatically next time; it is stored scrambled on the SD card and
keeps working if the card moves to another reader.

If a saved password stops working (it was changed on the router, say), the keyboard opens again,
headed **Saved password may be wrong**: type the new one and save it over the old.

Some public networks (hotels, trains) need you to log in through a browser first. The reader then
shows an address to open on another device; once you have logged in there, press **OK**.

### When it is running

The reader shows the address of its web interface, and a QR code for it:

- **On your network:** `http://` followed by the reader's IP address, for example
  `http://192.168.1.42/`, and below it `http://crosspoint.local/`, which works on most computers and
  on iPhones and iPads.
- **In hotspot mode:** first a QR code that joins your phone to the **CrossPoint-Reader** network,
  then one for the address, `http://crosspoint.local/` or `http://192.168.4.1/`. Some
  phones and computers open the home page by themselves after joining.

A bar shows the Wi-Fi signal; a stronger signal makes uploads faster.

## 2. Open the web interface

1. Make sure your computer, phone or tablet is on the **same network** as the reader (in hotspot
   mode: joined to **CrossPoint-Reader**).
2. Open a browser and type the address the reader shows, or scan the QR code with your phone's
   camera. Type `http://`, not `https://`: the reader does not serve encrypted pages.

Any current browser works: Chrome, Edge, Firefox or Safari, on a computer or a phone. A computer is
more comfortable for the File Manager and the optimizer.

## 3. The first visit: setup

The web interface's pages are kept in a small file on the SD card, which must match the firmware.
The first time you open the reader's address, and again after any firmware update that changed the
pages, you see the setup page instead:

- **On your Wi-Fi network** there is nothing to do. The page says *Downloading the web interface…*,
  the reader fetches the matching file from the release on GitHub, and after a few seconds the
  Welcome page opens.
- **In hotspot mode** the reader cannot download anything. The page names the file it needs
  (`web-` followed by a code, ending in `.bin`) and links to the release page where it is attached:

  ![The setup page in hotspot mode](images/web/setup-hotspot.png)

  Download that file beforehand, on a device with internet, then either choose it under **Install
  from a file** and click **Install**, or copy it to the top folder of the SD card and reload the
  page. Only the exact file for your firmware is accepted.

The setup page can also **upload books** by itself (they go to the top folder of the card), so you
can always add books, even before the web interface is installed. It stays available at
`http://<reader address>/setup`.

## 4. Finding your way around

![The Welcome page](images/web/welcome.png)

The Welcome page has a button for each part of the web interface. Every other page shows the same
entries along its top: **File Manager**, **Settings**, **Home Screen**, **Font Manager**, **Dictionaries**, **Reading
Stats**, **Plugins**, **System Info** and **Diagnostics**. On a phone this bar scrolls sideways.

The round button at the right end of the bar switches between automatic colours (following your
computer's or phone's light or dark setting), light and dark. Your browser remembers the choice.

![The File Manager in dark colours](images/web/files-dark.png)

## 5. File Manager

![The File Manager](images/web/files.png)

The File Manager shows one folder of the SD card at a time. The line under the title shows where you
are; click a part of it to go back up, or the house for the top of the card. Click a folder to open
it. Click a column heading to sort by name, type or size, and click it again to reverse the order.

Each book has a label with its format (EPUB, XTC, TXT, MD). At the right of each row:

| Button | Does |
| ------ | ---- |
| ℹ️ **Details** (books only) | Edit the book's title, author, series and cover (below) |
| 📂 **Move** | Move it to another folder |
| ✏️ **Rename** | Rename it |
| 🗑️ **Delete** | Delete it, after asking |

Tick the boxes in front of several items (or the box in the header for all of them) to **Move**,
**Delete** or **Optimize** them together with the buttons at the top.

Moving or renaming a book here keeps its reading position, bookmarks and reading stats. A folder can
only be deleted when it is empty, and deleting cannot be undone.

Files and folders whose names start with a dot, such as the reader's own `.crosspoint` folder, are
hidden unless **Show Hidden Files** is on in the reader's settings (**System**). Even then, the files
holding your saved Wi-Fi, OPDS and KOReader passwords can't be downloaded or overwritten here. To change
a saved password, use [Settings](#6-settings).

### Uploading

Click **Upload**, choose one or more files, and click **Upload** in the dialog: the files go into the
folder you are looking at. You can also **drag files or whole folders** from your computer onto the
page; a dropped folder is created on the card with everything in it. A bar shows the progress; the
list refreshes when the last file has arrived. If a file fails, a banner lists it with a **Retry**
button.

When a file of the same name is already in the folder, you are asked what to do:

![Replace, Keep both or Skip](images/web/upload-clash.png)

- **Replace** overwrites the file on the card;
- **Keep both** saves the upload as "name (2)";
- **Skip** leaves this file out.

Tick **Do the same for the other files that already exist** to answer once for the whole upload.

### Optimizing EPUBs

Many EPUBs carry pictures far larger than the reader's screen, and some have chapters so long that
the reader is slow to open them and can run short of memory laying them out. Tick **Optimize EPUB** in
the upload dialog and your browser prepares each EPUB before it is sent. Click **Advanced Mode** to see
the options:

![The optimizer's options](images/web/upload-optimize.png)

- **Pictures** are converted to grayscale JPEGs at the size of the reader's screen (**Target Device**;
  **Auto** uses the reader you are connected to), at the **JPEG Quality** you choose. Very tall
  pictures are turned or split into parts; the panel on the right shows the book's pictures, and you
  can set each one to Normal, split across or down, or rotate it. **Keep Cover in Color** leaves the
  cover in colour and only resizes it.
- **Split Long Chapters** (on by default) cuts a chapter of more than 8,000 words or 32 KB into
  several parts, at a heading or scene break where it can, and fixes every link and table-of-contents
  entry to match.
- Empty chapter stubs, comments and invisible characters are removed. Books protected with DRM are
  refused rather than broken.
- **Remember These Options** keeps your choices in this browser for next time. **Auto-download Log**
  saves a report of what was changed.

All the work happens in your browser, so a large book takes a moment on a slow phone.

> **KOReader sync and optimized books.** Optimizing changes the book file. KOReader sync can match a
> book by its contents (KOReader's default) or by its file name (Witch Reader's default). Matched by
> contents, an optimized copy no longer counts as the same book as the original on another device.
> Matched by file name it does, but if chapters were split or stubs removed, the chapters are numbered
> differently, so a synced position can land in the wrong chapter. If you sync with KOReader, read the
> **same file** on every device (copy the optimized book to your other devices too), or turn off
> **Split Long Chapters**.

#### Books already on the card

Tick the EPUBs in the list and click **Optimize** at the top:

![Optimizing books on the card](images/web/optimize-selected.png)

Each book is downloaded to your browser, optimized with the options from the upload dialog, and put
back in its place. The original is kept beside it as `name.epub.bak` (if there already is one, that
older copy stays). When no chapter had to be split or removed, the book keeps its reading position
and bookmarks; otherwise it starts over, like a newly uploaded book. Reading stats always stay.

### Book details

Click ℹ️ beside a book to correct what the reader shows for it:

![The Details dialog](images/web/details.png)

- **Title**, **Author**, **Sort author as** (how the author is filed in the Library's **Authors** tab,
  for example "Le Guin, Ursula K."; left empty, the last word of the name is used), **Language**,
  **Series** and its number, and **Description**.
- **Cover:** choose a picture file, or click the box and paste a picture copied in your browser
  (Ctrl+V, or Cmd+V on a Mac), or search **Open Library**, **Goodreads** or **Google Books** and
  pick one of the covers found. Searching needs the reader to be online. **Remove cover** goes back to
  the cover inside the book.
- For an EPUB, **Convert its fonts** opens the Font Manager with the fonts the book embeds (see
  [Font Manager](#8-font-manager)).

The book file itself is never changed: the details are saved in a `.opf` file beside the book (and
the cover in a picture file beside it), which the reader prefers to the book's own. Clear a field to
go back to the book's own value. This works for EPUB, XTC, TXT and Markdown books.

### Creating folders

Click **New folder**, type a name and confirm. Names cannot contain `" * : < > ? / \ |`.

### Plugin cards

Installed plugins can add cards below the file list, such as **Find Duplicates** and **Organize by
Author**. See [Plugins](#11-plugins).

## 6. Settings

![The Settings page with two unsaved changes](images/web/settings-changed.png)

The Settings page has the same settings as the reader's Settings screens, with a line explaining
each. Pick a category on the left (on a phone: along the top): **Display**, **Reader**, **Controls**
and **System**. Settings that only make sense on some models (the reading light, the touch screen)
appear only for a reader that has them.

Changes are not sent straight away. A bar at the bottom counts them, and changed settings and their
categories are marked with a dot; click **Save changes** to send them to the reader, or **Discard**.
The font size lists show the sizes the chosen font offers; after you change the font and save, the
list follows.

The **search box** finds a setting in any category:

![Searching the settings](images/web/settings-search.png)

### Wi-Fi networks and OPDS servers

Under **Connections**, **Wi-Fi networks** lists the networks the reader joins by itself, and **OPDS
servers** the catalogs it knows. You can add one, change its name or address, or forget it; each
entry is saved on its own.

![Saved Wi-Fi networks](images/web/settings-wifi.png)

A saved password is never shown, so you can't read it or correct a typo in it: type the whole password
again to replace it, leave the field empty to keep the stored one, or tick **Clear the stored
password**.

**Plugins** under Connections holds the cards of plugins that add themselves to the Settings page.

## 7. Home Screen

The **Home Screen** page arranges the Home screen of the **Lyra Tiles** theme: which rows it has and
what each row shows. What you build here is Lyra Tiles' **Custom** layout. Saving it also switches the
reader to Lyra Tiles with that layout, so you do not have to change anything on the reader. The three
ready-made layouts (Reader, Dashboard, Gallery) need no web page: choose them under **Settings →
Display → Home screen → Layout** on the reader (see [Home Screen](../USER_GUIDE.md#31-home-screen) in
the User Guide).

A line at the top says what the reader shows now: another theme or another layout (saving switches it
to this one), or that its Home already shows this layout.

### Building a layout

1. **Start from** picks the rows to begin with: **This reader's layout** (the Custom layout saved on
   the reader; without one, the ready-made layout the reader shows now), or the **Reader**,
   **Dashboard** or **Gallery** layout.
2. Change the rows. A row holds one widget across the screen, or two side by side (**Add a widget
   beside it**; **✕** next to the second one removes it). **Add a row** adds one at the bottom, the
   arrows move a row up or down, and **✕** on the row removes it. A layout has up to six rows.
3. Check the **Preview**: it shows Home as this reader lays it out, at its own screen size and held as it
   is now. Click a part of the preview to find its row in the list. A row the screen has no room for
   is marked **does not fit on this reader**, and the list under the preview says why a widget is left
   out.
4. Click **Save and use on the reader**. The line beside the button says what was saved and fetched.
   The reader shows the new Home when you leave File Transfer.

The widgets:

| Widget | Shows | Choices |
|---|---|---|
| **Book card** | The book you read last: its cover, title, author, reading history and progress. Choose it on the reader to read on. Takes a row alone. | **Large** takes the height the other rows leave; **Compact** is a smaller card of a fixed height |
| **Also reading** | The three books you read before it. Takes a row alone, and is shown only where the book card keeps enough room beside it (never on the Murphy M3) | |
| **Shortcut** | A tile that opens one place | **Library**, **Recent books**, **Reading Stats**, **Bookmarks**, **File Transfer** or **Weather**. Bookmarks is left out until you make a bookmark, Weather while Weather is off in the reader's settings |
| **Clock** | The time and the date, once the reader knows the time | |
| **Calendar** | This month, each day shaded by the time you read on it. Opens Reading Stats | |
| **This week** | The time you read on each day of this week, the books you finished and are reading, and your streak. Opens Reading Stats | The week starts on the reader's **First Day of Week** (Settings → System → Clock Settings) |
| **Weather** | The last forecast the reader's Weather screen fetched. Opens Weather | |
| **Picture** | A picture from the card or from the web (below) | |

**More** and **Settings** always close the screen in a row of their own, and **More** holds every place
your shortcuts leave out, so nothing is ever out of reach. Where the screen cannot hold every row with
a readable book card, the reader leaves rows out from the bottom; the preview shows which. Held
sideways, the rows with the book card take the left half of the screen and the others the right half.

### Pictures

A **Picture** widget shows one of these:

- **A picture from /home, a new one each start** (the default): a different picture from the card's
  `/home` folder each time the reader starts.
- One picture already in `/home`, chosen by name.
- **Another file or folder…**: a JPEG, PNG or BMP anywhere on the card, or a folder of them.
- **Upload a picture…**: a JPEG or PNG from your computer or phone. The page makes it smaller first (at
  most 1200 pixels on its longer side) and puts it into `/home`, so the reader converts it quickly.
- **From the web…**: a picture a service draws, fetched again and again (below).

The reader cuts a JPEG or PNG to the widget's place and converts it to black and white once, the first
time Home shows it (it says **Loading** meanwhile). A BMP is drawn as it is, so make one at the size of
its place. A layout shows at most two different pictures.

### Pictures from the web

**From the web…** offers these sources:

| Source | What it shows | Fetched |
|---|---|---|
| **A random photo (Lorem Picsum)** | A different photo each time, from the free photos on Unsplash | Again and again |
| **Month calendar** | This month and its public holidays, drawn for e-ink. Type the **country** in two letters (DE, US, PL…); it sets the holidays and the language. Choose whether weeks start on Monday or Sunday | Again and again |
| **The Sun now (NASA SDO)** | The Sun in visible light, with its sunspots; updated about every 15 minutes | Again and again |
| **The Earth now (NOAA GOES-19)** | The whole Earth as the GOES-19 satellite sees it; updated every 10 minutes, 126 KB a fetch | Again and again |
| **A note** | Your text, black on white | Once, when you save |
| **A QR code** | A link or a text as a QR code | Once, when you save |
| **My own address…** | A JPEG, PNG or BMP from an address you give. `{width}` and `{height}` in the address become the picture's size on this reader, so a server can draw it to fit | Again and again |

**A picture fetched again and again** gets a small plugin of its own, which the page installs when you
save: **Home screen picture 1** (or 2) on the [Plugins](#11-plugins) page, allowed to reach only that
picture's site. For each one, choose when the reader fetches it: **Each time the reader goes to sleep**,
or **After you close a book** (fetched at the next sleep). The reader then joins your last Wi-Fi network
as it goes to sleep, as for any [plugin that acts while you read](#11-plugins), so going to sleep takes
a few seconds longer. Saving fetches each picture once straight away. If a picture's server sends the
reader on to another site, the page asks whether to allow that site. When you remove a picture from the
web from the layout, saving removes its plugin as well.

**A note and a QR code never change.** Your browser asks the service for them once when you save (a
note from placehold.co, a QR code from goqr.me) and puts the picture on the card; the reader never asks
a service for them. Whatever you type for one goes to that service, so never put a password in a note
or a QR code.

Pictures from the web are kept in `/home/web` on the card.

### Going back

The Custom layout stays saved when you switch to another one. To return to a ready-made layout, choose
it under **Settings → Display → Home screen → Layout** on the reader, or on the [Settings](#6-settings)
page. If the layout saved on the reader cannot be read, the page says so: Home shows the Reader layout
until you save a new one here.

The layout is the file `/.crosspoint/home.json`, which you can also write by hand:
[Lyra Tiles' Home layout file](home-layout.md).

## 8. Font Manager

Fonts for reading live on the SD card. The menu on the left switches between five parts: the fonts on
the reader, three ways to add one, and the language packs, which are no fonts and have a group of
their own.

**Installed fonts** lists the font families on the card, with their sizes (a TrueType or OpenType
family shows "all sizes"), and deletes the ones you no longer use.

**Download fonts** lists the reading fonts in the online catalog. **Download** installs one, **Update**
appears when a newer version is available (the number beside the menu entry counts them), and
**Download all updates** fetches every missing family and every update, except the families for one
script you have not installed (CJK, Indic, Arabic, Hebrew and Myanmar, each sans serif and serif):
install your language's with its own **Download**. The reader checks the card has room first.
A family too large for the reader's font area is installed but not listed, and the page says so. The
reader downloads the files itself, so it must be online.

![Downloading fonts](images/web/fonts-download.png)

**Upload a font** is the way to add a TrueType or OpenType font of your own (`.ttf`, `.otf`): the
files go onto the card as they are, into the family's folder in `/.crosspoint/fonts/`, and the reader
draws them at every size, like the catalog's TrueType families. A `.woff` file is unpacked in your
browser first (WOFF2 is not). It works without internet, so also in hotspot mode:

![Uploading a font](images/web/fonts-upload.png)

1. Choose the family's files: up to four, for regular, bold, italic and bold italic. Each file's
   style is read from the font itself. Without a bold or italic file the reader draws bold text
   emboldened and italic text slanted. A variable font (one file for every weight) is drawn in its
   default style.
2. Check the **Family name** (letters, digits, `-` and `_`) and click **Upload**. Each style goes
   onto the card as `<family>-Regular.ttf`, `<family>-Bold.ttf` and so on, so uploading a style again
   replaces it.
3. Choose the font on the reader as your reading font.

The reader checks every file before it goes in: one it cannot draw (a variable CFF2 font, or one
without a Unicode character map) is refused, and so is a font collection (`.ttc`). A family that is
too large for the reader's font area is uploaded but not listed, and the page says so. When the
reader already has a family of that name, the page asks first and adds the files to it; a family
of `.cpfont` files cannot take TrueType files, nor the other way round, so choose another name or
delete that family first. **Upload a font** also takes `.cpfont` files you already have.

**Convert a font** makes `.cpfont` files at the sizes you choose from your own TrueType or OpenType
files (`.ttf`, `.otf`, `.woff`), in your browser, without internet:

![Converting a font](images/web/fonts-convert.png)

1. Choose the family's files: up to four, for regular, bold, italic and bold italic. Each file's
   style is read from the font itself; a regular one is needed. Variable fonts (one file for every
   weight) cannot be converted; use the family's separate files.
2. Check the **Family name** (letters, digits, `-` and `_`), the **Characters** to include, and the
   **Sizes**.
3. Click **Convert and install**. Then choose the font on the reader as your reading font.

The conversion is the same one the catalog's `.cpfont` fonts are made with, so a converted font looks
just as good. **The fonts of an EPUB** takes the fonts a book embeds instead: choose the book from
your computer (or use **Convert its fonts** in the File Manager's Details). Fonts protected with DRM
can't be converted. A font that came with a book is licensed with that book: convert it for reading on
your own reader only.

**Language packs**, under **Languages** in the menu, lists what the reader needs for a language besides
a font of its script, in three groups. English needs none of them.

![Language packs](images/web/fonts-packs.png)

- **Interface languages**: the menus and messages in one language, one pack each (see
  [Language packs](../USER_GUIDE.md#language-packs)). Install the language you set in Settings; the
  reader uses it from its next start.
- **Book titles in Chinese, Japanese and Korean**: the interface packs (ChineseUI, ChineseTradUI,
  JapaneseUI, KoreanUI) that draw such titles in the menus. They are not reading fonts: for a book,
  download a font of its script under **Download fonts**.
- **Hyphenation**: one pack per language; every language but English hyphenates with its pack (see
  [Hyphenation packs](../USER_GUIDE.md#hyphenation-packs)). **Download** installs one into the reader's
  `/.crosspoint/hyphenation/` folder, and on the X3 and X4 copies it into the reader's flash as well.

**Download** installs a pack, and the number beside **Language packs** in the menu counts the packs with
an update. **Update all packs** updates the packs you have, but fetches no missing one: install the
languages you need from their rows. **Delete** removes an installed pack, as the Font Manager on the reader
does; it first says what that changes. Without the interface language's own pack the menus are English from
the reader's next start; without a hyphenation pack its language's books are no longer hyphenated; without a
title pack, CJK book titles outside a book show boxes for the characters the menu fonts lack.

## 9. Dictionaries

The reader looks words up in StarDict dictionaries in the card's `/dictionaries` folder. This page
installs, checks and removes them:

![The Dictionaries page](images/web/dictionary.png)

To install one, choose the archive you downloaded (`.zip`, `.tar.gz`, `.tgz`, `.tar.bz2` or `.tar`),
or the dictionary's own files (`.ifo`, `.idx` or `.idx.gz`, `.dict` or `.dict.dz`, and optionally
`.syn`), or drop them on the page. Your browser unpacks what the reader can't read compressed, checks
the files, and uploads them into a folder of their own. An archive holding several dictionaries
becomes one folder each.

Each installed dictionary is marked **Ready**, **Works** (usable, with a note on what would make it
better) or **Not usable** (with the reason). Choose the dictionary you want on the reader, under
**Settings → Reader → Dictionary**. See [Dictionary](./dictionary.md) for where to find dictionaries.

## 10. Reading Stats

![Reading Stats](images/web/stats.png)

The same numbers as the Reading Stats screens on the reader: your total reading time, sessions,
streaks and pages, a chart of the last 30 days, and a list of the books you have read, with time,
progress and an estimate of the time left. Click a book for its details, where **Remove from stats**
takes it out of the totals.

**Download backup** saves your whole history as `reading-stats.json`. Keep it before you go back to an
older firmware.

## 11. Plugins

Plugins add tools to the File Manager and Settings pages. They run in your browser, not on the reader; some also
have the reader send their requests (see below).

![The Plugins page](images/web/plugins.png)

- **Installed plugins** lists what is on the card. **Switch off** keeps a plugin but stops loading
  it; **Remove** deletes it.
- **Available plugins** is the catalog of this project's plugins. Your browser reads it from GitHub,
  so it needs internet (in hotspot mode, use a file instead). Click **Install** on one. **CrossPoint community**
  shows the plugins written for CrossPoint instead, from its Plugin Hub, each marked **runs here**, **partly runs
  here** (with what does not) or **does not run here**. Witch Reader has not checked them.
- **Install from a file** takes a plugin as a `.zip` file.

Each plugin says which page it adds itself to and which internet sites it may reach through the reader
(**Reaches**: those its manifest lists, and those written out in the addresses its reading and sleep events use).
When the reader refused a site such a plugin wanted to send to (one its settings name, say), the plugin shows
"Wants to reach" that site: **Allow** lets it, and the site joins those **Approved by you**, where **Take back**
withdraws it again; **Dismiss** drops the question, and the reader does not ask about that site again.
Only install plugins you trust: a plugin can read, change and delete anything on the card that this web interface
can. For writing your own, see [SD-card web plugins](./sd-plugins.md).

**Plugins that act while you read.** Some community plugins have the reader send something when you read or put it
to sleep: **KOSync Auto-push** sends your reading progress to a KOSync server, **Month Wallpaper** fetches a new sleep
picture. The reader sends only to the sites such a plugin may reach: those it lists (**Reaches**) and those you
approved when its card asked. It keeps what they want to send on the card and sends it whenever it joins Wi-Fi; at
sleep it joins your last network for it (not under 20 % battery), so going to sleep takes a few seconds longer. On the
X3 and X4 it shows the sleep screen first and restarts twice while it sends. Such a plugin's settings (a server, an
account) are made on its card in the web interface. See [Events](./sd-plugins.md#events-the-reader-sends-a-plugins-requests).

**Books with Adobe DRM** (library loans and shops' "Adobe DRM" titles) come in through the community
plugin **Protected Content**: it adds itself to the File Manager, signs in with your Adobe ID, turns the
`.acsm` file the library or shop gives you into the book, and stores the book's key on the reader. The
reader does the plugin's cryptography for it, since the browser cannot on this page. See
[Protected books](../USER_GUIDE.md#protected-books) for what the reader then does.

## 12. System Info and Diagnostics

**System Info** shows the firmware version, the model, the network and IP address, the battery, the
memory, and (after **Load SD info**, which takes a moment on a large card) the card's size and free
space.

![System Info](images/web/systeminfo.png)

**Diagnostics** is for bug reports. It shows how the reader last went to sleep and started up, and the
history of the last sleeps and starts, in plain words. **Download report** saves all of it, with the
firmware version and device status, as one file to attach to an issue. The report leaves out the IP
and MAC address and holds no passwords.

![Diagnostics](images/web/diagnostics.png)

## 13. Other ways to reach the card

- **WebDAV.** While File Transfer runs, the card is also served over WebDAV at the same address, so
  you can connect to `http://<reader address>/` as a network drive in Windows Explorer, macOS Finder or
  a Linux file manager, and copy files without the web page. Folders must be empty to be deleted, and
  nothing inside a folder whose name starts with a dot can be changed this way.
- **Calibre.** Calibre's "Connect to device" works through **Calibre Wireless** on the File Transfer
  screen.
- **Scripts.** Every page uses a small HTTP API that `curl` or a script can use too; see
  [Webserver Endpoints](./webserver-endpoints.md).

## 14. Leaving File Transfer

Press **Back** on the reader (or tap **Exit** on a touch screen). The web server stops, Wi-Fi is
switched off to save battery, and the reader restarts quietly into the Home screen. Books you uploaded
are in the Library's **Books** tab straight away; **New** and **Authors** pick them up the next time
you open those tabs.

**Back** also works in the middle of an upload from the File Manager or the Font Manager: the upload
is cancelled and the part that arrived is deleted, so send that file again later. A file being copied
over WebDAV finishes arriving first.

## 15. Troubleshooting

**The page does not open.**
- Check that your computer or phone is on the same network as the reader (in hotspot mode, joined
  to **CrossPoint-Reader**; some phones drop a network without internet: allow it to stay connected).
- Type the address with `http://`, not `https://`. If `crosspoint.local` does not work (common on
  Android and on some Windows setups), use the IP address the reader shows.
- Guest or office networks often keep devices from seeing each other. Use your home network, or the
  hotspot.

**The setup page keeps coming back.** After every firmware update that changed the pages, the setup
runs once more; this is normal. In hotspot mode it can't download: use the `web-….bin` file from the
release page (see [The first visit](#3-the-first-visit-setup)). If it fails on your Wi-Fi network,
the download from GitHub did not work: **Try the download again**, or use the file.

**An upload fails.** Check the free space on the card (**System Info → Load SD info**). Uploads are
faster and more reliable with a strong Wi-Fi signal; the reader's screen shows it. A failed file can
be sent again with **Retry**.

**Something that needs the internet does not work** (font downloads, cover search, the plugin
catalog). The reader must be on a network with internet access for font downloads and cover search;
for the plugin catalog, your browser must be. Neither works in hotspot mode. Font conversion and the
EPUB optimizer work without internet.

**The reader seems slow or restarts while the web interface is open.** It has far less memory than a
computer. Avoid having the web interface open in several browser tabs at once, and wait for one large
job (an upload, a font install) to finish before starting the next.

## 16. Security

- The web interface has **no password**: anyone on the same network can use it while File Transfer
  runs. Use it on networks you trust, or in hotspot mode.
- It runs only while File Transfer is open on the reader, and stops when you leave.
- Pages are served over plain HTTP (port 80); fast uploads use port 81.
- The files that hold your saved Wi-Fi, OPDS and KOReader passwords can't be downloaded or overwritten
  through the File Manager or WebDAV. The Settings page never shows a saved password: you can replace
  it with a new one or clear it, but you can't read it or correct it in place.
- The reader joins 2.4 GHz Wi-Fi networks only.

---

Related: [User Guide](../USER_GUIDE.md) · [Webserver Endpoints](./webserver-endpoints.md) ·
[Dictionary](./dictionary.md) · [SD-card web plugins](./sd-plugins.md) ·
[Troubleshooting](./troubleshooting.md)
