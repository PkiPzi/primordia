#!/usr/bin/env bash
# Build Primordia and add it to your desktop's application menu.
set -e
DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
cmake -S "$DIR" -B "$DIR/build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$DIR/build" -j"$(nproc)"
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
for variant in "" "nvidia"; do
  name="Primordia"; file="primordia.desktop"; args=""
  if [ -n "$variant" ]; then name="Primordia (NVIDIA GPU)"; file="primordia-nvidia.desktop"; args=" --nvidia"; fi
  cat > "$APPS/$file" <<DESKTOP
[Desktop Entry]
Type=Application
Name=$name
GenericName=Evolution Simulator
Comment=Watch creatures with evolving bodies and brains adapt, speciate and go extinct
Exec="$DIR/run.sh"$args
Path=$DIR
Icon=$DIR/assets/primordia.png
Terminal=false
Categories=Education;Science;Biology;Simulation;
Keywords=evolution;simulation;neural;genetics;ecology;
StartupWMClass=primordia
DESKTOP
done
command -v update-desktop-database >/dev/null && update-desktop-database "$APPS" >/dev/null 2>&1 || true
echo "Installed. Look for \"Primordia\" in your application menu, or run $DIR/run.sh"
