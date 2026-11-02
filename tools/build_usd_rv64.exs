# Build OpenUSD 26.05 (the org's fork, branch riscv64-sandbox) and oneTBB 2021.12.0 static for
# riscv64, then usd.elf against them. Recreates the gates/0g-openusd build, which was never
# committed and was lost with the desk it ran on.
#
#   elixir tools/build_usd_rv64.exs <weft root> <out dir>
#
# <weft root> is a manifest-layout checkout holding 5-repository/riscv64-sysroot,
# 2-contract/guest-runtime, 2-contract/guest-common and 3-interactor/usd-guest. <out dir> gets
# src/ (OpenUSD and oneTBB checkouts), inst/ (oneTBB installed), usd/ (the OpenUSD build tree
# usd.cmake reads) and elf/ (the guest build). Needs clang, lld, cmake, ninja and git on PATH.
# J caps the parallelism of the builds.

defmodule BuildUsd do
  @usd_ref "7b709ed624bed1a25dd20e4a1875595bc77ac7c3"
  @tbb_ref "v2021.12.0"

  def main([weft, out]) do
    weft = Path.expand(weft)
    File.mkdir_p!(out)
    out = Path.expand(out)
    tc = Path.join([weft, "5-repository", "riscv64-sysroot", "toolchain.cmake"])
    j = System.get_env("J") || Integer.to_string(System.schedulers_online())

    # Through CFLAGS/CXXFLAGS, not -DCMAKE_CXX_FLAGS: that replaces the toolchain file's
    # CMAKE_CXX_FLAGS_INIT, whose -isystem paths are where <type_traits> comes from.
    # The prefix maps keep the build machine's paths out of the ELF (OpenUSD's TF_ messages
    # name their source files).
    map = "-ffile-prefix-map=#{out}/src/usd/=openusd/ -ffile-prefix-map=#{out}/src/oneTBB/=onetbb/"

    env = [
      {"CFLAGS", "-march=rv64gc -mabi=lp64d #{map}"},
      {"CXXFLAGS", "-march=rv64gc -mabi=lp64d -DTBB_USE_EXCEPTIONS=1 #{map}"}
    ]

    File.mkdir_p!(Path.join(out, "src"))
    fetch("https://github.com/uxlfoundation/oneTBB.git", @tbb_ref, Path.join([out, "src", "oneTBB"]))
    fetch("https://github.com/V-Sekai-fire/repository-openusd.git", @usd_ref, Path.join([out, "src", "usd"]))

    unless File.exists?(Path.join([out, "inst", "lib", "libtbb.a"])) do
      run("cmake", [
        "-S", Path.join([out, "src", "oneTBB"]), "-B", Path.join(out, "tbb"), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=#{tc}", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_INSTALL_PREFIX=#{Path.join(out, "inst")}", "-DBUILD_SHARED_LIBS=OFF",
        "-DTBB_TEST=OFF", "-DTBB_EXAMPLES=OFF", "-DTBB_STRICT=OFF", "-DTBBMALLOC_BUILD=OFF"
      ], env)

      run("ninja", ["-C", Path.join(out, "tbb"), "-j#{j}", "install"], env)
    end

    unless File.exists?(Path.join([out, "usd", "pxr", "usd", "usdSkel", "libusd_usdSkel.a"])) do
      run("cmake", [
        "-S", Path.join([out, "src", "usd"]), "-B", Path.join(out, "usd"), "-G", "Ninja",
        "-DCMAKE_TOOLCHAIN_FILE=#{tc}", "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF", "-DPXR_BUILD_MONOLITHIC=OFF",
        "-DPXR_ENABLE_PYTHON_SUPPORT=OFF", "-DPXR_BUILD_IMAGING=OFF", "-DPXR_BUILD_USD_IMAGING=OFF",
        "-DPXR_BUILD_USDVIEW=OFF", "-DPXR_BUILD_TESTS=OFF", "-DPXR_BUILD_EXAMPLES=OFF",
        "-DPXR_BUILD_TUTORIALS=OFF", "-DPXR_BUILD_USD_TOOLS=OFF", "-DPXR_BUILD_HTML_DOCUMENTATION=OFF",
        "-DPXR_ENABLE_MATERIALX_SUPPORT=OFF", "-DPXR_ENABLE_OPENVDB_SUPPORT=OFF",
        "-DPXR_ENABLE_PTEX_SUPPORT=OFF", "-DPXR_BUILD_ALEMBIC_PLUGIN=OFF", "-DPXR_BUILD_DRACO_PLUGIN=OFF",
        "-DPXR_ENABLE_OSL_SUPPORT=OFF", "-DPXR_PREFER_SAFETY_OVER_SPEED=ON",
        "-DTBB_ROOT=#{Path.join(out, "inst")}",
        "-DTBB_DIR=#{Path.join([out, "inst", "lib", "cmake", "TBB"])}"
      ], env)

      run("ninja", ["-C", Path.join(out, "usd"), "-j#{j}"], env)
    end

    # usd.cmake reads the generated headers from <build>/include and oneTBB's from inst/include.
    unless File.dir?(Path.join([out, "usd", "include", "pxr"])) do
      IO.puts(:stderr, "error: no #{out}/usd/include/pxr")
      System.halt(1)
    end

    run("cmake", [
      "-S", Path.join([weft, "3-interactor", "usd-guest", "tools", "standalone"]),
      "-B", Path.join(out, "elf"), "-G", "Ninja",
      "-DCMAKE_TOOLCHAIN_FILE=#{tc}", "-DCMAKE_BUILD_TYPE=Release", "-DWEFT_ROOT=#{weft}",
      "-DUSD_RV64_DIR=#{out}", "-DUSD_RV64_SRC=#{Path.join([out, "src", "usd"])}",
      "-DDOUBLE_PRECISION=ON", "-DSANDBOX_RISCV_EXT_V=OFF"
    ], env)

    run("ninja", ["-C", Path.join(out, "elf"), "-j#{j}", "usd"], env)

    elf = Path.join([weft, "3-interactor", "usd-guest", "tools", "standalone", "usd.elf"])
    sha = :crypto.hash(:sha256, File.read!(elf)) |> Base.encode16(case: :lower)
    IO.puts("#{sha}  #{elf}")
  end

  def main(_) do
    IO.puts(:stderr, "usage: elixir tools/build_usd_rv64.exs <weft root> <out dir>")
    System.halt(2)
  end

  # url ref dir: a shallow checkout of one commit, without cloning history.
  defp fetch(url, ref, dir) do
    File.mkdir_p!(dir)
    unless File.dir?(Path.join(dir, ".git")), do: run("git", ["init", "-q", dir], [])
    run("git", ["-C", dir, "fetch", "-q", "--depth", "1", url, ref], [])
    run("git", ["-C", dir, "checkout", "-q", "FETCH_HEAD"], [])
    {rev, 0} = System.cmd("git", ["-C", dir, "rev-parse", "HEAD"])
    IO.puts("#{dir} at #{String.trim(rev)}")
  end

  defp run(cmd, args, env) do
    exe = System.find_executable(cmd) || (IO.puts(:stderr, "error: #{cmd} is not on PATH"); System.halt(1))
    {_, status} = System.cmd(exe, args, env: env, into: IO.stream(:stdio, :line), stderr_to_stdout: true)

    if status != 0 do
      IO.puts(:stderr, "error: #{cmd} exited #{status}")
      System.halt(status)
    end
  end
end

BuildUsd.main(System.argv())
