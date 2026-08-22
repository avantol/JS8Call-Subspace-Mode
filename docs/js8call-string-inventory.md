# User-facing "JS8Call" string inventory

Every place the literal string **"JS8Call"** appears in user-visible text or
OS-level metadata, as of Build 152 (commit `b22fcdd2`). Use this as the
source-of-truth checklist when planning a rebrand or audit. Internal symbol
names (e.g. enum values like `JS8CallNormal`, member variables) are
intentionally **excluded** — they don't appear on screen.

Each entry is numbered for cross-reference. Numbers are stable; if the
inventory grows, append rather than renumber.

---

## §1. Window title and application identity

| #  | File:Line | What user sees / what it controls |
|----|-----------|-----------------------------------|
|  1 | `JS8_Main/revision_utils.cpp:27` | Main window title bar — `"%1 Subspace Edition \"Tranya\" (v4.0.0.152) by WM8Q"` where `%1` resolves to `applicationName` (set in #2). |
|  2 | `JS8_Main/main.cpp:127` | `QCoreApplication::setApplicationName("JS8Call")` — flows into Windows taskbar tooltip, taskbar grouping, Windows Settings → Apps, macOS process name, Linux process name. |
|  3 | `JS8_Main/main.cpp:182` | `QSettings(configDir + "/JS8Call.ini", ...)` — settings file name. **Renaming this orphans existing user configs across upgrade**; recommend leaving as-is even after a brand rename. |
|  4 | `JS8_Mainwindow/UI_Constructor.cpp:454` | `ui->labVersion->setText("JS8Call v" + version())` — version label in the main window's top banner. |

## §2. Main-window menus and chrome

| #  | File:Line | What user sees |
|----|-----------|----------------|
|  5 | `JS8_UI/mainwindow.ui:14`   | `<string>JS8Call</string>` — top-level QMainWindow title fallback (overridden at runtime by #1, but seen briefly during startup). |
|  6 | `JS8_UI/mainwindow.ui:2128` | Help menu item — "About &JS8Call" (`&` is the Alt-shortcut underline). |
|  7 | `JS8_UI/mainwindow.ui:2161` | Help menu item — "JS8Call User Guide". |
|  8 | `JS8_UI/mainwindow.ui:433`  | Top-of-window banner link — "Check for updates", URL `github.com/avantol/JS8Call-Subspace-Mode/releases/latest`. |
|  9 | `JS8_UI/mainwindow.ui:446`  | Top-of-window banner link — "Join the discussion", URL `groups.io/g/JS8Call-Subspace/topics`. |

## §3. About dialog

| #  | File:Line | What user sees |
|----|-----------|----------------|
| 10 | `JS8_UI/About.ui:17`        | Dialog title (Designer default) — "About JS8Call". Overridden at runtime by #11. |
| 11 | `JS8_UI/About.cpp:14`       | Runtime dialog title — `"About JS8Call Subspace Edition \"Tranya\""`. |
| 12 | `JS8_UI/About.cpp:20`       | Heading hyperlink — `<h2><a href="https://github.com/avantol/JS8Call-Subspace-Mode">…</a></h2>`. |
| 13 | `JS8_UI/About.cpp:28`       | Body — "JS8Call is a derivative of the WSJT-X application…" |
| 14 | `JS8_UI/About.cpp:31`       | Body (continuation) — "…development group. JS8Call is …" |
| 15 | `JS8_UI/About.cpp:35`       | Body — "JS8Call Subspace Edition was derived from 'JS8Call-Improved'." |
| 16 | `JS8_UI/About.cpp:36`       | Body — "JS8Call-Improved was developed by the team that …" |
| 17 | `JS8_UI/About.cpp:38`       | Body — "…JS8Call starting in late 2024. This team includes: …" |
| 18 | `JS8_UI/About.cpp:42`       | Body — "JS8Call 2.4 and later uses libraries from the FFmpeg project under …" |
| 19 | `JS8_UI/About.cpp:44`       | Body — "JS8Call is heavily inspired by WSJT-X, Fldigi and FSQCall and would …" |
| 20 | `JS8_UI/About.cpp:48`       | Body — "JS8Call stands on the shoulder of giants...the takeoff angle is …" |
| 21 | `JS8_UI/About.cpp:63`       | Closing line — "…JS8Call into the world." |

## §4. Configuration dialog

| #  | File:Line | What user sees |
|----|-----------|----------------|
| 22 | `JS8_UI/Configuration.ui:2671` | Tooltip on UDP server option — "JS8Call will accept certain requests from a UDP client sending WSJT-X protocol messages." |
| 23 | `JS8_UI/Configuration.ui:2691` | Tooltip on multicast network-interface picker — "…allow multiple other applications on the same machine to interoperate with JS8Call." |

## §5. Update-checker dialog

| #  | File:Line | What user sees / does |
|----|-----------|------------------------|
| 24 | `JS8_Mainwindow/checkVersion.cpp:34-37` | Dialog body — "A new version (%1) of JS8Call is now available…" with link `github.com/JS8Call-improved/JS8Call-improved/releases`. **⚠ Bug**: link still points to **J-IMP**, not Subspace's repo. Should be `github.com/avantol/JS8Call-Subspace-Mode/releases`. |
| 25 | `JS8_Mainwindow/checkVersion.cpp:49`    | Dialog body — "Your version (%1) of JS8Call is up-to-date." |
| 26 | `JS8_Mainwindow/checkVersion.cpp:59`    | Background URL fetched for version check — `github.com/JS8Call-improved/JS8Call-improved/releases/...`. **⚠ Same bug as #24**: hits J-IMP repo, so version check compares against the wrong project's releases. |

## §6. Source-code copyright text (shown in Help)

| #  | File:Line | What user sees |
|----|-----------|----------------|
| 27 | `JS8_UI/mainwindow.cpp:1522` | Legal text — "Further, the source code of JS8Call contains material Copyright (C)…" |

## §7. Crash-report dialogs (Windows)

| #  | File:Line | What user sees |
|----|-----------|----------------|
| 28 | `JS8_Main/CrashHandler.cpp:188` | Dialog body (dump-folder variant) — "JS8Call has crashed (%s).\n\n…" |
| 29 | `JS8_Main/CrashHandler.cpp:195` | Dialog body (alternate folder variant) — "JS8Call has crashed (%s).\n\n…" |
| 30 | `JS8_Main/CrashHandler.cpp:201` | Dialog title — "JS8Call Crash Report". |
| 31 | `JS8_Main/CrashHandler.cpp:207` | Dialog body when dump can't be written — "JS8Call has crashed (%s) but could not write a crash …" |
| 32 | `JS8_Main/CrashHandler.cpp:209` | Dialog body (continuation) — references `%LOCALAPPDATA%\JS8Call`. |
| 33 | `JS8_Main/CrashHandler.cpp:216` | Same dialog title (write-failure case) — "JS8Call Crash Report". |
| 34 | `JS8_Main/CrashHandler.cpp:282` | `--crash-test` info-dialog title — "JS8Call crash-test". |
| 35 | `JS8_Main/CrashHandler.cpp:298` | `--crash-test` warning-dialog title — "JS8Call crash-test". |
| 36 | `JS8_Main/CrashHandler.cpp:70`  | Crash-dump folder — `…\JS8Call`. |
| 37 | `JS8_Main/CrashHandler.cpp:108` | Crash-dump filename pattern — `JS8Call-crash-<timestamp>.dmp`. |
| 38 | `JS8_Main/CrashHandler.cpp:181` | Crash-dump candidate folder — `AppData\Local\JS8Call`. |

## §8. Logbook export

| #  | File:Line | What user sees |
|----|-----------|----------------|
| 39 | `JS8_Logbook/ADIF.cpp:451` | Header line written to every newly-created `.adi` log file — "JS8Call ADIF Export\<eoh\>". Visible in the exported file, not in the GUI. |

## §9. On-the-wire identity (third-party services)

| #  | File:Line | What other systems see |
|----|-----------|------------------------|
| 40 | `JS8_Main/APRSISClient.cpp:105` | APRS-IS server-login frame — `software_id="JS8Call"`. Visible to APRS-IS operators and on aprs.fi. |
| 41 | `JS8_UDP/WSJTXMessageClient.h:29` | Documentation comment — `"JS8Call"` as the `id` field sent in WSJT-X-protocol UDP messages to PSKReporter, JS8 Spotter, and similar third-party clients. |

## §10. Installer / OS-level metadata

| #  | File:Line | What user sees / where |
|----|-----------|------------------------|
| 42 | `deb-staging/usr/share/applications/js8call-subspace.desktop:2` | Linux app-menu entry — `Name=JS8Call Subspace`. |
| 43 | `deb-staging/usr/share/applications/js8call-subspace.desktop:3` | Linux app-menu tooltip — `Comment=JS8Call Subspace Edition - weak-signal messaging`. |
| 44 | `.github/workflows/scripts/ci-windows-installer.iss:1` | Inno Setup `MyAppName "JS8Call"` — Windows Add/Remove Programs entry, Start Menu folder, installer wizard title bar. |
| 45 | `.github/workflows/scripts/ci-windows-installer.iss:3` | Inno Setup `MyAppPublisher "JS8Call-improved"` — publisher field in Windows Add/Remove Programs. |
| 46 | `.github/workflows/scripts/ci-windows-installer.iss:5` | Inno Setup `MyAppExeName "JS8Call.exe"` — installed binary name. |
| 47 | `.github/workflows/scripts/ci-windows-installer.iss:35` | Inno Setup `OutputBaseFilename=JS8Call-installer` — name of the installer .exe users download. |
| 48 | `CMakeLists.txt:176` | macOS bundle Info.plist string — "JS8Call facilitates amateur radio communication using very weak signals.." Visible in macOS Finder Get-Info pane. |
| 49 | `CMakeLists.txt:183` | macOS `MACOSX_BUNDLE_GUI_IDENTIFIER "org.kn4crd.js8call"` — bundle identifier. **Note**: still uses KN4CRD's reverse-domain. |
| 50 | `CMakeLists.txt:59`  | CMake `project(JS8Call ...)` — propagates into installer/bundle metadata via CMake variables. |
| 51 | `CMakeLists.txt:67`  | `PROJECT_HOMEPAGE https://groups.io/g/js8call` — propagates into installer/bundle metadata. |
| 52 | `CMakeLists.txt:522` | `cpack` `--libdir JS8Call` — package layout. |
| 53 | `CMakeLists.txt:523` | `cpack` `--plugindir JS8Call` — package layout. |

---

## Cross-platform groupings (for synchronized rebrand)

If you do rebrand, items that **must** change in lockstep at a release boundary:

- **Window-title cluster**: #1, #2, #4 (and the `applicationName` referenced through #1).
- **Help-menu cluster**: #6, #7, plus the About dialog (§3 #10–#21) since the menu opens it.
- **Banner-link cluster**: #8, #9 (top-of-window links).
- **Update-checker cluster**: #24, #25, #26 (also fixes the J-IMP-URL bug).
- **Linux installer cluster**: #42, #43, plus the .deb package name (`Package: js8call-subspace` in `deb-staging/DEBIAN/control:1`).
- **Windows installer cluster**: #44, #45, #46, #47.
- **macOS bundle cluster**: #48, #49, #50, #51, #52, #53.
- **Third-party identity cluster**: #40 (APRS-IS), #41 (WSJT-X UDP) — affects PSKReporter, aprs.fi, JS8 Spotter dashboards. Coordinate with operators of those services if their dashboards index by software_id.

Items to **keep unchanged** even after a rename:

- **#3** (`JS8Call.ini`) — preserves user configs across upgrade.
- **#36, #37, #38** (`%LOCALAPPDATA%\JS8Call` and `JS8Call-crash-*.dmp`) — preserves crash-dump continuity for users who already have dumps; or migrate carefully if you want a fresh folder.

## Excluded from this inventory (intentionally)

- Internal enum identifiers: `JS8CallNormal`, `JS8CallFast`, `JS8CallTurbo`, `JS8CallSlow`, `JS8CallFT2`, `JS8CallData`, `JS8CallFirst`, `JS8CallLast`, `JS8CallUltra`. These are protocol-bit names in source code; users never see them.
- Internal class member references like `m_messageBuffer.cmd.JS8Call*` — pure source code.
- Build-time CI paths inside the Inno Setup `Source:` directives — these reference the runner's working directory, not the installed product.
- Background-config filenames `js8call.log`, `js8call_wisdom.dat` — files in the user config dir; not labelled in any GUI surface.

---

## File metadata

- **Generated**: 2026-05-06 against commit `b22fcdd2` (Build 152, v4.0.0.152).
- **Source**: search for `JS8Call` / `JS8call` across `*.ui`, `*.cpp`, `*.h`, `*.desktop`, `*.iss`, `CMakeLists.txt`, filtered to user-visible / OS-metadata / on-the-wire surfaces.
- **Numbering**: stable. Append-only when the inventory grows.
