NetBSD-perf
===========

This is my attempt at replicating iMil results that were reported on https://mail-index.netbsd.org/tech-kern/2024/01/23/msg029450.html and https://old.reddit.com/r/BSD/comments/197vfmq by using the perf branch he published on https://github.com/NetBSDfr/NetBSD-src

Currently, I'm still about 5x slower as I boot in 200 ms, but this is a good beginning:

```
# sh test-netbsd.sh
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

[   1.0000000] NetBSD 10.99.10 (MICROVM) #8: Wed Jan 31 09:01:58 CST 2024
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
[   1.0000030] cpu1 at mainbus0 apid 1
[   1.0000030] cpu1: 12th Gen Intel(R) Core(TM) i7-1270P, id 0x906a2
[   1.0000030] cpu1: node 0, package 0, core 1, smt 0
[   1.0000030] mpbios: bus 0 is type ISA
[   1.0000030] ioapic0 at mainbus0 apid 3: pa 0xfec00000, version 0x20, 24 pins
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
[   1.0000030] pvclock0 at pv0
[   1.0000030] timecounter: Timecounter "pvclock0" frequency 1000000000 Hz quality 1500
[   1.0001371] timecounter: Timecounter "lapic" frequency 999983000 Hz quality -100
[   1.0001371] timecounter: Timecounter "clockinterrupt" frequency 100 Hz quality 0
[   1.0001371] timecounter: Timecounter "TSC" frequency 2496000000 Hz quality 3000
[   1.0575299] boot device: ld0
[   1.0575299] root on ld0c dumps on ld0b
[   1.0575299] root file system type: ffs
[   1.0575299] kern.module.path=/stand/amd64/10.99.10/modules
[   1.0611265] boot: 198ms (entry tsc: 251543752)
[   1.0611265] exec /sbin/init: error 8
[   1.0611265] init: trying /sbin/oinit
bslinit v7 starting on NetBSD, will handle 12 signals, reaping zombies every 5 s
        arg 0: oinit
oinit: Failed to mount procfs on /proc: No such device
oinit: Failed to mount kernfs on /kern: Invalid argument
```

For reference, using the exact same filesystem but with the original kernel:

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

[   1.0000000] NetBSD 10.99.10 (MICROVM) #1891: Tue Jan 23 06:43:58 CET 2024
[   1.0000000]  imil@tatooine:/home/imil/src/github.com/NetBSD-src/sys/arch/amd64/compile/obj/MICROVM
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
[   1.0000030] kernel parameters: root=ld0c console=com rw -z -v virtio_mmio.device=512@0xfeb00e00:12
[   1.0000030] viommio: 512@0xfeb00e00:12
[   1.0000030] virtio0: VirtIO-MMIO-v2
[   1.0000030] virtio0: block device (id 2, rev. 0x01)
[   1.0000030] ld0 at virtio0: features: 0x110000a54<V1,INDIRECT_DESC,CONFIG_WCE,FLUSH,BLK_SIZE,GEOMETRY,SEG_MAX>
[   1.0000030] virtio0: allocated 4227072 byte for virtqueue 0 for I/O request, size 1024
[   1.0000030] virtio0: using 4194304 byte (262144 entries) indirect descriptors
[   1.0000030] allocated pic ioapic0 type level pin 12 level 6 to cpu0 slot 1 idt entry 96
[   1.0000030] virtio0: interrupting on -1
[   1.0000030] ld0: 512 MB, 1040 cyl, 16 head, 63 sec, 512 bytes/sect x 1048576 sectors
[   1.0000030] pvclock0 at pv0
[   1.0000030] timecounter: Timecounter "pvclock0" frequency 1000000000 Hz quality 1500
[   1.0070044] allocated pic ioapic0 type level pin 2 level 7 to cpu0 slot 2 idt entry 112
[   1.0232075] timecounter: Timecounter "clockinterrupt" frequency 100 Hz quality 0
[   1.0232075] timecounter: Timecounter "TSC" frequency 2496000000 Hz quality 3000
[   1.0244222] boot device: ld0
[   1.0244222] root on ld0c dumps on ld0b
[   1.0244222] root file system type: ffs
[   1.0244222] kern.module.path=/stand/amd64/10.99.10/modules
[   1.0285561] boot: 72ms (entry tsc: 168962022)
[   1.0294073] exec /sbin/init: error 8
[   1.0294073] init: trying /sbin/oinit
bslinit v7 starting on NetBSD, will handle 12 signals, reaping zombies every 30 s
        arg 0: oinit
