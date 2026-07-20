# As with the libraries, we cannot call `executable()` here since we would like
# to keep all relevant files in the root of the build directory, and Meson
# doesn't have a way to customize that yet.

if is_64bit_system
  host_64bit_deps = [
    configuration_dep,

    asio_dep,
    bitsery_dep,
    function2_dep,
    ghc_filesystem_dep,
    rt_dep,
    tomlplusplus_dep,
    wine_ole32_dep,
    wine_threads_dep,
    xcb_64bit_dep,
  ]
  if host_machine.system() == 'haiku'
    host_64bit_deps += [wine_win32u_dep]
  endif
  if with_clap
    host_64bit_deps += [clap_dep]
  endif
  if with_vst3
    host_64bit_deps += [
      vst3_sdk_hosting_wine_64bit_dep,
      wine_shell32_dep,
      wine_uuid_dep,
    ]
  endif
endif

if with_bitbridge
  message('Bitbridge enabled, configuring a 32-bit host application')

  host_32bit_deps = [
    configuration_dep,

    asio_dep,
    ghc_filesystem_dep,
    bitsery_dep,
    function2_dep,
    rt_dep,
    tomlplusplus_dep,
    wine_ole32_dep,
    wine_threads_dep,
    xcb_32bit_dep,
  ]
  if with_clap
    host_32bit_deps += [clap_dep]
  endif
  if with_vst3
    host_32bit_deps += [
      vst3_sdk_hosting_wine_32bit_dep,
      wine_shell32_dep,
      wine_uuid_dep,
    ]
  endif
endif

host_sources = files(
  '../common/communication/vst2.cpp',
  '../common/serialization/vst2.cpp',
  '../common/configuration.cpp',
  '../common/logging/common.cpp',
  '../common/logging/vst2.cpp',
  '../common/audio-shm.cpp',
  '../common/plugins.cpp',
  '../common/process.cpp',
  '../common/utils.cpp',
  '../include/llvm/small-vector.cpp',
  'bridges/common.cpp',
  'bridges/group.cpp',
  'bridges/vst2.cpp',
  'host.cpp',
  'utils.cpp',
)

if is_haiku
  # `editor.cpp` is X11/XCB throughout and `xdnd-proxy.cpp` implements the X11
  # drag-and-drop protocol, neither of which exists on Haiku. They are dropped
  # from the build rather than deleted, so upstream changes still apply to them.
  # `editor-haiku.cpp` provides the same interface with no embedding; see the
  # comments at the top of `editor-haiku.h` for why embedding is impossible.
  host_sources += files(
    '../common/editor-shm.cpp',
    'editor-capture.cpp',
    'editor-haiku.cpp',
  )

  # `editor-input.c` is C rather than C++ because it needs Wine's internal
  # `ntuser.h`, which does not compile as C++. See the comments at the top of
  # `editor-input.h`.
  #
  # It is built as its own library rather than added to `host_sources` because
  # Meson puts the executable's `cpp_args` on every source in the target,
  # C ones included, and those carry `-include cstdint` and
  # `-fvisibility-inlines-hidden` -- neither of which a C compile accepts.
  # Giving this one file its own target keeps the C++ flags off it.
  if is_64bit_system
    editor_input_lib = static_library(
      'vstbridge-editor-input',
      files('editor-input.c'),
      native : false,
      # The include and library paths come from the cross file's `c_args`; only
      # the architecture has to be repeated here, as it is for the C++ sources.
      #
      # `__WINESRC__` is what Wine's own drivers compile under, and the types
      # `ntuser.h` refers to -- `SCROLL_TRACKING_INFO` and
      # `NONCLIENT_BUTTON_TYPE` -- are declared behind it in `winuser.h`.
      # Without it those fields have incomplete types. It is deliberately not
      # set for the rest of the build, which is another reason this file is
      # compiled on its own.
      c_args : ['-m64', '-D__WINESRC__'],
    )

    host_64bit_deps += [
      declare_dependency(link_with : editor_input_lib),
    ]
  endif
else
  host_sources += files(
    'editor.cpp',
    'xdnd-proxy.cpp',
  )
endif

if with_clap
  host_sources += files(
    '../common/logging/clap.cpp',
    '../common/serialization/clap/ext/audio-ports.cpp',
    '../common/serialization/clap/ext/audio-ports-config.cpp',
    '../common/serialization/clap/ext/note-name.cpp',
    '../common/serialization/clap/ext/note-ports.cpp',
    '../common/serialization/clap/ext/params.cpp',
    '../common/serialization/clap/events.cpp',
    '../common/serialization/clap/host.cpp',
    '../common/serialization/clap/plugin.cpp',
    '../common/serialization/clap/process.cpp',
    '../common/serialization/clap/stream.cpp',
    'bridges/clap-impls/host-proxy.cpp',
    'bridges/clap.cpp',
  )
