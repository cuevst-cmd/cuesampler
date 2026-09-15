# CUE SAMPLER — Implementation Plan
## A. Make the MIDI mapping visible  ·  B. Direct BARS selector with time-position edit preservation

**Written against:** `248c413` (1.0.8) · `PluginEditor.cpp` 7,731 lines · `PluginProcessor.cpp` 7,126 lines

---

## Why these two together

They share a dependency. Part B's edit-preservation rewrite has to decide what happens
to a chop's MIDI assignment when chops merge or split — and that decision is only safe
once there is a single authority on what note plays what chop. Part A builds that
authority. **A0 is a hard prerequisite for B2.**

---

# PART A — Visible MIDI mapping

## A0. One resolver, one map  *(foundation — do this first)*

### The problem

There are two mapping systems and they disagree.

The audio thread resolves explicit-first (`PluginProcessor.cpp:3599`):

```cpp
for (const auto& candidate : currentChopState->chops)      // explicit wins
    if (candidate.assignedMidiNote == noteNumber) { targetChop = &candidate; break; }

if (targetChop == nullptr) {                                // positional fallback
    const auto chopIdx = noteNumber - effectiveRootNote;
    if (chopIdx >= 0 && chopIdx < (int) chops.size())
        if (chops[chopIdx].assignedMidiNote == -1) targetChop = &chops[chopIdx];
}
```

The UI resolves positionally, with no collision check (`PluginProcessor.cpp:5178`):

```cpp
if (chops[i].assignedMidiNote >= 0) return chops[i].assignedMidiNote;
if (chops[i].assignedMidiNote == -1) return getMidiRootNote() + (int) i;
```

So if a manual chop at index 3 is pinned to C2, and chop 0 is a `-1` positional chop
that also resolves to C2, the audio thread plays **chop 3** and `getMidiNoteForChopId(chop0)`
still reports **C2**. Chop 0 is silently dead and the API will happily tell the UI otherwise.

This is the audit's item #7. Any note name we draw on top of today's API would be a lie,
so the resolver has to come first.

### The fix

One resolver, built once per state change, consumed by both threads.

```cpp
struct ChopMidiMap
{
    std::array<int, 128> noteToChopId;   // -1 = no chop on this note
    std::vector<int>     chopIdToNote;   // parallel to chops; -1 = UNREACHABLE
};
```

Resolution order — matches today's audio behaviour, made total and explicit:

1. `assignedMidiNote >= 0` claims its note. On collision the **lowest chop index wins**
   (matches the `break` in the audio loop today).
2. `assignedMidiNote == -1` claims `root + index`, **only if still free**.
3. `assignedMidiNote == -2` never maps.
4. Any chop that loses a collision gets `chopIdToNote = -1` — *unreachable*. This is a
   real state the UI must render, not an error to hide.

**Publishing.** Store as `std::shared_ptr<const ChopMidiMap>` beside `chopState`, published
with `std::atomic_store`. Rebuild on:

- every `std::atomic_store(&chopState, …)` site, and
- `setMidiOctaveOffset` (`PluginProcessor.cpp:5157`) — it changes `root` **without** swapping
  `chopState`, so a map keyed on absolute notes goes stale there. Easy to miss.

**Bonus:** this also closes audit #8. `handleMidiEvent` becomes an O(1) array lookup instead
of a linear scan over all chops on the audio thread.

**Blast radius is small.** `getMidiNoteForChopId` has exactly one real consumer today
(`PluginEditor.cpp:7594`, the keyboard highlight). Reimplementing it on top of the map is low risk.

---

## A1. Note name on every chop header

**Where:** `PluginEditor.cpp:4858–4883`, inside `paintChops` (4643).

The header tab is 15 px tall. Today it carries the 1-based number (`centredLeft`,
`reduced(5,1)`) and an optional warp glyph at `header.getRight() - 12`.

**Add:**

- Note name `centredRight` when `header.getWidth() >= 52.0f` — number left, note right.
- When both note name and warp glyph are present, require `>= 72.0f` and move the glyph
  to sit between them (`getRight() - 30`). Below that, note name wins; the glyph already
  has a width gate at 34 px to follow as precedent.
