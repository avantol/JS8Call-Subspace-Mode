# Microsoft Store submission notes — JS8Call Subspace Edition

Reference checklist for the imminent Microsoft Store listing. Captured 2026-04-26
in the run-up to submission. This is a working document — verify each Microsoft-
specific detail against the current Microsoft Partner Center / MSIX documentation
before relying on it; the platform changes faster than this file will.

## Why we're doing this (strategic context)

Defensive positioning vs the upstream JS8Call-improved project's putative 3.0.0
release. They likely ship with a code-signing certificate this round (possibly
EV), which would give their `.exe` installer zero SmartScreen warnings on
Windows. Store distribution sidesteps that race entirely on a different surface
— the Store IS the signing authority for that channel, and we get one-click
install, automatic updates, and discoverability for free. The two distribution
paths compete on different axes; the `.exe`/`.msi` side stays an EV-cert
comparison until/unless we get our own signing cert later.

## 1. MSIX manifest capabilities

Qt desktop apps reach the Store via the "Desktop Bridge" / MSIX packaging path
(formerly "Desktop App Converter"). The `AppxManifest.xml` must declare every
restricted capability the app uses, or the app will silently fail in review when
the reviewer tries the corresponding feature.

Capabilities JS8Call Subspace needs to declare:

| Capability                  | Why                                                             |
|-----------------------------|-----------------------------------------------------------------|
| `serialcommunication`       | Hamlib CAT control over USB-serial to the rig                  |
| `microphone`                | Audio input from the soundcard for RX                           |
| `internetClient`            | TCP API on port 2442, PSK Reporter spotting, APRS-IS connection |
| `internetClientServer`      | If we want inbound API connections (we already do — port 2442)  |
| `USBDevice`                 | If any direct USB access (probably no — Hamlib goes via serial) |
| `runFullTrust`              | Required for any Win32 desktop app converted via Desktop Bridge |

Check the actual JS8Call code for any other restricted-API surface before
finalizing the manifest. The reviewer will exercise the rig and audio paths.

## 2. Settings / config write location

MSIX applies file-system virtualization. Writes to `%APPDATA%`, `%LOCALAPPDATA%`,
the registry, etc. get redirected to a per-package virtualized location so the
app sees them but they don't pollute the system.

What to verify before submission:

- JS8Call's Qt `QStandardPaths::AppConfigLocation` etc. should keep working
  unchanged — Qt resolves the right virtualized path under MSIX.
- Any code that writes to a hardcoded path (e.g. `~/.config/JS8Call/...` literal
  strings) will silently misbehave under virtualization. Grep the codebase for
  hardcoded user-home paths and confirm none exist outside the standard
  `QStandardPaths` API.
- Crash dumps: our `CrashHandler` writes minidumps to a folder. Confirm that
  folder uses `QStandardPaths::WritableLocation(...)` and not a hardcoded path.
- Diag log location: same check.
- Notification audio file paths: today the user picks files from anywhere, which
  is fine — that uses standard file dialogs that work in MSIX.

## 3. First-submission review latency

Typical Store review for a Desktop Bridge / MSIX desktop app with a clean
manifest:
- Most submissions: 1-3 business days.
- First submission: tends to be slower — the publisher account itself is also
  being vetted alongside the package.
- Major content updates: faster than first.
- Updates to existing approved packages: often a few hours.

Implication for the J-IMP 3.0.0 race window: "1-2 days to apply" is just the
prep time. Add the Store review queue on top. If J-IMP ships in 5 days and our
Store review takes 3-7 days, the announcement ("Subspace is on the Microsoft
Store") may not beat their release calendar, but it lands within the same news
cycle and on a different distribution surface — which is the point.

## 4. EV cert competitive frame

If J-IMP ships with EV signing, what they get vs what we get on Store:

| Aspect                      | J-IMP with EV-signed .exe            | Subspace on MS Store           |
|-----------------------------|--------------------------------------|--------------------------------|
| SmartScreen warning         | None — instant reputation            | None — Store is trusted        |
| Install friction            | Run installer, click through UAC     | "Get" button, one-click        |
| Auto-update                 | Whatever in-app updater they wrote   | Free, automatic, OS-level      |
| Discoverability             | Their website + word of mouth        | Store search + featured slots  |
| Per-platform cost           | EV cert ~$300-500/year               | Partner account one-time fee   |
| Sandboxed                   | No                                   | Yes (the cost: capability gates) |
| Comparison on .exe channel  | They win                             | We're absent                   |

Subspace can pursue our own code-signing cert later if we want to compete on the
.exe channel directly. For now, Store listing is the leverage move that costs
less and ships faster.

## Pre-submission checklist (suggested)

Before clicking "Submit" in Partner Center:

- [ ] MSIX package builds cleanly with all capabilities declared
- [ ] App launches in the MSIX sandbox on a clean Windows VM (no dev tools
      installed) and reaches the main window
- [ ] Rig connection works through `serialcommunication` capability
- [ ] Audio RX works through `microphone` capability
- [ ] TCP API on port 2442 accepts connections through `internetClientServer`
- [ ] Settings can be saved and persist across launches (virtualized AppData)
- [ ] Notification sounds play
- [ ] Crash handler writes a minidump to a virtualized location and the app
      recovers gracefully on restart
- [ ] No hardcoded `C:\Users\...` or `~/.config/...` paths in source — only
      `QStandardPaths` API
- [ ] Privacy declarations in the Store listing accurately describe what
      JS8Call collects (PSK Reporter spots, APRS-IS reports, etc.)
- [ ] Age rating questionnaire completed honestly
- [ ] Screenshots and store description match the actual app behavior
- [ ] Test on Windows 10 22H2 AND Windows 11 (minimum supported versions)

## Open questions worth confirming with current Microsoft docs

- Does Hamlib's USB-serial path work through `serialcommunication` capability
  alone, or does it also need brokered `USBDevice` access for some chipsets?
- Does our QSoundEffect-based notification audio path (post Build 108) work
  correctly under MSIX, or does it hit any sandbox restrictions on the
  notification audio API?
- Does the sandbox restrict child-process spawning? (Relevant if we ever do the
  Reticulum helper path — not v1 concern, but worth noting.)
- Is there a per-package size limit that affects us? Today's `.deb` is 6.5 MB;
  the equivalent MSIX should be similar order of magnitude. Future Reticulum
  bundling would add 40-80 MB and might bump into Store size constraints.

## References

- [Microsoft Store policies for desktop apps (verify against current docs)](https://learn.microsoft.com/en-us/windows/uwp/publish/store-policies)
- [Package a desktop app via MSIX](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-root)
- [App capability declarations](https://learn.microsoft.com/en-us/windows/uwp/packaging/app-capability-declarations)
- Qt's MSIX packaging tooling (QtMsixPackage / windeployqt) — check current Qt
  documentation for the version JS8Call builds against (Qt 6.4 on Linux, 6.9 on
  Windows CI).
