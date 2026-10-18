#!/usr/bin/env bash
# The blend-shape and clip check, on Linux (WSL on the Windows desk).
#
#   tests/run_blend_shapes.sh <work dir> <new usd.elf> <old usd.elf> <fixture.usda>...
#
# Builds a throwaway Godot project in <work dir> (Godot 4.8 double editor from
# V-Sekai-fire/service-godot-build v20260930-double.1, the godot_sandbox addon from
# transport-meshing-pen), dumps each fixture through both ELFs, and checks the dumps against
# OpenUSD's Python API. Passes when every fixture is CHECK-OK with the new ELF, and both
# controls fail: the old ELF (no blend-shape calls) on a fixture with blend shapes, and the
# oracle with --plant.
set -uo pipefail
W=$(mkdir -p "$1" && cd "$1" && pwd); NEW=$2; OLD=$3; shift 3
HERE=$(cd "$(dirname "$0")" && pwd)
PEN=${PEN:-$HERE/../../../1-transport/meshing-pen}
G=$W/godot.linuxbsd.editor.double.x86_64
if [ ! -x "$G" ]; then
	curl -fsSL -o "$G" https://github.com/V-Sekai-fire/service-godot-build/releases/download/v20260930-double.1/godot.linuxbsd.editor.double.x86_64
	chmod +x "$G"
fi
[ -d "$W/venv" ] || { python3 -m venv "$W/venv" && "$W/venv/bin/pip" -q install usd-core numpy; }
P=$W/proj; rm -rf "$P"; mkdir -p "$P/addons/godot_sandbox/bin" "$P/tests"
printf 'config_version=5\n[application]\nconfig/name="usd-guest-check"\n' > "$P/project.godot"
cp "$PEN/addons/godot_sandbox/bin/godot-riscv.gdextension" "$P/addons/godot_sandbox/bin/"
cp "$PEN/addons/godot_sandbox/bin/libgodot_riscv.linux.template_release.double.x86_64.so" "$P/addons/godot_sandbox/bin/"
cp "$NEW" "$P/new.elf"; cp "$OLD" "$P/old.elf"; cp "$HERE/blend_shapes_dump.gd" "$P/tests/"
"$G" --headless --path "$P" --import > "$W/import.log" 2>&1
fail=0
for f in "$@"; do
	f=$(realpath "$f") # Godot reads a relative path against the project, not the shell
	b=$(basename "$f" .usda)
	timeout 300 "$G" --headless --path "$P" -s tests/blend_shapes_dump.gd -- "$f" res://new.elf > "$W/$b.new.txt" 2>&1
	timeout 300 "$G" --headless --path "$P" -s tests/blend_shapes_dump.gd -- "$f" res://old.elf > "$W/$b.old.txt" 2>&1
	echo "== $b"
	"$W/venv/bin/python" "$HERE/blend_shapes_oracle.py" "$f" "$W/$b.new.txt" | tail -4 || fail=1
	if "$W/venv/bin/python" "$HERE/blend_shapes_oracle.py" "$f" "$W/$b.new.txt" --plant > /dev/null; then
		echo "CONTROL-BROKEN: --plant passed"; fail=1
	else echo "control --plant: fails, as it must"; fi
	if grep -q 'BlendShape' "$f"; then
		if "$W/venv/bin/python" "$HERE/blend_shapes_oracle.py" "$f" "$W/$b.old.txt" > /dev/null; then
			echo "CONTROL-BROKEN: the old ELF passed"; fail=1
		else echo "control old ELF: fails, as it must"; fi
	fi
done
echo "RESULT: $([ $fail = 0 ] && echo PASS || echo FAIL)"
exit $fail
