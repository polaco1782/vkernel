/*
 * vgui/imgui_config.h
 * ImGui compile-time configuration for the vkernel freestanding environment.
 *
 * Included by every ImGui translation unit via
 *   -DIMGUI_USER_CONFIG=\"imgui_config.h\"
 *
 * Constraints:
 *  - Newlib provides the C standard library.
 *  - The in-repo userspace/cpp shim provides the freestanding C++ runtime.
 *  - No exceptions, no RTTI.
 */

#pragma once

#include <new>

#define ImDrawIdx unsigned int

/* ---- Assertions ---- */
#include <assert.h>
#define IM_ASSERT(_EXPR) assert(_EXPR)

/* ---- Enable math operator overloads for ImVec2 / ImVec4 ---- */
#define IMGUI_DEFINE_MATH_OPERATORS
