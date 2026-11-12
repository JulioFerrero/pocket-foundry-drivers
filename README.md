# Pocket Foundry drivers

The drivers [Pocket Foundry](https://github.com/JulioFerrero/pocket-foundry) downloads. The app itself only has the
phone's own driver. When it starts, and before a game, it reads this repository's catalog, picks the driver for the
phone's GPU and downloads it, or the CPU driver when none works.

Every driver here is built from source by this repository's CI, in the app's own build container, so it links
against what the app ships. Nobody uploads a file.

## The drivers

The app also has the phone's own driver, which isn't here. Each driver's JSON in `drivers/` has the rest.

| Driver | What it is | For | Made by | Tested on |
|---|---|---|---|---|
| Zink + Turnip | Vulkan, through Zink | Adreno 6xx and newer | Mesa, MIT | 15 phones, Adreno 618 to 840: 60 FPS on the vivo X200 Ultra, Galaxy S23 and S25 Ultra, AYN Thor, POCO F5 Pro |
| Freedreno | OpenGL | Adreno 6xx and newer | Mesa, MIT | Nothing Phone (1), POCO X5 Pro 5G (Adreno 642L) |
| PanVK-kbase 26.3.1 | Vulkan, through Zink | Mali-G615; other Bifrost and Valhall Malis can try it | zenithblue, MIT | realme 16 Pro, 60 FPS; Xiaomi 14T, 56-59 FPS |
| PanVK-kbase 0.1.0 RC3 | Vulkan, through Zink | Mali-G77, G78; other Valhall Malis can try it | zenithblue, MIT | Xiaomi 11T, 60 FPS; Galaxy S20, 37-45 FPS; Pixel 6; Poco M6 Pro 4G (G57); Galaxy Tab S10 Ultra (G720) |
| PanVK 1.2.9 | Vulkan, through Zink | Mali-G76, built for the Huawei P40; other Bifrost Malis can try it | soulexit, MIT | Huawei P30, 60 FPS; Huawei P40; Galaxy S10, 17-37 FPS; Redmi Note 9 and Redmi 12 (G52) |
| RADV for Xclipse 1.9.0 RC1 | Vulkan, through Zink | Samsung Xclipse 530 to 960 | JimVulkan, MIT | Galaxy S25 FE, 60 FPS; Galaxy A56, 54-59 FPS |
| CPU | OpenGL on the processor (llvmpipe) | Any GPU, when no other driver starts | Mesa, MIT | |
| MobileGlues 2.3.0, experimental | The phone's own OpenGL ES, through MobileGlues | Any GPU: chosen by hand, and the one a game falls back on before the CPU | MobileGL-Dev, LGPL-2.1 | vivo X200 Ultra (Adreno 830), 59 FPS; Redmi 12 (Mali-G52), 45-60 FPS; Galaxy XCover7 (Mali-G57), 55-60 FPS |

A testing driver is listed in the app only after a long press on the driver screen's title.

## Adding a driver

Open a pull request with one JSON in `drivers/`. CI builds it and checks that every library it links is in the
app's runtime. Once merged, it's released and joins the catalog.

| Field | |
|---|---|
| `name`, `about`, `madeBy`, `licence` | What the app shows |
| `kind` | `vulkan`: a Vulkan driver the game reaches through Zink. `opengl`: a whole Mesa OpenGL driver. `cpu`: a whole Mesa that draws on the processor, for any GPU; app versions before 0.8.17-beta.10 skip it. `gles`: MobileGlues, which turns the game's OpenGL into the phone's own OpenGL ES, for any GPU, built by `build/gles.sh` with the shim in `gles/`; app versions before 0.8.18-beta.2 skip it |
| `testing` | `true`: not for players yet. The app lists it only after a long press on the driver screen's title, and never picks it by itself |
| `version` | Shown instead of the Mesa version, for forks with their own |
| `builtFor` | The phone it was made for, if it was made for one |
| `for` | The GPUs it's for, as the app shows them: `Adreno 6xx and newer` |
| `gpus` | The same GPUs as Android names them, to match the phone's: `*Adreno (TM) 7*`, `*Mali-G76*`. Start and end with `*`: on some phones Android wraps the name, as in `ANGLE ((Samsung Xclipse 540) on Vulkan 1.3.279)` |
| `family` | GPUs it might work on: they see it under To try |
| `android` | `{"min": 12}`: the oldest Android it runs on |
| `priority` | Lower comes first when several fit a GPU |
| `env` | Set whenever it draws |
| `settings` | Checkboxes in the app: `{"id", "label", "about", "env"}` |
| `compression` | `cpu` where the game's texture compressor fails on it |
| `tested` | Phones it ran on: `phone`, `gpu`, `android`, the driver's `version`, `result`, `fps`, `app`, `date`, `link` |
| `source` | `{"git", "ref"}`: a repository and a commit or tag |
| `build` | `packages` to install, `prepare` steps the source needs, `mesa` (where Mesa's tree ends up), `patches` from `patches/`, Meson `options`, `licences` to ship. A `gles` driver has `cmake`, the folder of MobileGlues' CMake project, instead of `mesa` and `options` |

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
