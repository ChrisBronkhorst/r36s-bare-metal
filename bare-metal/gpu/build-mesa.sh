#!/bin/bash
# Build Mesa's Bifrost compiler and disassembler on the Mac, plus our gpuc
# (gpuc.c, which builds shaders with Mesa's NIR builder and compiles them
# for the Mali-G31). Everything goes in $MESA (default: ./mesa, ignored by git).
#
#   ./build-mesa.sh            then: ./shader.sh ids
set -e
cd "$(dirname "$0")"
MESA=${MESA:-$PWD/mesa}
mkdir -p "$MESA"
[ -d "$MESA/src" ] || git clone -q --depth 1 https://gitlab.freedesktop.org/mesa/mesa.git "$MESA/src"
[ -x "$MESA/venv/bin/meson" ] || { uv venv -q -p 3.13 "$MESA/venv"; VIRTUAL_ENV="$MESA/venv" uv pip install -q meson mako pyyaml packaging; }
export PATH="$MESA/venv/bin:$PATH"
cd "$MESA/src"
# With no driver selected Mesa builds only a stub of NIR, its shader IR: we want all of it
sed -i '' 's/^with_nir_headers_only = not with_gfx_compute or (/with_nir_headers_only = false and (/' src/compiler/nir/meson.build
cp ../../gpuc.c src/panfrost/gpuc.c
grep -q "'gpuc'" src/panfrost/meson.build || python3 - <<'PY'
p = 'src/panfrost/meson.build'; s = open(p).read()
key = "    build_by_default : with_tools.contains('panfrost')\n  )\n"
s = s.replace(key, key + """
  executable(
    'gpuc',
    files('gpuc.c'),
    include_directories : [inc_include, inc_src, inc_panfrost],
    dependencies : [idep_nir, idep_mesautil, idep_bi_opcodes_h, libpanfrost_model_dep],
    link_with : [libpanfrost_compiler],
    build_by_default : true,
  )
""", 1)
open(p, 'w').write(s)
PY
[ -d ../build ] || meson setup ../build -Dgallium-drivers= -Dvulkan-drivers= -Dplatforms= \
  -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
  -Dpanfrost-disassemble=true -Dtools=panfrost -Dbuildtype=release >/dev/null
ninja -C ../build src/panfrost/gpuc src/panfrost/bifrost_compiler >/dev/null
echo "built $MESA/build/src/panfrost/gpuc and bifrost_compiler"
