#!/bin/sh
# make-hpkg.sh - build the `vstbridge` .hpkg on a running Haiku (x86_64).
#
# Ships both front-ends (the native 'VSTBridge' app and the 'vstbridgectl' CLI),
# the chainloader + bridge libraries, and the Wine host.
#
# Requires Wine (winegcc) and meson/ninja for the bridge, and g++ (C++20) for the
# two front-ends. Install first:
#     pkgman install -y wine meson ninja gcc make pkgconfig
#
# The layout below follows vstbridgectl's own search logic (src/config.cpp):
#   - the chainloaders are looked up in the standard library directories, of
#     which /boot/system/lib is one, and the vst3/clap ones must sit beside the
#     vst2 one -> everything goes in lib/.
#   - the Wine host is found by searching PATH for 'vstbridge-host.exe', which
#     is a shell wrapper that execs 'vstbridge-host.exe.so' from the same
#     directory -> both go in bin/, which packagefs maps onto /boot/system/bin.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.1.0
REVISION=1
STAGE="$HERE/stage"

cd "$ROOT"
rm -rf "$STAGE"

# 1) Bridge libraries + Wine host (meson/ninja, winegcc under the hood).
#
# The cross file points Meson at Haiku's winegcc/wineg++, and -Dsystem-asio
# uses the asio package instead of cloning the wrap.
#
# -Dclap=false because the CLAP bridge does not build here: src/wine-host/
# bridges/clap.h uses the Size struct, which is declared in the X11 editor.h
# that this port drops from the build, and editor-haiku.h has no equivalent.
# Porting the CLAP editor path is separate work; until then the package ships
# VST2 and VST3 only.
#
# An existing build directory is reused: a full rebuild of the Winelib host is
# expensive and ninja already knows what is out of date. Delete build/ by hand
# to change any of the options above.
if [ ! -d build ]; then
	meson setup build --cross-file cross-wine-haiku.conf \
		-Dsystem-asio=true -Dclap=false
fi
ninja -C build

# 2) Both front-ends. On Haiku 'all' builds the CLI and the Interface Kit app.
make -C tools/vstbridgectl

mkdir -p "$STAGE/bin" "$STAGE/lib" "$STAGE/apps" \
	"$STAGE/data/deskbar/menu/Applications"

# Copy a build artifact into the staging tree, failing loudly if it is missing.
# A silently incomplete package is worse than no package.
stage_required() {
	if [ ! -f "$1" ]; then
		echo "error: required build artifact missing: $1" >&2
		exit 1
	fi
	cp "$1" "$2"
}

# Same, but for the pieces this platform does not build yet.
stage_optional() {
	if [ -f "$1" ]; then
		cp "$1" "$2"
	else
		echo ">> skipping (not built on this platform): $1"
	fi
}

stage_required tools/vstbridgectl/build/vstbridgectl "$STAGE/bin/vstbridgectl"
stage_required tools/vstbridgectl/build/VSTBridge "$STAGE/apps/VSTBridge"

# Re-stamp the file type on the staged copy from the app signature resource
# baked into the binary, so the package carries it no matter what the copy did
# with the attributes.
mimeset -f "$STAGE/apps/VSTBridge"

# Deskbar's menu is built from data/deskbar/menu, not from apps/ -- an app only
# appears under Applications if something symlinks it in there. Every app in
# the menu on a stock install does exactly this, with the same relative target.
ln -s ../../../../apps/VSTBridge \
	"$STAGE/data/deskbar/menu/Applications/VSTBridge"

# Chainloader + bridge libraries, grouped in lib/ so vstbridgectl resolves every
# format as a sibling of the VST2 chainloader.
for so in libvstbridge-chainloader-vst2.so \
          libvstbridge-chainloader-vst3.so \
          libvstbridge-vst2.so \
          libvstbridge-vst3.so; do
	stage_required "build/$so" "$STAGE/lib/$so"
done

# CLAP is not built on Haiku yet; pick it up if a future build produces it.
for so in libvstbridge-chainloader-clap.so libvstbridge-clap.so; do
	stage_optional "build/$so" "$STAGE/lib/$so"
done

# The Wine host: the .exe wrapper and the Winelib payload it execs, together.
stage_required build/vstbridge-host.exe "$STAGE/bin/vstbridge-host.exe"
stage_required build/vstbridge-host.exe.so "$STAGE/bin/vstbridge-host.exe.so"

# The 32-bit host only exists on a multilib Wine setup.
stage_optional build/vstbridge-host-32.exe "$STAGE/bin/vstbridge-host-32.exe"
stage_optional build/vstbridge-host-32.exe.so "$STAGE/bin/vstbridge-host-32.exe.so"

cp "$HERE/vstbridge.PackageInfo" "$STAGE/.PackageInfo"
OUT="$HERE/vstbridge-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE" "$OUT"

echo ">> built $OUT"
echo ">> install with:  cp \"$OUT\" /boot/system/packages/"
echo ">> then check:    which vstbridgectl && which vstbridge-host.exe"
