# Idle look-around and player animation tracing

`WaitHold_Nrml_LookAround` ships in the owned game's
`Pack/Player.pack`, inside `Model/Player00_anim.szs` and
`Model/Player01_anim.szs`. Its FSKA clip is 330 frames long, but the v16
executable's 193-entry motion table at `0x100D9AC0` never requests it.
Normal weapon-holding idle (`WaitHold_Nrml`) is slot 57.

## Working implementation: direct handles v4

`recompiler/port/hooks/player_anim.cpp` remembers each primary controller
when the motion constructor (`0x026227C8`) registers slot 57. After the
constructor finishes, it resolves both idle names in the model's resident
BFRES skeletal dictionaries, following the resource chain used by
`02983BC4`. It accepts the alternate only if the resolved normal-idle handle
matches the handle the game already registered.

Skeletal handles have type zero in their upper half and a cumulative index
across resource files in their lower half. In the owned Player00 archive,
normal/look-around are indices 166/167; Player01 uses 164/165. The code
searches by exact name and never hardcodes these indices.

After five seconds continuously in slot 57 without an override, the hook
replaces the skeletal handle value in the slot's existing one-element buffer
and calls `025D06A4(controller, 57, 1, 1, 0)` with a 10-frame blend. After
330 motion updates it restores the original value and plays normal idle.
Movement, shooting, squid transitions and animation overrides restore the
original value without adding a playback call that could interrupt the new
action. The look-around plays once per uninterrupted idle period.

This changes one handle value. All six slot-array entries, the variation
count and the game-owned buffer pointers remain intact. Auxiliary tracks
continue using normal idle. The hook makes no guest allocations and never
calls the constructor-only registrar during playback. An unavailable clip,
unsupported slot layout or unmatched normal-idle handle preserves the
original animation. `RBREW_IDLE_LOOKAROUND=0` disables the feature.

## Diagnosis of earlier builds

The original hook re-registered a populated slot with `025CFB18`. The
single-name path allocates room for one skeletal handle, but appends at the
slot's previous count (`025CFC24..025CFC64`). Repeating it with count 1 can
write beyond that allocation. It also depends on the game's allocation
context, so it is unsuitable for calls from an arbitrary motion-update hook.

The v2/v3 live traces reported `count=1 skeletal=FFFFFFFF available=0`.
The v3 dictionary fallback did not execute because it was guarded by the
presence of a usable newly registered handle buffer. Earlier tests had
masked that failure by always providing a valid buffer. The working v4
implementation removes re-registration and the new buffer entirely.

## Verification (2026-10-22)

The authored `render_idle_lookaround` regression resolves clips from resident
dictionary fixtures and refuses any registrar call by the idle hook. It
checks the five-second delay, actual played handle, blend, 330-update
completion, exact restoration, once-per-idle behavior, movement/shooting/
squid/override interruptions, unchanged buffer ownership and auxiliary
tracks, no repeated registrations, missing-clip fallback, controller-address
reuse, caller-register preservation and no writes through address zero.
`render_guest_hooks` also passes.

The optional private `idle_asset_lookup` probe checked both actual owned
Player00/01 BFRES dictionaries and resolved the expected pairs. No game
assets are bundled with the tests.

A private game copy using a copied save then produced
`direct normal=000000A6 registered=000000A6 alternate=000000A7 ... available=1`
in both the plaza and Octo Valley. Its trace recorded playback starting at
71,545.3 ms and finishing at 78,679.8 ms. The user confirmed that the
animation works. Evidence is retained in
`build/lookaround-live-game/live-anim.log`; the working executable was copied
to `Game/Splatoon.exe` and its SHA-256 checked against the built executable.

## Recording another trace

1. Run `scripts/AnimTrace.bat`. It launches `Game/Splatoon.exe` and writes
   `scripts/anim-trace.log` (up to 60,000 lines).
2. Enter Octo Valley, a battle or Battle Dojo where the player holds a weapon.
   Stand still for about 15 seconds, then move, shoot and transform. Plaza
   idle uses a different slot and does not trigger this feature.
3. Quit the game so any buffered log output is flushed.

The first line identifies `direct handles v4`. The trace includes original
constructor/controller calls, resolved handle checks, motion-slot changes,
and look-around start, completion and interruption events. Preparation logs
are flushed immediately for live inspection.

The manifest's `[hooks]` section lists the wrapped guest entry addresses. Setup
passes the manifest to the recompiler and includes its hash in the regeneration
stamp. Wrappers call the original body whenever no hook is registered; tracing
is enabled only when `RBREW_ANIM_LOG` names a file.
