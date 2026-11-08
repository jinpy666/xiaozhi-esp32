#!/usr/bin/env bash
# Build the host-side UI simulator (scripts/simulator/ui_sim.cc) against the
# repo's own LVGL 9.5 managed component + system SDL2. Nothing here touches
# idf.py; the firmware build is unaffected.
#
# Usage:  bash scripts/simulator/build_ui_sim.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SIMDIR="$REPO_ROOT/scripts/simulator"
LVGL="$REPO_ROOT/managed_components/lvgl__lvgl"
OUT="$REPO_ROOT/build/ui_sim"
OBJDIR="$REPO_ROOT/build/ui_sim_obj"

if [ ! -d "$LVGL/src" ]; then
    echo "error: managed_components/lvgl__lvgl not found (run idf.py once to fetch components)" >&2
    exit 1
fi

SDL_LIBS=$(sdl2-config --libs 2>/dev/null || echo "-lSDL2")
# sdl2-config prints -I.../SDL2; LVGL includes "SDL2/SDL.h", so the parent
# include dir is what is needed.
SDL_CFLAGS=$(sdl2-config --cflags 2>/dev/null || echo "")
SDL_ROOT_INC=$(sdl2-config --cflags 2>/dev/null | sed -n 's/.*-I\([^ ]*\)\/SDL2.*/-I\1/p' || true)

mkdir -p "$(dirname "$OUT")" "$OBJDIR"
rm -f "$OBJDIR"/*.o

# LVGL 9.5 host whitelist: core + software draw + SDL driver; platform draw
# units (nxp/renesas/arm2d/eve/vg_lite...) need vendor headers and are excluded.
{
    find "$LVGL/src/core" "$LVGL/src/display" "$LVGL/src/font" "$LVGL/src/indev" \
         "$LVGL/src/layouts" "$LVGL/src/misc" "$LVGL/src/osal" "$LVGL/src/stdlib" \
         "$LVGL/src/tick" "$LVGL/src/widgets" "$LVGL/src/themes/default" -name '*.c'
    find "$LVGL/src/draw" -maxdepth 1 -name '*.c'
    find "$LVGL/src/draw/sw" "$LVGL/src/draw/convert" "$LVGL/src/draw/snapshot" -name '*.c'
    find "$LVGL/src/themes" -maxdepth 1 -name 'lv_theme.c'
    echo "$LVGL/src/lv_init.c"
    echo "$LVGL/src/drivers/sdl/lv_sdl_window.c"
    echo "$LVGL/src/drivers/sdl/lv_sdl_sw.c"
    echo "$LVGL/src/drivers/sdl/lv_sdl_mouse.c"
    echo "$LVGL/src/drivers/sdl/lv_sdl_keyboard.c"
    echo "$LVGL/src/drivers/sdl/lv_sdl_mousewheel.c"
    echo "$LVGL/src/libs/bin_decoder/lv_bin_decoder.c"
} | sort -u > "$REPO_ROOT/build/ui_sim_srcs.txt"

# LVGL is a C library: compile it with gcc (only ui_sim.cc goes through g++).
# Logs are filtered to real errors (LVGL emits conf-pragma warnings on every TU).
compile_one() {
    gcc -c -O2 -DLV_CONF_INCLUDE_SIMPLE $SDL_ROOT_INC -I"$SIMDIR" -I"$LVGL" -I"$LVGL/src" \
        "$1.c" -o "$OBJDIR/$(basename "$1").o" 2>"$OBJDIR/$(basename "$1").err" || {
            grep -E 'error:' "$OBJDIR/$(basename "$1").err" >&2 || cat "$OBJDIR/$(basename "$1").err" >&2
            exit 1
        }
}
export -f compile_one
export OBJDIR SIMDIR LVGL SDL_ROOT_INC

sed 's/\.c$//' "$REPO_ROOT/build/ui_sim_srcs.txt" | xargs -P 8 -I{} bash -c 'compile_one "{}"'

g++ -std=c++17 -O2 -DLV_CONF_INCLUDE_SIMPLE \
    -I"$SIMDIR" -I"$LVGL" -I"$LVGL/src" \
    -c "$SIMDIR/ui_sim.cc" -o "$OBJDIR/zz_ui_sim_main.o" 2>"$OBJDIR/main.err" || {
        grep -E 'error:' "$OBJDIR/main.err" >&2 || cat "$OBJDIR/main.err" >&2
        exit 1
    }

g++ "$OBJDIR"/*.o $SDL_LIBS -lm -lpthread -o "$OUT"
echo "built $OUT"
