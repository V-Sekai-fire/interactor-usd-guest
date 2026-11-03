# godot --headless --path <proj> -s write_pieces.gd -- <elf> <in dir> <out dir>
extends SceneTree

var sb: Object

func _initialize() -> void:
	var a := OS.get_cmdline_user_args()
	sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = 400
	sb.allocations_max = 1000000
	sb.references_max = 4096
	sb.execution_timeout = 2000
	sb.program = load(a[0])
	root.add_child(sb)
	var din: String = a[1]
	var dout: String = a[2]
	DirAccess.make_dir_recursive_absolute(dout)
	var man = JSON.parse_string(FileAccess.get_file_as_string(din.path_join("manifest.json")))
	for m in man:
		for with_mat in [true, false]:
			_one(din, dout, m, with_mat)
	quit(0)

func _one(din: String, dout: String, m: Dictionary, with_mat: bool) -> void:
	var n: String = m["name"]
	var np: int = m["np"]
	var nf: int = m["nf"]
	var p := FileAccess.get_file_as_bytes(din.path_join(n + ".p")).to_float32_array()
	var uv := FileAccess.get_file_as_bytes(din.path_join(n + ".uv")).to_float32_array()
	var f := FileAccess.get_file_as_bytes(din.path_join(n + ".f")).to_int32_array()
	var lab := FileAccess.get_file_as_bytes(din.path_join(n + ".l")).to_int32_array()
	var floats := PackedFloat32Array()
	floats.append_array(p)
	floats.append_array(uv)
	var ints := PackedInt32Array()
	for i in nf:
		ints.append(3)
	ints.append_array(f)
	ints.append_array(lab)
	var h := "%s\nnp=%d\nnuv=%d\nnrest=0\nnf=%d\nni=%d\nnlabel=%d\n" % [n, np, np * 2, nf, nf * 3, nf]
	if with_mat:
		h += "material=finch_blue\nbase_color=0.15,0.35,0.75\nroughness=0.6\nmetallic=0.1\nopacity=1.0\n"
	h += "author=Abcxyz51\nlicense=CC-BY-4.0\nlicense_url=https://creativecommons.org/licenses/by/4.0/\nauthor_url=https://sketchfab.com/Abcxyz51\n"
	var z = sb.vmcall("usd_write_mesh", floats, ints, h, "")
	if typeof(z) != TYPE_PACKED_BYTE_ARRAY:
		print("WRITEFAIL ", n, " ", z)
		return
	var path := dout.path_join(n + ("" if with_mat else ".nomat") + ".usdz")
	var fa := FileAccess.open(path, FileAccess.WRITE)
	fa.store_buffer(z)
	fa.close()
	# read back with the guest's own reader
	var r = sb.vmcall("usd_open", z)
	var info = sb.vmcall("usd_mesh_info", 0)
	var rb := {
		"file": path.get_file(), "open": str(r).substr(0, 60), "meshes": sb.vmcall("usd_mesh_count"),
		"materials": sb.vmcall("usd_material_count"), "layer_data": sb.vmcall("usd_layer_data"),
	}
	if typeof(info) == TYPE_DICTIONARY:
		rb["points"] = info["points"]
		rb["triangles"] = info["triangles"]
		rb["subsets"] = info["subsets"]
		rb["subset_count"] = info["subset_count"]
		rb["material"] = info["material"]
	else:
		rb["info"] = str(info)
	if rb["materials"] > 0:
		var md = sb.vmcall("usd_material", 0)
		rb["mat"] = str(md)
	print("RB ", JSON.stringify(rb))
	sb.vmcall("usd_close")
