#!/usr/bin/env -S godot --headless -s
# Dumps what usd.elf reads of a USD file's blend shapes and clips, as JSON on one line
# prefixed "DUMP ", for tests/blend_shapes_oracle.py to compare against OpenUSD's Python API.
#
#   godot --headless --path <project> -s tests/blend_shapes_dump.gd -- <file.usda> [<elf>]
#
# The project holds the godot_sandbox addon and usd.elf (default res://usd.elf).
extends SceneTree


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.is_empty():
		print("FAIL: no input file")
		quit(2)
		return
	var elf := "res://usd.elf" if args.size() < 2 else args[1]
	var sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = 240
	sb.allocations_max = 1000000
	sb.references_max = 4096
	sb.execution_timeout = 400
	sb.program = load(elf)
	root.add_child(sb)
	var bytes := FileAccess.get_file_as_bytes(args[0])
	var open: String = sb.vmcall("usd_open", bytes)
	var out := {"open": open, "meshes": [], "shapes": [], "anims": []}
	if not open.begins_with("ok "):
		print("DUMP " + JSON.stringify(out))
		quit(1)
		return
	for i in sb.vmcall("usd_mesh_count"):
		var m: Dictionary = sb.vmcall("usd_mesh_info", i)
		out.meshes.append({"path": m.path, "points": m.points, "indexed": m.indexed,
				"shape_first": m.get("shape_first", -1), "shapes": m.get("shapes", -1)})
	var has_shapes: bool = sb.has_function("usd_blend_shape_count")
	for k in (sb.vmcall("usd_blend_shape_count") if has_shapes else 0):
		var b: Dictionary = sb.vmcall("usd_blend_shape_info", k)
		var o = sb.vmcall("usd_blend_shape_offsets_slice", k, 0, 0)
		var n = sb.vmcall("usd_blend_shape_normals_slice", k, 0, 0) if b.has_normals else PackedFloat32Array()
		out.shapes.append({"name": b.name, "mesh": b.mesh, "primary": b.primary, "position": b.position,
				"has_normals": b.has_normals, "offsets": _arr(o), "normals": _arr(n)})
	for i in sb.vmcall("usd_skel_anim_count"):
		var a: Dictionary = sb.vmcall("usd_skel_anim_info", i)
		var w = sb.vmcall("usd_skel_anim_blend_weights", i) if has_shapes else PackedFloat32Array()
		out.anims.append({"path": a.path, "frames": a.frames, "blend_shapes": a.get("blend_shapes", ""),
				"times": a.get("times", ""), "weights": _arr(w)})
	print("DUMP " + JSON.stringify(out))
	quit(0)


# A packed array as an Array; anything else (a guest error answer) as its text, so the oracle
# reports it instead of the script stopping here.
func _arr(v) -> Variant:
	if v is PackedFloat32Array or v is PackedInt32Array:
		return Array(v)
	return "NOT AN ARRAY: " + str(v)