- **Unreachable chops** (`chopIdToNote == -1`): draw a dimmed `—` where the note would go.
  This is what turns audit #7 from an invisible bug into a visible state — the user can
  finally *see* that a chop has no pad.

**Refactor first:** `midiNoteName` (`PluginEditor.cpp:6518`) is a private static inside
`TransportSectionComponent`. `WaveformDisplayComponent` is a different class. Hoist it into
the `cue` namespace rather than duplicating it.

> Sanity check: the helper computes `octave = noteNumber/12 - 1`, so note 36 → `"C2"`,
> which matches `GlassKeyboard`'s `setOctaveForMiddleC(4)` (`PluginEditor.cpp:1506`).
> The two naming schemes already agree — don't "fix" either.

---

## A2. Light the keys that have chops

**Where:** `GlassKeyboard`, `PluginEditor.cpp:1500`.

The keyboard spans C1–B7 — 84 keys (`setAvailableRange(24, 107)`). Only `highlightedNote`
ever lights. With 8 chops mapped, 76 dead keys look exactly like the 8 live ones.

**Add:**

```cpp
void setMappedNotes (const std::bitset<128>& notes);   // repaint if changed
```

Override `drawWhiteNote` / `drawBlackNote` (already overridden at 1538/1546) to wash mapped
keys in a low-alpha accent, *underneath* the existing pressed overlay.

Three tiers must stay visually distinct:

| state | treatment |
|---|---|
| unmapped | flat cream / near-black, as today |
| mapped, silent | low-alpha accent wash |
| sounding | full `keyDownOverlayColourId` accent, as today |

**Refresh hook:** the main editor already subscribes to `editChangeBroadcaster`
(`PluginEditor.cpp:7173`, callback at 7470). Push the update from there — no new timer.
Also refresh on octave change, for the same staleness reason as A0.

---

## A3. Reassign a pad — drag, and MIDI learn

Both entry points need one new processor API:

```cpp
void setChopAssignedMidiNote (int chopId, int note);   // -2 to unassign
```

Reuse the dedup rule that `addManualChop` already establishes
(`PluginProcessor.cpp:6503–6507`): **newest assignment wins, previous holder demoted to -2.**
Push an undo snapshot, then `touchTempoUiRevision()` + `notifyEditStateChanged()`, matching
every sibling mutator.

### (a) Drag a chop header onto a key

The editor is already a `DragAndDropContainer` — the export drag at `PluginEditor.cpp:3845`
uses it. This is an *internal* drag (`startDragging`), with `GlassKeyboard` becoming a
`DragAndDropTarget` that maps x → note via `getNoteAndVelocityAtPosition`.

**Watch:** the chop header already owns click-to-select and double-click-to-favourite
(`PluginEditor.cpp:3375`). Gate the drag behind a movement threshold or it will eat both.

### (b) MIDI learn

`keyboardState.processNextMidiBuffer (…, true)` at `PluginProcessor.cpp:2573` means *all*
incoming MIDI — hardware and on-screen — flows through `keyboardState`. So a
`juce::MidiKeyboardState::Listener` is the clean hook.

> ⚠️ **`handleNoteOn` fires on the audio thread.** Marshal to the message thread before
> touching chop state. `juce::MessageManager::callAsync` with a `Component::SafePointer`,
> the pattern already used at `PluginEditor.cpp:7480`.

Arm learn on the selected chop; the next note-on assigns and disarms.

---

## A4. Retire the one-line hint

`getMidiMappingText()` (`PluginEditor.cpp:6556`) renders one 8.5 pt line at 6103:

> `MIDI: C2 = chop 1,  C#2 = chop 2 ...`

Once notes sit on the headers and the keys are lit, this is redundant. Replace it with live
status drawn from the A0 map — root note, how many chops are mapped, and a warning when any
chop is unreachable (`3 CHOPS UNREACHABLE`). That warning is the user-facing surfacing of
audit #7.

