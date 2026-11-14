#!/bin/sh
#
# mksmolset.sh <machine-arch>	(amd64 | i386 | aarch64 | ...)
#
# Build the utilities we ship ourselves for our kernel, listed in
# diverges/<machine-arch>.list, one per line:
#
#	<source-dir>	<install-path>	[extra make variables]
#
# Optional trailing make variables are appended to the build and
# install command lines (they override the directory Makefile's own
# assignments), e.g. to trim gallium's driver list.
# Each source dir is built in the NetBSD tree with the cross make
# wrapper found under obj/tooldir.<arch>, obj/tooldir-<arch>,
# obj/tooldir-<mach> or obj/tooldir (wrappers are named after
# MACHINE, e.g. nbmake-evbarm for aarch64) and installed into
# DESTDIR, by default the one baked into the wrapper;
# it must already hold a complete userland to link against: unpack
# the official base and comp sets into it once, or run build.sh
# distribution.  The results are packaged into
# obj/smolset.<arch>.tar.xz; smolBSD downloads and unpacks it as an
# extra set (KDIST/<arch>/...), see its setfetch target.
#
# Env: TOOLDIR, DESTDIR, OUT, BUILD=no (package without building),
#	CLEAN=yes (cleandir the built dirs afterwards; off by default so
#	reruns are incremental and only rebuild what is out of date).
#
# The obj dirs are shared between arch builds and bmake cannot tell
# that $CC changed: a build after an arch switch happily "reuses"
# foreign objects, which once shipped an arm64 binary inside the
# amd64 set (2026-11-13).  Two guards: an obj dir stamped by a
# previous run for another arch is cleaned before building, and
# every packaged ELF is checked against $arch before packaging.

set -e

root=$(cd "$(dirname "$0")" && pwd)

arch=$1
if [ -z "$arch" ]; then
	echo "usage: $0 <machine-arch>" >&2
	exit 1
fi

list=$root/diverges/$arch.list
if [ ! -f "$list" ]; then
	echo "missing $list" >&2
	exit 1
fi

# wrappers are named after MACHINE, which differs from MACHINE_ARCH
# for evbarm (aarch64)
case $arch in
aarch64)	mach=evbarm ;;
*)		mach=$arch ;;
esac

make=
for td in ${TOOLDIR:-$root/obj/tooldir.$arch $root/obj/tooldir-$arch $root/obj/tooldir-$mach $root/obj/tooldir}; do
	for w in nbmake-$arch nbmake-$mach; do
		[ -x "$td/bin/$w" ] && make=$td/bin/$w && break 2
	done
done
if [ "$BUILD" != "no" ] && [ -z "$make" ]; then
	echo "no nbmake-$arch wrapper in ${TOOLDIR:-obj/tooldir.$arch obj/tooldir}"
	echo "build tools first: ./build.sh -E -m $mach -T <tooldir> ... tools" >&2
	exit 1
fi

# install goes to the DESTDIR baked into the wrapper; staging must
# read from the same place
out=${OUT:-$root/obj/smolset.$arch.tar.xz}
destdir=${DESTDIR:-${make:+$(sed -n 's/^DESTDIR=\([^;]*\);.*/\1/p' "$make" | head -1)}}
destdir=${destdir:-$root/obj/destdir.$arch}

sudo=
[ "$(id -u)" = 0 ] || sudo=$(command -v sudo)

tmp=$(mktemp -d)
trap "rm -rf $tmp" EXIT

# ELF e_machine (little endian, 2 bytes at offset 18) per $arch;
# empty disables the check for architectures not listed
want_machine() {
	case $arch in
	amd64)		echo 3e00 ;;
	aarch64)	echo b700 ;;
	i386)		echo 0300 ;;
	*)		echo ;;
	esac
}

n=0
built=
while read -r src dest mkargs; do
	case $src in ''|'#'*) continue ;; esac
	if [ "$BUILD" != "no" ]; then
		stamp=$root/$src/obj/.smolset-arch
		prev=$(cat "$stamp" 2>/dev/null || echo unknown)
		if [ "$prev" != "$arch" ]; then
			echo "cleaning $src (obj was built for $prev)"
			(cd "$root/$src" && $make cleandir) || true
		fi
		echo "building $src"
		(cd "$root/$src" && $make $mkargs && $sudo $make $mkargs install)
		mkdir -p "$root/$src/obj"
		echo "$arch" > "$stamp"
		built="$built $src"
	fi
	if [ ! -f "$destdir/$dest" ]; then
		echo "missing $destdir/$dest, populate DESTDIR first" >&2
		exit 1
	fi
	mkdir -p "$tmp/$(dirname "$dest")"
	$sudo cp "$destdir/$dest" "$tmp/$dest"
	$sudo chmod 755 "$tmp/$dest"
	want=$(want_machine)
	if [ -n "$want" ] &&
	   [ "$(od -An -N4 -t x1 "$tmp/$dest" | tr -d ' \n')" = 7f454c46 ]; then
		got=$(od -An -j18 -N2 -t x1 "$tmp/$dest" | tr -d ' \n')
		if [ "$got" != "$want" ]; then
			echo "ERROR: $dest is ELF machine 0x$got, not $arch (0x$want)" >&2
			echo "obj dir or DESTDIR served another arch; rebuild" >&2
			exit 1
		fi
	fi
	echo "including $dest"
	n=$((n+1))
done < "$list"

# obj trees are kept by default: reruns are then incremental (bmake
# rebuilds only what changed).  Cleaning, if requested with CLEAN=yes,
# happens only after everything built and installed, because the
# directories link prebuilt objects from their siblings' obj trees
# (gallium needs obj/.../libdrm/libdrm.a).
if [ "$CLEAN" = yes ]; then
	for src in $built; do
		(cd "$root/$src" && $make cleandir) || true
	done
fi

if [ "$n" -eq 0 ]; then
	echo "$list is empty, nothing to do" >&2
	exit 1
fi

# directories must not inherit mktemp's 700, tar would restore it on extract
find "$tmp" -type d -exec chmod 755 {} +
mkdir -p "$(dirname "$out")"
# record root ownership without needing root (works on tar and bsdtar)
tar --owner=0 --group=0 -cJf "$out" -C "$tmp" .

echo "wrote $out with $n file(s)"
