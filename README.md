# interactor-usd-guest

OpenUSD compiled into a RISC-V sandbox guest that opens a stage from bytes, with the probe guest that first proved it could.

## What it is for

`usd.elf` opens a `.usdz` package the host hands over in memory, with no filesystem, and answers with packed arrays of its meshes, skeletons, skins, animations, materials, textures and curves; it also writes curves and skeletal clips back out. `usd_probe.elf` is the smaller guest that reads a stage from bytes and checksums its meshes.

## Building and running

`transport-meshing-pen` builds both guests from a goal-manifest checkout, where this repository finds the repositories it builds against as siblings. The build needs a static RISC-V build of the organisation's OpenUSD fork.

## Opening a loose document

A document that is a tree of files (a root layer, its sublayers and its textures) streams in one file at a time, under the path each file is referenced by, then the root is opened:

- `usd_asset(path, bytes)`: one file, up to the 16 MiB view.
- `usd_asset_push(path, chunk)` and `usd_asset_end(path)`: a file larger than that view, in pieces.
- `usd_open_asset(path)`: opens the document whose root layer is `path`.
- `usd_asset_list()` and `usd_asset_clear()`: the streamed paths, one per line, and drop them all.

A relative reference such as `../textures/x.png` in `layers/Materials.usda` resolves against the layer that holds it. A `.usdz` still opens through `usd_open`.

`elixir tests/run_stream_loose.exs <godot double editor> <project dir> <new usd.elf> [<old usd.elf>]` streams `tests/loose` and checks what the guest reads. The old ELF, which has no streaming calls, must fail the check.

## Licence

MIT. See [LICENSE](LICENSE).
