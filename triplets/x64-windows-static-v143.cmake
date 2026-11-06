# Pins the toolset, because by default vcpkg uses the newest one it can find, and v143 can't
# reliably link libraries built by a newer compiler.
set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_PLATFORM_TOOLSET v143)
