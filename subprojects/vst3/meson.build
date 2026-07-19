project(
  'vstsdk',
  'cpp',
  version : '3.8.0',
  default_options : ['cpp_std=c++17'],
)

# This meson.build file is intended as a minimal replacement for the CMake
# project until Meson supports mixing native and cross-compiled CMake
# subprojects.
# https://github.com/mesonbuild/meson/issues/8043
#
# We can't actually define the libraries here because Meson will then try to
# build them with our cross compiler and we still can't use them for all of our
# targets. Instead we just expose the source files, include directories and
# compiler options that are used by the CMake project, and we'll just have to
# assemble them ourselves in yabridge's meson.build file.
# To consume this, you'll have to build static libraries for `base` and
# `plugininterfaces` with the provided compiler options and include directories.
# and then build another static library for the actual plugin and host SDKs that
# links to those dependencies and also uses the same include directories and
# compiler options.

# We mark this with `is_system` to suppress all the warnings coming from these
# headers. This is a terrible solution, but everything involving `FUnkown` will
# spit out at least a few warnings, which means that the compilation output
# would otherwise be completely unreadable.
include_dir = include_directories('.', is_system : true)

# The absolute path to this file, used for patching the SDK for winegcc
# compatibility
sdk_base_dir = meson.current_source_dir()

# Taken from `base/CMakeLists.txt`
compiler_options = []
if get_option('buildtype') == 'debug'
  compiler_options += '-DDEVELOPMENT=1'
else
  compiler_options += '-DRELEASE=1'
endif

# Based on `base/CMakeLists.txt`
base_sources = files([
  'base/source/baseiids.cpp',
  'base/source/fbuffer.cpp',
  'base/source/fdebug.cpp',
  'base/source/fdynlib.cpp',
  'base/source/fobject.cpp',
  'base/source/fstreamer.cpp',
  'base/source/fstring.cpp',
  'base/source/timer.cpp',
  'base/source/updatehandler.cpp',
  'base/thread/source/fcondition.cpp',
  'base/thread/source/flock.cpp',
])

# Based on `pluginterfaces/CMakeLists.txt`
pluginterfaces_sources = files([
  'pluginterfaces/base/conststringtable.cpp',
  'pluginterfaces/base/coreiids.cpp',
  'pluginterfaces/base/funknown.cpp',
  'pluginterfaces/base/ustring.cpp',
])

# Based on `public.sdk/CMakeLists.txt`
sdk_common_sources = files([
  'public.sdk/source/common/commoniids.cpp',
  # New in SDK 3.8.0. `Vst::StringConvert` forwards to `StringConvert` defined
  # here, so omitting it is a link error rather than a missing feature. The
  # lists in this file were originally derived from 3.7.7's CMakeLists.
  'public.sdk/source/common/commonstringconvert.cpp',
  'public.sdk/source/common/openurl.cpp',
  'public.sdk/source/common/systemclipboard_win32.cpp',
  'public.sdk/source/common/threadchecker_linux.cpp',
  'public.sdk/source/common/threadchecker_win32.cpp',
  'public.sdk/source/vst/vstpresetfile.cpp',
])
sdk_sources = files([
  'public.sdk/source/common/memorystream.cpp',
  'public.sdk/source/common/pluginview.cpp',
  'public.sdk/source/main/moduleinit.cpp',
  'public.sdk/source/main/pluginfactory.cpp',
  'public.sdk/source/vst/utility/stringconvert.cpp',
  'public.sdk/source/vst/utility/vst2persistence.cpp',
  'public.sdk/source/vst/vstaudioeffect.cpp',
  'public.sdk/source/vst/vstbus.cpp',
  'public.sdk/source/vst/vstcomponent.cpp',
  'public.sdk/source/vst/vstcomponentbase.cpp',
  'public.sdk/source/vst/vsteditcontroller.cpp',
  'public.sdk/source/vst/vstinitiids.cpp',
  'public.sdk/source/vst/vstnoteexpressiontypes.cpp',
  'public.sdk/source/vst/vstparameters.cpp',
  'public.sdk/source/vst/vstrepresentation.cpp',
])
sdk_hosting_sources = files([
  'public.sdk/source/vst/hosting/connectionproxy.cpp',
  'public.sdk/source/vst/hosting/eventlist.cpp',
  'public.sdk/source/vst/hosting/hostclasses.cpp',
  'public.sdk/source/vst/hosting/module.cpp',
  'public.sdk/source/vst/hosting/parameterchanges.cpp',
  'public.sdk/source/vst/hosting/pluginterfacesupport.cpp',
  'public.sdk/source/vst/hosting/processdata.cpp',
  'public.sdk/source/vst/utility/stringconvert.cpp',
  'public.sdk/source/vst/vstinitiids.cpp',
])
