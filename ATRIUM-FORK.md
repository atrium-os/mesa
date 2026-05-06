# atrium-mesa — Atrium fork of Mesa

Soft fork of upstream Mesa for the Atrium platform. Additive-only
discipline so we can pull `upstream/main` quarterly with predictable
conflict resolution.

## Layout

```
atrium-mesa/
├── (entire upstream Mesa tree, untouched)
├── src/atrium/             ← all our code; subdir does not exist upstream
│   ├── vn_renderer_atrium.c   — venus backend over /dev/atrium-gpu0
│   ├── meson.build
│   └── README.md
├── .atrium-patches/        ← absolute-minimum upstream-file patches,
│                             one per file, rationale-headed, capped at
│                             ~10 patches. Apply via tools/atrium/apply-patches.sh.
└── ATRIUM-FORK.md          ← this file
```

## Drivers shipped

- **venus** (V5+) — paravirt Vulkan transport. Mesa's existing
  `src/virtio/vulkan/` driver, retargeted onto our `/dev/atrium-gpu0`
  cdev via `vn_renderer_atrium.c` (a sibling backend to upstream's
  `vn_renderer_virtgpu.c` / `vn_renderer_vtest.c`).

- **(future) radv / anv / nvk** (D5) — native vendor drivers. Same
  fork pattern: add a sibling `radv_atrium_winsys.c` at D5 time;
  upstream radv stays untouched.

## Build

Configure with `-Datrium=true` to enable the venus path. Toolchain is
upstream Mesa's standard meson build; the only addition is one
`subdir('src/atrium')` line in the top-level `meson.build`.

## Merge cadence and playbook

Pull upstream monthly:

    git fetch upstream                     # https://gitlab.freedesktop.org/mesa/mesa.git
    git checkout atrium/main
    git merge upstream/main
    ./tools/atrium/apply-patches.sh        # re-apply .atrium-patches/
    meson compile -C build venus
    ./tools/atrium/smoke.sh                # vkcube against venus

Expected conflict surface (in priority order):

1. `meson.build`, `meson_options.txt`            — append-only single-line additions; almost never conflict
2. `.atrium-patches/*.patch`                     — re-apply may need adjustment if upstream rearranged the patched file; fix the patch, not the upstream file
3. `src/atrium/*`                                — never conflicts (additive)

If conflicts appear OUTSIDE these categories, escalate. We've either
modified an upstream file (against discipline) or upstream has
restructured in a way our seam doesn't survive.

If `.atrium-patches/` grows past 10, freeze new patches. Either upstream
them, or rethink whether we're using Mesa's seams correctly.
