extends SceneTree
# usd_write_mesh with a material: one mesh with a UsdPreviewSurface material whose colour and normal maps are stored in
# the .usdz. Written with the new usd.elf, read back with its own readers, and compared with the textures that went in.
# Controls (each must come out the way the line says): a write with no material is byte-identical to the previous ELF's;
# a missing texture, a texture without uvs, an unknown mat_ key and mat_ keys without material= are each refused; and a
# planted one-byte change to a texture is caught by the comparison.
#
# usage: godot --headless --path <proj> -s res://run_write_materials.gd -- <new usd.elf> <previous usd.elf> <out.txt>
# (the project needs the Sandbox class, see references/stage-guest-build-and-check.md)

var out: FileAccess
var fails := 0


func _say(ok: bool, what: String) -> void:
	out.store_line(("PASS  " if ok else "FAIL  ") + what)
	out.flush()
	if not ok:
		fails += 1


func _sandbox(elf: PackedByteArray):
	# The limits every stage guest wants (references_max, allocations_max): the defaults are too small for geometry.
	var sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = 1024
	sb.references_max = 4096
	sb.allocations_max = 1000000
	sb.execution_timeout = 600000
	root.add_child(sb)
	sb.load_buffer(elf)
	sb.vmcall("usd_init")
	return sb


func _png(color: Color, size: int) -> PackedByteArray:
	var img := Image.create(size, size, false, Image.FORMAT_RGBA8)
	img.fill(color)
	return img.save_png_to_buffer()


# A two-triangle quad, written with uvs so a texture has something to sample.
func _quad() -> Array:
	var pts := PackedFloat32Array([0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0])
	var uvs := PackedFloat32Array([0, 0, 1, 0, 1, 1, 0, 1])
	var ints := PackedInt32Array([3, 3, 0, 1, 2, 0, 2, 3])
	var floats := PackedFloat32Array()
	floats.append_array(pts)
	floats.append_array(uvs)
	return [floats, ints]


func _header(extra: String, with_uv: bool = true) -> String:
	return "quad\nnp=4\nnuv=%d\nnrest=0\nnf=2\nni=6\n%s" % [8 if with_uv else 0, extra]


# Every zip member of a package: name, whether its data starts on a 64-byte boundary of the file.
func _members(z: PackedByteArray) -> Array:
	var res: Array = []
	var p := 0
	while p + 30 <= z.size() and z.decode_u32(p) == 0x04034b50:
		var csize := z.decode_u32(p + 18)
		var nlen := z.decode_u16(p + 26)
		var xlen := z.decode_u16(p + 28)
		var name := z.slice(p + 30, p + 30 + nlen).get_string_from_utf8()
		var data_at := p + 30 + nlen + xlen
		res.append([name, data_at % 64 == 0, csize])
		p = data_at + csize
	return res


func _open(sb, pkg: PackedByteArray) -> void:
	sb.vmcall("usd_init")
	var at := 0
	while at < pkg.size():
		sb.vmcall("usd_push", pkg.slice(at, min(at + 524288, pkg.size())))
		at += 524288
	sb.vmcall("usd_open_staged")


