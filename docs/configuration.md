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

## `fiducial-map`
Default value: unset (feature disabled)

Maps fiducials to glTF/GLB models for Quest passthrough mesh anchoring (see
`ROADMAP.md`). The Quest runtime tracks **QR codes** (its AprilTag capability
is not exposed to third-party apps), so each entry identifies its marker by
the exact decoded QR payload string. The server reads each model file, sends
the mapping plus content hashes to the headset on connect, and serves model
bytes on demand. The headset caches models by hash, so files are transferred
once.

Each entry has:

- `tag`: display-only label shown in the headset status UI, so the GUI
  row correlates with this config entry (optional; matching is by
  `marker-data` alone, and renaming never invalidates a calibration)
- `marker-data`: exact QR payload string to match, byte-for-byte (required)
- `marker-size-m`: physical marker size in meters (required for pose scale;
  measure the printed QR's outer edge)
- `model-path`: path to a `.glb`/`.gltf` file on the server (optional, models
  larger than 64MB are skipped)
- `position`: `[x, y, z]` marker-to-mesh offset in meters (default `[0,0,0]`)
- `orientation`: `[rx, ry, rz]` marker-to-mesh rotation in degrees
  (default `[0,0,0]`). Fixed-frame rotations about X, then Y, then Z, so
  single-axis values do the obvious thing (e.g. `[0,90,0]` yaws 90°)
- `scale`: uniform marker-to-mesh scale, number or single-element array
  (default `1`)
- `feather-px`: passthrough window feather width in screen pixels
  (alpha-gradient blend band around the mesh silhouette, default `24`;
  `0` disables feathering). Per object; applied live, no recalibration.
  The client selects a blur tier automatically (0 hard edge, 1 full-res,
  2/4/8 half/quarter/eighth with exact spread mapping, clamped to 128px),
  so tap density and cost stay flat at any width.
- `mode`: `"one-shot"` (default) or `"continuous"`. One-shot aligns once
  per Calibrate press. Continuous auto-anchors on first sighting, then
  smooth-follows the marker to correct drift without ever snapping: new
  samples are robustly averaged (median/MAD Cauchy soft-weighting) and the
  rendered pose exponentially approaches the resolved target every frame
  (large error = large step). SLAM-holds forever on marker loss.
- `static`: `true` (default) or `false`. Maps to the runtime's
  `optimizeForStaticMarker`: keep `true` for a stationary rig, set `false`
  for a moving reference marker. Toggling recreates the client spatial
  context (brief tracking hitch).
- Continuous tuning (all optional, all live per map change; shared Euro
  cutoff/beta cover position and orientation):
  `update-hz` (default `10`, live pose queries without full rediscovery),
  `window-size` (default `12`), `min-samples` (default `4`, warmup),
  `sigma-k` (default `3`), `pos-gain`/`rot-gain` (default `24`, per-second
  proportional catch-up), `euro-min-cutoff` (default `0.4`),
  `euro-beta` (default `0.07`), `knee-inner-mm`/`knee-outer-mm`
  (defaults `1`/`5`), `knee-inner-deg`/`knee-outer-deg`
  (defaults `0.1`/`0.5`).

The mesh pose on the headset is
`meshClientPose = observedMarkerPose * markerToMeshOffset`.

Print the QR encoding exactly the `marker-data` string, e.g.
`qrencode -o marker11.png -s 10 "wivrn:11"`. Matching is exact and
case-sensitive; any other QR code in view is ignored (the headset logs its
payload to help you copy it verbatim into the config).

### Example
```json
{
	"fiducial-map": [
		{
			"tag": "window",
			"marker-data": "wivrn:11",
			"marker-size-m": 0.08,
			"model-path": "/usr/share/wivrn/meshes/widget.glb",
			"position": [0, 0.05, 0.1],
			"orientation": [0, 0, 0],
			"scale": 1.0,
			"feather-px": 24
		}
	]
}
```
