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
Explicit refresh rates use progressive scan-line ordering, as required by the Windows CCD API;
unspecified scan-line ordering is only valid with an unspecified zero/zero refresh rate.
An unsupported mode fails explicitly instead of temporarily streaming a lower resolution.
Privacy requires a clean physical starting topology. Locked-console and headless starts are rejected
when Windows cannot prepare the private topology. Use the existing Desktop entry to unlock first.

## Lifecycle and recovery

The controller uses native stream start/end calls, not log parsing or UDP-port heuristics.
A setup with no real stream expires after 30 seconds. Its launch-specific lease must be committed before
video starts; late, mismatched and repeated commits are rejected. Resume failures roll back as launch failures do.
Experimental native applications currently allow one stream at a time and end after eight hours.
A dedicated worker owns the named mutex shared with the retained PrivacyGuard and DisplayKeeper.
The physical snapshot is committed before the adapter is enabled. Startup reads a bounded recovery journal.
Unsuccessful recovery retains its journal and adapter rather than disabling the only remaining output.
Lock confirmation precedes restore; the worker attaches to the locked input desktop without unlocking it.
If a captured physical monitor disappears, recovery requests a connected physical topology from Windows.
Physical keyboard events are blocked during privacy, injected Moonlight events remain allowed.
Press and release F12 three times within two seconds for local emergency recovery.
This is an input convenience, not protection against someone with administrative or physical access.

Ordinary physical use keeps the original display keeper for connector-specific saved preferences.
Native session restore preserves the captured primary, positions and modes; it does not force Win+P internal-only.
An exact layout cannot be restored when its physical monitor has been unplugged; fallback and connector-specific
preferences still need real hotplug validation. The original scripts remain the operational fallback.

## Black-frame scope

Preparation before encoder probing removes the scripted physical-to-virtual transition after video starts.
Windows may still blank physical monitors during a required modeset. This branch does not claim zero black
frames for GPU resets, driver failures, lock/unlock or client decoder restarts. No stale video frames are
cached across clients or sessions to hide these events. A private target that disappears or changes identity
never permits capture to fall back to another monitor; a privacy failure requests stream termination.

## Setup diagnostics

Normal log level now records a numeric setup session ID, requested mode, locally
buffered GameStream XML response status and elapsed time, RTSP TCP acceptance and bounded RTSP
command stages. No pairing certificate, session key, query string, or client name
is included in these diagnostic events. `HS_HTTP_RESPONSE_BUFFERED` means only that
the response was written to a local buffer; neither network transmission nor client
receipt is confirmed. `gamestream_status` is the XML status, not the HTTP status.
Elapsed time ends at buffering, not at network send or receipt. Session 0 means no
setup session was allocated, so it cannot correlate separate failed requests.

Expired pending setup is classified as `HS_NO_RTSP_CONNECTION` (no RTSP TCP accepted),
`HS_RTSP_REQUEST_TIMEOUT` (TCP accepted, no parsed command), or
`HS_RTSP_NEGOTIATION_TIMEOUT` (commands received, setup not completed). These codes
identify observed stages, not a proven firewall or relay failure. `PLAY_received`
does not prove the ENet control channel or video stream connected. `HS_PENDING_BUSY`
identifies a new setup rejected by an existing pending RTSP event.

`ND_MODE_UNSTABLE` includes both requested dimensions/refresh and the last measured
CCD mode. It remains a privacy failure: the host does not substitute physical capture.
If CCD substitutes a cached size after adapter activation or resume, Sunshine now
enumerates the verified virtual output's driver modes and tests/applies the exact
requested dimensions and refresh with ChangeDisplaySettingsEx. The subsequent
three-sample CCD verification still must pass. `ND_MODE_UNSUPPORTED` means that the
driver did not advertise or could not apply the exact request; it is not a network
or WOL failure. For example, a client requesting 1280x720 at 30 Hz cannot use a driver
configuration containing only 1920x1080, 2560x1440, 3840x2160 and 2246x1080. Add the
missing mode to the virtual driver configuration, then reload the adapter while no
stream is active. Do not disable the exclusive-target checks to mask this error.
Android Moonlight's own generic error 0 can be generated by its HTTP I/O or XML
exception handling before receiving a usable host response. Changing host logs
cannot replace that client-side dialog; client changes are a separate deployment.

## Validation gates

DXGI capture resolves the output's live HMONITOR with GetMonitorInfoW and binds both
device names to a fresh CCD snapshot of the exclusive virtual target at the requested
mode. It does not compare the DXGI render GPU LUID with the CCD indirect display
adapter LUID: on this host they differ for the same virtual output. Invalid handles,
stale names, physical or additional targets, and changed modes still fail closed.

The isolated GTest target compiles the actual Win32 implementation without changing host displays.
The Windows workflow also builds the complete Sunshine application and runs its existing validation.
Passing synthetic tests does not constitute a successful Moonlight hardware test.

Before deployment, test internal-only, external-only and extended modes both locally and through Moonlight,
with repeated disconnect/resume, canceled handshakes, unsupported modes, service crash recovery and physical
hotplug. Verify no invisible desktop remains, correct primary and geometry return, and legacy entry still works.
Do not replace the installed service until these checks and the complete build are successful.
