# Using virtio-gpu on NetBSD (viogpu-glx branch)

Two drivers share the single PCI/MMIO device ID `VIRTIO_DEVICE_ID_GPU`:

| Driver | Claims | Provides |
|---|---|---|
| `viogpu(4)` | plain virtio-gpu | `wsdisplay(4)` text console on `/dev/ttyE0..3` |
| `virtiodrm(4)` | GPUs offering `VIRTIO_GPU_F_VIRGL` | DRM/KMS `/dev/dri/cardN`, render nodes, virgl 3D |

The split happens in autoconf: `virtiodrm_match` claims virgl-capable GPUs
with confidence 2; everything else falls to `viogpu(4)` (confidence 1).
Attach the GPUs you want from QEMU — one of each is a common setup:

```
-device virtio-gpu-device          # -> viogpu0 + wsdisplay0 (text console)
-device virtio-gpu-gl-device       # -> virtiodrm0 + /dev/dri/card0 (3D)
```

## 1. Build the kernel

Config requirements (already in `sys/arch/x86/conf/MICROVM.common`, which
`arch/amd64/conf/SMOL` includes):

```
options   WSDISPLAY_DEFAULTSCREENS=4   # see gotcha below
wsdisplay* at viogpu?
virtiodrm* at virtio?
viogpu*    at virtio?
```

```sh
cd /path/to/NetBSD-src
./build.sh -U -u -j8 -m amd64 -O obj -T obj/tooldir kernel=SMOL
# result: obj/sys/arch/amd64/compile/SMOL/netbsd
```

## 2. Run under QEMU (>= 8.2, built with virglrenderer)

```sh
export DISPLAY=:0.0                       # host X/Wayland for egl-headless
qemu-system-x86_64 -smp 2 -accel kvm -M microvm,rtc=on,acpi=off,pic=off \
  -m 2048 -cpu host -kernel netbsd \
  -drive if=none,file=disk.img,format=raw,id=hd0 \
  -device virtio-blk-device,drive=hd0 \
  -append "console=com root=NAME=<wedge>" \
  -global virtio-mmio.force-legacy=false \
  -device virtio-gpu-device -device virtio-gpu-gl-device \
  -display egl-headless -d guest_errors \
  -serial file:/tmp/nb.log
```

Expected serial log:

```
viogpu0 at virtioN: features: ...
viogpu0: 1280x800, 32bpp
wsdisplay0 at viogpu0 ...                 # text console on ttyE0..3
virtiodrm0 at virtioM                     # the -gl- device
[drm] ... virgl                           # 3D capset active
```

3D smoke check inside the guest: open `/dev/dri/card0`, `GETPARAM`
`VIRTGPU_PARAM_3D_FEATURES` and the DRM round-trip (see
`~/src/smolBSD/testdrm3d.c` in the smolBSD workspace for a self-contained
test: prints `DRM 3D SMOKE PASS`).

## 3. Gotchas

- **`WSDISPLAY_DEFAULTSCREENS`**: a wsdisplay unit that is *not* the kernel
  console creates **zero** screens by default, so every `/dev/ttyE*` open
  returns ENXIO and X says `xf86OpenConsole: No console driver found`.
  Set it to >= 2 (4 recommended) for any config where the console stays
  on `com`/`viocon` — on microvm those are the *only* possible boot
  consoles; viogpu attaches too late to take over.
- The kernel console (and your dmesg serial log) stays on `com0`; X takes
  over ttyE0 without disturbing it.
- `virtiodrm` needs the GPU to be created **gl-capable** on the QEMU
  command line (`virtio-gpu-gl-device`, or `virgl=on` on 8.x); a plain
  device gets no 3D and is served by viogpu(4) instead.
- The x sets' Mesa (21.3.7) has **no virtio gallium driver** — GLX works
  but renders with llvmpipe until Mesa is rebuilt with the virtio driver
  (or pkgsrc Mesa is used); then `glxinfo` reports `renderer: virgl`.

## 4. smolBSD workflow (test images)

Workspace: `~/src/smolBSD`. Stage the branch kernel once per build:

```sh
cp $NB/src/obj/sys/arch/amd64/compile/SMOL/netbsd kernels/netbsd-SMOL-vioglx
```

### SMOLerfile example

`smolerfiles/SMOLerfile.vioglx`:

```dockerfile
# SMOLerfile.vioglx — X11 + GLX over virtio-gpu (virgl) on NetBSD.
# Boot with the viogpu-glx kernel and GPUs on the QEMU line:
#   ./smoler.sh run vioglx-amd64:latest -k kernels/netbsd-SMOL-vioglx
#   + -device virtio-gpu-device -device virtio-gpu-gl-device \
#     -display egl-headless   (tglx.sh wraps all of this)

FROM base,etc,xbase,xserver,xetc,xfont

LABEL smolbsd.service=vioglx
LABEL smolbsd.imgsize=6144

COPY testdrm3d /testdrm3d
COPY service/vioglx/etc/rc /vioglx-test.sh

ENV LD_LIBRARY_PATH=/usr/X11R7/lib
CMD sh /vioglx-test.sh
```

(`testdrm3d` is cross-built from `testdrm3d.c` with
`$NB/src/obj/tooldir/bin/x86_64--netbsd-gcc -static --sysroot=…
-I sys/external/bsd/drm2/dist/include/uapi` — see the head of `t3d.sh`.)

Build:

```sh
CHOUPI=n ./smoler.sh build smolerfiles/SMOLerfile.vioglx   # Linux host
# or directly: CHOUPI=n bmake SERVICE=vioglx base
```

### Iterating on /etc/rc without a full rebuild

`etc/rc` lives in the image; push edits in with debugfs (three steps, the
`sif` is **mandatory** — debugfs `write` clears the inode type bits and
init then fails with "exec format error"):

```sh
L=$(sudo losetup -fP --show images/vioglx-amd64.img)
sudo debugfs -w -R "rm /etc/rc"          ${L}p1
sudo debugfs -w -R "write service/vioglx/etc/rc /etc/rc" ${L}p1
sudo debugfs -w -R "sif /etc/rc mode 0100755" ${L}p1
sudo losetup -d $L
```

### Full 3D + Xorg smoke test

```sh
cd ~/src/smolBSD && ./tglx.sh
```

Waits for `VIOGLX-DONE` in the guest serial log (`/tmp/vglx.log`) and
prints the results. A passing run shows:

```
VIOGLX 3d: DRM 3D SMOKE PASS        (x5 rounds, ROUNDS FAILED: 0/5)
VIOGLX: Xorg up (:1) after 1s
VIOGLX GLX: direct rendering: Yes
VIOGLX GLX: OpenGL renderer string: llvmpipe (...)   # until virtio DRI lands
VIOGLX: glxgears rc=124 output: 3718 frames in 5.1 seconds = 735.346 FPS
VIOGLX-DONE result=ok
```

(rc=124 is just the 5s `timeout` wrapper ending glxgears.)
