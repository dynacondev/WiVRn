# Configurable items

Configuration is split between headset and server. Headset contains most configuration, while server has system specific items such as encoders and paths.
Files are read from
- `/usr/share/wivrn/config.json` (where `/usr` is selected at configure time with `CMAKE_INSTALL_PREFIX`)
- `/etc/wivrn/config.json`
- `$XDG_CONFIG_HOME/wivrn/config.json` or if `$XDG_CONFIG_HOME` is not set, `$HOME/.config/wivrn/config.json`.

Files later in the list replace top-level values from previous ones.

If you installed WiVRn from a flatpack, the config is in `$HOME/.var/app/io.github.wivrn.wivrn/config/wivrn/config.json`.

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
  - `position`: `[x, y, z]` marker-to-fiducial offset in meters
    (default `[0,0,0]`)
  - `orientation`: `[rx, ry, rz]` marker-to-fiducial rotation in degrees
    (default `[0,0,0]`). Fixed-frame rotations about X, then Y, then Z, so
    single-axis values do the obvious thing (e.g. `[0,90,0]` yaws 90°)

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
  surface-projected passthrough cutout. Unknown types are skipped with a
  warning (forward-compat for `3d-passthrough-reversed`, `3d-boundary`,
  `3d-boundary-reversed`, `3d-depth`).
- `id`: stable identifier (required, must be unique); `tag`: display-only
  label (optional)
- `fiducial`: referenced fiducial id, or list of ids (required; dangling
  references are dropped with a warning, and an object left with none is
  skipped). Relationships fan out: many objects may reference one
  fiducial, and one object may reference several fiducials (one live
  instance per visible pairing, never fused).
- `model-path`: path to a `.glb`/`.gltf` file on the server (optional,
  models larger than 64MB are skipped)
- `position`: `[x, y, z]` fiducial-to-object offset in meters
  (default `[0,0,0]`)
- `orientation`: `[rx, ry, rz]` fiducial-to-object rotation in degrees
  (default `[0,0,0]`, same fixed-frame convention as above)
- `scale`: uniform object scale, number or single-element array
  (default `1`, objects only, applied last)
- `feather-px`: passthrough window feather width in screen pixels
  (alpha-gradient blend band around the mesh silhouette, default `24`;
  `0` disables feathering). Mask stacks group by this value so each
  object feathers independently; applied live. The client selects a blur
  tier automatically (0 hard edge, 1 half-res with full-res upscale,
  2/4 quarter, 8 eighth
  with exact spread mapping, clamped to 128px), so tap density and cost
  stay flat at any width.
- `fade-in-ms`: alpha fade-in at first acquisition, in milliseconds
  (default `1000`; `0` = appears instantly). Render-only; applied live.
  During the fade the passthrough window ramps from fully transparent
  (game video) to fully present.

Poses compose as
`fiducialPose = observedMarkerPose * markerToFiducialOffset` then
`objectPose = fiducialPose * fiducialToObjectOffset` (scale last).
Objects auto-align on first sighting of a referenced marker, then
smooth-follow without ever snapping, and SLAM-hold forever on marker loss.

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
		}
	]
}
```