oinit: mount /proc failed: No such device
```

Note also that the MMIO in qemu append doesn't seem to survive a reboot:

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

Also the performance is not matching yet:

```
# sysctl -b debug.tslog > /tmp/tslog
0x0 275799932 ENTER main
0x2 295914148 THREAD idle/0
0x3 295927199 THREAD softnet/0
0x4 295946662 THREAD softbio/0
0x5 295962333 THREAD softclk/0
0x6 296029811 THREAD softser/0
0x7 296101498 THREAD xcall/0
0x8 296160256 THREAD modunload
0x9 296242469 THREAD pooldisp
0xa 297478103 THREAD iflnkst
0xb 297483183 THREAD ifwdog
0xc 297495935 THREAD sopendfree
0xd 297511476 THREAD pmfevent
0xe 297516835 THREAD pmfsuspend
0x0 300470812 ENTER config_attach_internal mainbus
0x0 302535470 ENTER config_attach_internal cpu
0x0 334981541 EXIT config_attach_internal
0x0 334995085 ENTER config_attach_internal cpu
0xf 335760098 THREAD idle/1
0x10 335798245 THREAD softnet/1
0x11 335825899 THREAD softbio/1
0x12 335850895 THREAD softclk/1
0x13 335875037 THREAD softser/1
0x14 335928985 THREAD xcall/1
0x0 390242684 EXIT config_attach_internal
0x0 391082852 ENTER config_attach_internal ioapic
0x0 569050116 EXIT config_attach_internal
0x0 569084610 ENTER config_attach_internal isa
0x0 569827191 ENTER config_attach_internal com
0x0 589518701 EXIT config_attach_internal
0x0 589525847 EXIT config_attach_internal
0x0 589529639 ENTER config_attach_internal pv
0x0 590079490 ENTER config_attach_internal virtio
0x0 595773519 ENTER config_attach_internal viornd
0x0 683067128 EXIT config_attach_internal
0x0 683078608 ENTER config_attach_internal virtio
0x0 696013228 ENTER config_attach_internal ld
0x0 828659203 EXIT config_attach_internal
0x0 828662014 EXIT config_attach_internal
0x0 828663123 EXIT config_attach_internal
0x0 828673724 ENTER config_attach_internal pvclock
0x0 831876908 EXIT config_attach_internal
0x0 831878663 EXIT config_attach_internal
0x0 831884395 EXIT config_attach_internal
0x15 833672855 THREAD entbutler
0x16 964636813 THREAD configintr
0x19 964651639 THREAD configintr
0x18 965754770 THREAD configintr
0x19 965772930 THREAD configintr
0x6f 965776243 THREAD configintr
0x6d 965782151 THREAD configintr
0x6a 965785487 THREAD configintr
0x61 965789332 THREAD configintr
0x1e 965860007 THREAD vmem_rehash
0x1f 965973444 THREAD rt_timer
0x36 965988880 THREAD icmp_wqinput/0
0x60 966064940 THREAD icmp_wqinput/1
0x61 966403212 THREAD nd6_timer
0x62 966673756 THREAD icmp6_wqinput/0
0x63 966678725 THREAD icmp6_wqinput/1
0x64 966715553 THREAD unpgc
0x65 966741324 THREAD rt_free
0x6b 976822639 THREAD configroot
0x67 976832285 THREAD configroot
0x68 977580970 THREAD pgdaemon
0x69 977592989 THREAD ioflush
0x6a 977695961 THREAD pooldrain
0x0 977726594 EXIT main
```

Building
--------

The all-in-one compile.sh will build a kernel for you in ~/obj/sys/arch/amd64/compile/MICROVM/netbsd

    sh ./compile.sh

Binaries
--------

When it'll be ready, you'll see the releases on the right handside

TODO
----

Adding flamecharts to understand why it's so slow

Adding a basic rootfs with simple tools such as bslinit, the tslog script to create flamecharts, and a shell
