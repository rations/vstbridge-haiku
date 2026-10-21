# vstbridge (Haiku)

> **Users:** install the whole stack from the
> [JackDAW release](https://github.com/rations/jackDAW-haiku/releases) (`sh install.sh`).
> This repository is the source of the `vstbridge` package.

Run **Windows VST2 and VST3 plug-ins** in native Haiku hosts such as
[JackDAW](https://github.com/rations/jackDAW-haiku). vstbridge is a Haiku port of
[yabridge](https://github.com/robbert-vdh/yabridge) by Robbert van der Helm. Each Windows
plug-in gets a small native stub add-on. When a host loads the stub, it starts the plug-in in
a Wine host process and passes audio, MIDI, parameters and the editor across.

## Use

1. Install the Windows plug-ins with Wine, into the default prefix (`~/.wine`).
2. Open **VSTBridge** (Deskbar → Applications), add the folder that holds the plug-ins (for
   example `~/.wine/drive_c/Program Files/Common Files/VST3`), and press Sync.
3. Rescan plug-ins in the host. The stubs are in `~/config/non-packaged/add-ons/media/VST3/vstbridge`
   and `.../media/vstplugins/vstbridge`.

`vstbridgectl` does the same from Terminal: `vstbridgectl add <folder>`, `vstbridgectl sync`,
`vstbridgectl status`.

To change a plug-in's settings, open VSTBridge's **Plugins** tab and double-click the plug-in.
Among other things you can give it its own Wine environment variables, for example
`WINEDLLOVERRIDES=d3d11,dxgi=n,b`, so plug-ins with different needs can share one prefix. The
settings go into the plug-in's section of the `vstbridge.toml` next to the stubs and apply the
next time the plug-in is loaded.

The plug-in's editor appears inside the host's window. Set the prefix's Wine graphics driver to
`null` so that it does not also open its own window:

```sh
wine reg add 'HKCU\Software\Wine\Drivers' /v Graphics /d null /f
```

Editors drawn with Direct3D need the Wayland driver (`/d wayland`) instead, because the software
OpenGL backend is part of it.

## Requirements

- **The stack's Wine 11.19** (the `wine` package from
  [haiku-wine-port-jackdaw](https://github.com/rations/haiku-wine-port-jackdaw)). The Wine host
  is a C++ Winelib program, and stock Haiku Wine does not run its static initialisers. The
  patches also cover iLok installers and PACE-protected plug-ins.
- For 32-bit installers and iLok/PACE-protected plug-ins, the stack's system package (see the
  JackDAW release).
- VST2 and VST3 only: the CLAP bridge depends on yabridge's X11 editor code, which this port
  replaces.

## Troubleshooting

- `VSTBRIDGE_DEBUG_FILE=<file>` and `VSTBRIDGE_DEBUG_LEVEL=1` (or `2` for every audio call),
  set when starting the host, log the bridge's messages and the Wine host's output.
- `WINELOADER=<path to wine>` makes the bridge use a different Wine build, for example one from
  a build tree.
- Per-plug-in options go in a `vstbridge.toml` next to the stubs, as in yabridge, or can be set
  in VSTBridge's Plugins tab. Besides yabridge's options there is `environment`, a list of
  `KEY=VALUE` variables for that plug-in's Wine host:

  ```toml
  ["Amped - Roots.vst3"]
  environment = ["WINEDLLOVERRIDES=d3d11,dxgi=n,b"]
  ```

  The first section whose pattern matches is used, and sections don't merge. Plug-ins in the
  same group share one Wine host, and with it the environment of the first one loaded.
  `WINELOADER` can't be set this way, because the bridge reads it before starting the host.

## Building

On Haiku x86_64 with the stack's Wine installed:

```sh
pkgman install -y meson ninja gcc make pkgconfig
packaging/make-hpkg.sh
```

It builds the bridge libraries and the Wine host with meson, both front-ends with
`make -C tools/vstbridgectl`, and stages them into one `.hpkg`. `docs/architecture.md`
describes how the bridge works.

## License

GPL v3, like yabridge. See `COPYING` and `CHANGELOG.md` for upstream's history.
