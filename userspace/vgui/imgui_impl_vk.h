/*
 * vgui/imgui_impl_vk.h
 * vkernel platform + renderer backend for Dear ImGui.
 *
 * Platform:  keyboard input via vk_poll_key, timing via vk_tick_count.
 * Renderer:  pure software rasterizer that writes textured triangles
 *            directly to the UEFI linear framebuffer.
 *
 * Usage (every frame):
 *   1. Drain vk_poll_key(), pass each event to ImGui_ImplVK_ProcessKey().
 *   2. ImGui_ImplVK_NewFrame()  then  ImGui::NewFrame().
 *   3. Build your UI.
 *   4. ImGui::Render()  then  ImGui_ImplVK_RenderDrawData().
 */

#pragma once

#include "../include/vk.h"  /* vk_framebuffer_info_t, vk_key_event_t, … */
#include "imgui/imgui.h"

/* ---- Lifecycle ---- */

/*
 * Initialise the backend.  Call once after ImGui::CreateContext().
 * Builds the font atlas and captures the font texture for the renderer.
 * Returns false on failure (no memory).
 */
bool ImGui_ImplVK_Init(const vk_framebuffer_info_t* fb);

/* Tear down — call before ImGui::DestroyContext(). */
void ImGui_ImplVK_Shutdown();

/* ---- Per-frame calls ---- */

/*
 * Update timing and display size.
 * Call once per frame BEFORE ImGui::NewFrame().
 */
void ImGui_ImplVK_NewFrame();

/*
 * Translate one vk_key_event_t into ImGui key / char events.
 * Call for every event returned by vk_poll_key().
 */
void ImGui_ImplVK_ProcessKey(const vk_key_event_t* evt);

/*
 * Feed a mouse movement / button event into ImGui.
 * Call for every event returned by vk_poll_mouse().
 */
void ImGui_ImplVK_ProcessMouse(const vk_mouse_event_t* evt);

/*
 * Rasterise ImGui's draw lists into an offscreen buffer then blit the
 * completed frame to the UEFI framebuffer in one shot (double-buffering
 * eliminates the partial-draw flicker visible with direct rendering).
 * Call after ImGui::Render(), passing ImGui::GetDrawData().
 */
void ImGui_ImplVK_RenderDrawData(ImDrawData* draw_data,
                                  const vk_framebuffer_info_t* fb);
