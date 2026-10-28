# viogpu-glx — handoff notes (2026-10-26)

Self-contained status + working notes for continuing this project in a fresh
session (e.g. on a GL-capable host). Branch: **`viogpu-glx`** in the NetBSD
src tree (referenced below as `$SRC`; originally `/home/imil/src/NetBSD-src`).

## Goal

GLX inside a small NetBSD-based guest ("smolBSD") on QEMU **microvm**
(virtio-mmio only, no PCI), GPU-accelerated by the **host** GPU via
virtio-gpu + virgl (3D) and virtio-gpu 2D for the console.

Phases:
- P0 restore `viogpu(4)` 2D console — done (f699562)
- P1 damage-based refresh (wsconsio + viogpu patch; xsrc wsfb patch pending) — done (c71221a)
- P2 virtio-gpu **DRM/KMS** driver `virtiodrm(4)` on NetBSD drm2 + virgl 3D — **current**
- P3 cleanup/upstreaming of the drm2 + virtio_mmio bits

## Status (all verified in the last session)

| Capability | State |
|---|---|
| `virtiodrm0 at virtio0` attach, KMS init (scanout/capsets/EDID) | works, boots to rc in ~2 s |
| `/dev/dri/card0`, GEM dumb ioctls (CREATE/MAP/DESTROY_DUMB) | works |
| GEM **mmap** (NetBSD uvm pager) | works |
| RESOURCE_ATTACH/DETACH_BACKING (guest-phys page lists) | works, QEMU `-d guest_errors` silent |
| GPU **interrupts** | work; the "lost interrupt" theory was false, workaround removed (9fdee5ce98c) |
| Guest smoke test | `DRM SMOKE PASS` from `/root/testdrm` (create/map/mmap-readback/destroy) |
| Console | still `com0` (see console policy below) |
| virgl 3D | **not started** — `virtgpu_virgl.c` not vendored; probe shows `-virgl`, 0 capsets |

### Commits on `viogpu-glx` (newest first)
- 9fdee5ce98c virtiodrm: drop the synchronous used-ring drain workaround
- 4a05fb1c085 virtiodrm: add a uvm pager for GEM objects
- 72acd4aa065 virtiodrm: iterate the page array when building backing lists
- ac3e5e523c7 virtiodrm: map kernel command buffers page-accurately for the ring
- c0ac4f7c9f6 SMOL: virtiodrm(4): VirtIO-GPU DRM/KMS driver with Linux-virtio-bus glue
- 376d884100d SMOL: restore PCI, i2c, firmload, ksyms files supasmol over-pruned
- (earlier P0/P1: f699562 viogpu restore, c71221a damage patch)

Working tree at handoff time: **clean** (except this file).

## Environment (paths on the originating host; adapt)

- `$SRC` = NetBSD src = `/home/imil/src/NetBSD-src` (branch `viogpu-glx`)
- `$VM` = `/home/imil/src/smolBSD`:
  - `images/p9test-amd64.img` — test guest disk (NOT in git; copy it!)
  - testdrm source `testdrm.c` (originally `/tmp/testdrm.c`, static binary in
    image `/root/testdrm`, invoked from `/etc/rc`, prints `DRM SMOKE PASS`)
- tooldir: `$SRC/obj/tooldir` (x86_64--netbsd)
- kernel config: `sys/arch/amd64/conf/SMOL` (microvm, virtiodrm enabled, viogpu yields)

### Build kernel
```sh
cd $SRC && ./build.sh -U -u -O obj -T $SRC/obj/tooldir -m amd64 -j8 kernel=SMOL
```
Gotcha: editing any `files.*` requires `touch sys/arch/amd64/conf/SMOL` first.
Kernel lands at `obj/sys/arch/amd64/compile/SMOL/netbsd`.

### Boot (headless regression test)
```sh
cd $VM && timeout 45 qemu-system-x86_64 -smp 1 -accel kvm -M microvm,rtc=on,acpi=off,pic=off \
  -m 1024 -cpu host -kernel $SRC/obj/sys/arch/amd64/compile/SMOL/netbsd \
  -drive if=none,file=images/p9test-amd64.img,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -append "console=com root=NAME=p9testroot" \
  -global virtio-mmio.force-legacy=false -device virtio-gpu-device -display none \
  -d guest_errors -serial file:/tmp/vboot.log 2>/tmp/qerr.log
```
Success = `DRM SMOKE PASS` in serial log + empty `-d guest_errors` stderr
+ no kernel `*ERROR*` lines. `timeout` exit 124 is normal (image halts at ~2 s).

### Boot (with display — for virgl/GL work)
- plain 2D visible: `-device virtio-gpu-device -display gtk` (or `sdl`)
- virgl 3D: `-device virtio-gpu-device,virgl=on -display gtk`; on a
  headless GL host use `-display egl-headless` and connect a VNC client.
  Check QEMU has virgl at all: `qemu-system-x86_64 -device virtio-gpu-device,help | grep virgl`.
  Guest side then shows `[drm] features: +virgl` and 1 capset.
- Console policy (agreed): fb console **only when started with a display**;
  implement via `virtiodrmfb` glue + pass a `console=` kernel arg in `-append`
  for those runs. Headless runs stay on `com0`.

### Image surgery recipe (e.g. to update testdrm)
```sh
losetup -o $((2048*512)) --sizelimit $((1046528*512)) /dev/loopN images/p9test-amd64.img
debugfs -w -R 'rm /root/testdrm' /dev/loopN
debugfs -w -R 'write /path/new/testdrm /root/testdrm' /dev/loopN
e2fsck -fy /dev/loopN    # always
# losetup -d, then dd back if you extracted a slice copy
```

## Next steps (in order)

1. **virgl 3D** (needs the GL host):
   - Import `dist/drm/virtio/virtgpu_virgl.c` from a recent Linux (drm-misc
     next ~6.6 matches the vendored tree), add to
     `sys/external/bsd/drm2/virtio/files.drmkms_virtio`, plus any missing
     uapi structs (capset, `virtio_gpu_cmd_get_capset*`,
     `virtio_gpu_resource_create_blob`) and fence-id plumbing
     (`virtgpu_fence.c` is vendored already).
   - Wire `virtio_has_capset_info()`/`virtio_has_fence()` feature detection
     (virtgpu_kms.c prints `features: -virgl +edid` today).
   - DRM ioctls for 3D: Linux uses `DRM_VIRTGPU_*` (getparam gem_handle,
     resource_create, execbuf, transfer_to_host_3d, transfer_from_host_3d,
     get_capset) — check `dist/drm/virtio/virtgpu_ioctl.c` (vendored) and the
     NetBSD drm ioctl plumbing (`.driver_ioctl` path + compat).
   - Guest userspace: pkgsrc Mesa with `GALLIUM_DRIVERS+=virgl`, libdrm,
     xorg or direct KMS; then `glxinfo` (mesa-utils) expecting `llvmpipe`→
     virgl renderer strings.
2. **virtiodrmfb console glue** (per policy above). Reference pattern:
   NetBSD in-tree `*fb.c` glue filling `struct drmfb_attach_args`
   (`da_fb_helper`, `da_fb_sizes`, `da_fb_vaddr`, `da_fb_linebytes`,
   `da_params`). Generic drm fbdev helpers are broken on NetBSD drm2
   (`drm_client_framebuffer_create` is a stub → NULL fb → trap).
3. P1b xsrc wsfb damage patch (record when rebuilding xsrc).
4. P3 upstream cleanup.

## Hard-won gotchas (do not re-learn)

1. **`PAGE_MASK` in drm2 TUs is the LINUX value `~(PAGE_SIZE-1)`** (inverted
   mask), not the kernel `PAGE_SIZE-1`. Page-offset math must use
   `(PAGE_SIZE - 1)` explicitly in `sys/external/bsd/drm2/**`.
2. `_BUS_VIRT_TO_BUS()`/`kvtop()` **drops the intra-page offset** and x86
   `bus_dmamap_load` merges only physically-contiguous runs. kmalloc slab
   pointers are cache-aligned, not page-aligned → multi-page kernel buffers
   get wrong DMA descriptions. Fixed by hand-building `dm_segs` page-by-page
   in `virtio_compat_map_entry()` (kva path).
3. `virtio_enqueue_reserve(nsegs)` must match the **exact** segment count
   enqueued later (indirect desc tables are sized there and padded with
   stale NEXT-chained descs). `virtqueue_add_sgs()` therefore maps all
   entries before reserving.
4. `virtio_enqueue(sc, vq, slot, dmamap, write)`: `write=true` = OUT
   (driver→device). Opposite-sounding vs Linux; wrong order ⇒ QEMU
   "Incorrect order for descriptors" + NEEDS_RESET.
5. NetBSD drm2 `sg_alloc_table*` = **shared page-array flavor**: all
   metadata lives in `sgl[0]` (`sg_pgs`, `sg_npgs`, `sg_dmamap`);
   `for_each_sg` slots > 0 are garbage. Iterate `sgl->sg_pgs`.
6. NetBSD GEM mmap requires `driver->mmap_object` **and**
   `driver->gem_uvm_ops` (KASSERT in `drm_gem_private_object_init`);
   virtgpu's uvm fault handler maps `shmem->pages[poff]` via
   `pmap_enter(uvm_vm_page_to_phys(...))` (pages pinned for resource life).
