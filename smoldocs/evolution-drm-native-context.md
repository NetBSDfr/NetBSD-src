# Future evolution: DRM native context for virtiodrm

Status: **idea / validated-prerequisite** — no code started (2026-10-29).
Origin: friend's suggestion — "if the virtio-gpu ioctls in NetBSD are
reasonably close to the ones in Linux, implementing DRM native context
could give a *huge* performance boost, and would prove the feasibility
of native context between different kernel platforms (AFAIK never tested)".

Take: **direction is right, and we sit on the one host GPU where the
reference implementation lives — but the kernel port is the easy third
of the job; stage it behind a Linux-guest measurement first.**

## 1. What native context is (one paragraph)

Virgl and Venus mediate at the *application API* level: guest Mesa
encodes GL/VK commands, the host re-interprets and re-issues them.
A DRM **native context** mediates at the *kernel UAPI* level instead:
the guest creates a context of type "native" on the virtio-gpu device,
then the **guest itself runs the real host driver's user-space**
(radeonsi/radv for AMD) and its `amdgpu_drm.h` ioctls are marshalled
over the virtqueue and executed verbatim on the host GPU. Buffers are
host blobs in a host-visible memory window mapped into the guest.
Result: no GL/VK translation layer, dramatically fewer protocol
round-trips.

## 2. Why this attacks *our* measured bottleneck

Session 9 measured the virgl cost first-hand: ~7 ms fence round-trips
(~24 execbuffer+fence pairs per glxgears frame); 5 FPS on the gtk
backend until `sdl,gl=on` + `VIRGL_3D_THROTTLE=0` bought us back to
~80 FPS. Native context removes precisely that protocol overhead.
Published results for amdgpu native context: "Unigine Heaven and
Superposition more or less running at 99% of host speed" (P-E
Pelloux-Prayer, AMD). Bonus our stack cannot get from virgl today:
**Vulkan in the guest** (radv via the same native path).

## 3. Upstream state (verified 2026-10-29)

- Guest userspace: **Mesa 25.0 merged amdgpu native context** (radeonsi
  + radeonsi_drv_video + radv); enablement patches in Mesa are small —
  same drivers, different transport under libdrm.
- Guest drivers implemented: **freedreno** (fully upstream), **amdgpu**
  (mostly merged), **i915** (MRs open), **asahi** (WIP).
- QEMU support: series "Support virtio-gpu DRM native context and
  MAP_FIXED API" (D. Osipenko, at v22 ≈ 2026-03):
  `-device virtio-gpu-gl,hostmem=8G,blob=on,drm_native_context=on`.
- Requirements per that series: virglrenderer **≥ 1.2.0** (async-fence
  bug ≤ 1.1.1), DRM backend effectively wants an **EGL display**,
  host kernel with blob/hostmem support (ours: 6.17, fine).
- Linux guest kernel side: `CONTEXT_INIT` + DRM capset, host-driver
  ioctl passthrough, hostmem blob windows + MAP_FIXED, **syncobj UAPI**
  (merged ~2023, needed by Venus and native contexts alike).

## 4. Our local prerequisites (checked on this host)

| Piece | Today | Needed |
|---|---|---|
| QEMU | 10.2.0-rc4 (`~/src/qemu`): `blob`/`hostmem` props exist, **no `drm_native_context`** | apply/land the series in our source build |
| virglrenderer | 1.0.0 (Ubuntu pkg) | ≥ 1.2.0 with DRM backend |
| Display backend | `sdl,gl=on` (session 9 perf choice) | DRM context may force egl — revisit, re-measure |
| Host GPU | AMD RX 7600 (radeonsi) | reference implementation ✓ |
| Guest Mesa | 21.3.7 (xsrc sets, PCI-less libdrm patch, session 8) | ≥ 25 with native-context enablement (pkgsrc 2026Q3 has it) + re-apply virtio busid hack |
| microvm/no-PCI | everything on virtio-mmio | **verify hostmem window works on virtio-mmio**, not PCI-only |

## 5. What the NetBSD kernel port actually costs

Our `virtiodrm` is the vendored Linux virtgpu driver
(`sys/external/bsd/drm2/dist/drm/virtio/` + `virtio_compat.c` shim),
so the ioctl surface is already Linux-close. The real items:

1. `CONTEXT_INIT` ioctl + DRM capset negotiation (we skipped
   context_init in the session-8 virgl work).
2. Host-driver **ioctl passthrough** command path over the control
   queue (amdgpu ioctls as opaque blobs + pointer fields resolved
   from guest memory by the host; descriptor tables already live in
   virglrenderer `src/drm`).
3. **Hostmem window mapping**: virtio-mmio region → GEM blob objects →
   userspace mmap on NetBSD (pmap/uvm work squarely inside the compat
   shim — see HANDOFF "hard-won gotchas" before starting).
4. **syncobj UAPI** (not in our tree yet) + fence/timeline semantics
   around presentation — *the biggest unknown*, schedule a spike.
5. Keep the existing split working: viogpu(4) console + virtiodrm
   render, scanout path unchanged (native context is render-only).

Wire compatibility is a non-issue for amd64: NetBSD and Linux share
the `amdgpu_drm.h` struct layouts, same endianness, no padding drift —
which is *exactly* why the cross-platform claim is credible.

## 6. Validation ladder (gate each step before the next)

- **Step 0** (minutes): rebuild QEMU with the series + virglrenderer
  1.2; confirm `drm_native_context=on` appears and which display
  backend it insists on.
- **Step 1** (half a day): boot a **Linux** guest with native context
  on *this* host; measure glmark2/tuxracer/Unigine vs our recorded
  virgl numbers (glxgears ~80–100 FPS, Urban Terror 109 FPS). This
  validates the host stack and quantifies the "huge" for RX 7600
  before any NetBSD code.
- **Step 2**: kernel port in increments with `testdrm3d`-style
  harness: capset → context init → one marshalled amdgpu ioctl →
  blob mmap → syncobj.
- **Step 3**: pkgsrc-Mesa-25 guest image, native radeonsi; re-validate
  the DRI2/AIGLX chain (sessions 8–9) and the microvm no-PCI story.

## 7. Bottom line

The architecturally correct endgame for the viogpu-glx project, and a
genuine first for NetBSD (native context across different kernel
stacks has no published precedent). Treat it as a staged project gated
on Step 1, not a weekend hack: if the Linux-guest measurement shows
the 2–5× gain on sync-heavy loads, the port is clearly worth doing;
if it doesn't on our GPU/driver combo, virgl + backend tuning is good
enough and Urban-Terror-class workloads are already served.
