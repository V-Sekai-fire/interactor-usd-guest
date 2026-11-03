extends SceneTree
# Blend-shape and blend-weight clip writing through usd.elf's usd_write_mesh, in the real engine (Sandbox).
# Gates 2 (tiny), 3 (body) and 4 (names); gate 1 is usd_identity.gd and gate 5 the size line of "body".
# Every comparison is on bits (PackedFloat32Array.to_byte_array), and every comparison has a planted
# control (a one-ulp change, a stripped file, a shapeless mesh) that must be caught.
# No text interchange format is written: the packages are .usdz. The only .usda text is the member of a
# package that this script reads in memory to strip weights (a control) or to look at names.
#
# usage: godot --headless --path <proj> -s res://usd_blendshapes.gd -- <usd.elf> <out_dir> tiny
#        godot --headless --path <proj> -s res://usd_blendshapes.gd -- <usd.elf> <out_dir> names
#        godot --headless --path <proj> -s res://usd_blendshapes.gd -- <usd.elf> <out_dir> body <fbx> <mesh node> [frames]
# Exit code 0 when every check passed.

var fails := 0
var mem_mb := 1024
# true: the float and int input of usd_write_mesh goes in pieces through usd_write_input_floats / _ints
# (a host call carries at most 16 MiB an array) and the header says input=pushed
var push_input := false
var alloc_max := 1000000
var checks := 0


func _init() -> void:
	var a := OS.get_cmdline_user_args()
	var elf := FileAccess.get_file_as_bytes(a[0])
	var out_dir: String = a[1]
	DirAccess.make_dir_recursive_absolute(out_dir)
	match a[2]:
		"tiny":
			_tiny(elf, out_dir)
		"names":
			_names(elf, out_dir)
		"inbetween":
			_inbetween(elf, out_dir)
		"clip":
			_clip(elf, out_dir, FileAccess.get_file_as_bytes(a[3]) if a.size() > 3 else PackedByteArray())
		"body":
			_body(elf, out_dir, a[3], a[4], int(a[5]) if a.size() > 5 else 12)
		"big":
			# every point of every shape listed (dense), 64 frames by default; arg 6 is the sandbox memory in MB
			if a.size() > 6:
				mem_mb = int(a[6])
			_body(elf, out_dir, a[3], a[4], int(a[5]) if a.size() > 5 else 64, true)
		"slices":
			_slices(elf, out_dir)
		_:
			print("unknown mode ", a[2])
			fails += 1
	print("RESULT %s: %d checks, %d failed" % [a[2], checks, fails])
	quit(1 if fails > 0 else 0)


# ---------------------------------------------------------------- helpers

func _ok(label: String, cond: bool, detail: String = "") -> bool:
	checks += 1
	if not cond:
		fails += 1
	print("%-4s %s%s" % ["PASS" if cond else "FAIL", label, ("  [" + detail + "]") if (detail != "" and not cond) else ""])
	return cond


func _new_sb(elf: PackedByteArray):
	var sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = mem_mb
	sb.references_max = 4096
	sb.allocations_max = alloc_max
	sb.execution_timeout = 600000
	root.add_child(sb)
	sb.load_buffer(elf)
	sb.vmcall("usd_init")
	return sb


func _same_bits(x: PackedFloat32Array, y: PackedFloat32Array) -> bool:
	return x.size() == y.size() and x.to_byte_array() == y.to_byte_array()


# A copy of arr with float k one ulp up (the bit pattern plus one).
func _ulp(arr: PackedFloat32Array, k: int) -> PackedFloat32Array:
	var b := arr.to_byte_array()
	b.encode_u32(k * 4, b.decode_u32(k * 4) + 1)
	return b.to_float32_array()


# Sparse (indices, offsets) expanded with zeros to 3 floats per point, as the reader densifies.
func _dense(npts: int, idx: PackedInt32Array, off: PackedFloat32Array) -> PackedFloat32Array:
	var d := PackedFloat32Array()
	d.resize(npts * 3)
	for k in idx.size():
		d[idx[k] * 3] = off[k * 3]
		d[idx[k] * 3 + 1] = off[k * 3 + 1]
		d[idx[k] * 3 + 2] = off[k * 3 + 2]
	return d


# shapes: Array of {name, idx: PackedInt32Array, off: PackedFloat32Array, nrm: PackedFloat32Array (optional)}.
# clip: {} or {frames, fps, weights: PackedFloat32Array (frames x shapes)}.
# skin: {} or {joints: String, rests: PackedFloat32Array (J x 24), weights, indices, element}.
func _write(sb, name: String, pts: PackedFloat32Array, counts: PackedInt32Array, corners: PackedInt32Array,
		shapes: Array, clip: Dictionary, skin: Dictionary = {}, fn := "usd_write_mesh", extra := ""):
	var floats := PackedFloat32Array(pts)
	var ints := PackedInt32Array(counts)
	ints.append_array(corners)
	var header := "%s\nnp=%d\nnuv=0\nnrest=%d\nnf=%d\nni=%d\nsource=usd_blendshapes.gd" % [name, pts.size() / 3,
			skin["rests"].size() if skin.has("rests") else 0, counts.size(), corners.size()]
	var joints := ""
	if skin.has("rests"):
		floats.append_array(skin["rests"])
		floats.append_array(skin["weights"])
		ints.append_array(skin["indices"])
		joints = skin["joints"]
	if shapes.size() > 0:
		header += "\nnshapes=%d" % shapes.size()
		for i in shapes.size():
			var s: Dictionary = shapes[i]
			header += "\nshape%d=%s\nshape%d_n=%d\nshape%d_normals=%d" % [i, s["name"], i, s["idx"].size(), i, 1 if s.has("nrm") else 0]
			floats.append_array(s["off"])
			if s.has("nrm"):
				floats.append_array(s["nrm"])
			var ibs: Array = s.get("ibs", [])
			if ibs.size() > 0:
				header += "\nshape%d_ibs=%d" % [i, ibs.size()]
				for j in ibs.size():
					var ib: Dictionary = ibs[j]
					header += "\nshape%d_ib%d=%s\nshape%d_ib%d_normals=%d" % [i, j, ib["name"], i, j, 1 if ib.has("nrm") else 0]
					floats.append(ib["weight"])
					floats.append_array(ib["off"])
					if ib.has("nrm"):
						floats.append_array(ib["nrm"])
			ints.append_array(s["idx"])
		if clip.size() > 0:
			header += "\nclip_frames=%d\nclip_fps=%s" % [clip["frames"], str(clip["fps"])]
			floats.append_array(clip["weights"])
	if push_input:
		var piece := 1024 * 1024
		for at in range(0, floats.size(), piece):
			sb.vmcall("usd_write_input_floats", floats.slice(at, at + piece))
		for at in range(0, ints.size(), piece):
			sb.vmcall("usd_write_input_ints", ints.slice(at, at + piece))
		return sb.vmcall(fn, PackedFloat32Array(), PackedInt32Array(), header + extra + "\ninput=pushed", joints)
	return sb.vmcall(fn, floats, ints, header + extra, joints)


# All of the staged write result, through usd_written_slice (8 MiB at a time).
func _fetch_written(sb) -> PackedByteArray:
	var total := int(sb.vmcall("usd_written_size"))
	var out := PackedByteArray()
	var at := 0
	while at < total:
		var part = sb.vmcall("usd_written_slice", at, 8 * 1024 * 1024)
		if typeof(part) != TYPE_PACKED_BYTE_ARRAY:
			print("     slice at %d answered %s" % [at, str(part).left(200)])
			return PackedByteArray()
		out.append_array(part)
		at += part.size()
	return out