endif

if with_vst3
  host_sources += files(
    '../common/logging/vst3.cpp',
    '../common/serialization/vst3/component-handler/component-handler.cpp',
    '../common/serialization/vst3/component-handler/component-handler-2.cpp',
    '../common/serialization/vst3/component-handler/component-handler-3.cpp',
    '../common/serialization/vst3/component-handler/component-handler-bus-activation.cpp',
    '../common/serialization/vst3/component-handler/progress.cpp',
    '../common/serialization/vst3/component-handler/unit-handler.cpp',
    '../common/serialization/vst3/component-handler/unit-handler-2.cpp',
    '../common/serialization/vst3/context-menu/context-menu.cpp',
    '../common/serialization/vst3/host-context/host-application.cpp',
    '../common/serialization/vst3/host-context/plug-interface-support.cpp',
    '../common/serialization/vst3/plug-view/parameter-finder.cpp',
    '../common/serialization/vst3/plug-view/plug-view.cpp',
    '../common/serialization/vst3/plug-view/plug-view-content-scale-support.cpp',
    '../common/serialization/vst3/plug-frame/plug-frame.cpp',
    '../common/serialization/vst3/plugin/audio-presentation-latency.cpp',
    '../common/serialization/vst3/plugin/audio-processor.cpp',
    '../common/serialization/vst3/plugin/automation-state.cpp',
    '../common/serialization/vst3/plugin/component.cpp',
    '../common/serialization/vst3/plugin/connection-point.cpp',
    '../common/serialization/vst3/plugin/edit-controller.cpp',
    '../common/serialization/vst3/plugin/edit-controller-2.cpp',
    '../common/serialization/vst3/plugin/edit-controller-host-editing.cpp',
    '../common/serialization/vst3/plugin/info-listener.cpp',
    '../common/serialization/vst3/plugin/keyswitch-controller.cpp',
    '../common/serialization/vst3/plugin/midi-learn.cpp',
    '../common/serialization/vst3/plugin/midi-mapping.cpp',
    '../common/serialization/vst3/plugin/note-expression-controller.cpp',
    '../common/serialization/vst3/plugin/note-expression-physical-ui-mapping.cpp',
    '../common/serialization/vst3/plugin/parameter-function-name.cpp',
    '../common/serialization/vst3/plugin/plugin-base.cpp',
    '../common/serialization/vst3/plugin/prefetchable-support.cpp',
    '../common/serialization/vst3/plugin/process-context-requirements.cpp',
    '../common/serialization/vst3/plugin/program-list-data.cpp',
    '../common/serialization/vst3/plugin/unit-data.cpp',
    '../common/serialization/vst3/plugin/unit-info.cpp',
    '../common/serialization/vst3/plugin/xml-representation-controller.cpp',
    '../common/serialization/vst3/plugin-factory/plugin-factory.cpp',
    '../common/serialization/vst3/attribute-list.cpp',
    '../common/serialization/vst3/base.cpp',
    '../common/serialization/vst3/bstream.cpp',
    '../common/serialization/vst3/component-handler-proxy.cpp',
    '../common/serialization/vst3/connection-point-proxy.cpp',
    '../common/serialization/vst3/context-menu-proxy.cpp',
    '../common/serialization/vst3/context-menu-target.cpp',
    '../common/serialization/vst3/event-list.cpp',
    '../common/serialization/vst3/host-context-proxy.cpp',
    '../common/serialization/vst3/message.cpp',
    '../common/serialization/vst3/param-value-queue.cpp',
    '../common/serialization/vst3/parameter-changes.cpp',
    '../common/serialization/vst3/physical-ui-map-list.cpp',
    '../common/serialization/vst3/plug-frame-proxy.cpp',
    '../common/serialization/vst3/plug-view-proxy.cpp',
    '../common/serialization/vst3/plugin-proxy.cpp',
    '../common/serialization/vst3/plugin-factory-proxy.cpp',
    '../common/serialization/vst3/process-data.cpp',
    'bridges/vst3-impls/component-handler-proxy.cpp',
    'bridges/vst3-impls/connection-point-proxy.cpp',
    'bridges/vst3-impls/context-menu-proxy.cpp',
    'bridges/vst3-impls/host-context-proxy.cpp',
    'bridges/vst3-impls/plug-frame-proxy.cpp',
    'bridges/vst3.cpp',
  )
endif
