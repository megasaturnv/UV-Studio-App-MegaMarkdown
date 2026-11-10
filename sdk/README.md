# Overlay-app SDK (v6.3 / API level 3)

The build needs the overlay-app headers and packer from the firmware. Those
files are not stored in this repository, so download them once before the first
build:

```sh
python3 sdk/fetch_sdk.py
```

Run it from the repository root, or run `./fetch_sdk.py` from this directory.
The script writes the files next to itself and verifies every download against
the SHA-256 hashes from the pinned firmware commit. Existing files with the
right hash are left alone; use `--force` to refresh them:

```sh
python3 sdk/fetch_sdk.py --force
```

| File | Purpose |
| --- | --- |
| `app_api.h` | The `app_api_t` service table passed to `app_main()` |
| `app_overlay.h` | The `.app` format, header fields, flags and capabilities |
| `pack_app.py` | Packs the linked binary into a `.app` blob |
| `app_assets.py` | Upstream helper for declaring read-only assets (not used by this app) |
| `app_art.py` | Upstream bitmap/title-screen helper (not used by this app) |

Only `app_api.h`, `app_overlay.h` and `pack_app.py` are required to build
MegaMarkdown; the other two are included so this folder matches upstream.

## Provenance

- Upstream: <https://github.com/armel/uv-k1-k5v3-firmware-custom>
- Tag: `v6.3.0`
- Commit: `ca3a4271386049e7b459b215cef0ad0eea1a30ca`
- Upstream directory: `App/apps/`
- API: ABI major 1, API level 3 (`sizeof(app_api_t) == 396`)
- Slots: 48 (`APP_SLOT_COUNT`)
- Assets: `APP_ASSET_MAX == 0x0F00` (3840 bytes)

These files are copied **unmodified** from the firmware source. The firmware
loader and the packer must agree byte for byte, so do not edit them by hand.

## Updating

When the firmware publishes a new API level, update `FIRMWARE_TAG`, `COMMIT`
and the hashes in `fetch_sdk.py` to the matching upstream release, then download
the files again and rebuild. Check `APP_API_LEVEL`, `APP_ABI_MAJOR` and the
`_Static_assert` offsets in `app_api.h` before shipping. A published app must be
rebuilt when the ABI changes.

The files are Apache-2.0 licensed; see the header in each file.