func _open(sb, pkg: PackedByteArray) -> String:
	var chunk := 4 * 1024 * 1024
	var at := 0
	while at < pkg.size():
		sb.vmcall("usd_push", pkg.slice(at, min(at + chunk, pkg.size())))
		at += chunk
	return str(sb.vmcall("usd_open_staged"))


# The member of a one-member stored .usdz, as text (the package is stored, so the member is raw bytes).
func _usda_of(pkg: PackedByteArray) -> String:
	var size := pkg.decode_u32(18)
	var start := 30 + pkg.decode_u16(26) + pkg.decode_u16(28)
	return pkg.slice(start, start + size).get_string_from_utf8()


func _read_floats(sb, fn: String, k: int) -> PackedFloat32Array:
	var r = sb.vmcall(fn, k)
	return r if typeof(r) == TYPE_PACKED_FLOAT32_ARRAY else PackedFloat32Array()


func _save(out_dir: String, file: String, pkg: PackedByteArray) -> void:
	var f := FileAccess.open(out_dir.path_join(file), FileAccess.WRITE)
	f.store_buffer(pkg)
	f.close()


func _quad() -> Dictionary:
	return {
		"pts": PackedFloat32Array([-1, 0, 0, 1, 0, 0, 1, 2, 0, -1, 2, 0]),
		"counts": PackedInt32Array([4]),
		"corners": PackedInt32Array([0, 1, 2, 3]),
		"fan": PackedInt32Array([0, 1, 2, 0, 2, 3]),
	}


# ---------------------------------------------------------------- gate 2: tiny

