NetBSD-perf
===========

This is my attempt at replicating iMil results that were reported on https://mail-index.netbsd.org/tech-kern/2024/01/23/msg029450.html and https://old.reddit.com/r/BSD/comments/197vfmq by using the perf branch he published on https://github.com/NetBSDfr/NetBSD-src

Currently, I'm still about 5x slower as I boot in 200 ms, but this is a good beginning:

```
[   1.0000000] cpu_rng: rdrand/rdseed
[   1.0000000] entropy: ready
[   1.0000000] NetBSD 10.99.10 (MICROVM)       Notice: this software is protected by copyright
[   1.0000000] Copyright (c) 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003,
[   1.0000000]     2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013,
[   1.0000000]     2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022, 2023,
[   1.0000000]     2024
[   1.0000000]     The NetBSD Foundation, Inc.  All rights reserved.
[   1.0000000] Copyright (c) 1982, 1986, 1989, 1991, 1993
[   1.0000000]     The Regents of the University of California.  All rights reserved.
[   1.0000000] NetBSD 10.99.10 (MICROVM) #5: Tue Jan 30 19:22:09 CST 2024
[   1.0000000]  charlotte@x1ng2:/home/charlotte/obj/sys/arch/amd64/compile/MICROVM
[   1.0000000] total memory = 127 MB
[   1.0000000] avail memory = 77184 KB
[   1.0000000] timecounter: Timecounters tick every 10.000 msec
[   1.0000000] timecounter: Timecounter "i8254" frequency 1193182 Hz quality 100
[   1.0000030] Hypervisor: KVM
[   1.0000030] VMM: Generic PVH
[   1.0000030] mainbus0 (root)
[   1.0000030] mainbus0: Intel MP Specification (Version 1.4) (QBOOT    000000000000)
[   1.0000030] cpu0 at mainbus0 apid 0
[   1.0000030] cpu0: Use lfence to serialize rdtsc
[   1.0000030] got tsc from vmware compatible cpuid
[   1.0000030] cpu0: TSC freq CPUID 2496000000 Hz
[   1.0000030] cpu0: 12th Gen Intel(R) Core(TM) i7-1270P, id 0x906a2
[   1.0000030] cpu0: node 0, package 0, core 0, smt 0
[   1.0000030] mpbios: bus 0 is type ISA
[   1.0000030] ioapic0 at mainbus0 apid 2: pa 0xfec00000, version 0x20, 24 pins
[   1.0000030] isa0 at mainbus0
[   1.0000030] com0 at isa0 port 0x3f8-0x3ff irq 4: ns16550a, 16-byte FIFO
[   1.0000030] com0: console
[   1.0000030] allocated pic ioapic0 type edge pin 4 level 8 to cpu0 slot 0 idt entry 129
[   1.0000030] pv0 at mainbus0
[   1.0000030] virtio0 at pv0
[   1.0000030] kernel parameters: root=ld0c rw console=com -z -v virtio_mmio.device=512@0xfeb00e00:12 virtio_mmio.device=512@0xfeb00c00:11
[   1.0000030] viommio: 512@0xfeb00e00:12
[   1.0000030] virtio0: VirtIO-MMIO-v2
[   1.0000030] virtio0: entropy device (id 4, rev. 0x01)
[   1.0000030] viornd0 at virtio0: features: 0x110000000<V1,INDIRECT_DESC>
[   1.0000030] virtio0: allocated 32768 byte for virtqueue 0 for Entropy request, size 1024
[   1.0000030] allocated pic ioapic0 type level pin 12 level 6 to cpu0 slot 1 idt entry 96
[   1.0000030] virtio0: interrupting on -1
[   1.0000030] virtio1 at pv0
[   1.0000030] viommio: 512@0xfeb00c00:11
[   1.0000030] virtio1: VirtIO-MMIO-v2
[   1.0000030] virtio1: block device (id 2, rev. 0x01)
[   1.0000030] ld0 at virtio1: features: 0x110000a54<V1,INDIRECT_DESC,CONFIG_WCE,FLUSH,BLK_SIZE,GEOMETRY,SEG_MAX>
[   1.0000030] virtio1: allocated 4227072 byte for virtqueue 0 for I/O request, size 1024
[   1.0000030] virtio1: using 4194304 byte (262144 entries) indirect descriptors
[   1.0000030] allocated pic ioapic0 type level pin 11 level 6 to cpu0 slot 2 idt entry 97
[   1.0000030] virtio1: interrupting on -1
[   1.0000030] ld0: 512 MB, 1040 cyl, 16 head, 63 sec, 512 bytes/sect x 1048576 sectors
[   1.0000030] timecounter: Timecounter "lapic" frequency 999998000 Hz quality -100
[   1.0000030] timecounter: Timecounter "clockinterrupt" frequency 100 Hz quality 0
[   1.0000030] timecounter: Timecounter "TSC" frequency 2496000000 Hz quality 3000
[   1.0001656] boot device: ld0
[   1.0001656] root on ld0c dumps on ld0b
[   1.0001656] root file system type: ffs
[   1.0001656] kern.module.path=/stand/amd64/10.99.10/modules
[   1.0001656] WARNING: clock gained 4 days
[   1.0050716] boot: 199ms (entry tsc: 281364007)
[   1.0050716] exec /sbin/init: error 8
[   1.0050716] init: trying /sbin/oinit
bslinit v7 starting on NetBSD, will handle 12 signals, reaping zombies every 30 s
        arg 0: oinit
oinit: mount /proc failed: Invalid argument
# Epoch 1706665417, time 2024-01-31T01:43:37
```

Note however that the qemu append doesn't seem to survive a reboot:

```
# init: can't add utmpx record for `system boot': No such file or directory
init: can't add utmpx record for `system down': No such file or directory
qemu-system-x86_64: terminating on signal 2
[   1.0000000] cpu_rng: rdrand/rdseed
[   1.0000000] entropy: ready
[   1.0000000] NetBSD 10.99.10 (MICROVM)       Notice: this software is protected by copyright
[   1.0000000] Copyright (c) 1996, 1997, 1998, 1999, 2000, 2001, 2002, 2003,
[   1.0000000]     2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011, 2012, 2013,
[   1.0000000]     2014, 2015, 2016, 2017, 2018, 2019, 2020, 2021, 2022, 2023,
[   1.0000000]     2024
[   1.0000000]     The NetBSD Foundation, Inc.  All rights reserved.
[   1.0000000] Copyright (c) 1982, 1986, 1989, 1991, 1993
[   1.0000000]     The Regents of the University of California.  All rights reserved.
(...)
[   1.0155438] root on ld0c dumps on ld0b
[   1.0155438] vfs_mountroot: can't open root device
[   1.0155438] cannot mount root, error = 6
[   1.0155438] root device (default ld0c): qemu-system-x86_64: terminating on signal 2
```

Building
--------

    sh ./compile.sh

Binaries
--------

When it'll be ready, you'll see the releases on the right handside

TODO
----

Adding flamecharts to understand why it's so slow

Adding a basic rootfs with simple tools such as bslinit, the tslog script to create flamecharts, and a shell