7. Generic fbdev on drm2 is dead (stub `drm_client_framebuffer_create`).
8. `interrupting on -1` for pv/viommio devices is **cosmetic** (handler is
   on the ioapic pin; the cmdline glue prints -1 after the ioapic lookup).
   QEMU itself injects `virtio_mmio.device=<size>@<base>:<gsi>` entries into
   the kernel command line for microvm (that's where the pv nodes come from).
9. GPU ctrl commands complete via normal used-ring interrupts (verified by
   tracing); no synchronous drain needed.
10. Build: `files.*` edits need `touch sys/arch/amd64/conf/SMOL`. Kernel
    debug symbols: addr2line via
    `obj/tooldir/bin/x86_64--netbsd-addr2line -f -i -e obj/sys/arch/amd64/compile/SMOL/netbsd <addr>`.

## Debug recipes that worked

- `-d guest_errors` → QEMU-side virtio-gpu complaints on stderr
  ("failed to map MMIO memory for element N", "command data size incorrect").
- QEMU monitor telnet (`-monitor telnet:127.0.0.1:PORT,server=on,wait=off`),
  python socket, `xp/8wx 0xGUESTPHYS` = guest **physical** RAM dump
  (gdbstub `x/` translates virtual, useless for PA dumps).
- QEMU source cross-check (v11.0.2 on this host):
  `https://gitlab.com/qemu-project/qemu/-/raw/v11.0.2/hw/display/virtio-gpu.c`
- printf-driven bisection is fast (build ~70 s, boot ~45 s); DDB not set up.

## Key source map (under `$SRC/sys/external/bsd/drm2`)

- `virtio/virtio_compat.c` — Linux<->NetBSD virtio shim: `struct
  virtio_device`, `virtio_find_vqs`, `virtqueue_add_sgs`, kva DMA map
  builder in `virtio_compat_map_entry`, `virtqueue_notify` (plain commit now).
- `dist/drm/virtio/virtgpu_vq.c` — ring command builders;
  `virtio_gpu_object_attach()` NetBSD page-array branch (guest-phys ents).
- `dist/drm/virtio/virtgpu_gem.c` — + NetBSD `virtio_gpu_gem_uvm_ops`.
- `dist/drm/virtio/virtgpu_drv.c` — driver incl. `.mmap_object`,
  `.gem_uvm_ops`.
- `virtio/files.drmkms_virtio` — the compile list (virgl not in yet).
- Native glue reference: `sys/dev/virtio/arch/x86/virtio_mmio_cmdline.c`
  (microvm enumeration/irq), `sys/dev/pci/virtio.c` (ring core,
  start/stop_vq_intr), `sys/dev/virtio/virtio_mmio.c` (`virtio_mmio_intr`).

## GL HOST STATUS (2026-10-26, this host — RX 7600 / Mesa / QEMU 8.2.2)

Checklist 1–5 complete. Verified working:

- Headless regression unchanged: `DRM SMOKE PASS`, `-d guest_errors` clean.
- **virgl available and negotiating**: `-device virtio-gpu-gl-device
  -display egl-headless` → guest prints `[drm] features: +virgl +edid` and
  boots in ~1 s. virglrenderer 1.0.0 + Mesa EGL + GBM on `/dev/dri/renderD128`
  = real host GPU (radeonsi), no swrast fallback.
- QEMU 8.2 removed the `virgl=` property → the device is `virtio-gpu-gl-device`
  (Ubuntu/Debian ship it as loadable module hw-display-virtio-gpu-gl.so).
- Display-backend sharp edges (ALL hang the PVH boot, no serial output at all):
  - `-display egl-headless -vnc :N` → deadlock at startup (even with plain
    virtio-gpu-device). Omit `-vnc`.
  - `-display gtk` / `-display sdl,gl=on` on X (`:0.0` is Xorg/Xwayland, the
    user's GNOME session) → stuck in poll() on the X socket during GL init.
    Use egl-headless; for a visible window consider Xephyr + a VNC client on
    :0.0, or revisit after a newer QEMU.
- Restored untracked files this host was missing (git doesn't track them):
  `smolBSD/images/p9test-amd64.img` (+ `.orig` scratch copy) and 22 `sys/`
  headers that `make depend` needs (sys/sys/agpio.h, sys/dev/acpi/*.h, …);
  full list = `git status --short -uall -- sys/` on smith minus compile/obj.
  If a future missing-header error appears at make depend, look there first.
- testdrm.c now at /tmp/testdrm.c on this host too.

Working virgl-test boot line (headless, hardware-accelerated host GL):
```sh
cd ../smolBSD && qemu-system-x86_64 -smp 1 -accel kvm \
  -M microvm,rtc=on,acpi=off,pic=off -m 1024 -cpu host \
  -kernel $SRC/obj/sys/arch/amd64/compile/SMOL/netbsd \
  -drive if=none,file=images/p9test-amd64.img,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -append "console=com root=NAME=p9testroot" \
  -global virtio-mmio.force-legacy=false \
  -device virtio-gpu-gl-device -display egl-headless \
  -d guest_errors -serial file:/tmp/vboot.log
```
Next up: implement virgl 3D in the guest driver (see Next steps §1).

## 3D ROUND-TRIP STATUS (2026-10-26, sessions 2–3 — WORKING 5/5)

`testdrm3d` runs 5 full rounds per boot, quiet kernel, QEMU 8.2.2,
`ROUNDS FAILED: 0/5`: GETPARAM/capsets, RESOURCE_CREATE, MAP,
TRANSFER_TO_HOST_3D, TRANSFER_FROM_HOST_3D with data intact
(`fb[1024]=0xc0ffee00`), EXECBUFFER, WAIT. Script: `$VM/t3d.sh`.

Two real bugs found and fixed, both in
`sys/external/bsd/drm2/virtio/virtio_compat.c` (never in the pristine
drm2 dist tree):

1. **Avail entry published at notify time, not add time**
   (`virtqueue_add_sgs`/`virtqueue_notify`): Linux semantics require
   add_sgs to make the buffer visible and notify to be a doorbell only.
   Deferred publish collapsed multiple adds into one ring entry.
   Now: `virtio_enqueue_commit(sc, nvq, slot, false)` at add; doorbell-
   only `virtio_enqueue_commit(sc, nvq, -1, true)` at notify.
2. **virtqueue_get_buf had the commit paths inverted**: empty poll
   (`virtio_dequeue` → ENOENT) called `virtio_dequeue_commit(sc, nvq, 0)`
   which frees UNCONDITIONALLY (sys/dev/pci/virtio.c `vq_free_slot`),
   pushing slot 0 onto the free list once per empty poll → duplicate
   slot → several in-flight requests aliased one indirect table →
   QEMU popped N chains but dispatched the last command N times
   (trace: `ctx_res_attach` ×3). Success path never committed →
   permanent slot leak. Fixed at `virtqueue_get_buf`.

Debug recipe that worked: QEMU `-trace enable='virt*'` + guest printf
pairing; beware Heisenbug (printfs in the queue path made it pass).
Event_IDX is NOT negotiated (features `0x110000003<V1,INDIRECT_DESC>`),
and QEMU sets `VRING_USED_F_NO_NOTIFY` on pop, so the core legitimately
skips doorbells — multiple adds accumulate before one pop burst; that is
what exposed bug 2.

Next up: guest userspace GL (Mesa virgl driver in p9test image +
`glxinfo`/`es2_info` over egl-headless display).

## On the new GL host — first checklist

1. `git clone`/pull `viogpu-glx`; copy `$VM` dir incl. `images/p9test-amd64.img`
   (and keep a scratch copy!).
2. Build the SMOL kernel (needs `obj/tooldir` — build.sh will rebuild tools
   if missing; expect a long first build).
3. Run the headless boot command → expect `DRM SMOKE PASS` unchanged.
4. `qemu-system-x86_64 -device virtio-gpu-device,help | grep virgl` → virgl
   available? Try `-device virtio-gpu-device,virgl=on -display gtk` (or
   egl-headless): guest should print `[drm] features: +virgl` (driver still
   ignores the capset until step 1 of Next steps is done — that is fine).
5. Host side virglrenderer presence: `dpkg -l | grep virgl` (or equivalent).

## X11/GLX STATUS (session 4, 2026-10-27) — Xorg runs, GLX = llvmpipe

smolBSD service `vioglx` (image `images/vioglx-amd64.img`, 6GB, sets
base,etc,smolset,xbase,xserver,xetc,xfont) boots on the SMOL kernel and
brings up **X.Org 21.1.25 + X11 GLX: direct rendering Yes, GLX 1.4,
llvmpipe (LLVM 13.0.0, Mesa 21.3.7), glxgears ~735 FPS** on the
virtiodrm card0. Harness: `~/src/smolBSD/tglx.sh` (two GPUs attached:
`virtio-gpu-device` → viogpu0+wsdisplay0 text console, `virtio-gpu-gl-device`
→ virtiodrm0 3D). testdrm3d 5/5 PASS in the same boot.

### Device-claim split (this commit)
`virtiodrm_match` now claims a GPU only when `VIRTIO_GPU_F_VIRGL` is offered
(confidence 2); plain GPUs go to `viogpu(4)` (wscons text console). Device
features are passed to children via `struct virtio_attach_args`
`.sc_device_features`, read by the virtio-mmio cmdline glue before
`config_found` (PCI glue already exposed them similarly). MICROVM.common
enables both `viogpu*` and `virtiodrm* at virtio?` (upstream had `no viogpu*`
because of wsdisplay deps; with only viogpu+wsdisplay+rasops32+vcons it
builds fine).

### ttyE root cause (fixed here) — "xf86OpenConsole: No console driver found"
A wsdisplay unit attached WITHOUT the console flag creates **zero** screens
unless `options WSDISPLAY_DEFAULTSCREENS>0` (sys/dev/wscons/wsdisplay.c
`wsdisplay_common_attach`: console unit starts at screen 1; non-console
starts at 0 with `wsdisplay_defaultscreens`). microvm console is com0
(only com/viocon can be boot consoles — viogpu attaches too late), so
wsdisplay0 at viogpu0 had `sc_scr[] == NULL` and every /dev/ttyE open
returned ENXIO → xorg-server `xf86OpenWScons()` (opens /dev/ttyE0..7)
probed as "no console driver". Fix: `options WSDISPLAY_DEFAULTSCREENS=4`.

Gotchas found along the way:
- xsrc `xf86OpenWScons` does NOT honor CONSOLEDEVICE/-vtdev; it just opens
  /dev/ttyE0..7 O_RDWR.
- smolBSD /dev is a boot-populated tmpfs with the correct static majors
  (ttyE=47 per sys/arch/amd64/conf/majors.amd64); MAKEDEV in rc changes
  nothing.
- Probe tool `ttypemaj.c` (in ~/src/smolBSD, injected at /ttypemaj): opens
  (maj,255) for all majors; (47,255)=ttyEcfg openable while (47,0) ENXIO
  pinpoints missing screen 0.

### Next: true virgl GLX
Image Mesa 21.3.7 has gallium_dri.so WITHOUT the virtio driver → llvmpipe.
For hardware GLX: rebuild pkgsrc Mesa with `-D gallium-drivers=...virtio`
(or probe virgl via kmscube/render node first). rc already runs the full
matrix and prints VIOGLX GLX: lines; re-inject via debugfs
(rm, write, sif mode 0100755).

## ACHIEVEMENT PATH (end of session 4, 2026-10-27)

How we got here, in order — each step verified in QEMU 8.2.2 microvm:

1. **Compat-shim publish fix** — requests must be published to the ring at
   `virtio_add_hw_buf` time (not at doorbell); removed the drain
   workaround (`9fdee5ce98c1`). Control queue: DRM SMOKE PASS.
2. **Ring slot accounting** (`238b0dfe4758`) — `virtqueue_get_buf` called
   `virtio_dequeue_commit` on the *empty-poll* path (unconditional free of
   slot 0 → duplicates → aliasing, the `ctx_res_attach`×3 trace) and never
   on the *success* path (slot leak). Fix gave a deterministic 5/5 PASS
   3D round-trip (RESOURCE_CREATE → TRANSFER_TO_HOST_3D → EXECBUFFER →
   TRANSFER_FROM_HOST_3D data intact) and a quiet kernel.
3. **Back to the branch origin** (user directive: use viogpu(4), kern/58681
   PR#49) — device-claim split so both drivers coexist: children learn
   device features via `virtio_attach_args.sc_device_features`;
   `virtiodrm_match` claims only `VIRTIO_GPU_F_VIRGL` GPUs (conf 2);
   plain GPUs → `viogpu(4)` + `wsdisplay0` text console. MICROVM.common
   enables both (`no viogpu*` from kern/59568 lifted).
4. **X11 milestone** (`7dc08e680b3f`) — smolBSD `vioglx` image (x sets);
   "No console driver found" root-caused to non-console wsdisplay creating
   zero screens → all ttyE ENXIO (probe: major 47 alive at minor 255, dead
   at minor 0) → fixed with `options WSDISPLAY_DEFAULTSCREENS=4`.
   Result: Xorg 21.1.25 up in 1s on ttyE0, `direct rendering: Yes`,
   GLX 1.4, llvmpipe (Mesa 21.3.7), glxgears ~735 FPS, testdrm3d 5/5 in
   the same boot. Kernel console stays com0; serial log unaffected.
 5. **Visible window** (session 5) — no PCI needed: microvm + sysbus
   `-device virtio-gpu-gl-device -device virtio-gpu-device` with
   `-display gtk,gl=on` on a local QEMU build (gtk+virgl). Gotchas: QEMU
   gtk shows the console of the FIRST-created graphic device, so the GL
   device must be listed first or the window stays black while X renders
   to the other GPU's scanout; the gtk placeholder text "Display output
   is not active" means exactly this — no scanout on the first device
   yet (wrong device order, OR X never came up: check the serial log for
   `Xorg up` before chasing card order); and a device-features print of
   `0x...003` means VIRGL+EDID negotiated (generic decoder names only
   transport bits). Desktop (twm/xterm/glxgears ~280 FPS) now visible in
   the window; launcher `smolBSD/vglx-gui.sh`, GUI mode gated by
   `-fw_cfg opt/org.smolbsd.var.gui,string=1` (qemufwcfg works on
   microvm). Fixed latent panic: `virtio_mmio_cmdline_parse()` called
   `strstr(NULL, ...)` when the cmdline has no `virtio_mmio.device=`
   entry (any pvbus machine without mmio cmdline args, e.g. q35).

Commits on `viogpu-glx`: `0a58346738d3` (58681 import) → `f699562332e3` →
`c71221a99fa1` (PR#86 damage) → `c0ac4f7c9f68` (virtiodrm) →
`9fdee5ce98c1` → `238b0dfe4758` (ring slots) → `7dc08e680b3f` (split+screens).

## WHAT REMAINS

- [ ] **True virgl GLX**: userspace Mesa in the image (21.3.7 from x sets)
      has no `virtio` gallium driver → llvmpipe. Options: pkgsrc `mesa-lib`
      (built with virtio gallium), or rebuild xsrc Mesa with
      `-D gallium-drivers=virtio,swrast`. Then `glxinfo` should print
      `OpenGL renderer string: virgl`. Kernel side is done.
- [ ] **Input devices in microvm**: no kbd/mouse (no ISA USB/PS2); X works
      headless-input. For interactive use add `-device virtio-keyboard-device`
      / `virtio-mouse-device` (check NetBSD virtio-input driver exists in
      this tree) or run the image under q35 with ps2.  (A q35 boot works
      via restored `virtio* at pci?` + files.pci `virtio_pci` file line,
      both uncommitted; virtio-gpu-gl-pci there exposed no virgl to the
      guest — parked, MMIO is the point.)
- [ ] **Upstream proposals**: (a) kern/59568 can be closed/reduced —
      viogpu now builds and works on MICROVM once non-console wsdisplay
      gets screens (propose DEFAULTSCREENS=4 or console? flag discussion);
      (b) send the device-claim split (virtiodrm vs viogpu) upstream — it
      resolves the two-drivers-one-device-ID conflict; (c) re-check
      kern/59576 (pckbd race) on configs with both viogpu and pckbd —
      microvm has no pckbd, untested there.
- [ ] **Regression check**: build GENERIC/amd64 + MICROVM (not just SMOL)
      with the split; only SMOL was build-tested.
- [ ] Optional hygiene: drop debug probes (ttypemaj, rc VIOGLX pre/ws dump)
      from the test image; decide whether `WSDISPLAY_DEFAULTSCREENS=4`
      belongs in MICROVM.common upstream or only in SMOL.

## SESSION 6 (2026-10-27): vioinput driver — input works, X up with it

READ THIS FIRST if you are resuming: the QEMU window launcher is
`~/src/smolBSD/vglx-gui.sh` (microvm + gtk, GL device first, input devices
LAST on the cmdline). Build/stage/boot/screenshot recipe:

    ./build.sh -U -u -j8 -m amd64 -O obj -T obj/tooldir kernel=SMOL
    # after config/files edits the FIRST link can fail on a stale ioconf.o;
    # re-run build.sh once and it passes.
    cp obj/sys/arch/amd64/compile/SMOL/netbsd ~/src/smolBSD/kernels/netbsd-SMOL-vioglx
    (cd ~/src/smolBSD && nohup ./vglx-gui.sh >/tmp/vin.log 2>&1 &)   # export DISPLAY=:0.0 inside script
    grep -a 'Xorg up\|Xorg FAILED\|panic\|ttyflags' /tmp/vin.log | grep -av dmesg
    DISPLAY=:0.0 xwd -root -silent | xwdtopnm | pnmtopng > /tmp/desktop.png   # ONLY way to "see" the window; TAKE ONE when testing
    pkill -x qemu-system-x86    # and actually kill it when done (bash `pkill -f` self-matches!)
    # rc edits into the image: sudo kpartx -av images/vioglx-amd64.img
    # printf 'rm /etc/rc\nwrite <path> /etc/rc\nsif /etc/rc mode 0100755\n' > /tmp/rcmds.txt
    # sudo debugfs -w -f /tmp/rcmds.txt /dev/mapper/loop0p1 ; sudo kpartx -dv /dev/loop0

### What was built
`sys/dev/virtio/virtio_input.c` — brand-new `vioinput(4)` (upstream NetBSD
has NO virtio-input driver). Copyright header = "The smolBSD Project"
(user rule: files WE create, not NetBSD Foundation). Registered in
`files.virtio` (`device vioinput: wskbddev, wsmousedev` + `attach vioinput
at virtio with vioinput` — the `with` name MUST equal the CFATTACH_DECL
name), `virtio.config` (`vioinput* at virtio?`), `MICROVM.common`
(`wskbd*/wsmouse* at vioinput?`, `pseudo-device wsmux`,
`wsdisplay* at viogpu? kbdmux -1`). QEMU side:
`-device virtio-keyboard-device -device virtio-mouse-device`.
Result: `vioinput0 ... virtio mouse` + `wsmouse0 mux 0`, `vioinput1 ...
virtio keyboard` + `wskbd0 mux 1`, Xorg :1 up in 1s, glxgears ~30 FPS
(llvmpipe), GUI section runs. **User should type/move mouse in the window
to confirm end-to-end** (was pending at time of writing).

### Gotchas found (each cost a debug cycle — do not rediscover)
1. `bus_dmamap_create` in this tree takes 7 args `(tag, size, nsegs,
   maxsegsize, alignment, flags, &map)` — flags BEFORE map (viocon.c:393
   is the reference call).
2. `config_found(parent, aux, print, CFARGS_NONE)` attaches the FIRST
   autoattach child of the parent class regardless of type — for a mouse
   device it grabbed the wskbd pdev and fed wsmouse args to wskbd_attach
   (crash in `wskbd_load_keymap`). This fork HAS no `config_found_sm_loc`;
   the interface filter is `CFARGS(.iattr = "wskbddev"/"wsmousedev")`
   (subr_autoconf.c `config_search_internal`; a parent declaring 2+iattrs
   MUST pass iattr — KASSERT says so).
3. Adding wskbd to the kernel sets NWSKBD=1 and changes wsdisplay.c
   behavior: **every ttyE ioctl now passes the
   `sc_input == NULL -> ENXIO` guard** — a non-console wsdisplay only gets
   `sc_input` via wsmux, so `pseudo-device wsmux` became REQUIRED the
   moment vioinput existed. Symptom: X `xf86OpenConsole:
   WSDISPLAYIO_MODE_MAPPED failed (Device not configured)` +
   `ttyflags: TIOCSFLAGS ... Device not configured` in boot log (silent =
   healthy). `kbdmux -1` gives wsdisplay its own empty "dmux" (sc_input
   non-NULL, no child walk) — X then uses /dev/wskbd0 + /dev/wsmouse0
   directly.
4. wskbd/wsmouse call `accessops->ioctl` **unguarded** on unknown ioctls —
   `.ioctl = NULL` in your accessops = `prevented execution of 0x0 (SMEP)`
   panic from ttyflags. Use an EPASSTHROUGH stub.
5. QEMU microvm assigns virtio-mmio slots in REVERSE cmdline order
   (inputs listed last enumerate at the LOWEST addresses / attach first).
   Device slot order was NOT the X failure (bisected to NWSKBD above);
   GPU creation order still governs which scanout the gtk window shows
   (GL device must be created first = listed first).
6. Kernel debug loop tooling: resolve panic `rip` via
   `obj/tooldir/bin/x86_64--netbsd-addr2line -f -e obj/.../netbsd 0x...`;
   no DDB in SMOL, panic reboots (boot loop in the log = same trap).
7. Do NOT `git stash -u` in this tree — it swallows untracked `obj/`
   (the whole tooldir); use `git stash push -- <paths>` for bisects.

### QEMU/window hygiene (user requests)
- Take a screenshot when testing the GUI; don't assume the window state.
- Kill the QEMU process when a test boot is finished.
- Screenshot: `DISPLAY=:0.0 xwd -root -silent | xwdtopnm | pnmtopng > f.png`
  (no imagemagick; xwdtopnm maxval-65535 warning is harmless).

## SESSION 7 (2026-10-27): click-wedge root-caused; input verified

- **The click wedge** (kernel froze on first real click): LOCKDEBUG panic
  "acquiring sleep lock from interrupt context" -> `virtio_compat_lock`
  (drm2 virtio shim) was adaptive/IPL_SOFTNET but the vq/cfg trampolines
  take it from hard IRQ (viogpu children attach at IPL_VM). Fixed: spin
  mutex at IPL_VM, init before first use, KM_SLEEP alloc outside.
  Without LOCKDEBUG this deadlocks silently. (418d1404ed32)
- **vioinput KASSERT fixes** (DIAGNOSTIC): status q needs
  virtio_init_vq_vqdone + is drained from the event softint (core only
  calls the handler for slots it dequeues itself); event DMA buffer must
  be vq_num*EV_SIZE — QEMU virtio-input queues are 1024 descs deep.
  (e23ef4cc4807)
- **Debug-config traps in this fork**: `options DDB` unbuildable (sys/ddb
  has no db_output.h, needed by x86/cpu.c); `options DIAGNOSTIC`
  boot-panics on tsc.c `curcpu_stable()` KASSERT from
  boot_duration_timer(). SMOL debug = LOCKDEBUG only. Kernel .depend does
  NOT track opt_*.h: rm -rf the build dir after config-option changes.
- **HMP automation** (added to vglx-gui.sh: `-monitor
  unix:/tmp/vmon,server=on,wait=off -s`): `printf 'mouse_move 300 200\n'
  | nc -q1 -U /tmp/vmon`; also `button 1`, `button -r 1` (repeat = up),
  `sendkey h`. Verified end-to-end: twm root menu opened, keys typed into
  xterm, /dev/wsmouse0 shows events (type 6/7 = dx/dy).
- **Cursor UX, not a bug**: glxgears calls glutSetCursor(NONE) (no cursor
  over gear window); gtk display backend grabs the pointer on click —
  Ctrl-Alt-G releases it. That grab was the user's "can't do anything".
- Long-idle QEMU window may show "Display output is not active." with X
  alive (DPMS off releases the scanout) — check serial `ps` before
  chasing.
- **Regression builds**: `kernel=MICROVM` passes with all three fixes.
  `kernel=GENERIC` impossible: supasmol pruned `sys/arch/amd64/conf/
  GENERIC` (only MICROVM/SMOL exist on amd64 + i386 in this fork), so
  MICROVM is the regression config.

## SESSION 7b: live test — X wedges ~1 min after every boot (NEW BUG, unfixed)

Symptom during user live test: mouse moved briefly after click, then
"cursor disappeared"; QEMU window froze (static frame, then gray-stipple/
placeholder), later "Display output is not active." with Xorg alive.

Evidence (fresh boot, confirmed twice):
- Xorg up 1s, desktop renders at least the first frames (gears visible in
  window at t=0), glxgears/twm/xterm start.
- By t+1-2 min: X processes no new requests at all. `timeout N xset q`
  -> 124/143 (hangs). Screen frozen (screenshot md5 identical over
  seconds), ignores HMP mouse warps, ignores client SIGKILL wakeups.
- NOT a kernel/input problem: vioinput delivers (dd on /dev/wsmouse0
  shows fresh dx/dy while wedged); serial shell fully responsive;
  no new lines in Xorg.log (static 48 lines); no panic.
- NOT glxgears' fault: killed glxgears AND its `timeout 60` wrapper;
  xdpyinfo still hangs.
- All userspace procs (Xorg Sl, twm/xterm/glxgears) sleep in poll(),
  0.0% CPU. Xorg uses `/dev/dri/card0`, config /tmp/xorg.conf
  (modesetting+glamor, DisableVT), llvmpipe.

Leaning theory: Present/DRI3 **flip-event deadlock** — modesetting does a
page flip and waits for the DRM flip-complete event; NetBSD virtiodrm has
no vblank/flip event emulation (Linux emulates vblank with a hrtimer;
QEMU virtio-gpu has no vblank signal), so poll(card0) never fires and X
stays inside the flip wait, deaf to other fds. The earlier DRI2 xorg.conf
experiment changed behavior (broke presentation) — consistent-ish.

Debug state for next session:
- QEMU `-s` gdbstub is on tcp::1234, CR3 was 0x4c297000; HMP on
  /tmp/vmon. Host gdb is broken for this stub (NetBSD ABI handler
  missing -> 32-bit addresses; 'g packet too long 608 vs 308').
- Working kernel-memory reader recipe: monitor `pmemsave <pa> <n> rel.bin`
  (RELATIVE filenames only — leading '/' fails 'invalid char'!) and
  VA->PA = VA-0xffffffff80000000 for direct map; kernel-heap (0xffff...)
  needs a real 4-level walk — script /tmp/kstack.py (watch: 2MB PSE
  entries at L2 carry NX in high bits; check `ent & 1` for present).
- /dev/dri has card0..3 + renderD128..131 = FOUR cards for two QEMU GPUs
  (virtiodrm claims more than one?) — check card order vs X config next.
- allproc is a LIST with newest proc at the head (LIST_INSERT_HEAD);
  proc0.p_comm='system' (walk from head, do not assume).

Suggested next experiments (cheap first):
1. `Option "DRI" "1"`?? no — try xorg.conf Device Section `Option
   "NoGLAM"` + plain shadowfb; and/or run gears with `vblank_mode=0`
   (MESA_WAIT_FOR_SWAP never blocks) to see if the wedge needs swaps.
2. Kill the rc watchdog loop during test so glxgears stays dead.
3. Real fix if theory holds: emulate vblank in virtiodrm (hrtimer on
   scanout flush) or make drm poll on card0 return POLLOUT... i.e. check
   whether X's poll includes card0 and whether virtiodrm implements
   .output_poll/driver->output_poll_changed & event wakeups.

=====================================================================
SESSION 7e (2026-10-27): X-deaf bisected via rc stage harness.
=====================================================================

CRUX RESULT (fully reproduced, self-driving experiments):
The "X stops accepting/reading new client connections" bug is
triggered by **twm coexisting with Xt/Xlib utility clients**.

Stage matrix (rc self-test, `timeout 5 xset q` at +20s/+35s/fresh
xterm+40s; OK/DEAF marker files in /root/):
  OK:  twm-only, apps-only (xterm+xeyes+xclock), glxgears-only,
       twm+glxgears (tg), apps->sleep15->twm (swslow),
       xsets+apps+getty+glx NO WM (wmless: all 3 checks incl. late
       fresh xterm, X healthy >7 min, mouse injection dispatched)
  DEAF: any stage with twm AND one of xterm/xeyes/xclock, in ANY
       order and ANY stagger up to 15s (wms=2s, wms10=10s both died);
       final (xsets,apps,sl15,twm,getty,glx): healthy at +20/+35s,
       a fresh xterm at +40s still bricked X (DEAF3).
Killed theories: getty-on-console innocent; ktrace innocent; GLX/glxgears
innocent; simultaneity race innocent (10-15s staggers die too);
live grab innocent (killing twm, then ALL clients, never recovers X);
uptime innocent (late re-run of twm+client bricks again).
Trace evidence at moment of death: X accepts new conn fd, registers it
with **events=0 in poll array** (read via /proc/631/mem, pollfd at the
last poll(2) buf addr), then poll(9,inf) sleeps forever — client hello
never read; 60s straggler-close visible. Every later connection: same.
=> X 21.1.25 listen/ospoll state permanently corrupted once twm +
   a normal Xt/Xlib client have both been active. Re-arm/grab path
   suspect (cf. xorg-devel "ClientReady level vs edge trigger" class).

DECISION PENDING (asked user): ship desktop WITHOUT twm (fully robust,
interactive: wmless verified) vs WITH twm but fixed client set (no new
terminals; any new connect bricks X). Note: no alternate WMs in image.

Stage harness in image /etc/rc (host copy: smolBSD/service/vioglx/etc/rc):
- /root/.stage file (one word) selects client burst:
  full,nogetty,getty,wm,twm,apps,twmn,wms,wms10,swslow,swapwm,tp1,tp2,
  tp3,tg,glx,xsets,final,wmless; unknown/absent = no clients (control).
- rc runs burst, checks X at +20s (+35s, +fresh-xterm+40s), touches
  /root/{OK,DEAF,OK3,DEAF3}-$STAGE, starts serial getty, idles with
  X-monitor loop (touches /root/XDEAD-$STAGE if X dies). Read markers
  from host: debugfs `ls /root`. Serial echoes "VIOGLX STAGE x: ...".
- /root/.noauto still skips glxinfo/probes entirely (pre-stage control).

FS HYGIENE (learned the hard way): debugfs writes to the image while the
guest was SIGTERM-killed corrupted ext2 accounting -> guest
`panic: ext2fs_valloc: dup alloc` / `ext2fs_mapsearch: map corrupted`
reboot loops. Also the FS superblock had s_blocks_count=1572608 vs
partition 1572603 (e2fsck refused). FIXED: debugfs
`set_super_value s_blocks_count 1572603` + `e2fsck -fy` (clean, twice).
NEW RULE: after EVERY debugfs write session, run `e2fsck -fy
/dev/mapper/loop0p1` before boot (cycle scripts do this now: /tmp/cycle2.sh).
NOTE glxinfo DOES exist in the image (the "not found" msg = the
/root/.noauto guard's else-branch, not a missing binary).

Other session notes:
- Serial console: no getty on serial unless rc starts one (stage harness
  starts it AFTER the burst+checks). Console is NOT hijacked by X (rc
  echoes reach serial fine).
- X input goes through /dev/wsmux0 (fd 13) via SIGIO (no read()/ioctl
  of fd 13 in kdump; wake path is async). Mouse injection proof: HMP
  mouse_move burst -> immediate read(0xa client requests (xeyes pointer
  tracking) => events delivered.
- screendump still "no surface" (GTK gl=on); can't inspect window from
  HMP this session.
- Current image state: /root/.stage = "wmless" (healthy desktop now
  running on host QEMU, load ~2.9 from llvmpipe gears — consider
  glxgears -display idle or timeout).
- Open cleanup: vglx-gui.sh still has bogus -D/trace flags; boot ktrace
  (xtry) should become optional (15-30MB boot.tr per boot); purge
  /root/{OK,DEAF}*-stage markers before shipping.

Suggested next steps:
1. Get user decision: WM-less desktop vs twm+fixed set.
2. If twm required: try X option `Option "DRI" "2"` (DRI2) — DRI3 fd
   dance on new connects is an untested confounder for the late-xterm
   brick; also try building X with WERROR off... (user said no rebuild).
3. If wmless ships: remove stage machinery from rc default path, keep
   xsets+apps+getty+glx + X-monitor; verify mouse/keyboard in xterm by
   typing via HMP sendkey; then real virgl (virtio gallium) Mesa.

UPDATE (same session) — ctwm SOLVES IT, DECISION RESOLVED:
NetBSD xbase ships ctwm alongside twm (already present in image at
/usr/X11R7/bin/ctwm; fvwm/openbox are pkgsrc-only via PKGSITE).
Stage tests: `cw` (ctwm+xterm) OK; `cwm` (xsets,ctwm,apps,getty,glx)
OK on ALL three checks incl. fresh xterm at +40s — the exact test that
bricks twm builds (DEAF3-final). The X 21.1.25 listen-path corruption
is triggered by twm specifically; the maintained ctwm fork avoids it.

SHIPPED CONFIG (installed to image, verified boot + 3min soak + fresh
xterm + serial interactive; markers OK-none/OK3-none, ctwm alive):
- /etc/rc GUI default (no /root/.stage): burst = xsets,ctwm,apps,
  getty,glx, three self-checks, X-monitor loop; serial getty.
- ktrace now opt-in: only when /root/.trace exists (xtry()).
- Stage harness retained (documented above) for future bisecting.

SESSION 7f — KEYBOARD DEAD AT X: ROOT CAUSE FOUND (mux):
Symptom: HMP sendkey produces ZERO input to X (mouse fine). Proof chain:
- Xorg fds: only /dev/wsmux0 (fd 13, char 65,0) + /dev/ttyE0 (fd 7) open.
  No /dev/wskbd* fd. X's NetBSD WSEMU keyboard path reads wsmux0 ONLY.
- ktrace during sendkey bursts: read(0xd...) fires for mouse_move but
  never for sendkey => keys never arrive on wsmux0.
- Attach log: wskbd0 at vioinputX mux 1; wsmouse0 at vioinputY mux 0.
  Reordering QEMU devices (mouse before keyboard) does NOT change this.
WHY mux 1: sys/conf/files:1067 `define wskbddev {[console = -1], [mux = 1]}`
is the implicit-wildcard default for ALL wskbd children. Console keyboards
ignore cfdata mux (go to console mux) which is why stock wscons desktops
work; our vioinput wskbd has console=0 (console=com serial) so it obeys
the mux-1 default and X (reading mux 0) never sees keys. virtio_input.c
memset()s wa and cannot set the cfdata locator from the parent.
FIX (kernel config, no driver change): sys/arch/amd64/conf/SMOL adds
    wskbd*		at wskbddev?	mux 0
so wskbd attaches to /dev/wsmux0 where X's WSEMU reads it. wsmouse dev
default is mux 0 (files:1068). Rebuild SMOL + boot; verify via focus
xterm + HMP sendkey typing, and ktrace read(0xd on sendkey.
OPS GOTCHA: obj/tools had been gutted between sessions (only nbmake
remained); kernel= build aborts "nbconfig does not exist" -> rerun
`./build.sh -U -u -j8 -O obj -T obj/tools tools` once, then kernel=.


## SESSION 7g - KEYBOARD WORKED: 4 stacked root causes, all fixed & committed

KEYBOARD IS FULLY WORKING. Unattended-boot E2E proof: fresh boot, HMP
click + sendkeys typed "touch /var/tmp/kf17"+Return into the rc-launched
desktop xterm -> file appeared. No manual intervention needed.

Four independent layers were broken; each masked the next:
1. KERNEL (the big one): virtio_input.c called wskbd_input(dev, code,
   down) but contract is (dev, type=UP/DOWN, value=keycode). Console
   survived (pre-translated ASCII); X's WSReadInput saw event.type=evdev
   keycode and dropped every event. Fixed + aligned wskbd_translate()
   call site; WSKBDIO_GTYPE passthroughs added (wskbd returns
   WSKBD_TYPE_PC_XT; wsmux forwards).
   -> NetBSD-src viogpu-glx commit 8c71a0be30ae (both hops).
2. WM FOCUS: no /root/.ctwmrc existed; twm/ctwm compiled-in defaults
   NEVER call SetInputFocus, so even perfect events reached no window.
   Added /root/.ctwmrc: ClickToFocus + RaiseOnClick + Button1/2/3 = :
   RaiseFocus (kept on disk at service/vioglx/root/.ctwmrc).
3. INPUT STARVATION: default GUI chain ended with glx (glxgears);
   llvmpipe glxgears pins a CPU and X stops reading wskbd/wsmouse fds -
   ALL input looks dead (0/10 delivered) while interrupts keep arriving
   and xset q still answers. Removed glx from default chain (rc line
   `*) C=xsets,ctwm,apps,getty ;;`).
   NOTE: smolBSD history was rewritten per user request - the vioglx
   test-service commits (90d105c, c915638) were DROPPED and main
   force-pushed to 6743e6f; service/vioglx + vglx-gui.sh survive as
   untracked files (backup branch: backup-vioglx). A proper graphics
   service will be created later.
4. HMP SYNTAX: mouse_button takes ONE arg (0|1); `mouse_button 1 1`
   errors "extraneous characters" silently through >/dev/null. Use one
   command per nc session for reliability, or one session multi-line
   only when X is not saturated.

DIAGNOSTICS THAT CRACKED IT (repeatable recipe):
- `xinput test 6` (kbd0) / `test 7` (mouse0) armed via
  `(DISPLAY=:1 timeout N xinput test D >log 2>&1 &)`; send HMP events;
  count lines. Proves QEMU->X pipeline independent of clients/focus.
- Burst pattern: all keys delivered correctly (X keycodes = evcode+8:
  38=a,52=z) once glxgears was killed. Keycode check catches arg-swap
  class bugs instantly.
- `vmstat -i` diff around an event: ioapic pin count +2 per 2 mouse
  moves => hardware interrupts fine => look downstream.
- `cat /dev/wskbd0` while X runs = "Device busy" (single listener).
- ktrace on X is DANGEROUS on this image: idle X produces ~1GB traces
  (kb3.tr was 759MB and filled the 5.9G root fs; fs-full wedges serial
  console output and makes X DEAF loops). If ktracing: trace short
  windows, delete file immediately after, check `df -h /`.
- Serial console freezes (output stalls, echo continues) whenever root
  fs is full or kdump grinds under load; commands queue and dump in one
  burst when released.
- rc VIOGLX STAGE output = one-shot diagnostic chain, harmless; the
  ~42s "os has halted" happens when Xorg dies (rc while-loop exits ->
  haltprompt waits for key on serial).
- Window cascade is predictable: requested +X+Y lands at +X+1/+Y+16
  (ctwm frame adds 1px border + 16px title).

OPS: image debris cleaned via debugfs (kb3.tr 759MB, kt.tr 45MB,
kt9.tr 38MB, test logs; e2fsck clean, fs now 27% used). Deployed kernel
= obj SMOL with 8c71a0be30ae (gitignored binaries; rebuild recipe
unchanged). Live guest /etc/rc updated in place (sed) to match repo.

NEXT: virgl. Guest Mesa = llvmpipe/swrast; no DRI dir in image (verified:
/usr/X11R7/lib/dri absent). Decide pkgsrc vs xsrc build of virtio
gallium driver, then check glxinfo/glxgears -info (glxgears pins a CPU
under llvmpipe - expected).

HUMAN TEST REPORT (user, live at the QEMU window - morning task list):
KEYBOARD typed fine interactively, BUT the MOUSE misbehaves:
- "really weird movements"
- cursor eventually DISAPPEARS while xeyes keeps following it
  => pointer position/events are alive in X (xeyes proves event stream)
  but the visible cursor sprite is gone/off-screen => suspect cursor
  rendering in viogpu/X (hw cursor vs llvmpipe sw cursor compositing),
  or acceleration/warp weirdness at HMP-vs-real-device level.
NOTE: HMP mouse_move is RELATIVE injection only; a real USB/PS2-style
device wasn't exercised yet - some "weirdness" may be HMP-only, but a
vanishing cursor with live events points at cursor sprite rendering.
Morning repro steps: boot, xeyes up by default; move mouse in circles;
watch cursor vanish; then `xset q`, check `xf86Cursor`/`-audit 4`? Try
`Option "SWCursor" "true"` in the viogpu Device section of /tmp/xorg.conf.


SESSION 7h (Oct 28) — MOUSE FIXED: invisible cursor + weird movement
=====================================================================
SYMPTOM (user, real mouse at QEMU window): pointer moves "really
weird", cursor sprite vanishes while xeyes keeps following.

ROOT CAUSES (two, both GUEST-side; kernel untouched):
 1. CURSOR: X runs on modesetting with HW cursor; QEMU gtk,gl=on never
    draws the guest cursor for a GL scanout (and hides host pointer)
    => sprite invisible although X pointer is alive. Fix: modesetting
    Option "SWCursor" "true" in Device section => X composites sprite
    into scanout, always visible. (Option "HWCursor" "false" is
    IGNORED by this modesetting build - logs 'not used', like
    GLXVBlank. Use SWCursor true!)
 2. MOVEMENT: X core pointer accel default 2/1 thresh 4 => double
    speed felt weird. Fix: `xset m 1 0` at end of stage chain.
    NOTE: boot-time xsets values get reverted mid-boot (DPMS came back
    enabled too, root cause not chased) -> added dedicated `xs` stage
    token AFTER apps (re-applies xset m 1 0 + xset -dpms; verified
    sticky in final state).
 3. BONUS FIX: X came up 640x480 (QEMU EDID preferred) leaving garbage
    outside desktop; xsets stage now runs `xrandr -s 1280x1024` before
    WM. Final verified state: 1280x1024, accel 1/1 thresh 0, DPMS off,
    visible SW cursor, xeyes aims at cursor, root-menu click works.

CHANGED FILES:
 - images/vioglx-amd64.img:/etc/rc (live-patched; image IS the state)
 - smolBSD repo copy service/vioglx/etc/rc overwritten from image
   (both untracked-on-disk examples, do not commit per user rule).
 No commits (no src changes). No kernel change.

OPS GOTCHAS LEARNED THIS SESSION:
 - FIFO launcher direction: the keeper must be a WRITER:
   `setsid sh -c 'exec sleep 999999 > /tmp/vfifo' &` then
   `(setsid nohup ./vglx-gui.sh < /tmp/vfifo > LOG 2>&1 &)` in same
   shell (read-open of < fifo blocks until writer exists).
 - `kill Xorg` => rc shutdown hook fires => "os has halted"; press any
   key on serial (printf '\n' > /tmp/vfifo) to reboot.
 - screendump is DEAD with gtk,gl=on ("no surface"). Screenshot recipe
   (host): DISPLAY=:0.0 ffmpeg -loglevel error -f x11grab -draw_mouse 0
   -video_size 640x530 -i :0.0+50+100 -frames:v 1 -y /tmp/x.png
   then view_image. QEMU window shows guest 1280x1024 scaled ~0.5.
   -draw_mouse 0 hides the HOST cursor so you only see the guest sprite.
 - Boot-time glxgears diagnostic leaves frozen frames until apps
   repaint - cosmetic, not a bug.

NEXT: (optional, user's-eye) verify physical mouse feel at window.
Then virgl recon: guest has no /usr/X11R7/lib/dri at all; decide
pkgsrc vs xsrc for virtio gallium driver; glxinfo should then stop
reporting llvmpipe.

SESSION 7i (Oct 28) - Y AXIS INVERTED (user, real mouse): fixed
=================================================================
Kernel bug, 3rd root cause of the mouse report: vioinput_handle_event
passed evdev REL_Y (positive = DOWN) straight to wsmouse_input, but
wscons convention is dy positive = UP (upstream hidms.c: "Y direction
must be flipped for WScons event processing"). Wheel already flipped,
axis Y was not. Fix: sc->sc_dy -= ev->value (commit on viogpu-glx,
pushed both hops; kernel rebuilt+deployed, same build recipe).
VERIFIED objectively: clamp top-left (HMP -3000 -3000), mouse_move 0
300 (down), click => ctwm root menu at guest y~300, not 724.
NOTE: REL_WHEEL flip comment predates this; wheel direction untested
by human - ask user to check scrolling (if inverted too, its comment
was a guess that happens to match; test before touching).

SESSION 8 (Oct 28) - VIRGL 3D WORKING (virgl renderer at last!)
================================================================
Two root causes killed, in order:

1) AIGLX "not DRI2 capable": Mesa's loader_get_driver_for_fd() ->
   drmGetDevice2() failed because libdrm's NetBSD path needs a pci:
   busid and microvm has NO PCI (virtio-mmio sets empty busid).
   Fix: patched xsrc libdrm (~/src/xsrc/.../libdrm/dist, UNCOMMITTED):
   xf86drm.c 3 hunks - drmParseSubsystemType falls back to
   drmGetVersion name "virtio*" -> DRM_BUS_VIRTIO; drmParsePciBusInfo
   accepts 0:0:0.0 for virtio kernel drivers; drmParsePciDeviceInfo
   returns synthetic 1af4:1050 (virtio-gpu). meson.build: NetBSD
   needs cc.find_library('pci') (pcibus_conf_read). Built 2.4.134 via
   meson cross (~/src/mesa-nb/build-libdrm), patchelf SONAME ->
   libdrm.so.3, deployed as /usr/X11R7/lib/libdrm.so.3.8 in image
   (official kept as .orig). drmGetDevices2 now returns 1 dev,
   drmGetDevice2 ok on card0+renderD128.
   Side quest red herrings: guest has NO hints file and no ldconfig
   (stripped); "Shared object libdrm.so.3 not found" applied only to
   our patchelf'd probe binaries (LD_LIBRARY_PATH fixed them); Xorg
   always found /usr/X11R7/lib fine (default search).

2) Xorg SIGSYS crash with the new libdrm in place: ktrace/kdump showed
   shmget(IPC_PRIVATE,4096,0200) -> ENOSYS then SIGSYS right after
   "Initializing extension MIT-SHM". MICROVM configs never had SVR4
   IPC and supasmol commit 7aba4f56a599 deleted sys/kern/sysv_*.c
   entirely; the svr4 stub sends SIGSYS (X's probe races the delayed
   signal and dies; -extension NoMIT-SHM does NOT skip the probe).
   Fix (committed this session): vendored netbsd-11 sys/kern/sysv_{ipc,
   msg,sem,shm}.c + sys/compat/common/compat_sysv_mod.h (trunk drifted
   post-2025-07 base; netbsd-11 versions are <=2025-05, all compile
   clean), 4 file stanzas in sys/kern/files.kern (upstream pattern),
   SMOL: options SYSVSHM + SYSVSEM. Kernel rebuilt, deployed
   (old kept as kernels/netbsd-SMOL-vioglx.pre-sysv).

RESULT (verified): /var/log/Xorg.1.log shows
  modeset(0): [DRI2] DRI driver: virtio_gpu
  AIGLX: Loaded and initialized virtio_gpu
  GLX: Initialized DRI2 GL provider for screen 0
glxinfo: renderer "virgl (AMD Radeon RX 7600 (radeonsi, navi33...)",
GL 4.3 core, direct rendering Yes. glxgears renders live on the gtk
window (screenshot-verified). rc debug hacks reverted (shutdown
include restored); MESA_LOADER_DRIVER_OVERRIDE proved UNNECESSARY once
libdrm is patched - commented out in rc (repo service/vioglx/etc/rc
resynced from image, still untracked per policy).

Guest state: /usr/X11R7/lib/libdrm.so.3.8 = patched build (keep!).
Probe binaries /root/{drmprobe,eglprobe} + testdrm3d in image.
FPS note: glxgears ~6 fps synchronized (low) - plausible host/gfx
vblank path; worth a look only if user cares.

NEXT: (optional) upstream libdrm patch to xsrc (gnats?); remove
pkgsrc meson/ninja from nbgdb if user wants; commit smolBSD etc/rc
if policy changes.

=================
SESSION 9 (2026-10-28): glxgears 5 FPS -> ~80 FPS (display backend)
=================

SYMPTOM: virgl works (RX 7600 renderer) but glxgears steady 5.4 FPS;
llvmpipe (no virgl) 3x faster. Diagnosis chain (all measured):
- Not vsync (vblank off no change; GLXVBlank false, PageFlip false).
- Guest kernel clean: glxgears ktrace = 133 execbuffer+fence pairs/sec
  (~24 per frame) from CLIENT Mesa, all ioctls <30us; Xorg only
  44 execbuffer/7s (~1/frame - normal).
- Guest blocks in VIRTGPU_WAIT each pair => ~7ms serial round-trip.
- Host NOT CPU-bound: QEMU main thread 2.5% CPU; ~2.3k renderD128
  ioctls/s at 10us each, spaced ~0.4-1ms by main-loop pacing.
- Minimizing gtk window: no FPS change (gtk *present* not pacer).
- Host GPU clocks auto->high: only 5.4->6.4. DRI3 off: no change.
- VIRGL_3D_THROTTLE=0 (guest Mesa): bursts to 26 but steady 5 on gtk.
- BREAKTHROUGH (user suggestion): -display sdl,gl=on instead of gtk:
  SDL+THROTTLE=0 => 59-81 FPS; SDL+THROTTLE=1 => 13. Both needed.
  => gtk GL scanout area paced each round-trip ~1ms; ~24 fence
     round-trips/frame x (gtk pacing + Mesa swap throttle) = 185ms.

FIXES PERSISTED:
1. Host: QEMU rebuilt with SDL (--enable-sdl; libsdl2-dev installed on
   Mint host). ~/src/smolBSD/vglx-pty.sh now -display sdl,gl=on.
   NOTE: guest still virgl-rendered on host GPU; only host UI changed.
2. Host: /etc/tmpfiles.d/90-amdgpu-virglgl.conf pins
   power_dpm_force_performance_level=high (marginal +10-15% alone).
3. Guest image /etc/rc: export VIRGL_3D_THROTTLE=0 (next to the
   commented MESA_LOADER line); repo copy service/vioglx/etc/rc
   resynced (also carries PageFlip false + commented override now).

Side notes: -display none rejects virtio-gpu-gl-device (needs GL
backend); egl-headless listed in -display help but aborts at startup
("egl: not available on this platform", not chased). ktrace -c is
WRONG (tried pid 0) - use ktrace -C -f <trfile>. top -bH first
iteration = since-boot averages; use -n2. Host strace needs
kernel.yama.ptrace_scope=0 (sysctl, reverted? currently 0).

REMAINING (optional): upstream libdrm virtio-mmio patch (xsrc, still
uncommitted); nbgdb pkgsrc meson/ninja cleanup; gtk remains fallback
if SDL window misbehaves (input grab/resize).

---

## Session 10 (2026-10-28): single GL GPU, single window

Question: two virtio-GPU devices existed because X needed a wscons
console (ttyE0) and Mesa needed DRM — the 2D device provided viogpu's
wsdisplay console, the GL device provided virtiodrm/KMS+virgl.  QEMU
SDL makes a window per console => two windows, and (SDL 2.30) mouse
routing/segfault trouble with two consoles.

Root cause of "X: No console driver found" when booting with ONLY the
GL device: virtiodrm_match() (fork's virtio_compat.c) claims every
virgl-capable GPU and NetBSD drm2 KMS devices attach no wsdisplay
(drmfb is only wired for SoC fb drivers in this tree).  So with one GL
GPU there was no ttyE0 at all.

Kernel fix (SMOL; 3 files):
- sys/external/bsd/drm2/virtio/virtio_compat.c: virtiodrm gains a real
  softc and, in attach, a vcons+rasops wsemuldisplaydev child
  (1024x768x32 kmem fb, never scanned out; accessops GTYPE=PCIMISC,
  GET_FBINFO/GINFO/LINEBYTES, mmap->-1).  Purpose: ttyE0 exists for X's
  xf86OpenConsole; pixels still go through DRM modesetting.
- sys/external/bsd/drm2/virtio/files.drmkms_virtio: device line +=
  wsemuldisplaydev, vcons, rasops32.
- sys/arch/x86/conf/MICROVM.common: added `wsdisplay* at virtiodrm?
  kbdmux -1` (viogpu line kept for plain 2D GPUs).
Rebuild: ./build.sh -U -u -j8 -m amd64 -O obj -T obj/tooldir kernel=SMOL
(deploy: cp obj/sys/arch/amd64/compile/SMOL/netbsd
~/src/smolBSD/kernels/netbsd-SMOL-vioglx).

QEMU fix (~/src/qemu/ui/sdl2.c, rebuild = ninja qemu-system-x86_64):
the second window was the SERIAL TEXT console: QEMU hid non-graphic
consoles only when index != 0.  Patch: hide all non-graphic consoles.
=> one `-device virtio-gpu-gl-device` => exactly one SDL window,
1280x1024 (KMS mode via EDID), boot console black until X (acceptable;
boot text still on serial).

Verification (fresh boot):
- dmesg: `wsdisplay0 at virtiodrm0`, screens 0-3 added; Xorg :1 up
  (modesetting), ctwm/xterm/xeyes visible in the single window.
- input: keyboard works ungrabbed (xinput test kbd0 sees XTEST keys via
  SDL).  Mouse ONLY works while SDL grab active: left-click inside the
  window grabs (ctrl-alt releases) — this is stock QEMU SDL behavior,
  not a bug.  Verified motion deltas end-to-end (xinput test mouse0).
- guest X cursor is a KMS overlay: invisible to xwd/host capture;
  check xeyes pupils or xinput instead.

Run recipe (current vglx-pty.sh): single GL device + `-display sdl,gl=on`
+ VIRGL_3D_THROTTLE=0 (in guest rc).  /root/.keepalive still exists in
the image (suppresses halt if X ever fails; remove when confident).

REMAINING (optional): libdrm xsrc patch upstreaming; nbgdb meson/ninja
cleanup.  (QEMU sdl2.c patch COMMITTED session 11: ce4fc79e51.)

## Session 11 (2026-10-28): SDL crash root-caused + serial login fixed

The "desktop disappeared" crash = WHOLE QEMU died, not ctwm:
- dmesg SIGSEGV x2, stack (now symbolized via debuginfod, build-id
  832936b3..., /tmp/sdl.dbg): SDL_PollEvent -> X11_PumpEvents ->
  X11_DispatchEvent (SDL_x11events.c:1240, ConfigureNotify MOVED case) ->
  SDL_IBus_UpdateTextRect (SDL_ibus.c:728) -> NULL deref of
  SDL_GetDisplayForWindow(focused_win) (focused window = the HIDDEN
  serial-console SDL window, never assigned a display because hidden
  windows skip SDL_AssignDisplay).  Known SDL 2.30.0 bug, NULL-check
  added upstream in 2.30.2+; host pinned at 2.30.0+dfsg-1ubuntu3.1 (noble).
- Trigger needs: ibus active + SDL TEXTINPUT enabled + ConfigureNotify on
  the hidden window WHILE it has SDL keyboard focus => sporadic (minutes).
- WORKAROUND: launchers set XDG_CONFIG_HOME=/tmp/qemu-no-ime so
  IBus_GetDBusAddressFilename finds no bus file -> ibus never inits ->
  TEXTINPUT stays disabled -> all 4 crash call sites guarded.  Applied in
  vglx-pty.sh, vglx-gui.sh, /tmp/qgdb.sh.  Cost: no host IME into guest
  (keysyms still work).  Stress-tested (clicks, grabs, window moves,
  minimize/restore, moves on the hidden window too) = no crash.
- Symbolized the crash by downloading DWARF: curl
  http://debuginfod.ubuntu.com/buildid/<build-id>/debuginfo -o dbg; then
  gdb -s dbg -e /lib/.../libSDL2-2.0.so.0 -ex 'info line *0x...'.

Serial console login was broken all along (prior "serial shell" input was
cosplay — nobody was logged in; tty00 getty = login prompt, root had locked
password, pwd.db was stale):
- /etc/rc (image + repo service/vioglx/etc/rc, dir is git-excluded):
  `getty) pwd_mkdb -p /etc/master.passwd >/dev/null 2>&1;
  /usr/libexec/getty -a root default </dev/console ...`.
- Serial login NOW WORKS: `root` + EMPTY password.  (getty -a autologin
  still flaky — init/ttys race; don't sink time into it, manual login is
  reliable.)
- legacy /etc/passwd root hash was `*` (locked); master.passwd empty.

ctwm: image /root/.ctwmrc now valid ctwm 4.1 syntax (ClickToFocus,
RaiseOnClick, Button1-3 root f.nop); rc logs ctwm stderr to
/var/log/ctwm.log (61 bytes = clean; the find warning is harmless).
Minimal rc merges with ctwm BUILT-IN DEFAULTS -> you WILL see a Help Menu
(some F-key/default binding), top-right list, right-edge icon column:
normal, not the old broken-menu symptom.

Debug wrapper /tmp/qgdb.sh (gdb -batch catch SIGSEGV; MUST handle
SIGUSR1 nostop pass or QEMU deadlocks at boot).  kpartx workflow for
image edits: kpartx -av img, mount /dev/mapper/loop0p1, umount + kpartx -d
when done (VM must be off for rw edits to stick).

REMAINING: upstream SDL2 users may want the same XDG_CONFIG_HOME trick or
SDL 2.30.2+; optional: private SDL 2.32 build in ~/opt if IME needed.

SESSION-11 ADDENDUM - the user's "WM crashes on second click" (again):
NOT a crash at all.  rc launches ctwm from init's env which has NO HOME,
so ctwm never found /root/.ctwmrc and ran on BUILT-IN DEFAULTS:
Button1 root = f.menu "defops" whose last items are "Restart" f.restart
and "Exit" f.quit (ctwm --dumpcfg shows all this; ctwm 4.1 binary flag).
Cursor parked bottom-right -> click1 pops defops menu -> click2 lands on
"Exit" -> ctwm exits 0, silently, no core, clean log.  Matches every
observation.  FIX: rc line 214 now `ctwm)  HOME=/root ctwm >/var/log/
ctwm.log 2>&1 & ;;` (image + repo).  Verified post-boot: no defaults icon
column/list, decorations drawn, Button1-on-root = f.nop (held-press test
shows no menu).  LESSON: verify config actually loads (visual signature
like the iconman column), not just that the file is syntactically valid.
Synthetic-input note: guest cursor position after SDL grabs is unknowable
without clamping (huge negative delta to hit 0,0, then +dx +dy) and xdotool
click press+release is too fast to screenshot a twm menu - use mousedown,
screenshot, mouseup.  xinput here has no `warp`; guest has no cc.

SESSION-12 - MOUSE: virtio-tablet + NetBSD-only absolute support (SOLVED).
Symptom chain: relative virtio-mouse under SDL "corner-freeze" - with the guest
cursor visible QEMU SDL leaves relative mode and warps the HOST pointer onto the
guest cursor every motion; at the window edge the warp clamps, so the next
delta computed from absolute coords is 0 forever (pointer pinned bottom-right,
keyboard still fine because SDL forwards keys regardless of grab).  A QEMU
sdl2.c xrel/yrel patch fixed mid-screen but a pointer parked at the corner is
still fatal -> user chose virtio-tablet-device (absolute), AND declared the
hard policy: QEMU stays as-is, all fixes on the NetBSD side.

Gotcha that ate a round: QEMU's input core (ui/input.c
qemu_input_queue_abs, INPUT_EVENT_ABS_MAX) normalizes ALL abs axes to 0..32767,
NOT pixels - a naive "abs==pixels" delta conversion runs 25.6x too fast
(32767/1280).  Linux handles this via VIRTIO_INPUT_CFG_ABS_INFO + evdev axis
scaling; NetBSD has no driver in-tree feeding WSMOUSE_INPUT_ABSOLUTE_* yet.

FIX (kernel only, sys/dev/virtio/virtio_input.c):
  - sc_abs set when device advertises EV_ABS.
  - EV_ABS ABS_X/ABS_Y stash raw values (screen convention, y-down, no flip);
    SYN_REPORT delivers wsmouse_input(dev, buttons, ax, ay, dz, dw,
    WSMOUSE_INPUT_ABSOLUTE_X|WSMOUSE_INPUT_ABSOLUTE_Y).
  - new vioinput_msioctl answers WSMOUSEIO_GCALIBCOORDS with {0..32767,
    samplelen=WSMOUSE_CALIBCOORDS_RESET} and swallows SCALIBCOORDS.  This works
    with NO wsmouse.c change: wsmouseioctl() forwards unrecognized
    WSMOUSEIO_* to the backend accessops->ioctl (wsmouse.c ~line 922).
X side needs ZERO changes: the xsrc "ws" driver (xsrc/external/mit/
xf86-input-ws/dist/src/ws.c, nia's 2021 calibration port) probes
WSMOUSEIO_GCALIBCOORDS in PreInit; RESET => raw mode, forces
WSMOUSE_TYPE_TPANEL, registers Absolute valuator axes 0..32767, X scales to
screen itself.  Verified post-boot in /var/log/Xorg.1.log:
  "mouse0 detected calibration data in raw mode, using touch panel mode"
  "mouse0 maximum x position: 32767" (and y).
User acceptance: cursor 1:1, window picking/dragging works, SDL GL still ~80
FPS.  QEMU is now PRISTINE v10.2.0-rc4: the session-10 local commit
ce4fc79e51 (hide non-graphic SDL consoles) was DROPPED per user request -
observed post-drop, stock QEMU 10.2 SDL maps only one window anyway (the
extra console window exists but stays unmapped), so the patch was
redundant.  Verified: single window shows the desktop, virgl fine, mouse
fine.

ws.c quirks to remember: ABS X value 4095 exactly is skipped by the driver
(`if (event->value == 4095) break;`), and ax==0/ay==0 events are dropped
(`if (ax)`) - both inherited touchscreen special-cases, one-pixel cosmetics.
Absolute axes are only registered when priv->type == WSMOUSE_TYPE_TPANEL;
wsmouse.c GTYPE always answers 13 (BAREPS2), so the calibration-RESET
detection is the ONLY way an X client becomes absolute here.

xsrc source access: not on GitHub NetBSD/src; ~/src/xsrc is a blob:none
partial clone of github/NetBSD/xsrc - ws.c is tracked but not checked out:
`cd ~/src/xsrc && git show HEAD:external/mit/xf86-input-ws/dist/src/ws.c`.
anonhg raw-file URLs are bot-gated (403).

WHEEL: RESOLVED, no changes needed - it was already working with the
session-12 commit.  QEMU's stock virtio-tablet advertises EV_REL REL_WHEEL
(wheel-axis prop default true) and converts wheel clicks to REL_WHEEL +-1;
vioinput's EV_REL path feeds dz into wsmouse_input (DELTA_Z, no ABSOLUTE_Z
flag); X ws driver maps DELTA_Z to buttons 4/5 (ZAxisMapping default "4 5
6 7"); the X server then turns 4/5 into smooth-scroll valuators.  Proof:
driver DebugLevel 4 shows "Relative Z -1 / post button event 4 1/0" and
xinput test-xi2 shows RawButtonPress detail 4/5.  Earlier "wheel dead" and
"no XI2 events" conclusions were testing artifacts: XTEST click 4 needs the
host pointer OVER the SDL window (it is now at :0 +296,28 - use DISPLAY=:0,
xdotool mousemove first), xdotool --window XSendEvent does NOT deliver
wheel to SDL, filtered "EVENT type 4" blocks have NO detail line (raw ones
do), and guest rc floods the 9600-baud serial at boot so send serial input
only after the backlog drains (poll for the prompt).  Guest rc HALTS+reboots
the box if Xorg dies - never pkill Xorg casually.  And never leave the
image host-mounted rw while QEMU boots on it (ext2fs map-corrupted panic,
fixed with e2fsck -fy loop0p1).

CLEANUP (post-session-12): NetBSD will NOT receive PRs from us - their new
anti-AI policy; the xsrc libdrm virtio-mmio patch stays a local uncommitted
patch in ~/src/xsrc (keep the diff, do not upstream).  nbgdb pkgsrc cleanup
DONE: meson-1.12.0nb1 and ninja-build-1.13.2 pkg_delete'd (nothing else
depended on them).  smolBSD service/vioglx/ + vglx-pty.sh deliberately left
UNCOMMITTED - user has different plans for them; do not delete, they will be
useful.