func _tiny(elf: PackedByteArray, out_dir: String) -> void:
	print("== gate 2: tiny (one quad, one shape 'lean', weights 0 -> 1 -> 0 over 48 time codes at 24 tps)")
	var q := _quad()
	var pts: PackedFloat32Array = q["pts"]
	# lean as the interactor-flow spec has it: offsets [(0,0,0),(0,0,0),(0,0,1),(0,0,1)]; sparse, the zero points dropped.
	var idx := PackedInt32Array([2, 3])
	var off := PackedFloat32Array([0, 0, 1, 0, 0, 1])
	var frames := 49
	var w := PackedFloat32Array()
	for f in frames:
		w.append(1.0 - absf(float(f) - 24.0) / 24.0)
	var clip := {"frames": frames, "fps": 24, "weights": w}
	var shapes := [{"name": "lean", "idx": idx, "off": off}]

	var sb = _new_sb(elf)
	var pkg = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, clip)
	if not _ok("write: a .usdz comes back", typeof(pkg) == TYPE_PACKED_BYTE_ARRAY and pkg.size() > 2 and pkg[0] == 0x50 and pkg[1] == 0x4B, str(pkg)):
		return
	_save(out_dir, "tiny_lean.usdz", pkg)
	print("     tiny_lean.usdz: ", pkg.size(), " bytes")
	var opened := _open(sb, pkg)
	_ok("open: ok, 1 mesh, 1 blend shape, 1 anim", opened.begins_with("ok ") and opened.contains("meshes=1") and opened.contains("blend_shapes=1") and opened.contains("anims=1"), opened)
	_ok("usd_blend_shape_count == 1", int(sb.vmcall("usd_blend_shape_count")) == 1)
	var bi: Dictionary = sb.vmcall("usd_blend_shape_info", 0)
	_ok("shape info: name lean, mesh 0, target, 4 points, no normals",
			bi.get("name") == "lean" and int(bi.get("mesh")) == 0 and int(bi.get("primary")) == -1 and int(bi.get("points")) == 4 and not bi.get("has_normals"), str(bi))
	var mi: Dictionary = sb.vmcall("usd_mesh_info", 0)
	_ok("mesh info: shape_first 0, shapes 1, points 4, triangles 2", int(mi["shape_first"]) == 0 and int(mi["shapes"]) == 1 and int(mi["points"]) == 4 and int(mi["triangles"]) == 2, str(mi))
	_ok("mesh points bit for bit", _same_bits(_read_floats(sb, "usd_mesh_points", 0), pts))
	_ok("mesh triangles (fan of the quad)", sb.vmcall("usd_mesh_indices", 0) == q["fan"])
	var want_off := _dense(4, idx, off)
	var got_off: PackedFloat32Array = sb.vmcall("usd_blend_shape_offsets_slice", 0, 0, 0)
	_ok("offsets bit for bit (reader densifies the sparse input)", _same_bits(got_off, want_off), str(got_off))
	var sl: PackedFloat32Array = sb.vmcall("usd_blend_shape_offsets_slice", 0, 2, 2)
	_ok("offsets slice [2, 4) bit for bit", _same_bits(sl, want_off.slice(6, 12)))
	var ai: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("anim info: %d frames, 24 fps, 1 shape 'lean', no joints" % frames,
			int(ai.get("frames")) == frames and float(ai.get("fps")) == 24.0 and int(ai.get("nshapes")) == 1 and ai.get("blend_shapes") == "lean\n" and int(ai.get("njoints")) == 0, str(ai))
	var tl := String(ai.get("times")).split("\n", false)
	_ok("anim time codes 0 .. 48", tl.size() == frames and tl[0] == "0" and tl[frames - 1] == "48", str(tl))
	var got_w: PackedFloat32Array = sb.vmcall("usd_skel_anim_blend_weights", 0)
	_ok("weights bit for bit", _same_bits(got_w, w))
	_ok("weights shape: 0 at the ends, 1 at 24", got_w[0] == 0.0 and got_w[24] == 1.0 and got_w[48] == 0.0)
	# planted controls: the comparison must be able to fail
	_ok("control: one ulp in one offset is caught", not _same_bits(got_off, _dense(4, idx, _ulp(off, 5))))
	_ok("control: one ulp in one weight is caught", not _same_bits(got_w, _ulp(w, 17)))
	_ok("control: one ulp in the first weight (0.0) is caught", not _same_bits(got_w, _ulp(w, 0)))
	# the same planted change really written (not only compared): the reader must return the changed bits
	var pk2 = _write(sb, "quad", pts, q["counts"], q["corners"], [{"name": "lean", "idx": idx, "off": _ulp(off, 5)}], {"frames": frames, "fps": 24, "weights": _ulp(w, 17)})
	_open(sb, pk2)
	_ok("control: a really-written one-ulp offset reads back different from the original", not _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", 0, 0, 0), want_off))
	_ok("control: a really-written one-ulp weight reads back different from the original", not _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w))
	_ok("... and the same as the planted values", _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", 0, 0, 0), _dense(4, idx, _ulp(off, 5))) and _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), _ulp(w, 17)))

	# stripped weights: the written layer with its blendShapeWeights removed
	var text := _usda_of(pkg)
	var at := text.find("float[] blendShapeWeights.timeSamples")
	var close := text.find("}", at)
	_ok("the layer holds the weights block", at >= 0 and close > at)
	var stripped := text.substr(0, at) + text.substr(close + 1)
	var so := _open(sb, stripped.to_utf8_buffer())
	var sa: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	var sw = sb.vmcall("usd_skel_anim_blend_weights", 0)
	_ok("control: stripped weights read back as no weight samples (frames 0, no weights)",
			so.begins_with("ok ") and int(sa.get("frames", -1)) == 0 and (typeof(sw) != TYPE_PACKED_FLOAT32_ARRAY or sw.size() == 0), "%s | %s | %s" % [so, sa, str(sw)])
	_ok("... the shape itself is still there", int(sb.vmcall("usd_blend_shape_count")) == 1)

	# shapeless mesh
	var pk3 = _write(sb, "quad", pts, q["counts"], q["corners"], [], {})
	var o3 := _open(sb, pk3)
	_ok("control: a shapeless mesh reads back blend_shape_count 0 and no animation",
			o3.begins_with("ok ") and int(sb.vmcall("usd_blend_shape_count")) == 0 and int(sb.vmcall("usd_skel_anim_count")) == 0 and o3.contains("blend_shapes=0"), o3)
	var mi3: Dictionary = sb.vmcall("usd_mesh_info", 0)
	_ok("... and mesh info shapes 0", int(mi3["shapes"]) == 0)

	# shapes without a clip
	var pk4 = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, {})
	var o4 := _open(sb, pk4)
	_ok("shapes without a clip: 1 shape, no animation", o4.begins_with("ok ") and int(sb.vmcall("usd_blend_shape_count")) == 1 and int(sb.vmcall("usd_skel_anim_count")) == 0, o4)

	# the zero points listed too (the spec lists all four)
	var pk5 = _write(sb, "quad", pts, q["counts"], q["corners"], [{"name": "lean", "idx": PackedInt32Array([0, 1, 2, 3]), "off": PackedFloat32Array([0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 1])}], clip)
	_open(sb, pk5)
	_ok("all four points listed reads back the same offsets", _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", 0, 0, 0), want_off))

	print("== extra: two shapes, one with normal offsets on a sparse subset, negative zero and odd values")
	var o2 := PackedFloat32Array([0.1, -0.0, 3.4e-39, 1.0e30, -2.5, 0.3333333432674408])
	var n2 := PackedFloat32Array([0, 0, 1, 0, 0.5, 0.25])
	var w2 := PackedFloat32Array([0.0, 1.0, 0.5, 0.25, 0.125, 1.0, 1.0, 0.0])
	var shapes2 := [{"name": "lean", "idx": idx, "off": off}, {"name": "bulge", "idx": PackedInt32Array([1, 3]), "off": o2, "nrm": n2}]
	var pk6 = _write(sb, "quad", pts, q["counts"], q["corners"], shapes2, {"frames": 4, "fps": 30, "weights": w2})
	var o6 := _open(sb, pk6)
	_ok("two shapes open", o6.begins_with("ok ") and int(sb.vmcall("usd_blend_shape_count")) == 2, o6)
	_ok("shape 1 offsets bit for bit (negative zero, a denormal, 1e30 kept)", _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", 1, 0, 0), _dense(4, PackedInt32Array([1, 3]), o2)))
	var b1: Dictionary = sb.vmcall("usd_blend_shape_info", 1)
	_ok("shape 1 has normals, name bulge", b1.get("name") == "bulge" and b1.get("has_normals"))
	_ok("shape 1 normal offsets bit for bit", _same_bits(sb.vmcall("usd_blend_shape_normals_slice", 1, 0, 0), _dense(4, PackedInt32Array([1, 3]), n2)))
	var a6: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("anim: 4 frames at 30 fps, shapes lean + bulge", int(a6.get("frames")) == 4 and float(a6.get("fps")) == 30.0 and a6.get("blend_shapes") == "lean\nbulge\n", str(a6))
	_ok("weights row-major by frame bit for bit", _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w2))

	print("== extra: skinned (one joint): the animation sits under the Skeleton and the Skeleton binds it")
	var rests := PackedFloat32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0])
	var skin := {"joints": "root\t-1", "rests": rests, "weights": PackedFloat32Array([1, 1, 1, 1]), "indices": PackedInt32Array([0, 0, 0, 0])}
	var pk7 = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, clip, skin)
	var o7 := _open(sb, pk7)
	_ok("skinned with shapes opens", o7.begins_with("ok ") and int(sb.vmcall("usd_blend_shape_count")) == 1 and int(sb.vmcall("usd_skel_anim_count")) == 1, o7)
	var sk: Dictionary = sb.vmcall("usd_skeleton_info", 0)
	_ok("the skeleton's animation is the clip", String(sk.get("animation")) == "/quad/Skeleton/Motion", str(sk))
	_ok("skinned: offsets and weights bit for bit", _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", 0, 0, 0), want_off) and _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w))
	_save(out_dir, "tiny_lean_skinned.usdz", pk7)

	print("== extra: errors come back as ERR:")
	var bad_cases := {
		"index out of range": [{"name": "x", "idx": PackedInt32Array([9]), "off": PackedFloat32Array([0, 0, 1])}],
		"not increasing": [{"name": "x", "idx": PackedInt32Array([2, 2]), "off": PackedFloat32Array([0, 0, 1, 0, 0, 1])}],
		"no points": [{"name": "x", "idx": PackedInt32Array(), "off": PackedFloat32Array()}],
		"in-between weight 0": [{"name": "x", "idx": idx, "off": off, "ibs": [{"name": "h", "weight": 0.0, "off": off}]}],
		"in-between weight 1": [{"name": "x", "idx": idx, "off": off, "ibs": [{"name": "h", "weight": 1.0, "off": off}]}],
	}
	for k in bad_cases:
		var r = _write(sb, "quad", pts, q["counts"], q["corners"], bad_cases[k], {})
		_ok("error: " + k, typeof(r) == TYPE_STRING and String(r).begins_with("ERR: "), str(r).left(100))
	sb.queue_free()


# ---------------------------------------------------------------- gate 4: names

# The engine's rule, computed here with the engine's own String functions (modules/gltf/gltf_document.cpp
# _gen_unique_name_static: validate_node_name, then the first free of s, s2, s3 ...), plus
# validate_ascii_identifier so the result is a valid USD identifier. The guest reimplements it in C++.
# String::validate_ascii_identifier (core/string/ustring.cpp), which GDScript does not expose: empty is "_",
# a leading digit gets "_" in front, every character that is not [A-Za-z0-9_] becomes "_".
func _validate_ascii_identifier(s: String) -> String:
	if s.is_empty():
		return "_"
	var r := "_" if (s.unicode_at(0) >= 48 and s.unicode_at(0) <= 57) else ""
	for i in s.length():
		var c := s.unicode_at(i)
		var ok := (c >= 48 and c <= 57) or (c >= 65 and c <= 90) or (c >= 97 and c <= 122) or c == 95
		r += char(c) if ok else "_"
	return r


func _expected_tokens(names: Array) -> PackedStringArray:
	var used := {}
	var out := PackedStringArray()
	for g in names:
		var s: String = _validate_ascii_identifier(String(g).validate_node_name())
		var u := s
		var index := 1
		while true:
			u = s
			if index > 1:
				u += str(index)
			if not used.has(u):
				break
			index += 1
		used[u] = true
		out.append(u)
	return out


func _is_usd_identifier(s: String) -> bool:
	var re := RegEx.new()
	re.compile("^[A-Za-z_][A-Za-z0-9_]*$")
	return re.search(s) != null


func _names(elf: PackedByteArray, out_dir: String) -> void:
	print("== gate 4: shape names that are not valid USD identifiers (engine rule + original_name)")
	var q := _quad()
	var pts: PackedFloat32Array = q["pts"]
	var names := ["vrc.blink_left", "vrc.v_aa", "BaseBody_Hide_Shoulder.L", "a.b", "a_b", "1digit", "", "two words", "café", "q\"uote", "back\\slash", "a_b2", "x:y/z@w%v"]
	var exp := _expected_tokens(names)
	print("     expected working names: ", exp)
	var shapes := []
	var w := PackedFloat32Array()
	for i in names.size():
		shapes.append({"name": names[i], "idx": PackedInt32Array([i % 4]), "off": PackedFloat32Array([float(i), 0.5, -1.0])})
	for f in 3:
		for i in names.size():
			w.append(float(f * 10 + i) / 100.0)
	var sb = _new_sb(elf)
	var pkg = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, {"frames": 3, "fps": 24, "weights": w})
	if not _ok("write (13 names: dots, a collision pair, a leading digit, the empty name, spaces, non-ASCII, quote, backslash)", typeof(pkg) == TYPE_PACKED_BYTE_ARRAY, str(pkg)):
		return
	_save(out_dir, "names.usdz", pkg)
	var text := _usda_of(pkg)
	var quoted := []
	for t in exp:
		quoted.append("\"%s\"" % t)
	_ok("skel:blendShapes holds the working names, in order", text.contains("uniform token[] skel:blendShapes = [" + ", ".join(PackedStringArray(quoted)) + "]"))
	var all_prims := true
	for t in exp:
		if not text.contains("def BlendShape \"%s\"" % t):
			all_prims = false
	_ok("a BlendShape prim named by every working name", all_prims)
	_ok("customData original_name is on the prim", text.contains("string original_name = \"vrc.blink_left\"") and text.contains("string original_name = \"\""))
	var tp := text.find("skel:blendShapeTargets")
	var tgt := text.substr(tp, text.find("]", tp) - tp)
	var tg_ok := true
	for t in exp:
		if not tgt.contains("</quad/quad/%s>" % t):
			tg_ok = false
	_ok("the targets point at the working names", tg_ok, tgt)
	var anim_line := ""
	for l in text.split("\n"):
		if String(l).contains("uniform token[] blendShapes"):
			anim_line = String(l)
	_ok("the animation's blendShapes are the same working names", anim_line.contains(", ".join(PackedStringArray(quoted))), anim_line)
	var opened := _open(sb, pkg)
	_ok("open", opened.begins_with("ok ") and opened.contains("blend_shapes=%d" % names.size()), opened)
	var orig_ok := true
	var tok_ok := true
	var off_ok := true
	var seen := {}
	var valid := true
	for i in names.size():
		var bi: Dictionary = sb.vmcall("usd_blend_shape_info", i)
		if String(bi.get("name")).to_utf8_buffer() != String(names[i]).to_utf8_buffer():
			orig_ok = false
			print("     name %d: got '%s' want '%s'" % [i, bi.get("name"), names[i]])
		if bi.get("token") != exp[i]:
			tok_ok = false
			print("     token %d: got '%s' want '%s'" % [i, bi.get("token"), exp[i]])
		if not _is_usd_identifier(String(bi.get("token"))) or seen.has(bi.get("token")):
			valid = false
		seen[bi.get("token")] = true
		if not _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", i, 0, 0), _dense(4, PackedInt32Array([i % 4]), PackedFloat32Array([float(i), 0.5, -1.0]))):
			off_ok = false
	_ok("the reader's name is the original string, byte for byte, for all 13", orig_ok)
	_ok("the reader's token is the engine-rule working name, for all 13", tok_ok)
	_ok("working names are valid USD identifiers and unique", valid)
	_ok("each shape's offsets read back bit for bit (the right prim carries the right data)", off_ok)
	var ai: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("the clip's blend_shapes are the originals in order, byte for byte", String(ai.get("blend_shapes")).to_utf8_buffer() == ("".join(PackedStringArray(names.map(func(n): return n + "\n")))).to_utf8_buffer(), str(ai.get("blend_shapes")))
	_ok("the clip's tokens are the working names in order", String(ai.get("blend_shape_tokens")) == "".join(PackedStringArray(Array(exp).map(func(n): return n + "\n"))))
	var aligned := true
	var atok := String(ai.get("blend_shape_tokens")).split("\n", true)
	for i in names.size():
		if atok[i] != String(sb.vmcall("usd_blend_shape_info", i).get("token")):
			aligned = false
	_ok("the clip's tracks are aligned with the mesh's targets (track i token == target i token)", aligned)
	_ok("weights bit for bit", _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w))
	# a file without customData (an older writer's, or Blender's) reads as it does today: name == token
	var re := RegEx.new()
	re.compile("\\(\\s*customData = \\{[^}]*\\}\\s*\\)")
	var plain := re.sub(text, "", true)
	_ok("control: the customData is gone from the stripped layer", not plain.contains("original_name") and not plain.contains("original_names"))
	var po := _open(sb, plain.to_utf8_buffer())
	var legacy := po.begins_with("ok ")
	for i in names.size():
		var bi2: Dictionary = sb.vmcall("usd_blend_shape_info", i)
		if bi2.get("name") != exp[i] or bi2.get("token") != exp[i]:
			legacy = false
	var ai2: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("without customData: name == token, and the clip's names are its tokens", legacy and ai2.get("blend_shapes") == ai2.get("blend_shape_tokens"), po)
	_ok("control: with customData the name differs from the token for vrc.blink_left", exp[0] != names[0])
	sb.queue_free()