---

# PART B — BARS as a direct selector

## B1. Four segments instead of a cycle

**Today:** a single `SmoothHoverButton barsButton` (`PluginEditor.cpp:6607`), 104 × 40 px at
`centerBlockX + 140` (6145), cycling `1 → 2 → 4 → 8 → 1` (7213–7218). Getting from 1 to 8
means three clicks, each one a full destructive rebuild of the chop list.

**Replace with** four segments in the *same* 104 px box — 26 px each, which fits single digits
at 9.5 pt mono comfortably. **No layout change required.**

> Headroom, if you want a roomier control: at the 1266 px default the centred knob bank starts
> at x = 463 and BARS currently ends at x = 424 — **39 px spare**. Widening
> `barsToolButtonWidth` (5826) from 104 to 124 stays clear; 132 leaves only 11 px.

**Build:** a new `cueStyle = "segment"` painter in the LookAndFeel (styles are dispatched at
`PluginEditor.cpp:1058–1290`), driving four `SmoothHoverButton`s with `setRadioGroupId`,
`setClickingTogglesState(true)`, and `setConnectedEdges` for the joined look.

**Two cleanups that come with it:**

- Tooltip at 5916 still says *"cycles 1 / 2 / 4 / 8"* — reword.
- **Guard the no-op.** `setChopBarsCount` (`PluginProcessor.cpp:5940`) skips the undo snapshot
  when the value is unchanged, but still calls `buildChopsFromAnalysis`. With a cycling button
  that was unreachable; with a segmented one, clicking the active segment is a normal gesture.
  Add an early return.

---

## B2. Preserve edits by time position, not index

### The problem

`buildChopsFromAnalysis` carries per-chop edits across a rebuild by **array index**
(`PluginProcessor.cpp:5828`):

```cpp
if (existingState != nullptr && chopIndex < (int) existingState->chops.size())
{
    const auto& old = existingState->chops[(size_t) chopIndex];   // ← index, not time
    …
}
```

`computeGridTimingMetrics` (`PluginProcessor.cpp:5877`) holds `gridAnchorSeconds` and
`barPeriodSeconds` **fixed** across a bars change — only `chopPeriodSeconds = barPeriod × barsPerChop`
scales. So the boundaries at 1 bar are a superset of 2 bars ⊃ 4 bars ⊃ 8 bars, all anchored
identically.

Going 1 → 2 bars, new chop *k* spans old chops *2k* and *2k+1*. The code hands it old chop *k*.

| new chop | covers | inherits edits from | should be |
|---|---|---|---|
| 0 | bars 0–1 | old chop 0 (bar 0) | ✅ |
| 1 | bars 2–3 | old chop 1 (bar 1) | ❌ old chop 2 |
| 2 | bars 4–5 | old chop 2 (bar 2) | ❌ old chop 4 |
| 3 | bars 6–7 | old chop 3 (bar 3) | ❌ old chop 6 |

The drift compounds with *k*. Chop 3's envelope, cue point and warp markers land on audio
they were never authored against.

### The fix

Match by start time. Old chops are already sorted by `startSample`, so this is a linear merge
walk — O(n + m), not a nested scan.

```
for each new chop:
    match = old chop containing newChop.startSample
            (else nearest old start within ± half a chop)
```

**Per-field rules:**

| field | rule |
|---|---|
| `gainDecibels`, `pitchSemitones`, `favorite`, `reversed`, ADSR | inherit from match, unconditional |
| `warpMarkers` | absolute `sourceSample`; the existing bounds filter (5842) is already correct under time matching — keep it |
| `cueOffsetSamples` | **relative to chop start.** Carry directly only when starts coincide (≈1 ms). Otherwise re-base as `oldStart + oldCue − newStart`, and if that falls outside the new chop, drop to auto-cue. Today it is carried blindly whenever `> 0` (5830), which is already wrong for any shifted start. |
| `assignedMidiNote` | **the hazard — see below** |

### The assignment hazard

