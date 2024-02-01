#!/bin/sh
# Replication of iMil result reported on https://mail-index.netbsd.org/tech-kern/2024/01/23/msg029450.html
m="amd64"
a="x86_64"
CPUS=4
PATHSKC="sys/arch/${m}/conf/"
TOOLCHAIN="${HOME}/obj"

[ -z ${1} ] \
 && printf "# Defaulting to MICROVM\n" \
 && KERNELCONFIG="MICROVM" \
 || KERNELCONFIG="${1}"

[ ! -f "${PATHSKC}/${1}" ] \
 && echo "Missing ${KERNELCONFIG} in ${PATHSKC}" \
 && exit 1

[ -f "${PATHSKC}/${1}" ] \
 && printf "# Using kernel config ${1}" \
 && KERNELCONFIG="${1}" \
 && stat -c "%y %A %n size: %s" ${PATHSKC}/${KERNELCONFIG} \
 || echo "Missing ${KERNELCONFIG} in ${PATHSKC}"

# Get the sources
#git clone https://github.com/NetBSDfr/NetBSD-src
git clone https://github.com/csdvrx/NetBSD-fr-src

# Show the branches available
git branch -a

# Change to result-replication, based on the perf branch
git branch| grep result-replication2  || git checkout result-replication2 || exit 2

#cf https://www.netbsd.org/docs/guide/en/chap-build.html#chap-boot-cross-compiling-kernel
echo "# Building the cross compilation toolchain into ${TOOLCHAIN}":

stat -c "%y %A %n size: %s" ${TOOLCHAIN}/tooldir*x86_64/bin/nbmake \
 || sh ./build.sh -U -O ${TOOLCHAIN} -j ${CPUS} -m ${m} -a ${a} tools \
 || exit 3

echo "# Using the kernel configuration ${KERNELCONFIG}":
stat -c "%y %A %n size: %s" sys/arch/amd64/conf/${KERNELCONFIG} \
 || exit 4

#echo "# Checking pvclock is disabled as it requires the missing <dev/pv/pvreg.h>"
#grep -v ^pvclock sys/arch/amd64/conf/${KERNELCONFIG} \
# || exit 5

echo "# Refreshing the kernel build with -u" \
 && sh ./build.sh -u -U -O ${TOOLCHAIN} -j ${CPUS} -m ${m}  kernel=${KERNELCONFIG} \
 && stat -c "%y %A %n size: %s" ${TOOLCHAIN}/sys/arch/${m}/compile/${KERNELCONFIG}/netbsd \
 || exit 6

## Alternatives:
# Use nbconfig and nbmake directly like sys/arch/amd64/conf/kern-compile.sh:
#NBCONFIG=$( ls ~/obj/tooldir*${a}/bin/nbconfig | tail -n1 )
#NBMAKE=$( ls ~/obj/tooldir*${a}/bin/nbmake-${m} | tail -n1 )
#printf "\tnbconfig:\t${NBCONFIG}\n\tnbmake-xxx:\t${NBMAKE}\n\n"
#${NBCONFIG} ${KERNELCONFIG} | grep "Build directory is" \
# && cd ${TOOLCHAIN}/sys/arch/${m}/compile \
# && ${NBMAKE} depend

# Don't just update the kernel: remove the '-u' that's for update only

# Compile from scratch the release and the kernel
#sh ./build.sh -U -O ${TOOLCHAIN} -j ${CPUS} -m amd64 -a x86_64 release kernel=${KERNELCONFIG}

# TODO: add qemu scripts, which should have console=com etc
# TODO: add a basic init_args parsing arguments like sys/arch/x86/x86/x86_autoconf.c
