# Selection & Mode Switch Specification

## Scope

This spec covers what happens when the user selects a callsign or clicks a line
in any of the three windows. It does NOT cover frequency changes, message
composition, or TX behavior — those remain unchanged.

## Definitions

- **Left window**: Band Activity table (`tableWidgetRXAll`) — decoded messages by frequency
- **Right window**: Call Sign table (`tableWidgetCalls`) — active callsigns
- **Center window**: Conversation history (`textEditRX`) — orange RX/TX text area
- **"Directed to" button**: Shows currently selected callsign (`queryButton`)
- **Mode buttons**: N / F / T / ⚡ on action panel
- **Selected callsign**: The callsign shown in "Directed to" and used for outgoing messages

## Central Functions (new)

### `selectCallsign(QString call, int submode)`

Single entry point for all callsign selection. Every click path calls this.

**Behavior:**
1. Set `m_selectedCallsign = call`
2. Call `autoSwitchMode(submode)` if submode is known (>= 0)
3. Update placeholder text ("Type your outgoing directed message to CALL here")
4. Call `updateButtonDisplay()` and `updateTextDisplay()` (covers HB button enable/disable state)

**Rules:**
- If `call` is empty, this is a no-op. Use `clearSelection()` to deselect.
- If `call == m_selectedCallsign`, this is a no-op (no re-fire).
- Never called from signal handlers or display rebuilds — only from user actions.

### `clearSelection()`

Single entry point for deselection. Deselect button and no-callsign clicks call this.

**Behavior:**
1. Block signals on both tables
2. Clear both table selections
3. Unblock signals
4. Set `m_selectedCallsign = ""`
5. Update placeholder text ("Type your outgoing messages here")
6. Call `updateButtonDisplay()` and `updateTextDisplay()` (covers HB button enable/disable state)

### `autoSwitchMode(int submode)`

Single entry point for call-triggered mode switching. Replaces 4 duplicated blocks.

**Behavior:**
```
if submode == FT2 and current != FT2:
    save current as m_prevStandardSubmode
    switchSubmode(FT2)
else if submode != FT2 and current == FT2:
    switchSubmode(m_prevStandardSubmode)
```

**Rules:**
- Only called from `selectCallsign()` and band activity double-click.
- Never called from `tableSelectionChanged` or display rebuilds.
- Uses `switchSubmode()` (lightweight), never `setSubmode()`.

### `callsignSelected()` (modified)

Returns `m_selectedCallsign`. No table lookups, no frequency matching, no fallbacks.

```cpp
QString callsignSelected(bool) {
    return m_selectedCallsign;
}
```

The caller is responsible for determining the callsign before calling `selectCallsign()`.
This function is now just a getter.

## User Actions

### Left Window (Band Activity)

| Action | Behavior |
|--------|----------|
| **Single-click** | 1. Deselect right window. 2. Read callsign from last occurrence of callsign in row text. If inconclusive, fall back to `m_callActivity` lookup by offset. 3. Read submode from row's `m_bandActivity` data (last standard mode or subspace — no fallback needed). 4. If callsign found: `selectCallsign(call, rowSubmode)`. If not found: `clearSelection()`. 5. Call `displayCallActivity()`. |
| **Double-click** | 1. Everything from single-click. 2. Change frequency to row's offset. 3. Display conversation history in center window. |

**Source of truth for callsign**: `m_callActivity` lookup by offset (standard JS8Call behavior).
**Source of truth for mode**: Row's `m_bandActivity` data (what's displayed).

### Right Window (Call Sign Table)

| Action | Behavior |
|--------|----------|
| **Single-click** | 1. Deselect left window. 2. Read callsign from selected row's `Qt::UserRole`. 3. Read submode from `m_callActivity[call].submode`. 4. `selectCallsign(call, submode)`. 5. Call `displayBandActivity()`. |
| **Double-click** | 1. Everything from single-click. 2. Add callsign to outgoing message box via `addMessageText(call)`. |

