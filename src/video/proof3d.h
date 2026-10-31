/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file proof3d.h Optional, presentation-only 3D terrain viewport. */
#ifndef VIDEO_PROOF3D_H
#define VIDEO_PROOF3D_H

struct Viewport;
struct Vehicle;
#include "../core/geometry_type.hpp"
#include "../vehicle_type.h"
#include <chrono>
#include <string_view>
#include <utility>
struct DrawPixelInfo;

#ifdef WITH_3D_PROOF
/** Toggle client presentation under the existing game-state lock; false means unsupported. */
bool SetProof3DEnabled(bool enabled);
/** Whether the user has enabled the terrain viewport. */
bool IsProof3DEnabled();
/** Whether this view currently renders and picks in 3D: enabled and within the supported zoom range. */
bool IsProof3DZoomActive(const Viewport &vp);
/** Driver and blitter support, independently of the saved enable preference. */
bool IsProof3DSupported();
/** Delimit dirty-block drawing; clipped viewports share one authoritative snapshot. */
void BeginProof3DFrame();
void EndProof3DFrame();
/** Phase 7 end-to-end timing: a game tick, and a completed draw tick from its start and the start of painting. */
void NoteProof3DGameTick(std::chrono::steady_clock::time_point start);
void NoteProof3DPresent(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point paint_start);
/** Presentation invalidation; callers hold the existing game-state lock. */
void MarkTerrain3DDirty(int x, int y);
void ResetTerrain3D();
void SetTerrain3DVerification(bool enabled);
void SetProof3DGPUVerification(bool enabled);
/** Compare the composited direct layer with each current viewport texture (diagnostic readback). */
void SetProof3DLayerVerification(bool enabled);
/** Phase 8 ground textures; disabling forces the flat-colour fallback (diagnostics and A/B benchmarks). */
void SetTerrain3DGroundTextures(bool enabled);
/** False before the first 3D frame, when forced flat, or when the driver could not create the textures. */
bool AreTerrain3DGroundTexturesActive();
/** Phase 9.5c shadows: 0 off (original lighting), 1 contact shadows, 2 contact and sun shadows. */
void SetTerrain3DShadows(int mode);
/** Presentation-only obstruction fading and focus outlines. Manual visibility locks take precedence. */
void SetTerrain3DAutoVisibility(bool enabled);
/** The shadow mode drawn: the preference, or 0 when the driver could not create them. */
int GetTerrain3DShadows();
/** Shadow pass counters and memory, for diagnostics. */
std::string GetTerrain3DShadowStats();
/** Diagnostic A/B control; never affects simulation or saved settings. */
void SetProof3DPerformanceMode(bool optimized, bool retained, bool tile_meshes, bool dirty_tiles, bool deferred, bool direct);
/** Diagnostic A/B control; off redraws every view when anything in 3D changes, as before priority 7. */
void SetProof3DScopedInvalidation(bool enabled);
/** Diagnostic A/B control; off renders a view whole whenever it sees a change, as before Phase 9.5g idea 2. */
void SetProof3DPartialRender(bool enabled);
/** Compare each partial render with the same view rendered whole (diagnostic readback). */
void SetProof3DPartialVerification(bool enabled);
/** Diagnostic A/B control; falls back to uniform draws when instanced attributes are unavailable. */
void SetProof3DInstancing(bool enabled);
void SetProof3DStaticInstances(bool enabled);
/** Retain bounded static instance streams; off rebuilds and streams them for every view. */
void SetProof3DInstanceStreams(bool enabled);
/** Opt-in reduced distant model meshes; full detail is the default pending visual approval. */
void SetProof3DModelLOD(bool enabled);
/** Draw into the supplied clipped buffer on the GL context thread; false preserves the classic draw path. */
bool DrawProof3D(const Viewport &vp, const DrawPixelInfo &dpi, int screen_x, int screen_y);
/** Replace the transparent holes of on-screen views in screenshot rows with the presented 3D layer. */
void ResolveProof3DScreenshot(void *buf, uint y, uint pitch, uint n);
/** Release proof resources before the owning GL context is destroyed. */
void DestroyProof3D();
void SetTerrain3DCamera(int yaw, int tilt);
/** Trailer moves: place the shared camera exactly, without easing, and read where it is. */
void PlaceTerrain3DCamera(float yaw, float tilt);
std::pair<float, float> GetTerrain3DCamera();
/** Trailer zooms: magnify the shared camera about the view centre; 1 is the plain zoom level. */
void SetTerrain3DMagnify(float magnify);
/** Presentation-only orbit by degrees/tilt percent; preserves the active construction tool. */
void OrbitTerrain3DCamera(float yaw, float tilt, bool animate);
/** Home: animate between the classic camera and the last orbit angle. */
void ToggleTerrain3DCamera();
/** Return to the building angle, including while an orbit animation is in progress. */
void ReturnTerrain3DCamera();
/** True when the management camera has the classic projection. */
bool IsTerrain3DCameraClassic();
/** Convert a screen-space scroll delta into the classic scroll delta for the rotated camera. */
Point Terrain3DScrollDelta(int dx, int dy);
void SetTerrain3DChunkSize(int size);
void SetTerrain3DWireframe(bool enabled);
Point PickTerrain3D(const Viewport &vp, int x, int y, bool clamp_to_map);
/** Hit the drawn vehicle geometry in an orthographic view; perspective views have no targets. */
Vehicle *PickVehicle3D(const Viewport &vp, int x, int y);
/** Project an authoritative world anchor into the viewport's virtual label coordinates. */
Point ProjectTerrain3DLabel(const Viewport &vp, int x, int y, int z);
/** Explicit scripted diagnostics: projected label bounds and optional application click dispatch. */
void CheckViewportLabels(std::string_view tag, bool click);
/** Scripted read-only focus and ground-picking diagnostics. */
void CheckTerrain3DVisibility(std::string_view tag);
/** Explicit rotated-management integration checks; never run by rendering. */
void CheckTerrain3DManagement(std::string_view tag);
void CheckViewportManagement(std::string_view tag);
std::string CheckTerrain3D(const Viewport &vp);
std::string AuditInfrastructure3D();
std::string AuditAssets3D();
std::string AuditCoverage3D();
void HighlightTerrain3D(int x, int y);
bool UpdateViewportCamera3D(Viewport &vp, uint32_t delta_ms);
void SetViewportCamera3DLog(bool enabled);
/** Test scripts: called with a ride-along view's vehicle, the event's name and how dark the view is. */
using RideEvent3DHook = void (*)(VehicleID vehicle, std::string_view event, float dark);
void SetViewportCamera3DEventHook(RideEvent3DHook hook);
#else
inline bool UpdateViewportCamera3D(Viewport &, uint32_t) { return false; }
inline bool SetProof3DEnabled(bool) { return false; }
inline bool IsProof3DEnabled() { return false; }
inline bool IsProof3DZoomActive(const Viewport &) { return false; }
inline bool IsProof3DSupported() { return false; }
inline void SetTerrain3DGroundTextures(bool) {}
inline void SetTerrain3DShadows(int) {}
inline void SetTerrain3DAutoVisibility(bool) {}
inline int GetTerrain3DShadows() { return 0; }
inline std::string GetTerrain3DShadowStats() { return {}; }
inline void BeginProof3DFrame() {}
inline void EndProof3DFrame() {}
inline void NoteProof3DGameTick(std::chrono::steady_clock::time_point) {}
inline void NoteProof3DPresent(std::chrono::steady_clock::time_point, std::chrono::steady_clock::time_point) {}
inline void MarkTerrain3DDirty(int, int) {}
inline void ResetTerrain3D() {}
inline bool DrawProof3D(const Viewport &, const DrawPixelInfo &, int, int) { return false; }
inline void ResolveProof3DScreenshot(void *, uint, uint, uint) {}
inline void OrbitTerrain3DCamera(float, float, bool) {}
inline void PlaceTerrain3DCamera(float, float) {}
inline std::pair<float, float> GetTerrain3DCamera() { return {0, 100}; }
inline void SetTerrain3DMagnify(float) {}
inline void ToggleTerrain3DCamera() {}
inline void ReturnTerrain3DCamera() {}
inline bool IsTerrain3DCameraClassic() { return true; }
inline Point Terrain3DScrollDelta(int dx, int dy) { return {dx, dy}; }
inline Point ProjectTerrain3DLabel(const Viewport &, int, int, int) { return {}; }
#endif

#endif
