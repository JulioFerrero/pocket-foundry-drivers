# Pocket Foundry drivers

The GPU drivers [Pocket Foundry](https://github.com/JulioFerrero/pocket-foundry) downloads. The app itself only has
the phone's own driver and the CPU. When it starts, and before a game, it reads this repository's catalog, picks the
driver for the phone's GPU and downloads it.

Every driver here is built from source by this repository's CI, in the app's own build container, so it links
against what the app ships. Nobody uploads a file.

## Adding a driver

Open a pull request with one JSON in `drivers/`. CI builds it and checks that every library it links is in the
app's runtime. Once merged, it's released and joins the catalog.

| Field | |
|---|---|
| `name`, `about`, `madeBy`, `licence` | What the app shows |
| `kind` | `vulkan`: a Vulkan driver the game reaches through Zink. `opengl`: a whole Mesa OpenGL driver |
| `version` | Shown instead of the Mesa version, for forks with their own |
| `builtFor` | The phone it was made for, if it was made for one |
| `for` | The GPUs it's for, as the app shows them: `Adreno 6xx and newer` |
| `gpus` | The same GPUs as Android names them, to match the phone's: `Adreno (TM) 7??`, `Mali-G76*` |
| `family` | GPUs it might work on: they see it under To try |
| `android` | `{"min": 12}`: the oldest Android it runs on |
| `priority` | Lower comes first when several fit a GPU |
| `env` | Set whenever it draws |
| `settings` | Checkboxes in the app: `{"id", "label", "about", "env"}` |
| `compression` | `cpu` where the game's texture compressor fails on it |
| `tested` | Phones it ran on: `phone`, `gpu`, `android`, the driver's `version`, `result`, `fps`, `app`, `date`, `link` |
| `source` | `{"git", "ref"}`: a repository and a commit or tag |
| `build` | `packages` to install, `prepare` steps the source needs, `mesa` (where Mesa's tree ends up), `patches` from `patches/`, Meson `options`, `licences` to ship |

A change to `tested`, `about` or the other shown fields only updates the catalog. A change to `kind`, `source`,
`build` or the patches builds the driver again.

## Fixes

`fixes/` holds settings some drivers need on some phones, like the Adreno 740's on Android 16. The app applies them
by itself: `drivers`, `gpus`, `android` and the `env` to set.

## The catalog

`catalog.json` in the [catalog release](https://github.com/JulioFerrero/pocket-foundry-drivers/releases/tag/catalog),
made by CI from every JSON here and signed (`catalog.json.sig`, ECDSA P-256 with SHA-256). The app trusts only a
catalog signed with the key it was built with, and checks each download against the SHA-256 there.

The scripts here are GPL-3.0, like the app. Each driver keeps its own licence, which its package carries.
