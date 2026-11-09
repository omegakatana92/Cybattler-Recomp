#!/bin/sh
# Regenerate the recompiled C from the ROM set. Usage: tools/regen.sh path/to/cybattlr.zip
set -e
cd "$(dirname "$0")/.."
ROM="${1:-cybattlr.zip}"
mkdir -p build-tools generated
python3 tools/extract_programs.py "$ROM" build-tools
[ -f tools/libdasm.so ] || gcc -O2 -shared -fPIC -w -Ithird_party/musashi -o tools/libdasm.so tools/dasm_helper.c third_party/musashi/m68kdasm.c
[ -f tools/handlermap.json ] || python3 tools/gen_handlermap.py
python3 tools/recomp68k.py build-tools/main.bin --prefix rc_main --out generated/main_recomp.c \
  --seeds seeds/main_seeds.txt --listing build-tools/main.lst
python3 tools/recomp68k.py build-tools/sound.bin --prefix rc_snd --out generated/sound_recomp.c \
  --seeds seeds/sound_seeds.txt --listing build-tools/sound.lst
