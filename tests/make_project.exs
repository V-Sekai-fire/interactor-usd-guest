# A Godot project holding the godot_sandbox addon, for the checks that load usd.elf on Linux.
#
#   elixir tests/make_project.exs <dir holding libgodot_riscv.linux.template_release.double.x86_64.so> <project dir>

[lib_dir, project] =
  case System.argv() do
    [a, b] ->
      [a, b]

    _ ->
      IO.puts(:stderr, "usage: elixir tests/make_project.exs <dir with the addon library> <project dir>")
      System.halt(2)
  end

library = "libgodot_riscv.linux.template_release.double.x86_64.so"
bin = Path.join([project, "addons", "godot_sandbox", "bin"])
File.rm_rf!(project)
File.mkdir_p!(bin)
File.write!(Path.join(project, "project.godot"), "config_version=5\n[application]\nconfig/name=\"usd-guest-check\"\n")
File.cp!(Path.join(lib_dir, library), Path.join(bin, library))

File.write!(Path.join(bin, "godot-riscv.gdextension"), """
[configuration]

entry_symbol = "riscv_library_init"
compatibility_minimum = "4.1"

[libraries]

linux.debug.double.x86_64 = "res://addons/godot_sandbox/bin/#{library}"
linux.release.double.x86_64 = "res://addons/godot_sandbox/bin/#{library}"
""")

IO.puts("project at #{project}")