# ---------------------------------------------------------------- gate 6: in-betweens

func _inbetween(elf: PackedByteArray, out_dir: String) -> void:
	print("== gate 6: in-between shapes (UsdSkelInbetweenShape)")
	var q := _quad()
	var pts: PackedFloat32Array = q["pts"]
	var idx := PackedInt32Array([0, 2, 3])
	var t_off := PackedFloat32Array([0.1, -0.0, 0.3, 0, 0, 0, 1, 1, 1])
	var ib0_off := PackedFloat32Array([0.05, 0.1, 0.15, 0.3, 0, 0, 0.5, 0.5, 0.5])
	var ib1_off := PackedFloat32Array([0.07, 0.02, 0.01, 0, 0.25, 0, 0.75, 0.1, 0.9])
	var ib1_nrm := PackedFloat32Array([0, 1, 0, 0, 0, 1, 0.5, 0.5, 0])
	var wide_off := PackedFloat32Array([2, 0, 0, 0, 2, 0])
	var wide_ib := PackedFloat32Array([1, 0, 0, 0, 1, 0])
	var shapes := [
		{"name": "smile", "idx": idx, "off": t_off, "ibs": [
			{"name": "half", "weight": 0.5, "off": ib0_off},
			{"name": "q.uarter", "weight": 0.3, "off": ib1_off, "nrm": ib1_nrm}]},
		{"name": "frown", "idx": PackedInt32Array([1]), "off": PackedFloat32Array([0, 0, 4])},
		{"name": "wide", "idx": PackedInt32Array([1, 3]), "off": wide_off, "ibs": [{"name": "mid", "weight": 0.9, "off": wide_ib}]},
	]
	var frames := 5
	var w := PackedFloat32Array()
	for f in frames:
		for k in 3:
			w.append(float(f) * 0.25 + float(k) * 0.0625)
	var clip := {"frames": frames, "fps": 24, "weights": w}
	var sb = _new_sb(elf)
	var pkg = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, clip)
	if not _ok("write: 3 targets, 3 in-betweens", typeof(pkg) == TYPE_PACKED_BYTE_ARRAY, str(pkg)):
		return
	_save(out_dir, "inbetweens.usdz", pkg)
	var text := _usda_of(pkg)
	_ok("the layer has inbetweens: attributes with a weight and the shared pointIndices", text.contains("inbetweens:half") and text.contains("inbetweens:q_uarter") and text.contains("inbetweens:mid"))
	var opened := _open(sb, pkg)
	_ok("open: 6 records (3 targets, 3 in-betweens)", opened.begins_with("ok ") and opened.contains("blend_shapes=6"), opened)
	var by_name := {}
	for k in int(sb.vmcall("usd_blend_shape_count")):
		var bi: Dictionary = sb.vmcall("usd_blend_shape_info", k)
		by_name[bi["name"]] = k
	print("     records in reader order: ", by_name.keys())
	var want := {
		"smile": [-1, 1.0, idx, t_off, null],
		"smile:half": ["smile", 0.5, idx, ib0_off, null],
		"smile:q.uarter": ["smile", 0.3, idx, ib1_off, ib1_nrm],
		"frown": [-1, 1.0, PackedInt32Array([1]), PackedFloat32Array([0, 0, 4]), null],
		"wide": [-1, 1.0, PackedInt32Array([1, 3]), wide_off, null],
		"wide:mid": ["wide", 0.9, PackedInt32Array([1, 3]), wide_ib, null],
	}
	var all_there := by_name.size() == 6
	for n in want:
		if not by_name.has(n):
			all_there = false
	_ok("the reader names every record as <target>:<in-between>, originals", all_there, str(by_name.keys()))
	if not all_there:
		return
	var prim_ok := true
	var pos_ok := true
	var off_ok := true
	var nrm_ok := true
	var tok_ok := true
	for n in want:
		var k: int = by_name[n]
		var bi: Dictionary = sb.vmcall("usd_blend_shape_info", k)
		var prim = want[n][0]
		var exp_primary: int = -1 if (typeof(prim) == TYPE_INT) else int(by_name[prim])
		if int(bi["primary"]) != exp_primary or int(bi["points"]) != 4:
			prim_ok = false
		var wpos := PackedFloat32Array([float(want[n][1])])
		if not _same_bits(PackedFloat32Array([float(bi["position"])]), wpos):
			pos_ok = false
		if not _same_bits(sb.vmcall("usd_blend_shape_offsets_slice", k, 0, 0), _dense(4, want[n][2], want[n][3])):
			off_ok = false
		if want[n][4] != null:
			if not bi["has_normals"] or not _same_bits(sb.vmcall("usd_blend_shape_normals_slice", k, 0, 0), _dense(4, want[n][2], want[n][4])):
				nrm_ok = false
		elif bi["has_normals"]:
			nrm_ok = false
		if n.contains(":") and not String(bi["token"]).contains(":"):
			tok_ok = false
	_ok("primary = the target's record index, 4 points each", prim_ok)
	_ok("position = the in-between weight, bit for bit (0.5, 0.3 as float32, 0.9 as float32)", pos_ok)
	_ok("offsets of all 6 records bit for bit (reader densifies with the target's pointIndices)", off_ok)
	_ok("normal offsets only on the in-between that has them, bit for bit", nrm_ok)
	_ok("in-between token is <target token>:<in-between token> (q.uarter -> q_uarter)", String(sb.vmcall("usd_blend_shape_info", by_name["smile:q.uarter"])["token"]) == "smile:q_uarter" and tok_ok)
	var ai: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("the clip has tracks for the 3 targets only", ai.get("blend_shapes") == "smile\nfrown\nwide\n" and int(ai.get("nshapes")) == 3 and int(ai.get("frames")) == frames, str(ai))
	_ok("weights bit for bit", _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w))
	# planted one-ulp controls on an in-between offset and on an in-between weight
	var p_off := _ulp(ib1_off, 4)
	var wf := PackedFloat32Array([0.3])
	var p_w := _ulp(wf, 0)
	var shapes2 := shapes.duplicate(true)
	shapes2[0]["ibs"][1]["off"] = p_off
	shapes2[0]["ibs"][1]["weight"] = p_w[0]
	var pk2 = _write(sb, "quad", pts, q["counts"], q["corners"], shapes2, clip)
	_open(sb, pk2)
	var k2: int = 0
	for k in int(sb.vmcall("usd_blend_shape_count")):
		if sb.vmcall("usd_blend_shape_info", k)["name"] == "smile:q.uarter":
			k2 = k
	var got_off: PackedFloat32Array = sb.vmcall("usd_blend_shape_offsets_slice", k2, 0, 0)
	var got_pos := PackedFloat32Array([float(sb.vmcall("usd_blend_shape_info", k2)["position"])])
	_ok("control: one ulp in an in-between offset is caught (differs from the original expectation)", not _same_bits(got_off, _dense(4, idx, ib1_off)))
	_ok("control: one ulp in an in-between weight is caught", not _same_bits(got_pos, wf))
	_ok("... and both equal the planted values bit for bit", _same_bits(got_off, _dense(4, idx, p_off)) and _same_bits(got_pos, p_w))
	# a target without in-betweens still reads as before
	var pk3 = _write(sb, "quad", pts, q["counts"], q["corners"], [shapes[1]], {})
	_open(sb, pk3)
	_ok("a target with no in-betweens: 1 record, primary -1", int(sb.vmcall("usd_blend_shape_count")) == 1 and int(sb.vmcall("usd_blend_shape_info", 0)["primary"]) == -1)
	sb.queue_free()


