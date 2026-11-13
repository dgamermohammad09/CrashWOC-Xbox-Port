#!/bin/bash
# Packs NuWorld for Xbox Dev Mode: xbox/package.sh <build dir> <out dir>
#   <build dir>  a NUWORLD_UWP=ON build (holds NuWorld.exe)
#   env CWX_ROOT the workspace with deps/ (SDL3-uwp build, Mesa UWP) and tools/llvm-mingw (tools/env.sh sets it)
# Writes <out dir>/NuWorld_<version>_x64.msix, <out dir>/NuWorld.cer and <out dir>/Dependencies/.
set -euo pipefail
BUILD=${1:?build dir}
OUT=${2:?out dir}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=${CWX_ROOT:?source tools/env.sh first}
SDK="/c/Program Files (x86)/Windows Kits/10/bin/10.0.22621.0/x64"
VC=$(ls -d "/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/"*/bin/Hostx64/x64 | tail -1)
SDL_DLL="$ROOT/deps/sdl3-uwp-out/SDL3-UWP/SDL3.dll"
MESA="$ROOT/deps/mesa-uwp/bin"
DXIL="/c/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/dxil.dll"
VCLIBS="/c/Program Files (x86)/Microsoft SDKs/Windows Kits/10/ExtensionSDKs/Microsoft.VCLibs/14.0/Appx/Retail/x64/Microsoft.VCLibs.x64.14.00.appx"

# The version only goes up: 0.8.<day of the year 2026 on>.<minute of the day>
DAYS=$(( ($(date -u +%s) - $(date -u -d 2026-01-01 +%s)) / 86400 ))
MIN=$(( 10#$(date -u +%H) * 60 + 10#$(date -u +%M) ))
VERSION=${NW_PACKAGE_VERSION:-0.8.$DAYS.$MIN}

PKG="$OUT/pkg"
# A running copy registered from this layout keeps its files open.
powershell.exe -NoProfile -Command "Get-Process NuWorld -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 1"
rm -rf "$PKG"
mkdir -p "$PKG/Assets" "$OUT/Dependencies"
cp "$BUILD/NuWorld.exe" "$PKG/NuWorld.exe"
"$ROOT/tools/llvm-mingw/bin/llvm-strip" "$PKG/NuWorld.exe"
"$VC/editbin.exe" -nologo -appcontainer "$(cygpath -w "$PKG/NuWorld.exe")"
cp "$SDL_DLL" "$MESA/opengl32.dll" "$MESA/libgallium_wgl.dll" "$MESA/z-1.dll" "$DXIL" "$PKG/"
# The Vulkan renderer's driver (Mesa Dozen, Vulkan on Direct3D 12), loaded with LoadPackagedLibrary.
cp "$MESA/vulkan_dzn.dll" "$PKG/"
cp "$HERE"/Assets/*.png "$PKG/Assets/"
sed "s/@VERSION@/$VERSION/" "$HERE/AppxManifest.xml.in" > "$PKG/AppxManifest.xml"

echo "== imports outside the UWP API set"
python "$(cygpath -w "$ROOT/tools/check_uwp_imports.py")" "$(cygpath -w "$PKG/NuWorld.exe")" --packaged SDL3.dll,opengl32.dll

# Test certificate: self-signed, public on purpose (Dev Mode installs any package; one key = updates keep the data).
CERT="$HERE/cert"
if [ ! -f "$CERT/NuWorld.pfx" ]; then
  mkdir -p "$CERT"
  powershell.exe -NoProfile -Command "\$c = New-SelfSignedCertificate -Type Custom -Subject 'CN=NuWorldXbox' -KeyUsage DigitalSignature -FriendlyName 'NuWorld Xbox test' -CertStoreLocation 'Cert:\\CurrentUser\\My' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}'); \$p = ConvertTo-SecureString -String 'nuworld' -Force -AsPlainText; Export-PfxCertificate -Cert \$c -FilePath '$(cygpath -w "$CERT/NuWorld.pfx")' -Password \$p | Out-Null; Export-Certificate -Cert \$c -FilePath '$(cygpath -w "$CERT/NuWorld.cer")' | Out-Null"
fi

MSIX="$OUT/NuWorld_${VERSION}_x64.msix"
rm -f "$OUT"/NuWorld_*_x64.msix
"$SDK/makeappx.exe" pack -o -d "$(cygpath -w "$PKG")" -p "$(cygpath -w "$MSIX")" > "$OUT/makeappx.log"
"$SDK/signtool.exe" sign -fd SHA256 -a -f "$(cygpath -w "$CERT/NuWorld.pfx")" -p nuworld "$(cygpath -w "$MSIX")" > "$OUT/signtool.log"
cp "$CERT/NuWorld.cer" "$OUT/"
cp "$VCLIBS" "$OUT/Dependencies/"
echo "packed $MSIX ($VERSION)"
