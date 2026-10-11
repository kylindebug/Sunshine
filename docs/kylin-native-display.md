# Kylin native display branch

Base: Sunshine `63d35f702ee9e362e43263742981836ec0710384` (2026.914.233613).
Experimental, opt-in Windows host implementation. Moonlight needs no modification.
The installed production service and previous PrivacyGuard scripts are not changed by this branch.

## Application settings

`native-display` in apps.json and the Windows application editor:

* `disabled` (default): upstream behavior, including existing script applications.
* `physical`: preserve internal-only, external-only or extended physical topology; lock when the last stream ends.
* `privacy`: journal the exact active physical topology; enable the single installed `Root\MttVDD` adapter;
  select its `MTT1337` target alone at the client resolution and refresh rate; verify three stable samples;
  select that capture output before probing the encoder. Restore and disable the adapter on disconnect.

Use a new application with an empty command and no display-changing prep commands.
Keep the existing script-based application separately with `native-display=disabled`.
Do not combine upstream automatic display configuration with this experimental controller.
No drivers are installed, and no network, authentication, firewall or pairing settings are changed.

The supported mode range is 320..7680 pixels on each axis and 1..240 Hz, subject to driver support.
An unsupported mode fails explicitly instead of temporarily streaming a lower resolution.
Privacy requires a clean physical starting topology. Locked-console and headless starts are rejected
when Windows cannot prepare the private topology. Use the existing Desktop entry to unlock first.

## Lifecycle and recovery

The controller uses native stream start/end calls, not log parsing or UDP-port heuristics.
A setup with no real stream expires after 30 seconds. Resume failures roll back as launch failures do.
A dedicated worker owns the named mutex shared with the retained PrivacyGuard and DisplayKeeper.
The physical snapshot is committed before the adapter is enabled. Startup reads a bounded recovery journal.
Unsuccessful recovery retains its journal and adapter rather than disabling the only remaining output.
Physical keyboard events are blocked during privacy, injected Moonlight events remain allowed.
Press and release F12 three times within two seconds for local emergency recovery.
This is an input convenience, not protection against someone with administrative or physical access.

Ordinary physical use keeps the original display keeper for connector-specific saved preferences.
Native session restore preserves the captured primary, positions and modes; it does not force Win+P internal-only.
If a captured physical monitor disappears during streaming, exact recovery may remain pending until it returns.
The original scripts therefore remain the operational fallback until real hotplug validation is complete.

## Black-frame scope

Preparation before encoder probing removes the scripted physical-to-virtual transition after video starts.
Windows may still blank physical monitors during a required modeset. This branch does not claim zero black
frames for GPU resets, driver failures, lock/unlock or client decoder restarts. No stale video frames are
cached across clients or sessions to hide these events.

## Validation gates

The isolated GTest target compiles the actual Win32 implementation without changing host displays.
The Windows workflow also builds the complete Sunshine application and runs its existing validation.
Passing synthetic tests does not constitute a successful Moonlight hardware test.

Before deployment, test internal-only, external-only and extended modes both locally and through Moonlight,
with repeated disconnect/resume, canceled handshakes, unsupported modes, service crash recovery and physical
hotplug. Verify no invisible desktop remains, correct primary and geometry return, and legacy entry still works.
Do not replace the installed service until these checks and the complete build are successful.
