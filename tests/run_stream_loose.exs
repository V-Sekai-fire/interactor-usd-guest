# The streaming check: tests/loose (a root layer, a sublayer and a texture) goes into usd.elf one file at a time.
#
#   elixir tests/run_stream_loose.exs <godot double editor> <project dir> <new usd.elf> [<old usd.elf>]
#
# <project dir> holds the godot_sandbox addon (tests/make_project.exs makes one). LOOSE_DIR names another
# fixture directory, for the controls. Passes when the new ELF reads the document whole and chunked,
# reports the left-out texture as missing, lists the streamed paths, and refuses to open after
# usd_asset_clear. The old ELF, which has no streaming calls, must fail the same checks, and so must a
# planted expectation.

defmodule StreamLoose do
  @timeout 120_000

  def main([godot, project, new_elf | rest]) do
    old_elf = List.first(rest)
    here = Path.dirname(Path.expand(__ENV__.file))
    loose = System.get_env("LOOSE_DIR") || Path.join(here, "loose")

    File.mkdir_p!(Path.join(project, "tests"))
    File.cp!(Path.join(here, "stream_loose.gd"), Path.join([project, "tests", "stream_loose.gd"]))
    File.cp!(new_elf, Path.join(project, "stream_new.elf"))
    if old_elf, do: File.cp!(old_elf, Path.join(project, "stream_old.elf"))
    exec(godot, ["--headless", "--path", project, "--import"])

    new_lines = dump(godot, project, loose, "stream_new")
    Enum.each(new_lines, &IO.puts/1)

    IO.puts("== new ELF")
    new_ok = check(new_lines)

    old_ok =
      if old_elf do
        if check(dump(godot, project, loose, "stream_old"), false) do
          IO.puts("CONTROL-BROKEN: the old ELF passed")
          false
        else
          IO.puts("control old ELF: fails, as it must")
          true
        end
      else
        true
      end

    # Control: a planted expectation the new ELF cannot meet.
    planted_ok =
      if Enum.any?(new_lines, &(&1 =~ ~r/^STREAM whole .*textures=2/)) do
        IO.puts("CONTROL-BROKEN: planted textures=2 matched")
        false
      else
        IO.puts("control planted textures=2: no match, as it must")
        true
      end

    pass = new_ok and old_ok and planted_ok
    IO.puts("RESULT: #{if pass, do: "PASS", else: "FAIL"}")
    System.halt(if pass, do: 0, else: 1)
  end

  def main(_) do
    IO.puts(:stderr, "usage: elixir tests/run_stream_loose.exs <godot> <project dir> <new usd.elf> [<old usd.elf>]")
    System.halt(2)
  end

  defp dump(godot, project, loose, name) do
    args = ["--headless", "--path", project, "-s", "tests/stream_loose.gd", "--", loose, "res://#{name}.elf"]
    output = exec(godot, args)
    File.write!(Path.join(project, "#{name}.txt"), output)
    output |> String.split("\n") |> Enum.filter(&String.starts_with?(&1, "STREAM"))
  end

  # Every expectation, one regex per labelled line. Returns true when all hold.
  defp check(lines, verbose \\ true) do
    expectations = [
      {"whole", ~r/^STREAM whole ok .*meshes=1 .*materials=1 textures=1 .*textures_missing=0/},
      {"chunked", ~r/^STREAM chunked ok .*meshes=1 .*materials=1 textures=1 .*textures_missing=0/},
      {"no_texture", ~r/^STREAM no_texture .*textures_missing=1 .*missing=.*x\.png/},
      {"listing", ~r{^STREAM listing root\.usda,layers/Materials\.usda,textures/x\.png}},
      {"after_clear", ~r/^STREAM after_clear ERR: no asset/}
    ]

    Enum.reduce(expectations, true, fn {label, regex}, ok ->
      if Enum.any?(lines, &Regex.match?(regex, &1)) do
        ok
      else
        if verbose, do: IO.puts("  missing: #{label} ~ #{inspect(regex)}")
        false
      end
    end)
  end

  # Runs a program to its end or the timeout (a SceneTree script with an error never quits), returns its output.
  defp exec(exe, args) do
    path = System.find_executable(exe) || exe
    port = Port.open({:spawn_executable, path}, [:binary, :exit_status, :stderr_to_stdout, args: args])
    {:os_pid, pid} = Port.info(port, :os_pid)
    collect(port, pid, "", System.monotonic_time(:millisecond) + @timeout)
  end

  defp collect(port, pid, acc, deadline) do
    left = max(deadline - System.monotonic_time(:millisecond), 0)

    receive do
      {^port, {:data, data}} -> collect(port, pid, acc <> data, deadline)
      {^port, {:exit_status, _}} -> acc
    after
      left ->
        System.cmd("kill", ["-9", Integer.to_string(pid)])
        acc <> "\nTIMEOUT\n"
    end
  end
end

StreamLoose.main(System.argv())
