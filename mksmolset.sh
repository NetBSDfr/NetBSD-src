#!/bin/sh
#
# mksmolset.sh <machine-arch>	(amd64 | i386 | aarch64 | ...)
#
# Build the utilities we ship ourselves for our kernel, listed in
# diverges/<machine-arch>.list, one per line:
#
#	<source-dir>	<install-path>
#
# Each source dir is built in the NetBSD tree with the cross make
# wrapper found under obj/tooldir.<arch> or obj/tooldir (wrappers
# are named after MACHINE, e.g. nbmake-evbarm for aarch64) and
# installed into DESTDIR, by default the one baked into the wrapper;
# it must already hold a complete userland to link against: unpack
# the official base and comp sets into it once, or run build.sh
# distribution.  The results are packaged into
# obj/smolset.<arch>.tar.xz; smolBSD downloads and unpacks it as an
# extra set (KDIST/<arch>/...), see its setfetch target.
#
# Env: TOOLDIR, DESTDIR, OUT, BUILD=no (package without building).

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
for td in ${TOOLDIR:-$root/obj/tooldir.$arch $root/obj/tooldir}; do
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

n=0
while read -r src dest; do
	case $src in ''|'#'*) continue ;; esac
	if [ "$BUILD" != "no" ]; then
		echo "building $src"
		(cd "$root/$src" && $make && $sudo $make install \
			&& $make cleandir)
	fi
	if [ ! -f "$destdir/$dest" ]; then
		echo "missing $destdir/$dest, populate DESTDIR first" >&2
		exit 1
	fi
	mkdir -p "$tmp/$(dirname "$dest")"
	$sudo cp "$destdir/$dest" "$tmp/$dest"
	$sudo chmod 755 "$tmp/$dest"
	echo "including $dest"
	n=$((n+1))
done < "$list"

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
