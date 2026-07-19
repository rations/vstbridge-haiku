# Meson does not allow mixing native and non native dependencies from
# subprojects. The only workaround is to only define only the necessary
# variables there, and to then assemble the dependencies here ourselves.

vst3 = subproject('vst3', version : '3.8.0')

# Also include our normal compiler options so symbols are hidden by default
vst3_compiler_options = compiler_options + vst3.get_variable('compiler_options')
vst3_include_dir = vst3.get_variable('include_dir')

# NOTE: `subprojects/vst3` is vendored pre-patched, so there is no
# `tools/patch-vst3-sdk.sh` step here anymore.
#
# Upstream fetched a stock SDK 3.7.7 and patched it at configure time for
# Winelib compatibility. This port instead vendors SDK 3.8.0 with two sets of
# changes already applied:
#
#   1. The Haiku platform port (`SMTG_OS_HAIKU`), without which the *native*
#      static libraries do not compile at all -- 3.7.7 has no Haiku branch, so
#      BYTEORDER, PLUGIN_API, strnicmp and vsnwprintf are all undefined. This
#      also supplies `kPlatformTypeHaikuBView`, which the plugin side must
#      advertise.
#   2. The Winelib fixes that upstream's patch file used to apply, rebased onto
#      3.8.0, plus one new one: 3.8.0's `SMTG_DEPRECATED_MSG` expands to
#      `__declspec(deprecated(...))` on Windows, which winegcc mangles, so it
#      falls through to the standard C++17 attribute under `__WINE__`.
#
# One SDK serves both sides: the Wine host builds it as SMTG_OS_WINDOWS via
# winegcc, the native plugin builds it as SMTG_OS_HAIKU.

# We'll create a dependency for the plugin SDK for our native VST3 plugin
vst3_base_native = static_library(
  'base_native',
  vst3.get_variable('base_sources'),
  native : true,
  cpp_args : vst3_compiler_options + ['-Wno-cpp'],
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)
vst3_pluginterfaces_native = static_library(
  'pluginterfaces_native',
  vst3.get_variable('pluginterfaces_sources'),
  native : true,
  cpp_args : vst3_compiler_options,
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)
vst3_sdk_native = static_library(
  'sdk_native',
  vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_sources'),
  native : true,
  link_with : [vst3_base_native, vst3_pluginterfaces_native],
  cpp_args : vst3_compiler_options + ['-Wno-multichar'],
  include_directories : vst3_include_dir,
  override_options : ['warning_level=0'],
)

# We need a separate host SDK dependency for our Wine plugin host applications
if is_64bit_system
  vst3_base_wine_64bit = static_library(
    'vst3_base_wine_64bit',
    vst3.get_variable('base_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options + [ '-Wno-cpp'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_pluginterfaces_wine_64bit = static_library(
    'vst3_pluginterfaces_wine_64bit',
    vst3.get_variable('pluginterfaces_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options,
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_sdk_hosting_wine_64bit = static_library(
    'vst3_sdk_hosting_wine_64bit',
    vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_hosting_sources'),
    native : false,
    link_with : [vst3_base_wine_64bit, vst3_pluginterfaces_wine_64bit],
    cpp_args : vst3_compiler_options + wine_64bit_compiler_options + ['-Wno-multichar'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
endif

# And another time for the 32-bit version
if with_bitbridge
  vst3_base_wine_32bit = static_library(
    'vst3_base_wine_32bit',
    vst3.get_variable('base_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_32bit_compiler_options + ['-Wno-cpp'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_pluginterfaces_wine_32bit = static_library(
    'vst3_pluginterfaces_wine_32bit',
    vst3.get_variable('pluginterfaces_sources'),
    native : false,
    cpp_args : vst3_compiler_options + wine_32bit_compiler_options,
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
  vst3_sdk_hosting_wine_32bit = static_library(
    'vst3_sdk_hosting_wine_32bit',
    vst3.get_variable('sdk_common_sources') + vst3.get_variable('sdk_hosting_sources'),
    native : false,
    link_with : [vst3_base_wine_32bit, vst3_pluginterfaces_wine_32bit],
    cpp_args : vst3_compiler_options + wine_32bit_compiler_options + ['-Wno-multichar'],
    include_directories : vst3_include_dir,
    override_options : ['warning_level=0'],
  )
endif

#
# VST3 dependencies
#

vst3_sdk_native_dep = declare_dependency(
  link_with : vst3_sdk_native,
  include_directories : vst3_include_dir,
  compile_args : vst3_compiler_options,
)

if is_64bit_system
  vst3_sdk_hosting_wine_64bit_dep = declare_dependency(
    link_with : vst3_sdk_hosting_wine_64bit,
    include_directories : vst3_include_dir,
    # This does mean that we now have a lot of defines in our code, but the
    # alternative would be patching every location in the SDK where they include
    # `windows.h`
    compile_args : vst3_compiler_options + wine_64bit_compiler_options,
  )
endif

if with_bitbridge
  vst3_sdk_hosting_wine_32bit_dep = declare_dependency(
    link_with : vst3_sdk_hosting_wine_32bit,
    include_directories : vst3_include_dir,
    # This does mean that we now have a lot of defines in our code, but the
    # alternative would be patching every location in the SDK where they include
    # `windows.h`
    compile_args : vst3_compiler_options + wine_32bit_compiler_options,
  )
endif