**Source of truth for callsign**: Row's `Qt::UserRole` data.
**Source of truth for mode**: Most recent mode detected for this callsign as shown in left window (band activity row data).

### Center Window (Conversation History)

| Action | Behavior |
|--------|----------|
| **Single-click** | No action (cursor positioning only). |
| **Double-click** | 1. Parse line text for last valid `CALLSIGN:` pattern. 2. If own callsign before colon, extract target after colon. 3. Read mode from line prefix (N/F/T/S/⚡). 4. If valid callsign found: select it in right table if present, `selectCallsign(call, lineSubmode)`. 5. If no valid callsign: `clearSelection()`. |

**Source of truth for callsign**: Parsed from displayed text (last `CALLSIGN:` pattern).
**Source of truth for mode**: Line prefix character.

### Deselect Button

| Action | Behavior |
|--------|----------|
| **Click** | `clearSelection()` |

### Mode Buttons (N / F / T / ⚡)

| Action | Behavior |
|--------|----------|
| **Click** | `setSubmode(mode)` — full reconfiguration. Does NOT change selected callsign. Does NOT trigger `selectCallsign()`. |

**Rule**: Mode buttons override call-based mode. If user explicitly picks a mode,
that choice stands until the next callsign selection.

## `tableSelectionChanged` (signal handler)

**This handler should do minimal work:**

```cpp
void tableSelectionChanged(...) {
    currentTextChanged();
    ui->extFreeTextMsgEdit->setFocus();
}
```

No callsign lookup. No mode switching. No `callsignSelectedChanged()`.
All selection logic is handled by the explicit click handlers above.

## What Gets Removed

1. `callsignSelectedChanged()` — replaced by `selectCallsign()` and `clearSelection()`
2. `m_prevSelectedCallsign` — replaced by `m_selectedCallsign`
3. Mode switch blocks in `on_tableWidgetRXAll_cellClicked` — moved to `autoSwitchMode()`
4. Mode switch blocks in `on_tableWidgetRXAll_cellDoubleClicked` — removed (single-click already did it)
5. Mode switch blocks in event filter — moved to `autoSwitchMode()`
6. `callsignSelected()` fallback logic — becomes a simple getter

## What Does NOT Change

- `setSubmode()` / `setupJS8()` — unchanged, used by mode buttons and menu
- `switchSubmode()` — unchanged, used by `autoSwitchMode()`
- `displayBandActivity()` / `displayCallActivity()` — unchanged
- `displayTextForFreq()` — unchanged
- `addMessageText()` — unchanged
- Band activity grouping (FT2 vs standard separation) — unchanged
- Dedup cache, decode pipeline, TX path — unchanged

## State Variables

| Variable | Purpose | Written by | Read by |
|----------|---------|-----------|---------|
| `m_selectedCallsign` | Currently selected callsign | `selectCallsign()`, `clearSelection()` | `callsignSelected()`, `updateButtonDisplay()` |
| `m_nSubMode` | Current operating mode | `setSubmode()`, `switchSubmode()` | everywhere |
| `m_prevStandardSubmode` | Saved standard mode for FT2 toggle | `autoSwitchMode()` | `autoSwitchMode()` |

## Testing Checklist

- [ ] Single-click left window with callsign: selects call, switches mode
- [ ] Single-click left window without callsign: clears selection
- [ ] Single-click left window subspace row: switches to subspace
- [ ] Single-click left window standard row while in subspace: switches back
- [ ] Double-click left window: selects call, switches mode, shows history
- [ ] Single-click right window: selects call, switches mode
- [ ] Double-click right window: selects call, adds to message box
- [ ] Double-click center window on CALL: line: selects call, switches mode
- [ ] Double-click center window on WM8Q: TARGET line: selects TARGET
- [ ] Double-click center window on no-callsign line: clears selection
- [ ] Deselect button: clears selection
- [ ] Mode button click: changes mode, does NOT change selected call
- [ ] Mode button click with call selected: mode stays, not overridden by call's mode
- [ ] Rapid clicking between rows: no flicker, no stale state
- [ ] Send button: no flash during mode changes
