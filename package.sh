#!/usr/bin/env bash
# Builds the DLLs, embeds them in the launcher and publishes dist/SWGBModern.exe
set -e
cd "$(dirname "$0")"
./build.sh
mkdir -p src/launcher/Payload dist
cp build/out/ddraw.dll build/out/dsound.dll src/launcher/Payload/
cp config/SWGBModern.ini src/launcher/Payload/
VER="${1:-$(grep -oP '(?<=<Version>)[^<]+' src/launcher/SWGBModern.csproj)}"
dotnet publish src/launcher/SWGBModern.csproj -c Release -p:Version="$VER" -o build/publish
cp build/publish/SWGBModern.exe dist/SWGBModern.exe
ls -la dist
