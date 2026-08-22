# JS8Call-Improved Upstream Open Bugs

Survey date: 2026-03-22
Source: https://github.com/JS8Call-improved/JS8Call-improved/issues

## High Severity

### #96 — Callsign prefix bypassed when own call in message text
- **Date**: 2025-12-14
- **Description**: When a user's own callsign appears anywhere in the message text, the automatic callsign prepending is bypassed, causing messages to transmit without proper station identification.
- **Impact**: FCC regulatory concern (Part 97 identification requirements)
- **Workaround**: None

### #53 — Long callsigns split across TX periods, lost from heard table
- **Date**: 2025-11-20
- **Description**: Extended callsigns (e.g., DA1BXYWYQ) cause message fragmentation across TX periods. The callsign splits at the boundary and fails to appear in the heard table on reception.
- **Workaround**: None. Use shorter/standard callsigns.

## Medium Severity

### #170 — CQ messages hijacked to @ALLCALL regardless of selected group
- **Date**: 2026-01-25
- **Description**: Messages starting with "CQ" are automatically redirected to @ALLCALL even when a different group is selected. The software's "magic" logic overrides user intent.
- **Workaround**: None

### #156 — TX delay not honored when transmitting late in period
- **Date**: 2026-01-21
- **Description**: Configured TX delay is bypassed when transmission is initiated late in a sending period. The modulator code actively disregards the delay setting to avoid waiting for the next period.
- **Related**: Our user's missing-audio issue. Upstream fix commit `02781de2` (PTT_DELAY_MS) not yet ported.
- **Workaround**: None

### #200 — Audio input only shows default device (Linux Qt6)
- **Date**: 2026-02-25
- **Description**: Audio input device selector shows only the default device on Linux with PulseAudio/PipeWire. Output selectors work fine. Config file edits are overwritten on restart.
- **Root cause**: Qt6 upstream — deprecated native Linux audio backends in favor of FFmpeg.
- **Workaround**: Use PulseAudio `module-remap-source` to expose monitor sources.

### #95 — Spurious HB transmission when HB disabled
- **Date**: 2025-12-14
- **Description**: An unexpected heartbeat transmission occurred despite HB networking being disabled via the UI. Could not be immediately reproduced.
- **Workaround**: None

### #137 — PTT drops before TX audio finishes (network rigs)
- **Date**: 2026-01-15
- **Description**: PTT signal drops prematurely before transmission audio finishes, especially over RemoteRig or network-based CAT setups with 50-100ms latency.
- **Related**: Same class of issue as our IC-7300 USB codec problem.
- **Workaround**: Python script monitoring virtual COM port, adding 100ms tail delay before releasing RTS.

## Low Severity

### #19 — API race condition on TX parameter changes
- **Date**: 2025-11-07
- **Description**: API race condition where TX parameter changes (e.g., speed) can be reverted before transmission completes because the API has no synchronous completion mechanism.
- **Workaround**: Manually delay parameter restoration until TX completes.

### #186 — Linux AppImage ignores system Qt theme
- **Date**: 2026-02-13
- **Description**: Qt theming is non-functional in AppImage builds due to FUSE sandboxing isolating bundled Qt from system theme plugins.
- **Workaround**: Build from source, or mount AppImage and run directly with Qt environment variables.

## Relevance to Subspace

| Bug | Affects us? | Status in our fork |
|-----|------------|-------------------|
| #96 Callsign prefix | Yes — same codebase | Not fixed |
| #53 Long callsigns | Yes — same framing | Not fixed |
| #170 CQ → ALLCALL | Yes — same TX logic | Not fixed |
| #156 TX delay | Yes — related to missing audio | PTT_DELAY_MS not ported |
| #200 Audio input | Yes — same Qt6 | Not fixed (Qt upstream) |
| #95 Spurious HB | Possibly | Not investigated |
| #137 PTT tail | Yes — IC-7300 issue is similar | Modulator guard added (Build 57) |
| #19 API race | Yes — same API | Not fixed |
| #186 AppImage theme | N/A — we use .deb | N/A |
