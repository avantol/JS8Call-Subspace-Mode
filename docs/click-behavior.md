# Click/Double-Click Behavior — Standard JS8Call vs Subspace

## Summary Table

| Action | Band Activity (left) | Call Sign (right) | RX/TX Text (center/orange) |
|--------|---------------------|-------------------|---------------------------|
| **Single-Click** | Select freq → update "directed to" → auto-mode switch | Select call → update "directed to" → auto-mode switch | Cursor movement only |
| **Double-Click** | Single-click + change freq + show history in orange | Single-click + add callsign to msg box | Add selected word to msg box |
| **Changes Frequency** | Double-click only | No | No |
| **Adds to Message Box** | No | Double-click only | Double-click only |
| **Shows History** | Double-click only | No | No |
| **Mode Switch** | Yes (single + double) | Yes (single + double) | Double-click only (Subspace addition) |
| **Updates "Directed To"** | Yes (single + double) | Yes (single + double) | Double-click only (Subspace addition) |

## Detailed Behavior

### Band Activity Table (tableWidgetRXAll) — Left Pane

**Single-Click:**
1. Deselects callsign table (blocks signals to prevent cascade)
2. Rebuilds callsign table via `displayCallActivity()`
3. Finds callsign at selected frequency via `callsignSelected()`
4. Updates "directed to" via `callsignSelectedChanged()`
5. Auto-switches mode if callsign's submode differs (Subspace ↔ standard)
6. Focuses message box

**Double-Click:**
1. Everything from single-click, plus:
2. Changes RX frequency to selected offset via `setFreqOffsetForRestore()`
3. Collects all message history at that frequency
4. Displays full conversation in orange RX/TX text area
5. Auto-switches mode based on activity submode

### Call Sign Table (tableWidgetCalls) — Right Pane

**Single-Click:**
1. Deselects band activity table
2. Rebuilds band activity via `displayBandActivity()`
3. `tableSelectionChanged` fires → `callsignSelected()` returns the clicked call
4. Updates "directed to" via `callsignSelectedChanged()`
5. Auto-switches mode (Subspace addition)
6. Focuses message box

**Double-Click:**
1. Everything from single-click, plus:
2. Adds callsign to message box via `addMessageText(call)`
3. (Optional: show inbox messages — currently disabled)

### RX/TX Text Area (textEditRX) — Center Orange Window

**Single-Click:**
- Standard Qt text cursor positioning — no custom behavior

**Double-Click (Subspace additions):**
1. Scans backwards for last valid `CALLSIGN:` pattern in the line
2. Validates callsign (letters+digits, 3-10 chars, not own call)
3. If found:
   - Updates "directed to" via `callsignSelectedChanged()`
   - Selects callsign in callsign table (if present)
   - Auto-switches mode based on `m_callActivity` submode
4. If not found:
   - Deselects current callsign
5. Focuses message box

**Standard JS8Call behavior (no Subspace mods):**
- Selects word at click position
- Adds selected word to message box via `addMessageText()` after 150ms delay

## Key Internal Functions

| Function | Purpose |
|----------|---------|
| `callsignSelected()` | Returns selected callsign: checks callsign table first, falls back to band activity offset matching |
| `callsignSelectedChanged(old, new)` | Updates placeholder text, pauses HB, records QSO time, updates buttons |
| `tableSelectionChanged()` | Fires on EITHER table selection change — calls `callsignSelected()` + `callsignSelectedChanged()` |
| `displayBandActivity()` | Rebuilds band activity table — can trigger `tableSelectionChanged` |
| `displayCallActivity()` | Rebuilds callsign table — can trigger `tableSelectionChanged` |

## Known Hazards

1. **Cascade from table rebuild**: `displayCallActivity()` / `displayBandActivity()` rebuild tables, which fire `tableSelectionChanged`, which can clear the selection just set. Fix: `blockSignals(true)` during rebuild.
2. **Mode switch recursion**: `setSubmode()` → `setupJS8()` → table rebuilds → `tableSelectionChanged` → mode switch. Fix: only switch when callsign actually changes.
3. **"Directed to" cleared on rebuild**: callsign table deselect + rebuild fires `tableSelectionChanged` with empty selection. Fix: handle callsign selection explicitly in `cellClicked`, not through the generic `tableSelectionChanged` path.

## Subspace-Specific Additions

| Feature | Standard JS8Call | Subspace |
|---------|-----------------|----------|
| Mode switch on click | No | Yes — FT2 ↔ standard based on callsign's mode |
| Double-click orange text | Add word to msg box | Parse callsign, update "directed to", switch mode |
| Mode buttons (N/F/T/⚡) | No | Yes — on action panel |
| Mode in status bar | Static "JS8" | Live: "⚡ Subspace" / "Normal" / "Fast" / "Turbo" |
| Mode indicator in text | No | Yes — prefix each line with ⚡/N/F/T/S |
