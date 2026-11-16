# Protocol reference

The display is a USB CDC serial device. The baud rate is ignored. The host sends ASCII
lines terminated by `\n` (a trailing `\r` is ignored).

A line is `COMMAND arguments`. The command word is case-insensitive, and the arguments are
fields separated by `|`. Spaces around fields are trimmed. Missing trailing fields take
their defaults. A line that doesn't start with a known command is appended to the log.

Lines longer than 1024 characters are cut. Bytes outside printable ASCII are shown as
spaces.

## Levels

`level` fields accept these values:

| Level | Colour | Also accepted |
|---|---|---|
| `ok` | green | `good`, `green` |
| `warn` | amber | `warning`, `yellow`, `amber` |
| `crit` | red | `critical`, `err`, `error`, `alarm`, `red` |
| `info` (default) | cyan | `blue`, `cyan` |
| `off` | grey | `dim`, `idle`, `grey`, `gray` |

## Header and page

| Command | Fields | Notes |
|---|---|---|
| `TITLE` | text | Header title. The default is `USB DISPLAY`. |
| `STATUS` | `level\|text`, or `text` | Status pill at the right of the header. An empty text hides it. |
| `TICKER` | text | Scrolls continuously along the bottom. `TICKER` with no text removes it. |
| `PAGE` | `auto` \| `dash` \| `log` | `auto` (the default) shows the dashboard once any tile exists, and the log otherwise. |
| `BRIGHT` | 0–255 | Backlight level. The BOOT button switches between this and 1/6 of it. |
| `ROT` | `1` \| `3` | Landscape with the USB port on the left (1, the default) or the right (3). |

The green dot in the header pulses while lines arrive. It turns grey after 60 seconds
without input.

## Tiles

Every tile is identified by its **label**. Sending the same label again updates that tile;
a new label adds a tile. Up to 6 tiles are shown, laid out automatically:
- 1 or 2 tiles: one row
- 3 or 4 tiles: two columns
- 5 or 6 tiles: three columns

When a seventh label arrives, the tile updated longest ago is replaced. If a label is
reused with a different command, its tile is rebuilt as the new type.

| Command | Fields (defaults) | Notes |
|---|---|---|
| `BIG` | `label\|value\|unit\|level` | A numeric value animates to its new value; a text value is shown as is. |
| `BAR` | `label\|value\|max(100)\|text\|level` | The fill animates. `text` replaces the percentage caption. |
| `GAUGE` | `label\|value\|min(0)\|max(100)\|unit\|level` | A 270° arc with the value in the centre. |
| `SPARK` | `label\|v1,v2,...\|unit\|level` | Up to 48 points, auto-scaled, latest value top right. `SPARK label\|+v` appends one point (the oldest drops off). |
| `KV` | `label\|value\|level` | Text value, in the largest font that fits. |
| `LED` | `label\|level(ok)\|text` | A glowing dot plus text. `warn` and `crit` pulse. |
| `DEL` | label | Removes one tile. |
| `CLEAR` | — | Removes all tiles. |

Updated tiles glow briefly.

## Log, alerts and housekeeping

| Command | Fields | Notes |
|---|---|---|
| `LOG` | text | Appends to the log; a plain non-command line does the same. A leading `!`, `?` or `+` colours the line crit, warn or ok. Lines wrap by width, and the last 40 are kept. |
| `CLS` | — | Clears the log. |
| `ALERT` | `level(crit)\|text\|seconds(10)` | Overlay with pulsing border and up to 3 wrapped lines. `seconds` 0 keeps it until the KEY button or `ALERT off`. `ALERT text` alone uses level crit. |
| `ALERT off` | — | Hides the alert. |
| `RESET` | — | Clears tiles, log, ticker and status, and restores the default title, page and alert state. |

## Queries

The firmware writes replies to the same port, as ASCII lines.

| Command | Reply |
|---|---|
| `PING` | `PONG tdisplay-usb <version> <width>x<height> uptime=<s>s lines=<n>` |
| `HELP` | The command list, several lines. |
| `SHOT` | `SHOT <width> <height>`, then the frame buffer as base64 (76 characters per line), then `END`. |

The screenshot data is RGB565, big-endian, row by row from the top-left corner (320 × 170
× 2 = 108,800 bytes). `tdisplay.py shot` converts it to PNG.

## Buttons

| Button | Action |
|---|---|
| KEY (GPIO14) | Dismiss an alert; otherwise cycle the page (auto → dashboard → log). |
| BOOT (GPIO0) | Toggle the backlight between `BRIGHT` and 1/6 of it. |

## Opening the port without resetting the board

The ESP32-S3's built-in USB serial/JTAG controller resets the chip whenever RTS is
asserted while DTR is not. Two cases:
- **Opening and closing the TTY normally** (shell redirects, `cat`, `stty`) is fine: the
  kernel changes both lines together.
- **Libraries that set the lines one at a time** need care. With pyserial, set
  `dtr=True, rts=False` before `open()`, so the lines never pass through RTS=1, DTR=0.
