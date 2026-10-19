#!/bin/bash
# Compile one of gpuc.c's shaders for the Mali-G31, disassemble it, and turn
# it into a C array for the kernel:
#   ./shader.sh ids    -> shaders/ids.bin, shaders/ids.txt (what it compiled
#                         to, and the numbers the descriptors need), and
#                         ../kernel/shaders.h (every shader in shaders/)
set -e
cd "$(dirname "$0")"
MESA=${MESA:-$PWD/mesa}
bin=$MESA/build/src/panfrost
cp gpuc.c "$MESA/src/src/panfrost/gpuc.c"
ninja -C "$MESA/build" src/panfrost/gpuc >/dev/null
mkdir -p shaders
for s in "$@"; do
  info=$("$bin/gpuc" "$s" "shaders/$s.bin")
  { echo "// $info"; "$bin/bifrost_compiler" disasm "shaders/$s.bin"; } > "shaders/$s.txt"
  echo "$s: $info"
done
python3 -I - <<'PY'
import glob, os, re
out = ["// Made by bare-metal/gpu/shader.sh from shaders/*.bin: Mali-G31 machine code.",
       "// The comment above each is what Mesa's compiler said about it; the",
       "// disassembly is in bare-metal/gpu/shaders/NAME.txt.", "#pragma once", "#include <stdint.h>", ""]
for path in sorted(glob.glob("shaders/*.bin")):
    name = os.path.basename(path)[:-4]
    data = open(path, "rb").read()
    info = open(path[:-4] + ".txt").readline().strip()
    regs = int(re.search(r"work registers (\d+)", info).group(1))
    fau = int(re.search(r"fau count (\d+)", info).group(1))
    preload = int(re.search(r"preload ([0-9a-f]+)", info).group(1), 16)
    out.append(info)
    out.append("static const uint8_t shader_%s[%d] __attribute__((aligned(128))) = {" % (name, len(data)))
    for i in range(0, len(data), 16):
        out.append("\t" + ", ".join("0x%02x" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    out.append("#define SHADER_%s_REGS %d\n#define SHADER_%s_FAU %d\n#define SHADER_%s_PRELOAD 0x%xULL\n"
               % (name.upper(), regs, name.upper(), fau, name.upper(), preload))
open("../kernel/shaders.h", "w").write("\n".join(out))
PY