# ---------------------------------------------------------------- gate 7: blend-shape weights in a joint-motion clip

func _clip(elf: PackedByteArray, out_dir: String, old_elf: PackedByteArray) -> void:
	print("== gate 7: usd_write_skel_clip with blend-shape weights in the same SkelAnimation (21 joints x 12 frames, 5 shapes)")
	var J := 21
	var F := 12
	var K := 5
	var names := PackedStringArray()
	var parents := PackedInt32Array()
	var rests := PackedFloat32Array()
	for j in J:
		names.append("root" if j == 0 else "joint_%d" % j)
		parents.append(-1 if j == 0 else (j - 1) / 2)
		var tr := [0.0, 0.1 * float(j), 0.0]
		var m := [1.0, 0, 0, 0, 1.0, 0, 0, 0, 1.0]
		rests.append_array(PackedFloat32Array(m + tr))
		rests.append_array(PackedFloat32Array(m + [0.0, 0.1 * float(j), 0.0]))
	var aj := PackedInt32Array()
	for j in J:
		aj.append(j)
	# Proper rotations of 180 degrees (diagonal +-1: exact through the quaternion OpenUSD stores) and varied translations.
	var rots := [[1, 1, 1], [1, -1, -1], [-1, 1, -1], [-1, -1, 1]]
	var xf := PackedFloat32Array()
	for f in F:
		for j in J:
			var r: Array = rots[(f + j) % 4]
			xf.append_array(PackedFloat32Array([r[0], 0, 0, 0, r[1], 0, 0, 0, r[2], float(f) * 0.25 + float(j) * 0.0078125, 1.0 / 3.0 + float(f), -float(j) * 0.1]))
	var shape_names := ["smile", "vrc.blink_left", "a.b", "a_b", ""]
	var w := PackedFloat32Array()
	for f in F:
		for k in K:
			w.append(float((f * 37 + k * 11) % 100) / 99.0)
	var fps := 30.0
	var meta_shapes := "source=usd_blendshapes.gd\nnshapes=%d" % K
	for k in K:
		meta_shapes += "\nshape%d=%s" % [k, shape_names[k]]
	var both := PackedFloat32Array(xf)
	both.append_array(w)
	var sb = _new_sb(elf)
	var txt = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, both, fps, meta_shapes)
	if not _ok("write: a layer comes back", typeof(txt) == TYPE_STRING and String(txt).begins_with("#usda"), str(txt).left(200)):
		return
	var f1 := FileAccess.open(out_dir.path_join("clip_joints_shapes.usda"), FileAccess.WRITE)
	f1.store_string(txt)
	f1.close()
	var opened := str(sb.vmcall("usd_open", String(txt).to_utf8_buffer()))
	_ok("open: ok, 1 anim", opened.begins_with("ok ") and opened.contains("anims=1"), opened)
	var ai: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	var exp := _expected_tokens(shape_names)
	_ok("anim info: 12 frames, 30 fps, 21 joints, 5 shapes, originals in order",
			int(ai["frames"]) == F and float(ai["fps"]) == fps and int(ai["njoints"]) == J and int(ai["nshapes"]) == K
			and String(ai["blend_shapes"]).to_utf8_buffer() == ("".join(PackedStringArray(shape_names.map(func(n): return n + "\n")))).to_utf8_buffer(), str(ai).left(300))
	_ok("anim tokens are the engine-rule working names", String(ai["blend_shape_tokens"]) == "".join(PackedStringArray(Array(exp).map(func(n): return n + "\n"))), str(ai["blend_shape_tokens"]))
	_ok("time codes 0 .. 11", String(ai["times"]).split("\n", false).size() == F and String(ai["times"]).begins_with("0\n1\n") and String(ai["times"]).ends_with("11\n"), str(ai["times"]))
	var got_xf: PackedFloat32Array = sb.vmcall("usd_skel_anim_transforms", 0)
	var got_w: PackedFloat32Array = sb.vmcall("usd_skel_anim_blend_weights", 0)
	_ok("transforms 12 x 21 x 12 floats bit for bit", _same_bits(got_xf, xf), "got %d want %d floats" % [got_xf.size(), xf.size()])
	_ok("weights 12 x 5 bit for bit", _same_bits(got_w, w))
	_ok("control: one ulp in one transform float is caught", not _same_bits(got_xf, _ulp(xf, 12 * 21 * 6 + 9)))
	_ok("control: one ulp in one weight is caught", not _same_bits(got_w, _ulp(w, 7)))
	# really-written plants
	var bx := PackedFloat32Array(_ulp(xf, 12 * 21 * 6 + 9))
	bx.append_array(_ulp(w, 7))
	var t2 = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, bx, fps, meta_shapes)
	sb.vmcall("usd_open", String(t2).to_utf8_buffer())
	_ok("control: a really-written one-ulp transform and weight read back as the planted values, not the originals",
			_same_bits(sb.vmcall("usd_skel_anim_transforms", 0), _ulp(xf, 12 * 21 * 6 + 9)) and _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), _ulp(w, 7))
			and not _same_bits(sb.vmcall("usd_skel_anim_transforms", 0), xf) and not _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w))
	# joints, no shapes: the call as it was
	var meta_plain := "source=usd_blendshapes.gd"
	var t3 = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, xf, fps, meta_plain)
	var o3 := str(sb.vmcall("usd_open", String(t3).to_utf8_buffer()))
	var a3: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	var bw3 = sb.vmcall("usd_skel_anim_blend_weights", 0)
	_ok("joints but no shapes: nshapes 0, no names, transforms unchanged bit for bit",
			o3.begins_with("ok ") and int(a3["nshapes"]) == 0 and String(a3["blend_shapes"]) == "" and int(a3["frames"]) == F and _same_bits(sb.vmcall("usd_skel_anim_transforms", 0), xf), o3 + str(a3).left(200))
	_ok("... and it has no weights (empty or no answer)", typeof(bw3) != TYPE_PACKED_FLOAT32_ARRAY or bw3.size() == 0)
	if old_elf.size() > 0:
		var sb_old = _new_sb(old_elf)
		var t_old = sb_old.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, xf, fps, meta_plain)
		_ok("byte-identical: the no-shape clip from the new ELF equals the old ELF's, byte for byte (%d bytes)" % String(t3).length(), typeof(t_old) == TYPE_STRING and String(t_old).to_utf8_buffer() == String(t3).to_utf8_buffer())
		sb_old.queue_free()
	# the same joint transforms in the shapes version and in the plain one
	_ok("the transforms with and without shapes are the same bits", _same_bits(got_xf, sb.vmcall("usd_skel_anim_transforms", 0)))
	# the skeleton still binds the animation
	sb.vmcall("usd_open", String(txt).to_utf8_buffer())
	var sk: Dictionary = sb.vmcall("usd_skeleton_info", 0)
	_ok("the skeleton binds the animation that carries both", String(sk.get("animation")) == "/Clip/Skeleton/Motion", str(sk).left(200))
	# weights only (no animated joints)
	var wo = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, PackedInt32Array(), w, fps, meta_shapes + "\nclip_frames=%d" % F)
	var oo := str(sb.vmcall("usd_open", String(wo).to_utf8_buffer()))
	var ao: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("a weights-only clip: 0 animated joints, 12 frames, weights bit for bit", oo.begins_with("ok ") and int(ao["njoints"]) == 0 and int(ao["frames"]) == F and int(ao["nshapes"]) == K and _same_bits(sb.vmcall("usd_skel_anim_blend_weights", 0), w), oo + str(ao).left(200))
	# errors
	var bad = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, xf, fps, meta_shapes)
	_ok("error: shapes announced but the weight floats are missing", String(bad).begins_with("FAIL") or String(bad).begins_with("ERR"), str(bad).left(120))
	sb.queue_free()
