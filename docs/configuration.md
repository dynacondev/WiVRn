# Configurable items

Configuration is split between headset and server. Headset contains most configuration, while server has system specific items such as encoders and paths.
Files are read from
- `/usr/share/wivrn/config.json` (where `/usr` is selected at configure time with `CMAKE_INSTALL_PREFIX`)
- `/etc/wivrn/config.json`
- `$XDG_CONFIG_HOME/wivrn/config.json` or if `$XDG_CONFIG_HOME` is not set, `$HOME/.config/wivrn/config.json`.

Files later in the list replace top-level values from previous ones.

If you installed WiVRn from a flatpack, host files are also searched: `/run/host/usr/share/wivrn/config.json`, `/run/host/etc/wivrn/config.json` and the host `$HOST_XDG_CONFIG_HOME/wivrn/config.json` (usually `~/.config/wivrn/config.json`, read from outside the sandbox like `model-path` files, logged with `(via host)`), all before the sandbox files. When the host does not export `XDG_CONFIG_HOME` (the usual default setup), `$HOME/.config` is used instead, since `$HOME` names the same location inside and outside the sandbox. The sandbox per-app config in `$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json` is read last, so dashboard edits win. The dashboard home page lists every searched location with its status, and opening any entry shows it in the file browser.
Paths in the config (such as `model-path` below) may be regular host paths: the sandboxed server reads them from outside the sandbox when needed.

All elements are optional and have default values.

## `bit-depth`
Default value: 10-bit if supported by the selected codec, encoder and decoder. 8-bit otherwise.

Bit depth of the video. 8-bit is supported by all encoders. 10-bit is supported by `vulkan`, `vaapi` and `nvenc` with `h265` or `av1` codecs.

## `encoder`
The encoder to use, either a single string or object applied to all streams, or a list of string or objects with values for left, right and alpha.
When a string it is used, it is equivalent to the `encoder` item of the object.

WiVRn encodes each eye separately, and the alpha channel as one for both eyes. Each stream is processed independently, this may use resources more effectively and reduce latency.

### `encoder`
Default value, in order: `vulkan` if supported, `nvenc` on Nvidia GPU, `vaapi` on all other GPU, else `x264`.

Identifier of the encoder, one of
* `x264`: software encoding
* `nvenc`: Nvidia hardware encoding
* `vaapi`: AMD/Intel hardware encoding
* `vulkan`: Vulkan Video encode, cross-vendor hardware encoding

### `codec`
Default value: best supported by both headset and encoder of `av1`, `h264`, `h265`.

One of `h264`, `h265`, `av1`, `raw`.

Not all encoders support every codec:
- `x264` encoder only supports `h264` codec
- `vulkan` encoder supports `h264` and `h265` codecs
- `raw` encoder only supports `raw` codec
- `nvenc` and `vaapi` support all codecs, except `raw`

