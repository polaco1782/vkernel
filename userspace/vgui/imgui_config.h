/*
 * vgui/imgui_config.h
 * ImGui compile-time configuration for the vkernel freestanding environment.
 *
 * Included by every ImGui translation unit via
 *   -DIMGUI_USER_CONFIG=\"imgui_config.h\"
 *
 * Constraints:
 *  - Newlib provides the C standard library.
 *  - No C++ STL (no <vector>, <string>, …).
 *  - No exceptions, no RTTI.
 *  - Placement new must be self-contained (no <new> header).
 */

#pragma once

/* ---- Assertions ---- */
#include <assert.h>
#define IM_ASSERT(_EXPR) assert(_EXPR)

/* ---- Enable math operator overloads for ImVec2 / ImVec4 ---- */
#define IMGUI_DEFINE_MATH_OPERATORS

/*
 * Placement new shim.
 *
 * We compile with -nostdinc++ so <new> is unavailable.  ImGui 1.89+
 * already defines its own ImNewWrapper-based placement new in imgui.h,
 * but older compilers or edge-cases can still emit a bare ::new(ptr).
 * Provide a fallback here that is guarded against double-definition.
 */
#if defined(__cplusplus) && !defined(VGUI_PLACEMENT_NEW_DEFINED)
#define VGUI_PLACEMENT_NEW_DEFINED
struct VguiPlacementNewTag {};
inline void* operator new(decltype(sizeof(0)), VguiPlacementNewTag, void* p) noexcept { return p; }
inline void  operator delete(void*, VguiPlacementNewTag, void*) noexcept {}
#endif
