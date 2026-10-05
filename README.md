# interactor-usd-guest

OpenUSD compiled into a RISC-V sandbox guest that opens a stage from bytes, with the probe guest that first proved it could.

## What it is for

`usd.elf` opens a `.usdz` package the host hands over in memory, with no filesystem, and answers with packed arrays of its meshes, skeletons, skins, animations, materials, textures and curves; it also writes curves and skeletal clips back out. `usd_probe.elf` is the smaller guest that reads a stage from bytes and checksums its meshes.

## Building and running

`transport-meshing-pen` builds both guests from a goal-manifest checkout, where this repository finds the repositories it builds against as siblings. The build needs a static RISC-V build of the organisation's OpenUSD fork.

## Licence

Not stated. The repository has no LICENSE file and its sources carry no licence header.