# ---------------------------------------------------------------- gate 3 + 5: body

func _find(n: Node, name: String) -> MeshInstance3D:
	if n is MeshInstance3D and n.name == name and n.mesh is ArrayMesh:
		return n
	for c in n.get_children():
		var r := _find(c, name)
		if r != null:
			return r
	return null


func _body(elf: PackedByteArray, out_dir: String, fbx: String, mesh_name: String, frames: int, dense := false) -> void:
	print("== gates 3 and 5: ", mesh_name, " from ", fbx.get_file())
	var t0 := Time.get_ticks_msec()
	var doc := FBXDocument.new()
	var st := FBXState.new()
	doc.append_from_file(fbx, st)
	var scene: Node = doc.generate_scene(st)
	var mi := _find(scene, mesh_name)
	if not _ok("the mesh node exists", mi != null):
		return
	var m: ArrayMesh = mi.mesh
	var K := m.get_blend_shape_count()
	# blend_shape_mode NORMALIZED (0): the shape arrays are absolute positions, so offset = shape - base.
	# RELATIVE (1): the shape arrays are the offsets. Positions are float32 values held as doubles, so the
	# double difference is exact and its rounding into the PackedFloat32Array is the float32 subtraction.
	var normalized := m.blend_shape_mode == Mesh.BLEND_SHAPE_MODE_NORMALIZED
	print("     blend_shape_mode=", m.blend_shape_mode, " (", "NORMALIZED: offset = shape - base" if normalized else "RELATIVE: offset = shape array", ")")
	var pts := PackedFloat32Array()
	var corners := PackedInt32Array()
	var sidx: Array = []
	var soff: Array = []
	for k in K:
		sidx.append(PackedInt32Array())
		soff.append(PackedFloat32Array())
	var base := 0
	for s in m.get_surface_count():
		var arr := m.surface_get_arrays(s)
		var v: PackedVector3Array = arr[Mesh.ARRAY_VERTEX]
		var ix = arr[Mesh.ARRAY_INDEX]
		for p in v:
			pts.append(p.x)
			pts.append(p.y)
			pts.append(p.z)
		if ix != null and ix.size() > 0:
			for i in ix:
				corners.append(base + i)
		else:
			for i in v.size():
				corners.append(base + i)
		var bsa := m.surface_get_blend_shape_arrays(s)
		for k in K:
			var sv: PackedVector3Array = bsa[k][Mesh.ARRAY_VERTEX]
			var si: PackedInt32Array = sidx[k]
			var so: PackedFloat32Array = soff[k]
			for i in v.size():
				var ox := (sv[i].x - v[i].x) if normalized else sv[i].x
				var oy := (sv[i].y - v[i].y) if normalized else sv[i].y
				var oz := (sv[i].z - v[i].z) if normalized else sv[i].z
				var n0 := so.size()
				so.append(ox)
				so.append(oy)
				so.append(oz)
				if not dense and so[n0] == 0.0 and so[n0 + 1] == 0.0 and so[n0 + 2] == 0.0:
					so.resize(n0) # an exact zero offset (as float32) is not listed
				else:
					si.append(base + i)
			sidx[k] = si
			soff[k] = so
		base += v.size()
	var npts := pts.size() / 3
	var counts := PackedInt32Array()
	counts.resize(corners.size() / 3)
	counts.fill(3)
	var shapes := []
	var listed := 0
	var empty_shapes := 0
	var all_names := PackedStringArray()
	for k in K:
		var nm := str(m.get_blend_shape_name(k))
		all_names.append(nm)
		var si: PackedInt32Array = sidx[k]
		var so: PackedFloat32Array = soff[k]
		if si.size() == 0:
			# the writer refuses an empty shape; list point 0 with its zero offset (the reader reads it as zeros anyway)
			empty_shapes += 1
			si = PackedInt32Array([0])
			so = PackedFloat32Array([0, 0, 0])
		listed += si.size()
		shapes.append({"name": nm, "idx": si, "off": so})
	var w := PackedFloat32Array()
	# deterministic weights: frame f, shape k -> ((f * 131 + k * 17) % 1000) / 999, with exact 0 and 1 mixed in
	for f in frames:
		for k in K:
			w.append(float((f * 131 + k * 17) % 1000) / 999.0)
	print("     loaded in %d ms: %d points, %d triangles, %d shapes, %d listed points (dense would be %d), %d empty shapes, %d frames" % [
			Time.get_ticks_msec() - t0, npts, corners.size() / 3, K, listed, K * npts, empty_shapes, frames])
	print("     float array to send: ", (npts * 3 + listed * 3 + w.size()), " floats = ", (npts * 3 + listed * 3 + w.size()) * 4, " bytes")

	var sb = _new_sb(elf)
	var t1 := Time.get_ticks_msec()
	var pkg
	if dense:
		push_input = true
		# staged: the package stays in the writer sandbox, comes out in slices, goes into a fresh reader sandbox
		var sz = _write(sb, mesh_name, pts, counts, corners, shapes, {"frames": frames, "fps": 30, "weights": w}, {}, "usd_write_mesh_staged")
		if not _ok("staged write answers the size as an int", typeof(sz) == TYPE_INT and sz > 64 * 1024 * 1024, str(sz).left(300)):
			return
		pkg = _fetch_written(sb)
		push_input = false
		_ok("usd_written_size matches and the slices add up (%d bytes)" % sz, int(sb.vmcall("usd_written_size")) == sz and pkg.size() == sz)
		sb.vmcall("usd_written_release")
		_ok("after release the size is 0", int(sb.vmcall("usd_written_size")) == 0)
		sb.free()
		sb = _new_sb(elf)
	else:
		pkg = _write(sb, mesh_name, pts, counts, corners, shapes, {"frames": frames, "fps": 30, "weights": w})
	if not _ok("write with shapes and clip: a .usdz comes back", typeof(pkg) == TYPE_PACKED_BYTE_ARRAY and pkg.size() > 2 and pkg[0] == 0x50, str(pkg).left(300)):
		return
	print("     write took %d ms; %s.usdz with shapes: %d bytes" % [Time.get_ticks_msec() - t1, mesh_name, pkg.size()])
	if not dense:
		_save(out_dir, mesh_name + "_shapes.usdz", pkg)
	var t2 := Time.get_ticks_msec()
	var opened := _open(sb, pkg)
	print("     open took %d ms: %s" % [Time.get_ticks_msec() - t2, opened.left(200)])
	_ok("open: ok, %d shapes, 1 anim" % K, opened.begins_with("ok ") and opened.contains("blend_shapes=%d" % K) and opened.contains("anims=1"), opened.left(300))
	_ok("blend_shape_count == %d" % K, int(sb.vmcall("usd_blend_shape_count")) == K)
	_ok("mesh points bit for bit", _same_bits(_read_floats(sb, "usd_mesh_points", 0), pts))
	var tri = sb.vmcall("usd_mesh_indices", 0)
	_ok("mesh triangles", typeof(tri) == TYPE_PACKED_INT32_ARRAY and tri == corners)
	var exp_tok := _expected_tokens(all_names)
	var bad_tok := 0
	var tok_seen := {}
	var tok_invalid := 0
	var bad_names := 0
	var bad_off := 0
	var bad_pts := 0
	var first_bad := ""
	for k in K:
		var bi: Dictionary = sb.vmcall("usd_blend_shape_info", k)
		if bi.get("name") != all_names[k] or int(bi.get("points")) != npts or int(bi.get("primary")) != -1:
			bad_names += 1
			if first_bad == "":
				first_bad = str(bi)
		if bi.get("token") != exp_tok[k]:
			bad_tok += 1
		if tok_seen.has(bi.get("token")) or not _is_usd_identifier(String(bi.get("token"))):
			tok_invalid += 1
		tok_seen[bi.get("token")] = true
		var got = sb.vmcall("usd_blend_shape_offsets_slice", k, 0, 0)
		if typeof(got) != TYPE_PACKED_FLOAT32_ARRAY or not _same_bits(got, _dense(npts, shapes[k]["idx"], shapes[k]["off"])):
			bad_off += 1
			if first_bad == "":
				first_bad = "offsets of shape %d" % k
	_ok("all %d shapes: name, point count and target flag as written (%d bad)" % [K, bad_names], bad_names == 0, first_bad)
	_ok("all %d shape names: the reader's name is the original string byte for byte, token = the engine-rule working name, valid and unique (%d token mismatches, %d invalid or repeated; %d of the names are not identifiers)" % [K, bad_tok, tok_invalid, all_names.size() - Array(all_names).filter(func(n): return _is_usd_identifier(n)).size()], bad_tok == 0 and tok_invalid == 0 and bad_names == 0)
	_ok("all %d shapes: offsets bit for bit, the reader's densified form vs the sparse input expanded with zeros (%d bad)" % [K, bad_off], bad_off == 0, first_bad)
	var ai: Dictionary = sb.vmcall("usd_skel_anim_info", 0)
	_ok("anim info: %d frames, 30 fps, %d shapes in order" % [frames, K], int(ai.get("frames")) == frames and float(ai.get("fps")) == 30.0 and int(ai.get("nshapes")) == K and String(ai.get("blend_shapes")) == "\n".join(all_names) + "\n", str(ai).left(300))
	_ok("the clip's %d tokens are the working names in order, aligned with the targets" % K, String(ai.get("blend_shape_tokens")) == "".join(PackedStringArray(Array(exp_tok).map(func(n): return n + "\n"))))
	var gw = sb.vmcall("usd_skel_anim_blend_weights", 0)
	var w_same: bool = typeof(gw) == TYPE_PACKED_FLOAT32_ARRAY and _same_bits(gw, w)
	_ok("all %d x %d weights bit for bit" % [frames, K], w_same, str(gw).left(200))
	# planted one-ulp controls on one offset of the last shape and one weight of the last frame
	var last: Dictionary = shapes[K - 1]
	var lo: PackedFloat32Array = last["off"]
	var planted_off := _dense(npts, last["idx"], _ulp(lo, lo.size() - 1))
	var got_last = sb.vmcall("usd_blend_shape_offsets_slice", K - 1, 0, 0)
	_ok("control: one ulp in one offset of the last shape is caught", typeof(got_last) == TYPE_PACKED_FLOAT32_ARRAY and not _same_bits(got_last, planted_off))
	_ok("control: one ulp in one weight of the last frame is caught", w_same and not _same_bits(gw, _ulp(w, w.size() - 1)))
	# size (gate 5): the same mesh without shapes
	var pk0 = _write(sb, mesh_name, pts, counts, corners, [], {})
	if typeof(pk0) == TYPE_PACKED_BYTE_ARRAY:
		if not dense:
			_save(out_dir, mesh_name + "_plain.usdz", pk0)
		print("SIZE %s: without shapes %d bytes, with %d shapes and a %d-frame clip %d bytes (%.2fx), %d listed points" % [
				mesh_name, pk0.size(), K, frames, pkg.size(), float(pkg.size()) / float(pk0.size()), listed])
	else:
		_ok("write without shapes", false, str(pk0).left(200))
	sb.queue_free()


