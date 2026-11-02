#!/usr/bin/env -S godot --headless -s
# Streams the loose document in tests/loose into usd.elf one file at a time and prints what the guest reads,
# as lines starting "STREAM ". tests/run_stream_loose.exs checks them.
#
#   godot --headless --path <project> -s tests/stream_loose.gd -- <loose dir> [<elf>]
#
# Cases: whole (every file streamed), no_texture (the texture left out: the open must report it missing),
# chunked (the texture streamed in three pieces with usd_asset_push and usd_asset_end), then after
# usd_asset_clear a second open must find no asset.
extends SceneTree

var sb: Object


func _initialize() -> void:
	var args := OS.get_cmdline_user_args()
	if args.is_empty():
		print("FAIL: no loose dir")
		quit(2)
		return
	var dir: String = args[0]
	var elf := "res://usd.elf" if args.size() < 2 else args[1]
	sb = ClassDB.instantiate("Sandbox")
	sb.memory_max = 240
	sb.allocations_max = 1000000
	sb.references_max = 4096
	sb.execution_timeout = 400
	sb.program = load(elf)
	root.add_child(sb)

	var files := ["root.usda", "layers/Materials.usda", "textures/x.png"]
	_say("whole", _run(dir, files, false))
	_say("no_texture", _run(dir, ["root.usda", "layers/Materials.usda"], false))
	_say("chunked", _run(dir, files, true))
	_say("listing", str(sb.vmcall("usd_asset_list")).replace("\n", ","))
	sb.vmcall("usd_asset_clear")
	_say("after_clear", str(sb.vmcall("usd_open_asset", "root.usda")))
	quit(0)


func _say(label: String, text: String) -> void:
	print("STREAM %s %s" % [label, text])


func _run(dir: String, files: Array, chunk_texture: bool) -> String:
	sb.vmcall("usd_asset_clear")
	for f in files:
		var bytes := FileAccess.get_file_as_bytes(dir.path_join(f))
		if chunk_texture and f.ends_with(".png"):
			var third := bytes.size() / 3
			sb.vmcall("usd_asset_push", f, bytes.slice(0, third))
			sb.vmcall("usd_asset_push", f, bytes.slice(third, 2 * third))
			sb.vmcall("usd_asset_push", f, bytes.slice(2 * third))
			sb.vmcall("usd_asset_end", f)
		else:
			sb.vmcall("usd_asset", f, bytes)
	return str(sb.vmcall("usd_open_asset", "root.usda"))