If `nvenc` encoder is in use, you can refer to [nvidia website](https://developer.nvidia.com/video-encode-decode-support-matrix) to make sure that your GPU supports encoding with the desired codec.

### Examples
1. Simple configuration
```json
{
	"encoder": {
		"encoder": "vaapi",
		"codec": "h265"
	}
}
```
Use vaapi hardware encoding, h265 video codec (HEVC).

2. Hardware + software encoder
```json
{
	"encoder": [
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
		{
			"encoder": "x264",
			"codec": "h264",
		},
		{
			"encoder": "vaapi",
			"codec": "h265",
		},
	]
}
```
Creates a hardware encoder for left eye and transparency, and a software encoder for right eye.

### `device`, only for vaapi
Default value: unset

Manually specify the device for encoding, can be used to offload encode to an iGPU. Device shall be in the form "/dev/dri/renderD128".


### `options` (very advanced), only for vaapi
Default value: unset

Json object of additional options to pass directly to ffmpeg `avcodec_open2`'s `option` parameter.

## `application`
Default value: unset

An application to start when connection with the headset is established, can be a string or an array of strings if parameters need to be provided.

### Example
```json
{
	"application": ["steam", "steam://launch/275850/VR"]
}
```
Launch No Man's Sky in VR mode on Steam when connection with headset is established.

## `tcp-only`
Default value: `false`

Only use TCP for communications with the client, this may have increased latency.
If `false` or unset, WiVRn will use both TCP and UDP.

### Example
```json
{
	"tcp-only": true
}
```

## `publish-service`
Default value: `avahi`

How to publish the service over the network, `avahi` or null.

If set to null, service will not be published and address has to be entered manually on the headset.

## `openvr-compat-path`
Default value: unset

Provides the path to the directory of an OpenVR compatibility tool (such as OpenComposite).

If unset, WiVRn will autodetect the path of such a tool as usual (see [the SteamVR guide](./steamvr.md)).

If set to an null, WiVRn will not manage the OpenVR configuration.

## `hid-forwarding`
Default value: `false`

Only available when the `uinput` kernel module is loaded and the user has write access.

Mirrors input devices forwarded from the headset (keyboard, mouse, gamepad) to `uinput`
devices, for applications that do not read them through OpenXR. Which devices are forwarded is
chosen on the headset.

A forwarded gamepad is also exposed as a native OpenXR gamepad at `/user/gamepad`, which needs
no permission and is always available. Only the digested controller state is forwarded
(buttons, two sticks, analog triggers and a d-pad), so device specific features such as gyro,
adaptive triggers or the touchpad are not available.

## `debug-gui`
Default value: `false`

Only available when built with `WIVRN_FEATURE_DEBUG_GUI`.

Enables the Monado debug gui.

## `use-steamvr-lh`
Default value: `false`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Enables the driver to load SteamVR Lighthouse devices.

## `lh-max-extrapolation`
Default value: unset

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`.

Maximum time in milliseconds that poses may be extrapolated ahead for SteamVR Lighthouse devices.

## `lh-stick-deadzone`
Default value: `0`

Only available when built with `WIVRN_FEATURE_STEAMVR_LIGHTHOUSE`

Applies a deadzone to joysticks on SteamVR controllers (e.g. Index).

## `port`
Default value: `9757`

Change the TCP/UDP port used for the connection.

## `hostname`
Default value: unset

If set, overrides the name displayed in the server list.

## `fade-in-ms`
Default value: `1000`

Global passthrough fade-in at first acquisition, in milliseconds
(`0` = appears instantly). Render-only; applied live. One clock for
all objects: it starts on first placement per map and retriggers on
reacquire jumps, so every window ramps together. Objects placing later
appear instantly. Per-object `fade-in-ms` entries are warned + ignored
(a single alpha channel admits exactly one clock).

## `suppress-meta-boundary`
Default value: `true`

Suppress the Meta (OS) boundary while WiVRn passthrough is shown.
Pushed to the headset on connect (there is no in-headset toggle for
this). This is a request: the runtime may refuse, and the boundary is
always shown in opaque VR regardless. The headset logs which verdict
the server sent on every map receipt.

## `fiducials` and `passthrough`
Default value: unset (feature disabled)

Quest fiducial tracking + behavior objects (see `ROADMAP.md`). Tracking
(`fiducials`) is separate from behavior (`passthrough` objects that
reference fiducials by id): the same QR printed twice places the object
twice (one instance per sighted marker), and one object may reference
several fiducials (one instance per visible pairing, never fused).

The Quest runtime tracks **QR codes** (its AprilTag capability
is not exposed to third-party apps), so each marker is identified by
the exact decoded QR payload string. The server sends both maps plus
model content hashes to the headset on connect, and serves model
bytes on demand. The headset caches models by hash, so files are
transferred once.

Each `fiducials` entry has:

- `id`: stable link key referenced by passthrough objects (required,
  must be unique)
- `tag`: display-only label shown in the headset status UI (optional;
  renaming never affects tracking or linkage)
- `static`: `true` (default) or `false`. Maps to the runtime's
  `optimizeForStaticMarker`: keep `true` for a stationary rig, set `false`
  for a moving reference marker. Toggling recreates the client spatial
  context (brief tracking hitch).
- Resolver/smoothing tuning (all optional; shared Euro cutoff/beta cover
  position and orientation): `window-size` (default `12`),
  `min-samples` (default `4`), `sigma-k` (default `3`),
  `pos-gain`/`rot-gain` (default `24`, per-second proportional catch-up),
  `euro-min-cutoff` (default `0.4`), `euro-beta` (default `0.07`),
  `knee-inner-mm`/`knee-outer-mm` (defaults `1`/`5`),
  `knee-inner-deg`/`knee-outer-deg` (defaults `0.1`/`0.5`).
- `markers`: list of markers resolving to the one fiducial frame, each with:
  - `marker-data`: exact QR payload string to match, byte-for-byte (required)
  - `marker-size-m`: physical marker size in meters (required for pose
    scale; measure the printed QR's outer edge)
  - `position`: `[x, y, z]` tag center in fiducial meters, in fiducial
    axes (default `[0,0,0]`). This is what a ruler measures: where the
    QR sits relative to the model origin, along the model's axes —
    regardless of the angles below. Correcting a position moves the
    code's assumed spot along world axes; the model compensates
    opposite to keep a glued tag glued.
  - `orientation`: `[rx, ry, rz]` tag orientation relative to fiducial
    axes, in degrees (default `[0,0,0]`). Fixed-frame rotations about
    X, then Y, then Z, so single-axis values do the obvious thing
    (e.g. `[0,90,0]` yaws 90°). Absolute, not incremental: editing it
    spins the assumed tag pose about the tag center, and the model
    pivots about the tag — the tag never slides across the model.
  - The client inverts these once at load (vote = observed × offset⁻¹),
    so authoring stays in model coordinates throughout.

> [!NOTE]
> A QR payload is a single identity: printing the *same* payload twice
> does not create two trackables. The runtime reports one jumping track
> for identical prints, so one payload anchoring two places is not
> supported. For multi-location coverage, print distinct payloads (one
> per location) and link a single object to all of their fiducials —
> the supported direction is many objects to one code, never one code
> to many places.

Each `passthrough` entry (behavior object) has:

- `type`: behavior type. `"3d-passthrough"` renders the model as a
  surface-projected passthrough cutout. `"3d-passthrough-reversed"`
  reverse-cuts the model from the mask instead: game video shows through
  the silhouette (e.g. a virtual screen floating in front of a real-desk
  window). Reversed objects cut every mask layer and inherit each
  layer's feathering through its blur; a negative cuts only where it is
  nearer than the windows covering that pixel (depth-tested per layer),
  so a distant reversed object behind a nearer window never cuts it.
  Reversed objects need the mask path (they stay inactive on the binary
  projected fallback) and do not accept `feather-px` (warned + ignored).
  `"3d-boundary"` shows game video only *inside* the model volume and
  passthrough outside it — a Quest-guardian-style zone with adjustable
  size, shape and fade. `"3d-boundary-reversed"` inverts it (passthrough
  inside, game outside: exclusion zones around e.g. a desk). Zones
  combine as a union: game video shows iff the headset is inside ANY
  normal zone (or no normal zone is placed) AND outside ALL reversed
  zones; unplaced zones are ignored (fail-open to game, so tracking loss
  never flashes passthrough). Boundary meshes must be **closed volumes**
  (inside/outside comes from ray parity, undefined for open meshes);
  keep them simple (room-size boxes are ideal, ~5k triangles budget).
  Both eyes fade together from a single per-frame headset distance.
  Unknown types are skipped with a warning (forward-compat for
  `3d-depth`).
- `id`: stable identifier (required, must be unique); `tag`: display-only
  label (optional)
- `fiducial`: referenced fiducial id, or list of ids (required; dangling
  references are dropped with a warning, and an object left with none is
  skipped). Relationships fan out: many objects may reference one
  fiducial, and one object may reference several fiducials (one live
  instance per visible pairing, never fused).
- `model-path`: path to a `.glb`/`.gltf` file on the server (optional,
  models larger than 64MB are skipped). Under flatpak this may be a host
  absolute path (e.g. `/home/user/models/widget.glb`); the server reads it
  from outside the sandbox, and the server log shows `(via host)` when it does.
- `position`: `[x, y, z]` fiducial-to-object offset in meters
  (default `[0,0,0]`)
- `orientation`: `[rx, ry, rz]` fiducial-to-object rotation in degrees
  (default `[0,0,0]`, same fixed-frame convention as above)
- `scale`: uniform object scale, number or single-element array
  (default `1`, objects only, applied last)
- `feather-px`: passthrough window feather width in screen pixels
  (alpha-gradient blend band around the mesh silhouette, default `24`;
  `0` disables feathering). Not accepted for `3d-passthrough-reversed`
  or `3d-boundary(-reversed)` (warned + ignored): negatives cut every
  layer and inherit each layer's feathering, while zones fade by
  distance instead (see `transition-distance`). Mask stacks group by this value so each
  object feathers independently; applied live. The client selects a blur
   tier automatically (0 hard edge, 1 half-res, 2/4 quarter, 8 eighth
   with exact spread mapping, clamped to 128px), so tap density and cost
   stay flat at any width. Submitted mask size follows the tier:
   tier-0 stays full (exact hard edge), tier-1 submits half-resolution,
   tiered submits quarter-resolution (the V pass writes the submitted
   size; the compositor expands to full).
- `transition-distance`: boundary fade width in meters
  (`3d-boundary(-reversed)` only, default `0.3`, applied live). The
  scene cross-fades over ± this distance around the zone surface
  (symmetric smoothstep, 0.5 at the surface); `0` is a hard step.
  Composited by lifting the flat union's mask floor, so no extra layers
  or swapchains: exiting saturates the existing flat feed to fullscreen.
- `surface-projected`: project camera imagery onto this object's mesh
  (surface-projected passthrough) instead of the flat reconstruction
  feed (default `true`; opt out per object with `false`). Prefer
  well-aligned objects: mismatched geometry causes conflicting depth
  cues and wrong scale. Mapped interiors feather through the same mask
  as the flat feed, so edges keep flat quality with geometry-correct
  parallax inside. Not applicable to `3d-passthrough-reversed` or
  `3d-boundary(-reversed)` (warned + ignored): zones never map camera
  onto geometry. Applied live; a per-object override in the
  headset Passthrough tab (as configured / force on / force off) takes
  precedence for the session.

Poses compose as `fiducialPose = observedMarkerPose * inverse(markerOffset)` then
`objectPose = fiducialPose * fiducialToObjectOffset` (scale last).
Marker offsets are tag-in-board (see above); object offsets are
object-in-board, so both sections author in model coordinates.
Objects auto-align on first sighting of a referenced marker, then
smooth-follow without ever snapping, and SLAM-hold forever on marker loss.

> [!WARNING]
> Marker offset semantics changed (tag-in-board, inverted at load).
> Existing configs must invert their marker entries: with identity
> rotation, negate `position`; in general `new = old⁻¹` (negate the
> angles for single-axis entries, e.g. `[0,45,0]` → `[0,-45,0]`, and
> rotate the negated position by the new angles). Verify with a known
> identity case: it must reproduce the old overlay exactly.

Print the QR encoding exactly the `marker-data` string, e.g.
`qrencode -o marker11.png -s 10 "wivrn:11"`. Matching is exact and
case-sensitive; any other QR code in view is reported in the headset
Passthrough tab (red) to help you copy it verbatim into the config.

### Example
```json
{
	"fiducials": [
		{
			"id": "press",
			"tag": "Press rig",
			"static": true,
			"markers": [
				{
					"marker-data": "wivrn:11",
					"marker-size-m": 0.08
				}
			]
		}
	],
	"passthrough": [
		{
			"type": "3d-passthrough",
			"id": "press-window",
			"tag": "Press window",
			"fiducial": ["press"],
			"model-path": "/usr/share/wivrn/meshes/widget.glb",
			"position": [0, 0.05, 0.1],
			"orientation": [0, 0, 0],
			"scale": 1.0,
			"feather-px": 24
		},
		{
			"type": "3d-passthrough-reversed",
			"id": "press-screen",
			"tag": "Virtual screen (game content)",
			"fiducial": ["press"],
			"model-path": "/usr/share/wivrn/meshes/screen.glb",
			"position": [0, 0.3, 0.2],
			"orientation": [0, 0, 0],
			"scale": 1.0
		},
		{
			"type": "3d-boundary",
			"id": "press-room",
			"tag": "Play area around the press",
			"fiducial": ["press"],
			"model-path": "/usr/share/wivrn/meshes/room.glb",
			"transition-distance": 0.3
		}
	]
}
```