func _init() -> void:
	var a := OS.get_cmdline_user_args()
	out = FileAccess.open(a[2], FileAccess.WRITE)
	var sb = _sandbox(FileAccess.get_file_as_bytes(a[0]))
	var old = _sandbox(FileAccess.get_file_as_bytes(a[1]))
	var q := _quad()
	var diffuse := _png(Color(0.8, 0.2, 0.1), 8)
	var normal := _png(Color(0.5, 0.5, 1.0), 4)

	# 1. The round trip: material with both maps and two constants.
	sb.vmcall("usd_write_texture", "textures/diffuse.png", diffuse)
	sb.vmcall("usd_write_texture", "textures/normal.png", normal)
	var pkg = sb.vmcall("usd_write_mesh", q[0], q[1],
			_header("material=SkinMat\nmat_diffuse_tex=textures/diffuse.png\nmat_normal_tex=textures/normal.png\nmat_metallic=0.55\nmat_roughness=0.4"), "")
	_say(typeof(pkg) == TYPE_PACKED_BYTE_ARRAY, "write with a material returns a package (got %s)" % str(pkg).left(120))
	if typeof(pkg) == TYPE_PACKED_BYTE_ARRAY:
		var members := _members(pkg)
		var names: Array = []
		var aligned := true
		for m in members:
			names.append(m[0])
			aligned = aligned and m[1]
		_say(names == ["quad.usda", "textures/diffuse.png", "textures/normal.png"], "package members %s (layer first, then the textures)" % str(names))
		_say(aligned, "every member's data starts on a 64-byte boundary")
		_open(sb, pkg)
		_say(int(sb.vmcall("usd_material_count")) == 1, "one material in the document (got %s)" % str(sb.vmcall("usd_material_count")))
		_say(int(sb.vmcall("usd_texture_count")) == 2, "two textures in the document (got %s)" % str(sb.vmcall("usd_texture_count")))
		var mat = sb.vmcall("usd_material", 0)
		out.store_line("material: " + str(mat).left(600))
		out.flush()
		var found := {}
		for i in int(sb.vmcall("usd_texture_count")):
			var t = sb.vmcall("usd_texture", i)
			if typeof(t) == TYPE_PACKED_BYTE_ARRAY:
				found[t.size()] = t
		_say(found.has(diffuse.size()) and found[diffuse.size()] == diffuse, "the diffuse texture reads back byte for byte")
		_say(found.has(normal.size()) and found[normal.size()] == normal, "the normal texture reads back byte for byte")
		var info = sb.vmcall("usd_mesh_info", 0)
		_say(typeof(info) == TYPE_DICTIONARY and info.get("has_uvs", false), "the mesh keeps its uvs")
		# usd_mesh_info answers the index of the bound material (-1 when none), here the document's only one.
		_say(typeof(info) == TYPE_DICTIONARY and int(info.get("material", -1)) == 0, "the mesh is bound to the material (material index = %s)" % str(info.get("material", "")))
		# The planted control: a texture that differs by one byte is not the one that went in.
		var planted := diffuse.duplicate()
		planted[planted.size() / 2] = planted[planted.size() / 2] ^ 1
		_say(not (found.has(diffuse.size()) and found[diffuse.size()] == planted), "control: a texture changed by one byte is told apart")

	# 2. No material: the same bytes as the previous ELF writes.
	var plain_new = sb.vmcall("usd_write_mesh", q[0], q[1], _header(""), "")
	var plain_old = old.vmcall("usd_write_mesh", q[0], q[1], _header(""), "")
	_say(typeof(plain_new) == TYPE_PACKED_BYTE_ARRAY and plain_new == plain_old, "a write with no material is byte-identical to the previous ELF's")
	if typeof(plain_new) == TYPE_PACKED_BYTE_ARRAY:
		_open(sb, plain_new)
		var plain_info = sb.vmcall("usd_mesh_info", 0)
		_say(int(sb.vmcall("usd_material_count")) == 0 and typeof(plain_info) == TYPE_DICTIONARY and int(plain_info.get("material", 0)) == -1, "control: a write with no material reads back with none bound")

	# 3. Controls that must be refused.
	var r1 = sb.vmcall("usd_write_mesh", q[0], q[1], _header("material=M\nmat_diffuse_tex=textures/nothere.png"), "")
	_say(typeof(r1) == TYPE_STRING and str(r1).begins_with("ERR:"), "control: a texture that was never handed over is refused (%s)" % str(r1).left(90))
	sb.vmcall("usd_write_texture", "textures/diffuse.png", diffuse)
	var nouv = _quad()
	var f_nouv := PackedFloat32Array(nouv[0].slice(0, 12))
	var r2 = sb.vmcall("usd_write_mesh", f_nouv, nouv[1], _header("material=M\nmat_diffuse_tex=textures/diffuse.png", false), "")
	_say(typeof(r2) == TYPE_STRING and str(r2).begins_with("ERR:"), "control: a texture on a mesh without uvs is refused (%s)" % str(r2).left(90))
	var r3 = sb.vmcall("usd_write_mesh", q[0], q[1], _header("material=M\nmat_shininess=3"), "")
	_say(typeof(r3) == TYPE_STRING and (str(r3).begins_with("ERR:") or str(r3).begins_with("FAIL:")), "control: an unknown mat_ key is refused (%s)" % str(r3).left(90))
	var r4 = sb.vmcall("usd_write_mesh", q[0], q[1], _header("mat_metallic=0.5"), "")
	_say(typeof(r4) == TYPE_STRING and (str(r4).begins_with("ERR:") or str(r4).begins_with("FAIL:")), "control: mat_ keys without material= are refused (%s)" % str(r4).left(90))
	var r5 = sb.vmcall("usd_write_mesh", q[0], q[1], _header("material=M\nmat_diffuse_tex=../escape.png"), "")
	_say(typeof(r5) == TYPE_STRING and str(r5).begins_with("ERR:"), "control: a texture name with .. is refused (%s)" % str(r5).left(90))

	out.store_line("RESULT: %s (%d failed)" % ["PASS" if fails == 0 else "FAIL", fails])
	out.close()
	quit(1 if fails > 0 else 0)