- **Merging** (1 → 2 bars): two old chops collapse into one. Only one assignment survives.
- **Splitting** (2 → 1 bar): two new chops inherit the *same* note from one old chop — creating
  exactly the duplicate-note collision that silently kills a chop.

**Rule:** the first (lowest-index) claimant keeps the note; the rest reset to **`-1`**
(fall back to positional), **not** `-2` (which would leave them permanently dead). The A0
resolver then reports any residual collision honestly instead of hiding it.

This is why A0 lands first.

---

## B3. The same fix repairs three other callers

`buildChopsFromAnalysis` is also called from `setGridBpmTrim`, `setGridStartOffset`, and
`resizeChopBoundaryAndTempo`. All three slide boundaries in time while index-preserved edits
stay put — the same drift, just triggered by a different control. Time matching fixes all
callers at once.

The trade-off: a large grid nudge will now legitimately re-home edits rather than leaving
them pinned to an index. The tolerance is what governs this — **± half a chop** is the
recommended starting point.

---

# Sequencing

| # | Task | Depends on | Notes |
|---|---|---|---|
| 1 | **A0** resolver + map | — | unblocks everything; also closes audit #7 and #8 |
| 2 | **A1** header note names | A0 | cheapest visible win |
| 3 | **B2** time-position preservation | A0 | needs A0's dedup rule |
| 4 | **B1** segmented selector | — | fully independent, land it whenever |
| 5 | **A2** keyboard lighting | A0 | |
| 6 | **A3** drag + MIDI learn | A0 | largest surface, most interaction risk |
| 7 | **A4** replace the hint line | A1, A2 | only once the new cues are in |

---

# Testing

There are no automated tests (audit #17). **Both** new pieces of logic are pure functions of
their inputs and are the natural place to start:

- `ChopMidiMap` construction — collisions, `-1` / `-2` semantics, octave shift, unreachable chops.
- The time-position matcher — merge (1→2), split (2→1), grid nudge, cue re-basing, note dedup.

Neither needs an audio device or a host. Add `enable_testing()` to CMake.

**Manual checks to append to `MANUAL_TEST.md`:**

- [ ] Pin a manual chop to C2 while a positional chop already resolves to C2. The positional
      chop's header shows the unreachable dash, and the status line reports it.
- [ ] Every chop header note name matches the key that actually fires it, at every octave offset.
- [ ] `OCT +/−` updates both header names and the lit keys immediately.
- [ ] Mapped / silent / sounding keys are distinguishable in both light and dark themes.
- [ ] Drag a chop header onto a key — it takes that pad, the previous holder goes unassigned,
      and a single undo restores both.
- [ ] Drag threshold: a plain click still selects, a double-click still toggles favourite.
- [ ] MIDI learn from a hardware controller with a 64-sample buffer — no audio-thread assertion.
- [ ] Set a distinct envelope + cue on chop 4 at 1 BAR, switch to 2 BARS: the edits follow the
      *audio*, not the index.
- [ ] Switch 1 → 2 → 4 → 8 → 1 and confirm no chop ends up sharing a note with another.
- [ ] Click the already-active BARS segment — nothing rebuilds, no undo slot consumed.
- [ ] Nudge TEMPO trim and grid offset hard; edits re-home sensibly and no duplicate notes appear.

---

# Risks

**No state-format bump needed.** `assignedMidiNote` is already serialized
(`PluginProcessor.cpp:3770` / `3926`); A3 only writes an existing field. The map is derived,
never persisted. Version stays at 5 — which is just as well, given audit #13 (nothing reads it).

**Audio-thread discipline.** Two places to be careful: the `MidiKeyboardState::Listener`
(A3b) fires on the audio thread, and the A0 map must be published with the same
release/acquire discipline as `chopState` so `handleMidiEvent` never reads a torn map.

**The TU problem.** All of A1–A4 and B1 land in `PluginEditor.cpp`, which is 7,731 lines in a
single translation unit — audit #16, still open. Every iteration on this work pays the full
recompile. Splitting `WaveformDisplayComponent` out first is pure code movement and would pay
for itself over the course of this plan.