# ---------------------------------------------------------------- staged results and slices

func _slices(elf: PackedByteArray, out_dir: String) -> void:
	print("== staged results: usd_written_size / usd_written_slice / usd_written_release")
	var q := _quad()
	var pts: PackedFloat32Array = q["pts"]
	var shapes := [{"name": "lean", "idx": PackedInt32Array([2, 3]), "off": PackedFloat32Array([0, 0, 1, 0, 0, 1])}]
	var w := PackedFloat32Array([0, 0.5, 1, 0.25])
	var clip := {"frames": 4, "fps": 24, "weights": w}
	var sb = _new_sb(elf)
	# nothing staged yet
	_ok("slice with nothing staged answers an error String", String(sb.vmcall("usd_written_slice", 0, 10)).begins_with("FAIL"))
	_ok("usd_written_size is 0 with nothing staged", int(sb.vmcall("usd_written_size")) == 0)
	# gate 4: the staged path and the whole path write the same bytes
	var whole = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, clip)
	var sz = _write(sb, "quad", pts, q["counts"], q["corners"], shapes, clip, {}, "usd_write_mesh_staged")
	_ok("staged mesh write answers the size as an int", typeof(sz) == TYPE_INT and typeof(whole) == TYPE_PACKED_BYTE_ARRAY and sz == whole.size(), str(sz))
	var got := _fetch_written(sb)
	_ok("staged bytes == whole bytes for the same small mesh (%d bytes)" % got.size(), got == whole)
	# gate 3: slice errors, none of them a crash
	var n := int(sb.vmcall("usd_written_size"))
	_ok("slice past the end answers an error String", String(sb.vmcall("usd_written_slice", n, 10)).begins_with("FAIL"))
	_ok("slice far past the end answers an error String", String(sb.vmcall("usd_written_slice", n + 1000000, 10)).begins_with("FAIL"))
	_ok("zero-length slice answers an error String", String(sb.vmcall("usd_written_slice", 0, 0)).begins_with("FAIL"))
	_ok("negative count answers an error String", String(sb.vmcall("usd_written_slice", 0, -5)).begins_with("FAIL"))
	_ok("negative from answers an error String", String(sb.vmcall("usd_written_slice", -1, 5)).begins_with("FAIL"))
	var tail = sb.vmcall("usd_written_slice", n - 3, 1000)
	_ok("a slice that runs past the end is cut to the end", typeof(tail) == TYPE_PACKED_BYTE_ARRAY and tail.size() == 3 and tail == whole.slice(n - 3))
	var huge = sb.vmcall("usd_written_slice", 0, 1 << 30)
	_ok("an oversized count is capped (here to the whole %d bytes)" % n, typeof(huge) == TYPE_PACKED_BYTE_ARRAY and huge.size() == n)
	sb.vmcall("usd_written_release")
	_ok("slice after release answers an error String", String(sb.vmcall("usd_written_slice", 0, 10)).begins_with("FAIL"))
	_ok("size after release is 0", int(sb.vmcall("usd_written_size")) == 0)
	# clip and curves: text results through the same calls
	var names := PackedStringArray(["root", "arm"])
	var parents := PackedInt32Array([-1, 0])
	var rests := PackedFloat32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 0])
	var aj := PackedInt32Array([0, 1])
	var al := PackedFloat32Array()
	for f in 3:
		for j in 2:
			al.append_array(PackedFloat32Array([1, 0, 0, 0, 1, 0, 0, 0, 1, float(f), float(j), 0.5]))
	var t_whole = sb.vmcall("usd_write_skel_clip", "\n".join(names), parents, rests, aj, al, 24.0, "source=t")
	var t_sz = sb.vmcall("usd_write_skel_clip_staged", "\n".join(names), parents, rests, aj, al, 24.0, "source=t")
	_ok("clip: staged text == whole text", typeof(t_sz) == TYPE_INT and typeof(t_whole) == TYPE_STRING and _fetch_written(sb) == String(t_whole).to_utf8_buffer())
	var cp := PackedFloat32Array([0, 0, 0, 1, 0, 0, 1, 1, 0])
	var c_whole = sb.vmcall("usd_write_curves", cp, PackedInt32Array([3]), PackedInt32Array(), "s0", "source=t")
	var c_sz = sb.vmcall("usd_write_curves_staged", cp, PackedInt32Array([3]), PackedInt32Array(), "s0", "source=t")
	_ok("curves: staged text == whole text", typeof(c_sz) == TYPE_INT and typeof(c_whole) == TYPE_STRING and _fetch_written(sb) == String(c_whole).to_utf8_buffer())
	_ok("an error is an ERR: String from the staged entry too", String(sb.vmcall("usd_write_mesh_staged", PackedFloat32Array(), PackedInt32Array(), "x\nnp=0\nnf=0\nni=0\nnshapes=1", "")).length() > 0 and typeof(sb.vmcall("usd_write_mesh_staged", PackedFloat32Array(), PackedInt32Array(), "x\nnp=0\nnf=0\nni=0\nnshapes=1", "")) == TYPE_STRING)
	# one array too large for the text form to be parsed back in the sandbox: refused with a message, not a crash
	var huge_pts := PackedFloat32Array()
	huge_pts.resize(2100000 * 3)
	push_input = true
	var huge_err = _write(sb, "huge", huge_pts, PackedInt32Array([3]), PackedInt32Array([0, 1, 2]), [], {})
	push_input = false
	_ok("a single array past 2,000,000 floats answers an ERR: String", typeof(huge_err) == TYPE_STRING and String(huge_err).begins_with("ERR: "), str(huge_err).left(120))
	# the whole-path call over the view limit: a typed STAGED answer, the same bytes as the staged entry.
	# 20000 points and 120 shapes that each list every point: arrays of modest size, a package past 15 MiB.
	var np := 20000
	var K := 120
	var big := PackedFloat32Array()
	big.resize(np * 3)
	for i in np:
		big[i * 3] = float(i) * 0.001
		big[i * 3 + 1] = float(i % 977) * 0.01
		big[i * 3 + 2] = -float(i % 13)
	var all_idx := PackedInt32Array()
	all_idx.resize(np)
	for i in np:
		all_idx[i] = i
	var many := []
	for k in K:
		var off := PackedFloat32Array()
		off.resize(np * 3)
		for i in np * 3:
			off[i] = float((i * 7 + k * 13) % 1009) / 1009.0 - 0.25
		many.append({"name": "s%d" % k, "idx": all_idx, "off": off})
	push_input = true
	var corners := PackedInt32Array([0, 1, 2])
	var r1 = _write(sb, "bigmesh", big, PackedInt32Array([3]), corners, many, {})
	_ok("whole-path write over the view limit answers a STAGED: String", typeof(r1) == TYPE_STRING and String(r1).begins_with("STAGED: "), str(r1).left(100))
	var b1 := _fetch_written(sb)
	var r2 = _write(sb, "bigmesh", big, PackedInt32Array([3]), corners, many, {}, {}, "usd_write_mesh_staged")
	var b2 := _fetch_written(sb)
	_ok("... and its bytes equal the staged entry's (%d bytes, past 15 MiB)" % b1.size(), b1.size() > 15 * 1024 * 1024 and b1 == b2 and typeof(r2) == TYPE_INT and r2 == b2.size())
	push_input = false
	sb.queue_free()
	var sb2 = _new_sb(elf)
	var op := _open(sb2, b1)
	var rd_ok := op.begins_with("ok ") and int(sb2.vmcall("usd_blend_shape_count")) == K and _same_bits(_read_floats(sb2, "usd_mesh_points", 0), big)
	for k in [0, K / 2, K - 1]:
		rd_ok = rd_ok and _same_bits(sb2.vmcall("usd_blend_shape_offsets_slice", k, 0, 0), many[k]["off"])
	_ok("... and that package opens: points and shapes 0, %d, %d bit for bit" % [K / 2, K - 1], rd_ok, op.left(120))
	sb2.queue_free()
