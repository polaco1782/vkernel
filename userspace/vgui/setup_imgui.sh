#!/usr/bin/env bash
# vgui/setup_imgui.sh
# Downloads the Dear ImGui source files needed to build vgui.
# Run once from the vgui/ directory before running make.

set -euo pipefail

IMGUI_TAG="v1.91.8"
RAW="https://raw.githubusercontent.com/ocornut/imgui/${IMGUI_TAG}"

FILES=(
    "imgui.h"
    "imgui.cpp"
    "imgui_internal.h"
    "imgui_draw.cpp"
    "imgui_tables.cpp"
    "imgui_widgets.cpp"
    "imgui_demo.cpp"
    "imconfig.h"
    "imstb_rectpack.h"
    "imstb_textedit.h"
    "imstb_truetype.h"
)

mkdir -p imgui

for f in "${FILES[@]}"; do
    echo "  Downloading ${f} ..."
    curl -fsSL -o "imgui/${f}" "${RAW}/${f}"
done

echo ""
echo "Dear ImGui ${IMGUI_TAG} downloaded to ./imgui/"
echo "You can now run:  make"
