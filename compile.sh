#!/bin/sh
# Replication of iMil result reported on https://mail-index.netbsd.org/tech-kern/2024/01/23/msg029450.html
KERNELCONFIG="MICROVM"
TOOLCHAIN="$HOME/obj"
CPUS=4

# Get the sources
#git clone https://github.com/NetBSDfr/NetBSD-src

# Show the branches available
#git branch -a

# Change to result-replication, based on the perf branch
git branch| grep result-replication || git switch result-replication

#cf https://www.netbsd.org/docs/guide/en/chap-build.html#chap-boot-cross-compiling-kernel
echo "# Building the cross compilation toolchain into ${TOOLCHAIN}":

stat -c "%y %A %n size: %s" ${TOOLCHAIN}/tooldir*x86_64/bin/nbmake \
 || sh ./build.sh -U -O ${TOOLCHAIN} -j ${CPUS} -m amd64 -a x86_64 tools \
 || exit 1

echo "# Using the kernel configuration ${KERNELCONFIG}":
stat -c "%y %A %n size: %s" sys/arch/amd64/conf/${KERNELCONFIG} \
 || exit 2

echo "# Checking pvclock is disabled as it requires the missing <dev/pv/pvreg.h>"
grep -v ^pvclock sys/arch/amd64/conf/${KERNELCONFIG} \
 || exit 3

echo "# Refreshing the kernel build with -u" \
 && sh ./build.sh -u -U -O ${TOOLCHAIN} -j ${CPUS} -m amd64  kernel=${KERNELCONFIG} \
 && stat -c "%y %A %n size: %s" ${TOOLCHAIN}/sys/arch/amd64/compile/${KERNELCONFIG}/netbsd \
 || exit 4

## Alternatives:
# Use nbconfig and nbmake directly with sys/arch/amd64/conf/kern-compile.sh

# Don't just update the kernel: remove the '-u' that's for update only

# Compile from scratch the release and the kernel
#sh ./build.sh -U -O ${TOOLCHAIN} -j ${CPUS} -m amd64 -a x86_64 release kernel=${KERNELCONFIG}

# TODO: add qemu scripts, which should have console=com etc
# TODO: add a basic init_args parsing arguments like sys/arch/x86/x86/x86_autoconf.c
