# smoldocs — smolBSD project notes & design documents

Working documents for the smolBSD effort (microvm NetBSD guest,
virtio-mmio, virtio-gpu/virgl GLX on QEMU microvm — see the
`viogpu-glx` / `supasmol` branches).

This directory is for *ideas, designs and future-evolution notes*,
kept next to the sources they concern. Session-by-session operational
state lives in the handoff documents instead.

## Index

- `evolution-drm-native-context.md` — replacing virgl with DRM
  **native context** (native radeonsi/radv in guest, host-driver
  ioctls over the virtqueue): why, what upstream requires, the
  NetBSD port budget, and the staged validation plan. Idea validated
  2026-10-29, not started.

## Related documents

- `HANDOFF-viogpu-glx.md` — full session log of the GLX bring-up
  (checked out from the `viogpu-glx` branch root): the compat-shim
  bugs, vioinput, virgl, FPS/backend findings, hard-won gotchas.
