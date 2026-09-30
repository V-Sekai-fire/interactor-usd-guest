# interactor-usd-guest

OpenUSD from bytes as a godot-sandbox guest: usd.elf and its Gate 0G probe.

Split out of `interactor-dress-on` at `310b52e` with its history (`git subtree`). It sits at `3-interactor/usd-guest` in the goal manifest (`contract-manifest-taskweft`), and finds the repositories it builds against as sibling checkouts at their manifest paths. `transport-meshing-pen` builds the guest ELFs (`build.sh`, `tools/build.exs`).
