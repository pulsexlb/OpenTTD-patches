/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file proof3d_impl.h Private implementation included only by opengl.cpp, sharing its GL loader. */
#include "terrain3d_slots.h"
#include <cfloat>
#include "terrain3d_cache.h"
#include <bit>
#include <map>

namespace {
#define PROOF_GL(fn) static decltype(&fn) p_##fn = nullptr
PROOF_GL(glGenFramebuffers);
PROOF_GL(glDeleteFramebuffers);
PROOF_GL(glBindFramebuffer);
PROOF_GL(glFramebufferTexture2D);
PROOF_GL(glCheckFramebufferStatus);
PROOF_GL(glGenRenderbuffers);
PROOF_GL(glDeleteRenderbuffers);
PROOF_GL(glBindRenderbuffer);
PROOF_GL(glRenderbufferStorage);
PROOF_GL(glFramebufferRenderbuffer);
PROOF_GL(glReadPixels);
PROOF_GL(glIsEnabled);
PROOF_GL(glDepthFunc);
PROOF_GL(glDepthMask);
PROOF_GL(glClearDepth);
PROOF_GL(glGetFloatv);
PROOF_GL(glPolygonMode);
PROOF_GL(glUniform4f);
PROOF_GL(glUniformMatrix4fv);
PROOF_GL(glGenQueries);
PROOF_GL(glDeleteQueries);
PROOF_GL(glBeginQuery);
PROOF_GL(glEndQuery);
PROOF_GL(glGetQueryObjectui64v);
PROOF_GL(glTexImage3D);
PROOF_GL(glGenerateMipmap);
PROOF_GL(glStencilFunc);
PROOF_GL(glStencilOp);
PROOF_GL(glStencilMask);
PROOF_GL(glClearStencil);
PROOF_GL(glBlendFuncSeparate);
PROOF_GL(glGetQueryObjectiv);
PROOF_GL(glMultiDrawArrays);
PROOF_GL(glDrawArraysInstanced);
PROOF_GL(glVertexAttribDivisor);
PROOF_GL(glBlitFramebuffer);
PROOF_GL(glColorMask);
PROOF_GL(glPolygonOffset);
PROOF_GL(glDrawBuffer);
PROOF_GL(glReadBuffer);
PROOF_GL(glBlendEquation);
PROOF_GL(glScissor);
#undef PROOF_GL

static std::atomic<bool> _proof_enabled{false};
static GLuint _proof_fbo = 0, _proof_colour = 0, _proof_depth = 0;
static GLuint _proof_program = 0, _proof_vao = 0, _proof_vbo = 0;
/* A ring of timer queries: direct presentation collects each result a few renders later instead of waiting for the GPU. */
static std::array<GLuint, 8> _proof_queries{};
static std::array<bool, 8> _proof_query_pending{};
static size_t _proof_query_next = 0;
/* Phase 8 ground: texture array (unit 1) and per-tile layer map (unit 2). Without them ground is flat-coloured. */
static GLuint _ground_textures = 0, _ground_map_texture = 0;
static bool _ground_textured = false, _ground_flat_forced = false;
static uint64_t _ground_map_generation = 0;
static int _ground_map_width = 0, _ground_map_height = 0;
static uint64_t _ground_map_upload_bytes = 0;
static int _proof_width = 0, _proof_height = 0;
struct ProofImage {
	std::array<int, 28> key; ///< Width, height, left, top, zoom, map size X/Y, direct presentation, then the yaw, tilt and pivot height bits, then a perspective eye x/y/z, heading, pitch and field of view bits, then whether and where a cab view leaves out its own vehicle, then how far it has faded to black; then the focus kind, world x/y/z and radius; last the trailer magnification.
	std::vector<Colour> pixels;
	GLuint texture = 0; ///< Direct presentation keeps the rendered view on the GPU instead of reading it back.
	uint64_t verified = 0; ///< Frame of the last kept-image reference comparison.
	uint64_t revision = 0; ///< Rendered image identity, independent of recycled texture names.
	Terrain3D::DirtyCells stale; ///< Cells of a kept direct image to draw again before it is next used.
};
static std::vector<ProofImage> _proof_images;
static uint64_t _proof_image_revision = 0;

/* Direct presentation (Phase 7 priority 8). Views render into retained textures, which are drawn into a
 * screen-sized layer beneath the 2D UI; the 2D buffer holds transparent holes where a view is visible.
 * The layer's stencil records which view owns each pixel, so a 3D-only change redraws the layer
 * without redrawing or uploading the UI. Layer draws are queued while drawing and run when the 2D
 * buffer is uploaded, so the layer and the UI always present the same frame. */
struct ProofTexture { GLuint texture; int width, height; };
/* Released textures may still be referenced by queued layer draws; they become reusable after the next flush. */
static std::vector<ProofTexture> _proof_free_textures, _proof_released_textures;
struct ProofLayerDraw { GLuint texture; int left, top, height; int x0, y0, x1, y1; uint8_t id; bool claim; };
static std::vector<ProofLayerDraw> _proof_layer_queue;
static GLuint _proof_fade_program = 0, _proof_fade_vao = 0; ///< Darkens a view fading out in a tunnel.
static GLint _proof_fade_dark = -1;
static GLuint _proof_layer_fbo = 0, _proof_layer_colour = 0, _proof_layer_stencil = 0, _proof_layer_program = 0, _proof_layer_vao = 0;
static int _proof_layer_width = 0, _proof_layer_height = 0;
/** Diagnostic control; off restores the readback path. Active while the screen holds holes. */
static bool _proof_direct = true, _proof_layer_active = false;
/** Stencil value of each viewport in the layer; 0 is unowned. */
static std::array<const Viewport *, 256> _proof_layer_ids{};
/** Image last queued across all pixels owned by each view, including outside its 2D dirty clips. */
static std::array<uint64_t, 256> _proof_layer_revisions{};
static uint64_t _proof_layer_draws = 0, _proof_layer_refreshes = 0;

static bool _proof_auto_visibility = true;
static std::array<float, 4> _proof_focus{}; ///< Screen X/Y, depth and radius of this image's focus.
static GLuint _proof_outline_program = 0, _proof_outline_vao = 0, _proof_outline_vbo = 0;
static GLint _proof_outline_offset = -1, _proof_outline_colour = -1;
static bool _proof_retained = true;
static bool _proof_tile_meshes = true;
static bool _proof_dirty_tiles = true;
static bool _proof_deferred = true;
static uint64_t _proof_mesh_us = 0, _proof_chunks_rebuilt = 0, _proof_evictions = 0;
static uint64_t _proof_cold_chunks = 0;
static bool _proof_optimized = true, _proof_refreshed = false, _proof_verify_gpu = false;
static uint64_t _proof_draws = 0, _proof_renders = 0, _proof_scan_us = 0, _proof_render_us = 0, _proof_read_us = 0;
static uint64_t _proof_frame_serial = 0;
static uint64_t _proof_gpu_ns = 0, _proof_tiles_scanned = 0;
static uint64_t _proof_tiles_scan_candidates = 0;
static uint64_t _proof_tiles_rebuilt = 0, _proof_tiles_reused = 0;
static uint64_t _proof_slot_writes = 0, _proof_slot_relayouts = 0; ///< Chunk uploads that rewrote only changed tile slots, or laid out a resident buffer again.
static uint64_t _proof_terrain_runs = 0; ///< Merged tile-slot ranges submitted for retained terrain.
static std::chrono::steady_clock::time_point _proof_frame_start;

static void ReleaseProofImage(ProofImage &image)
{
	if (image.texture != 0) _proof_released_textures.push_back({image.texture, image.key[0], image.key[1]});
	image.texture = 0;
}

static void ClearProofImages()
{
	for (auto &image : _proof_images) ReleaseProofImage(image);
	_proof_images.clear();
}

/** Whether on-screen views use direct presentation; the GPU reference comparison needs read-back pixels. */
static bool ProofDirectMode() { return _proof_direct && _proof_optimized && _proof_retained && !_proof_verify_gpu; }

/** 3D content changed: drop cached views. Direct presentation re-renders the layer and leaves the 2D UI alone. */
static void InvalidateProof3DImages()
{
	ClearProofImages();
	if (!_proof_layer_active || !ProofDirectMode()) MarkWholeScreenDirty();
}

/** World-space bounds of something that changed in 3D. */
using ProofBox = std::pair<Terrain3D::Vec, Terrain3D::Vec>;
/** Diagnostic control: off invalidates every view for any change. */
static bool _proof_scoped = true;
static bool _proof_verify_layer = false;
/** Diagnostic control: off drops every view that sees a change and renders it whole. */
static bool _proof_partial = true;
static bool _proof_verify_partial = false;
static uint64_t _proof_partial_renders = 0, _proof_partial_pixels = 0, _proof_partial_kept = 0;
/** Past this share of its cells, a stale view renders whole. */
static constexpr float PROOF_PARTIAL_LIMIT = 0.5f;
static uint64_t _proof_scoped_boxes = 0, _proof_scoped_drops = 0, _proof_scoped_kept = 0;
static void InvalidateProof3DRegions(std::span<const ProofBox> boxes);

static uint64_t ProofMicros(std::chrono::steady_clock::time_point start)
{
	return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

/** Restore all GL state touched by this pass, including mapped screen upload bindings. */
struct ProofGLState {
	GLint draw_fbo, read_fbo, rbo, program, vao, buffer, pack, unpack, texture, active, texture_array, texture_map;
	GLint viewport[4], pack_alignment, pack_row, pack_skip_rows, pack_skip_pixels, depth_func, depth_mask;
	GLint unpack_alignment, unpack_row;
	GLint polygon_mode[2];
	GLfloat clear[4], clear_depth;
	bool depth, blend, cull, scissor;
	ProofGLState()
	{
		_glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &this->draw_fbo);
		_glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &this->read_fbo);
		_glGetIntegerv(GL_RENDERBUFFER_BINDING, &this->rbo);
		_glGetIntegerv(GL_CURRENT_PROGRAM, &this->program);
		_glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &this->vao);
		_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &this->buffer);
		_glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &this->pack);
		_glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &this->unpack);
		_glGetIntegerv(GL_ACTIVE_TEXTURE, &this->active);
		_glActiveTexture(GL_TEXTURE1);
		_glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &this->texture_array);
		_glActiveTexture(GL_TEXTURE2);
		_glGetIntegerv(GL_TEXTURE_BINDING_2D, &this->texture_map);
		_glActiveTexture(GL_TEXTURE0);
		_glGetIntegerv(GL_TEXTURE_BINDING_2D, &this->texture);
		_glGetIntegerv(GL_UNPACK_ALIGNMENT, &this->unpack_alignment);
		_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &this->unpack_row);
		_glGetIntegerv(GL_VIEWPORT, this->viewport);
		_glGetIntegerv(GL_PACK_ALIGNMENT, &this->pack_alignment);
		_glGetIntegerv(GL_PACK_ROW_LENGTH, &this->pack_row);
		_glGetIntegerv(GL_PACK_SKIP_ROWS, &this->pack_skip_rows);
		_glGetIntegerv(GL_PACK_SKIP_PIXELS, &this->pack_skip_pixels);
		_glGetIntegerv(GL_DEPTH_FUNC, &this->depth_func);
		_glGetIntegerv(GL_DEPTH_WRITEMASK, &this->depth_mask);
		_glGetIntegerv(GL_POLYGON_MODE, this->polygon_mode);
		p_glGetFloatv(GL_COLOR_CLEAR_VALUE, this->clear);
		p_glGetFloatv(GL_DEPTH_CLEAR_VALUE, &this->clear_depth);
		this->depth = p_glIsEnabled(GL_DEPTH_TEST);
		this->blend = p_glIsEnabled(GL_BLEND);
		this->cull = p_glIsEnabled(GL_CULL_FACE);
		this->scissor = p_glIsEnabled(GL_SCISSOR_TEST);
	}
	~ProofGLState()
	{
		p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, this->draw_fbo);
		p_glBindFramebuffer(GL_READ_FRAMEBUFFER, this->read_fbo);
		p_glBindRenderbuffer(GL_RENDERBUFFER, this->rbo);
		_glUseProgram(this->program);
		_glBindVertexArray(this->vao);
		_glBindBuffer(GL_ARRAY_BUFFER, this->buffer);
		_glBindBuffer(GL_PIXEL_PACK_BUFFER, this->pack);
		_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, this->unpack);
		_glActiveTexture(GL_TEXTURE1);
		_glBindTexture(GL_TEXTURE_2D_ARRAY, this->texture_array);
		_glActiveTexture(GL_TEXTURE2);
		_glBindTexture(GL_TEXTURE_2D, this->texture_map);
		_glActiveTexture(GL_TEXTURE0);
		_glBindTexture(GL_TEXTURE_2D, this->texture);
		_glActiveTexture(this->active);
		_glPixelStorei(GL_UNPACK_ALIGNMENT, this->unpack_alignment);
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, this->unpack_row);
		_glViewport(this->viewport[0], this->viewport[1], this->viewport[2], this->viewport[3]);
		_glPixelStorei(GL_PACK_ALIGNMENT, this->pack_alignment);
		_glPixelStorei(GL_PACK_ROW_LENGTH, this->pack_row);
		_glPixelStorei(GL_PACK_SKIP_ROWS, this->pack_skip_rows);
		_glPixelStorei(GL_PACK_SKIP_PIXELS, this->pack_skip_pixels);
		p_glDepthFunc(this->depth_func);
		p_glDepthMask(this->depth_mask);
		p_glPolygonMode(GL_FRONT_AND_BACK, this->polygon_mode[0]);
		p_glClearDepth(this->clear_depth);
		_glClearColor(this->clear[0], this->clear[1], this->clear[2], this->clear[3]);
		(this->depth ? _glEnable : _glDisable)(GL_DEPTH_TEST);
		(this->blend ? _glEnable : _glDisable)(GL_BLEND);
		(this->cull ? _glEnable : _glDisable)(GL_CULL_FACE);
		(this->scissor ? _glEnable : _glDisable)(GL_SCISSOR_TEST);
	}
};

static bool LoadProofGL()
{
	if (!IsOpenGLVersionAtLeast(3, 2)) return false;
#define LOAD_PROOF(fn) if (!BindGLProc(p_##fn, #fn)) return false
	LOAD_PROOF(glGenFramebuffers); LOAD_PROOF(glDeleteFramebuffers); LOAD_PROOF(glBindFramebuffer);
	LOAD_PROOF(glFramebufferTexture2D); LOAD_PROOF(glCheckFramebufferStatus);
	LOAD_PROOF(glGenRenderbuffers); LOAD_PROOF(glDeleteRenderbuffers); LOAD_PROOF(glBindRenderbuffer);
	LOAD_PROOF(glRenderbufferStorage); LOAD_PROOF(glFramebufferRenderbuffer);
	LOAD_PROOF(glReadPixels); LOAD_PROOF(glIsEnabled); LOAD_PROOF(glDepthFunc); LOAD_PROOF(glDepthMask);
	LOAD_PROOF(glClearDepth); LOAD_PROOF(glGetFloatv);
	LOAD_PROOF(glPolygonMode); LOAD_PROOF(glUniform4f); LOAD_PROOF(glUniformMatrix4fv);
	LOAD_PROOF(glStencilFunc); LOAD_PROOF(glStencilOp); LOAD_PROOF(glStencilMask); LOAD_PROOF(glClearStencil);
	LOAD_PROOF(glBlendFuncSeparate); LOAD_PROOF(glMultiDrawArrays); LOAD_PROOF(glScissor);
	/* Timer queries are optional on the GL 3.2 fallback. */
	if (IsOpenGLVersionAtLeast(3, 3)) {
		LOAD_PROOF(glGenQueries); LOAD_PROOF(glDeleteQueries); LOAD_PROOF(glBeginQuery);
		LOAD_PROOF(glEndQuery); LOAD_PROOF(glGetQueryObjectui64v); LOAD_PROOF(glGetQueryObjectiv);
	}
#undef LOAD_PROOF
	/* Optional: ground textures fall back to flat colours without them. */
	BindGLProc(p_glTexImage3D, "glTexImage3D");
	BindGLProc(p_glGenerateMipmap, "glGenerateMipmap");
	/* Instanced attributes are optional. A resolved address alone does not establish GL 3.2 support. */
	p_glDrawArraysInstanced = nullptr;
	p_glVertexAttribDivisor = nullptr;
	if (IsOpenGLVersionAtLeast(3, 3)) {
		BindGLProc(p_glDrawArraysInstanced, "glDrawArraysInstanced");
		BindGLProc(p_glVertexAttribDivisor, "glVertexAttribDivisor");
	}
	/* Optional: shadows stay off without them. */
	BindGLProc(p_glBlitFramebuffer, "glBlitFramebuffer");
	BindGLProc(p_glColorMask, "glColorMask");
	BindGLProc(p_glPolygonOffset, "glPolygonOffset");
	BindGLProc(p_glDrawBuffer, "glDrawBuffer");
	BindGLProc(p_glReadBuffer, "glReadBuffer");
	BindGLProc(p_glBlendEquation, "glBlendEquation");
	return true;
}

/** CPU-projected vertex; world/tile/layer are only used by ground faces. */
struct ProofVertex { float x, y, z, r, g, b; float transparent = 0; float wx = 0, wy = 0, tx = 0, ty = 0, layer = -1; };
struct ProofModelVertex { float x, y, z, r, g, b, nx, ny, nz; };
/** Retained terrain vertex, 32 bytes. The tile origin (at most 4095 * TILE_SIZE), transparency option and layer are
 * whole numbers the GPU converts exactly. Colour stays float: 16-bit colour changed 1/255 on some pixels. */
struct ProofStaticVertex {
	float x, y, z, r, g, b;
	uint16_t tx, ty, option;
	int16_t layer;
};
static_assert(sizeof(ProofStaticVertex) == 32);
/** A chunk's retained geometry; \c count is the allocated size in vertices. The runs merge back-to-back tile slots for drawing. */
struct ProofChunkBuffer { GLuint buffer = 0; uint64_t revision = 0; GLsizei count = 0; Terrain3D::TileSlots slots; std::vector<GLint> run_first; std::vector<GLsizei> run_count; };
static std::vector<ProofChunkBuffer> _proof_chunks;
/** Vehicle model library, uploaded once in model space; bounds are for view culling, the footprint (local min x, max x, min y, max y) for contact shadows. */
struct ProofModelRange { GLint first = 0; GLsizei count = 0; float radius = 0, low = 0, high = 0; std::array<float, 4> footprint{}; };
static GLuint _proof_models = 0;
static std::vector<ProofModelRange> _proof_model_ranges;
static uint64_t _proof_vehicle_draws = 0, _proof_vehicle_instances = 0, _proof_instance_upload_bytes = 0;
static uint64_t _proof_model_triangles = 0, _proof_full_model_triangles = 0, _proof_lod_instances = 0;
static size_t _proof_model_bytes = 0;
static uint64_t _proof_static_draws = 0, _proof_static_instances = 0;
static uint64_t _proof_static_group_us = 0, _proof_instance_stream_us = 0;
static bool _proof_instanced = true;
static GLuint _proof_instances = 0;
/** Four vec4s, shared by the uniform reference and per-instance vertex attributes. */
struct ProofInstance {
	std::array<float, 4> modelA, modelB, companyA, companyB;
	bool operator==(const ProofInstance &) const = default;
};
struct ProofInstanceBatch { size_t model, first; GLsizei count; };
struct ProofStaticChunkStream {
	size_t index;
	uint64_t revision;
	std::vector<ProofInstanceBatch> spans;
	std::vector<ProofInstance> data; ///< Model-grouped CPU placements, compared exactly when this chunk changes.
};
/** Camera-independent placements for one visible chunk set. No pointers into evictable meshes. */
struct ProofStaticStream {
	GLuint buffer = 0;
	uint64_t used = 0;
	size_t count = 0, bytes = 0;
	std::vector<ProofStaticChunkStream> chunks;
	std::vector<ProofInstanceBatch> batches;
	Terrain3D::TileSlots slots; ///< Variable-sized model ranges; unused capacity is never drawn.
};
static std::vector<ProofStaticStream> _proof_static_streams;
/** The placements of the chunks a partial render draws. */
static ProofStaticStream _proof_partial_statics;
static bool _proof_retain_streams = true;
static uint64_t _proof_stream_generation = 0, _proof_stream_serial = 0;
static uint64_t _proof_stream_hits = 0, _proof_stream_builds = 0, _proof_stream_patches = 0, _proof_stream_evictions = 0;
/** Retained static streams, CPU placements plus GPU slots; a maximum zoom-out view of a 2048x512 map is about 30 MB. */
static constexpr size_t PROOF_STREAM_BUDGET = 256 * 1024 * 1024;

static void ClearProofStaticStreams()
{
	for (auto &stream : _proof_static_streams) if (stream.buffer) _glDeleteBuffers(1, &stream.buffer);
	_proof_static_streams.clear();
	if (_proof_partial_statics.buffer) _glDeleteBuffers(1, &_proof_partial_statics.buffer);
	_proof_partial_statics = {};
}

static size_t ProofStaticStreamBytes()
{
	size_t bytes = 0;
	for (const auto &stream : _proof_static_streams) bytes += stream.bytes;
	return bytes;
}
static std::vector<ProofInstanceBatch> _proof_instance_batches;
static std::vector<ProofInstance> _proof_instance_upload;
/** Locations belong to a linked program and are refreshed when its GL context is recreated. */
struct ProofLocations {
	GLint cameraA, cameraB, cameraC, selection, selectedTile, visibility, viewProjection, eye, fog;
	GLint modelA, modelB, companyA, companyB, groundTextures, groundMap, groundInfo, groundCrisp;
	GLint shadowMap, shadowMatrix, shadowInfo, shadowFade, autoFocus;
	std::array<GLint, 6> vertex;
	std::array<GLint, 4> instance;
};
static ProofLocations _proof_locations, _proof_instance_locations;
static GLuint _proof_instance_program = 0;
static bool _proof_instance_failed = false;
static GLint _proof_layer_view, _proof_layer_screen, _proof_layer_rect, _proof_layer_origin, _proof_layer_retro;
static uint64_t _proof_generation = 0;
static uint64_t _proof_upload_bytes = 0, _proof_prepare_us = 0;
static uint64_t _proof_static_upload_bytes = 0, _proof_upload_us = 0, _proof_projection_us = 0;

static void BindProofVertices(GLuint buffer, bool world = false, const ProofLocations &locations = _proof_locations)
{
	_glBindBuffer(GL_ARRAY_BUFFER, buffer);
	/* Byte offsets per format; -1 leaves the attribute disabled, so retained terrain reads transparent as 0. */
	struct Attribute { GLint size; GLenum type; GLboolean normalized; int offset; };
	static const Attribute cpu[] = {
		{3, GL_FLOAT, GL_FALSE, offsetof(ProofVertex, x)}, {3, GL_FLOAT, GL_FALSE, offsetof(ProofVertex, r)},
		{1, GL_FLOAT, GL_FALSE, offsetof(ProofVertex, transparent)}, {3, GL_FLOAT, GL_FALSE, -1},
		{4, GL_FLOAT, GL_FALSE, offsetof(ProofVertex, wx)}, {1, GL_FLOAT, GL_FALSE, offsetof(ProofVertex, layer)}};
	static const Attribute retained[] = {
		{3, GL_FLOAT, GL_FALSE, offsetof(ProofStaticVertex, x)}, {3, GL_FLOAT, GL_FALSE, offsetof(ProofStaticVertex, r)},
		{1, GL_FLOAT, GL_FALSE, -1}, {3, GL_UNSIGNED_SHORT, GL_FALSE, offsetof(ProofStaticVertex, tx)},
		{4, GL_FLOAT, GL_FALSE, -1}, {1, GL_SHORT, GL_FALSE, offsetof(ProofStaticVertex, layer)}};
	for (int i = 0; i < 6; ++i) {
		GLint location = locations.vertex[i];
		if (location < 0) continue;
		const Attribute &a = world ? retained[i] : cpu[i];
		if (a.offset < 0) { _glDisableVertexAttribArray(location); continue; }
		_glEnableVertexAttribArray(location);
		_glVertexAttribPointer(location, a.size, a.type, a.normalized, world ? sizeof(ProofStaticVertex) : sizeof(ProofVertex), reinterpret_cast<void *>(static_cast<uintptr_t>(a.offset)));
	}
}

/** Model vertices need only position, colour and local normal (36 bytes instead of 44). */
static void BindProofModelVertices(const ProofLocations &locations = _proof_locations)
{
	_glBindBuffer(GL_ARRAY_BUFFER, _proof_models);
	const int sizes[] = {3, 3, 1, 3, 4, 1};
	const int offsets[] = {0, 3, -1, 6, -1, -1};
	for (size_t i = 0; i < locations.vertex.size(); ++i) {
		GLint location = locations.vertex[i];
		if (location < 0) continue;
		if (offsets[i] < 0) {
			_glDisableVertexAttribArray(location);
			continue;
		}
		_glEnableVertexAttribArray(location);
		_glVertexAttribPointer(location, sizes[i], GL_FLOAT, GL_FALSE, sizeof(ProofModelVertex), reinterpret_cast<void *>(offsets[i] * sizeof(float)));
	}
}

static void ProofUniform(GLint location, float a, float b, float c, float d)
{
	p_glUniform4f(location, a, b, c, d);
}

/** Haze a perspective view fades into; its images are cleared to it, so the draw distance leaves no edge. */
static constexpr std::array<float, 3> PROOF_FOG{0.62f, 0.71f, 0.78f};

/** Perspective projection and fog; an orthographic camera leaves eye.w at 0 and the shaders keep their own projection. */
static void ProofPerspectiveUniforms(const ProofLocations &locations, const Terrain3D::Camera &camera)
{
	p_glUniformMatrix4fv(locations.viewProjection, 1, GL_FALSE, camera.matrix.data());
	ProofUniform(locations.eye, camera.eye.x, camera.eye.y, camera.eye.z, camera.distance);
	ProofUniform(locations.fog, PROOF_FOG[0], PROOF_FOG[1], PROOF_FOG[2], camera.distance * 0.6f);
}

/* Shadows (phase 9.5c): a client preference; 0 keeps the original lighting and skips every shadow pass. */
static int _proof_shadows = 0; ///< 0 off, 1 contact shadows, 2 contact shadows and sun shadows.
static bool _proof_shadow_failed = false; ///< The driver lacks something shadows need; they stay off.
static constexpr int PROOF_SHADOW_SIZE = 4096; ///< Sun shadow map texels per side.
static constexpr float PROOF_SUN_STRENGTH = 0.55f; ///< Share of the direct light a sun shadow removes.
static constexpr float PROOF_SUN_RANGE = 448; ///< World units from a perspective eye that sun shadows reach, fading over the last quarter.
static constexpr float PROOF_CONTACT_STRENGTH = 0.22f; ///< Darkening at the foot of a contact shadow; overlapping ones take the darkest.
static GLuint _proof_shadow_fbo = 0, _proof_shadow_depth = 0;
static GLuint _proof_contact_fbo = 0, _proof_contact_depth = 0, _proof_contact_program = 0, _proof_contact_vao = 0;
static GLuint _proof_occlusion_fbo = 0, _proof_occlusion = 0, _proof_occlusion_program = 0, _proof_occlusion_vao = 0;
static GLint _proof_occlusion_map = -1, _proof_occlusion_strength = -1;
static int _proof_contact_width = 0, _proof_contact_height = 0;
static GLint _proof_contact_view, _proof_contact_inverse, _proof_contact_viewer, _proof_contact_screen, _proof_contact_bounds,
	_proof_contact_heights, _proof_contact_visibility, _proof_contact_eye, _proof_contact_fog, _proof_contact_depth_map;
static std::array<GLint, 4> _proof_contact_instance{};
/** Sun shadow sampling for the current render: world to shadow-map texture coordinates, and (sample, strength, normal offset, depth pass). */
static std::array<float, 16> _proof_shadow_matrix{};
static std::array<float, 4> _proof_shadow_info{};
static std::array<float, 4> _proof_shadow_fade{}; ///< Perspective eye and sun shadow range; range 0 does not fade.
static uint64_t _proof_shadow_passes = 0, _proof_contact_passes = 0, _proof_shadow_us = 0, _proof_contact_us = 0;

/** Direction towards the sun; the faces' lighting uses the same one. */
static constexpr Terrain3D::Vec PROOF_SUN{-0.4f, -0.5f, 0.768f};

/** Fragment helper shared by the terrain and model programs: the fraction of a face's light left after its sun shadow.
 * The face normal comes from the screen derivatives of the world position, so flat-shaded faces need no normal input. */
static constexpr const char *PROOF_SUN_GLSL = R"GLSL(
uniform sampler2DShadow shadowMap; uniform mat4 shadowMatrix; uniform vec4 shadowInfo, shadowFade;
float sunLight(vec3 place, vec3 dx, vec3 dy) {
 if(shadowInfo.x<0.5) return 1.0;
 float strength=shadowInfo.y;
 if(shadowFade.w>0.0) strength*=1.0-smoothstep(0.75*shadowFade.w,shadowFade.w,distance(place,shadowFade.xyz));
 if(strength<=0.0) return 1.0;
 vec3 n=cross(dx,dy); float len=length(n);
 if(len<1e-8) return 1.0;
 n/=len; if(n.z<0.0) n=-n;
 float facing=dot(n,vec3(-0.4,-0.5,0.768));
 if(facing<=0.0) return 1.0;
 vec4 s=shadowMatrix*vec4(place+n*shadowInfo.z,1.0);
 if(s.x<=0.0||s.y<=0.0||s.x>=1.0||s.y>=1.0||s.z>=1.0) return 1.0;
 float lit=0.25*(textureLodOffset(shadowMap,s.xyz,0.0,ivec2(-1,-1))+textureLodOffset(shadowMap,s.xyz,0.0,ivec2(1,-1))
  +textureLodOffset(shadowMap,s.xyz,0.0,ivec2(-1,1))+textureLodOffset(shadowMap,s.xyz,0.0,ivec2(1,1)));
 float base=0.62+0.38*facing;
 return (base-0.38*facing*strength*(1.0-lit))/base;
}
)GLSL";

static void ProofShadowUniforms(const ProofLocations &locations)
{
	_glUniform1i(locations.shadowMap, 5);
	p_glUniformMatrix4fv(locations.shadowMatrix, 1, GL_FALSE, _proof_shadow_matrix.data());
	ProofUniform(locations.shadowInfo, _proof_shadow_info[0], _proof_shadow_info[1], _proof_shadow_info[2], _proof_shadow_info[3]);
	ProofUniform(locations.shadowFade, _proof_shadow_fade[0], _proof_shadow_fade[1], _proof_shadow_fade[2], _proof_shadow_fade[3]);
}

/** Compile a vertex and fragment shader pair; 0 if either fails. */
static GLuint ProofCompileProgram(std::initializer_list<const char *> vertex, std::initializer_list<const char *> fragment)
{
	GLuint vs = _glCreateShader(GL_VERTEX_SHADER), fs = _glCreateShader(GL_FRAGMENT_SHADER);
	std::vector<const char *> v(vertex), f(fragment);
	_glShaderSource(vs, GLsizei(v.size()), v.data(), nullptr); _glCompileShader(vs);
	_glShaderSource(fs, GLsizei(f.size()), f.data(), nullptr); _glCompileShader(fs);
	GLuint program = 0;
	if (VerifyShader(vs) && VerifyShader(fs)) {
		program = _glCreateProgram();
		_glAttachShader(program, vs); _glAttachShader(program, fs);
		_glBindFragDataLocation(program, 0, "pixel");
		_glLinkProgram(program);
		if (!VerifyProgram(program)) { _glDeleteProgram(program); program = 0; }
	}
	_glDeleteShader(vs); _glDeleteShader(fs);
	return program;
}

static bool CreateGroundTextures();

static bool InitProofProgram()
{
	if (_proof_program != 0) return true;
	const char *vertex = R"GLSL(#version 150
in vec3 position; in vec3 colour; in float transparent; in vec3 metadata; in vec4 world; in float layer;
uniform vec4 cameraA, cameraB, cameraC, selection, selectedTile, visibility;
uniform vec4 modelA, modelB, companyA, companyB;
uniform mat4 viewProjection; uniform vec4 eye, fog;
out vec3 tint; out float stipple; flat out int hidden; out vec2 worldPos; flat out vec2 faceTile; flat out float groundLayer; out float haze;
out vec3 shadowPlace; out float shadowReceive; flat out float obstruction;
// Perspective views (eye.w is the draw distance) fade into the fog colour before the draw distance cuts anything off.
void perspective(vec3 place) {
 gl_Position=viewProjection*vec4(place,1);
 haze=smoothstep(fog.w,eye.w*0.98,distance(place,eye.xyz));
}
void main() {
 tint=colour; stipple=mod(transparent,2.0); hidden=0; haze=0.0; obstruction=transparent>=2.0?1.0:0.0;
 worldPos=world.xy; faceTile=world.zw; groundLayer=layer;
 vec3 place=position;
 shadowReceive=1.0;
 if(cameraC.w>1.5) {
  groundLayer=-1.0;
  // Library model (metadata is the local face normal): AppendModel3D's placement and AddTerrainFace's light.
  vec4 a=modelA, b=modelB;
  vec3 primary=companyA.rgb, secondary=companyB.rgb;
  vec3 scale=vec3(a.w,1.0,b.z);
  vec3 p=position*scale, n=metadata/scale;
  place=vec3(a.x+p.x*b.x-p.y*b.y,a.y+p.x*b.y+p.y*b.x,a.z+p.z);
  n=vec3(n.x*b.x-n.y*b.y,n.x*b.y+n.y*b.x,n.z);
  if(n.z<0.0) n=-n;
  float light=0.62+0.38*max(0.0,dot(vec3(-0.4,-0.5,0.768),n)/length(n));
  tint=colour.r>1.5?colour-2.0:(colour.r<-1.5?secondary:colour.r<0.0?primary:colour)*light;
  if(colour.r>1.5) shadowReceive=0.0;
  stipple=b.w;
  if(cameraC.w>2.5) {
   vec2 tile=vec2(companyA.w,companyB.w);
   int option=int(modelB.w); int mask=1<<option;
   obstruction=option>=1 && option<=6 && option!=5 && (int(visibility.z)&mask)==0?1.0:0.0;
   stipple=(int(visibility.x)&mask)!=0?1.0:0.0;
   hidden=(int(visibility.y)&mask)!=0?1:0;
   bool selected=all(equal(tile,selectedTile.xy));
   if(selectedTile.z>1.5) {
    vec2 at=vec2(tile.x+tile.y,tile.x-tile.y);
    vec2 a=vec2(selection.x+selection.y,selection.x-selection.y), b=vec2(selection.z+selection.w,selection.z-selection.w);
    if(all(greaterThanEqual(at,min(a,b))) && all(lessThanEqual(at,max(a,b)))) selected=true;
   } else if(selectedTile.z>0.5 && tile.x>=selection.x && tile.y>=selection.y && tile.x<selection.z && tile.y<selection.w) selected=true;
   if(selected) { tint=selectedTile.w>0.5?vec3(0.9,0.2,0.15):vec3(0.95,0.80,0.22); shadowReceive=0.0; }
  }
 }
 shadowPlace=place;
 if(cameraC.w>0.5) {
  worldPos=place.xy; faceTile=metadata.xy;
  vec2 delta=place.xy-cameraA.xy;
  float x=delta.x*cameraB.x-delta.y*cameraB.y;
  float y=delta.x*cameraB.y+delta.y*cameraB.x;
  if(eye.w>0.0) perspective(place);
  else gl_Position=vec4(4.0*(y-x)*cameraA.z/cameraA.w,
    -2.0*((x+y)*cameraB.z-(place.z-cameraC.y))*cameraA.z/cameraB.w,
    -(x+y+2.0*cameraB.z*place.z)/cameraC.x,1);
  if(cameraC.w>1.5) return;
  int option=int(metadata.z); int mask=1<<option;
  obstruction=option>=1 && option<=6 && option!=5 && (int(visibility.z)&mask)==0?1.0:0.0;
  if((int(visibility.x)&mask)!=0) stipple=1;
  if((int(visibility.y)&mask)!=0) hidden=1;
  bool selected=all(equal(metadata.xy,selectedTile.xy));
  if(selectedTile.z>1.5) {
    vec2 at=vec2(metadata.x+metadata.y,metadata.x-metadata.y);
    vec2 a=vec2(selection.x+selection.y,selection.x-selection.y), b=vec2(selection.z+selection.w,selection.z-selection.w);
    if(all(greaterThanEqual(at,min(a,b))) && all(lessThanEqual(at,max(a,b)))) selected=true;
   } else if(selectedTile.z>0.5 && metadata.x>=selection.x && metadata.y>=selection.y && metadata.x<selection.z && metadata.y<selection.w) selected=true;
  if(selected) { tint=selectedTile.w>0.5?vec3(0.9,0.2,0.15):vec3(0.95,0.80,0.22); groundLayer=-1.0; shadowReceive=0.0; }
 } else if(eye.w>0.0) perspective(position); // CPU vertices of a perspective view stay in world space.
 else { gl_Position=vec4(position,1); shadowReceive=0.0; }
})GLSL";
	/* Ground: the texture repeats every two tiles in world X/Y and is scaled by the face's light, recovered
	 * from its tint (the layer mean times light). Within 0.3 tiles of an edge it blends with the
	 * neighbouring tile's layer from the layer map; a noise warp of the blend position hides the grid.
	 * Fields and concrete keep straight tile edges, as their classic sprites do.
	 * Every term depends only on the world position, so both sides of an edge agree. */
	const char *fragment = R"GLSL(
in vec3 tint; in float stipple; flat in int hidden; in vec2 worldPos; flat in vec2 faceTile; flat in float groundLayer; in float haze;
in vec3 shadowPlace; in float shadowReceive; flat in float obstruction;
uniform vec4 autoFocus;
uniform sampler2DArray groundTextures; uniform usampler2D groundMap; uniform vec4 groundInfo, groundCrisp, fog;
out vec4 pixel;
float hash(vec2 p){return fract(sin(dot(p,vec2(127.1,311.7)))*43758.5453);}
float valueNoise(vec2 p){vec2 i=floor(p),f=fract(p);f=f*f*(3.0-2.0*f);
 return mix(mix(hash(i),hash(i+vec2(1,0)),f.x),mix(hash(i+vec2(0,1)),hash(i+vec2(1,1)),f.x),f.y);}
void main(){
 vec2 uv=worldPos/32.0; vec2 du=dFdx(uv), dv=dFdy(uv);
 vec3 sx=dFdx(shadowPlace), sy=dFdy(shadowPlace);
 if(hidden!=0) discard;
 if(stipple>0.5 && mod(floor(gl_FragCoord.x)+floor(gl_FragCoord.y),4.0)>0.5) discard;
 // Only obstructing fragments nearer than the target fade. A soft border and a fixed screen pattern
 // avoid whole-object popping; depth and picking remain independent of presentation.
 if(shadowInfo.w<0.5 && obstruction>0.5 && autoFocus.w>0.0 && gl_FragCoord.z<autoFocus.z-0.000001) {
  float fade=0.84*(1.0-smoothstep(0.65,1.0,length(gl_FragCoord.xy-autoFocus.xy)/autoFocus.w));
  int x=int(gl_FragCoord.x)&3, y=int(gl_FragCoord.y)&3;
  int b=((x&1)*2+(y&1))*4+((x>>1)*2+(y>>1));
  if((float(b)+0.5)/16.0<fade) discard;
 }
 if(shadowInfo.w>0.5){pixel=vec4(1);return;} // Sun shadow depth pass.
 float sun=shadowReceive>0.5?sunLight(shadowPlace,sx,sy):1.0;
 if(groundLayer<0.0 || groundInfo.x<0.5){pixel=vec4(mix(tint*sun,fog.rgb,haze),1);return;}
 float own=mod(groundLayer,128.0);
 vec3 mean=textureLod(groundTextures,vec3(0.5,0.5,own),groundInfo.w).rgb;
 float light=dot(tint,vec3(1))/max(dot(mean,vec3(1)),0.01);
 vec3 colour;
 if(groundLayer>=128.0) colour=textureGrad(groundTextures,vec3(uv,own),du,dv).rgb;
 else {
  vec2 warp=vec2(valueNoise(worldPos*0.19),valueNoise(worldPos*0.19+vec2(17,5)))-0.5;
  warp+=0.5*(vec2(valueNoise(worldPos*0.47+vec2(3,11)),valueNoise(worldPos*0.47+vec2(29,13)))-0.5);
  vec2 t=worldPos/16.0, q=t+warp*0.6, ownTile=faceTile/16.0;
  vec3 ownColour=textureGrad(groundTextures,vec3(uv,own),du,dv).rgb;
  /* Soft layers blend at the warped position; crisp layers (fields, concrete) keep their tile edges and win over soft ones. */
  vec3 soft=0.02*ownColour, crisp=vec3(0); float softTotal=0.02, crispTotal=0.0;
  for(int i=0;i<9;++i){
   vec2 cell=floor(t)+vec2(float(i%3),float(i/3))-1.0;
   float layer=own;
   if(any(notEqual(cell,ownTile)) && all(greaterThanEqual(cell,vec2(0))) && all(lessThan(cell,groundInfo.yz))){
    uint value=texelFetch(groundMap,ivec2(cell),0).r;
    if(value<128u) layer=float(value);
   }
   bool hard=(layer>=groundCrisp.x && layer<=groundCrisp.y) || layer==groundCrisp.z;
   vec2 p=hard?t:q;
   float weight=1.0-smoothstep(0.0,hard?0.04:0.3,length(max(max(cell-p,p-cell-1.0),0.0)));
   if(weight<=0.0) continue;
   vec3 c=layer==own?ownColour:textureGrad(groundTextures,vec3(uv,layer),du,dv).rgb;
   if(hard){crisp+=weight*c; crispTotal+=weight;} else {soft+=weight*c; softTotal+=weight;}
  }
  colour=mix(soft/softTotal,crisp/max(crispTotal,0.0001),clamp(crispTotal,0.0,1.0));
 }
 pixel=vec4(mix(colour*light*sun,fog.rgb,haze),1);
})GLSL";
	_proof_program = ProofCompileProgram({vertex}, {"#version 150\n", PROOF_SUN_GLSL, fragment});
	if (_proof_program == 0) return false;
	_proof_locations.cameraA = _glGetUniformLocation(_proof_program, "cameraA");
	_proof_locations.cameraB = _glGetUniformLocation(_proof_program, "cameraB");
	_proof_locations.cameraC = _glGetUniformLocation(_proof_program, "cameraC");
	_proof_locations.selection = _glGetUniformLocation(_proof_program, "selection");
	_proof_locations.selectedTile = _glGetUniformLocation(_proof_program, "selectedTile");
	_proof_locations.visibility = _glGetUniformLocation(_proof_program, "visibility");
	_proof_locations.autoFocus = _glGetUniformLocation(_proof_program, "autoFocus");
	_proof_locations.viewProjection = _glGetUniformLocation(_proof_program, "viewProjection");
	_proof_locations.eye = _glGetUniformLocation(_proof_program, "eye");
	_proof_locations.fog = _glGetUniformLocation(_proof_program, "fog");
	_proof_locations.modelA = _glGetUniformLocation(_proof_program, "modelA");
	_proof_locations.modelB = _glGetUniformLocation(_proof_program, "modelB");
	_proof_locations.companyA = _glGetUniformLocation(_proof_program, "companyA");
	_proof_locations.companyB = _glGetUniformLocation(_proof_program, "companyB");
	_proof_locations.groundTextures = _glGetUniformLocation(_proof_program, "groundTextures");
	_proof_locations.groundMap = _glGetUniformLocation(_proof_program, "groundMap");
	_proof_locations.groundInfo = _glGetUniformLocation(_proof_program, "groundInfo");
	_proof_locations.groundCrisp = _glGetUniformLocation(_proof_program, "groundCrisp");
	_proof_locations.shadowMap = _glGetUniformLocation(_proof_program, "shadowMap");
	_proof_locations.shadowMatrix = _glGetUniformLocation(_proof_program, "shadowMatrix");
	_proof_locations.shadowInfo = _glGetUniformLocation(_proof_program, "shadowInfo");
	_proof_locations.shadowFade = _glGetUniformLocation(_proof_program, "shadowFade");
	const char *vertices[] = {"position", "colour", "transparent", "metadata", "world", "layer"};
	for (size_t i = 0; i < _proof_locations.vertex.size(); ++i) _proof_locations.vertex[i] = _glGetAttribLocation(_proof_program, vertices[i]);
	_glGenVertexArrays(1, &_proof_vao);
	_glGenBuffers(1, &_proof_vbo);
	_glBindVertexArray(_proof_vao);
	_glBindBuffer(GL_ARRAY_BUFFER, _proof_vbo);
	BindProofVertices(_proof_vbo);
	p_glGenFramebuffers(1, &_proof_fbo);
	_glGenTextures(1, &_proof_colour);
	p_glGenRenderbuffers(1, &_proof_depth);
	if (p_glGenQueries != nullptr) p_glGenQueries(GLsizei(_proof_queries.size()), _proof_queries.data());
	_ground_textured = CreateGroundTextures();
	Debug(driver, 1, "3D ground: {}", _ground_textured ? "textured" : "flat colours");
	return true;
}

} // namespace
#include "terrain3d_impl.h"
namespace {

/** The camera an image was rendered with, from its key. */
static Terrain3D::Camera KeyTerrainCamera(const std::array<int, 28> &key)
{
	auto bits = [&](int i) { return std::bit_cast<float>(key[i]); };
	TerrainAngle view{bits(8), bits(9), bits(10), {bits(11), bits(12), bits(13)}, bits(14), bits(15), bits(16), {bits(18), bits(19), bits(20)}, key[17] != 0, bits(21)};
	view.magnify = bits(27);
	return TerrainCamera(key[0], key[1], key[2], key[3], key[4], view);
}

/** Pixel bounds of a world box in a view, as x0, y0, x1, y1 with rows from the top. */
static std::array<float, 4> ProofScreenBounds(const Terrain3D::Camera &camera, Terrain3D::Vec low, Terrain3D::Vec high)
{
	std::array<float, 4> bounds{FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
	for (int i = 0; i < 8; ++i) {
		auto p = camera.Project({i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z});
		bounds = {std::min(bounds[0], p.x), std::min(bounds[1], p.y), std::max(bounds[2], p.x), std::max(bounds[3], p.y)};
	}
	return bounds;
}

/** Whether a change can redraw only the cells of this image it covers: a direct orthographic view without
 * a focus outline or sun shadows, whose passes all keep to the stencil of the cells. */
static bool ProofPartialAllowed(const ProofImage &image)
{
	return _proof_partial && image.texture != 0 && image.key[7] != 0 && image.key[22] == 0 && _proof_shadows != 2 && !KeyTerrainCamera(image.key).Perspective();
}

/** Things changed inside these world boxes: drop only the cached views whose camera can see one, and
 * redraw only the on-screen views that see one. Views that see none keep their images; a view that can
 * be drawn again in part keeps its image too, with the cells the changes cover marked stale. */
static void InvalidateProof3DRegions(std::span<const ProofBox> boxes)
{
	if (boxes.empty()) return;
	/* Past this many boxes, testing them costs more than re-rendering. */
	if (!_proof_scoped || !_proof_optimized || boxes.size() > 8192) { InvalidateProof3DImages(); return; }
	_proof_scoped_boxes += boxes.size();
	/* Shadows reach past what changed: contact shadows by a few units, sun shadows away from the sun by up to its height and more downhill. */
	std::vector<ProofBox> shaded;
	if (_proof_shadows != 0) {
		shaded.assign(boxes.begin(), boxes.end());
		for (auto &[low, high] : shaded) {
			float reach = 4 + (_proof_shadows == 2 ? (high.z - low.z) + 32 : 0);
			low.x -= reach; low.y -= reach; low.z -= 8; high.x += reach; high.y += reach;
		}
		boxes = shaded;
	}
	auto sees = [&](const Terrain3D::Camera &camera) {
		return std::ranges::any_of(boxes, [&](const ProofBox &box) { return Terrain3D::Visible(box.first, box.second, camera); });
	};
	for (auto it = _proof_images.begin(); it != _proof_images.end();) {
		auto camera = KeyTerrainCamera(it->key);
		if (sees(camera)) {
			if (ProofPartialAllowed(*it)) {
				/* Keep the image; the cells the changes cover are drawn again before it is next used. */
				if (it->stale.Empty()) it->stale = Terrain3D::DirtyCells(it->key[0], it->key[1]);
				for (const auto &[low, high] : boxes) {
					if (!Terrain3D::Visible(low, high, camera)) continue;
					auto [x0, y0, x1, y1] = ProofScreenBounds(camera, low, high);
					it->stale.Mark(x0, y0, x1, y1);
				}
				if (it->stale.Fraction() <= PROOF_PARTIAL_LIMIT) {
					++it;
					++_proof_partial_kept;
					continue;
				}
			}
			ReleaseProofImage(*it);
			it = _proof_images.erase(it);
			++_proof_scoped_drops;
		} else {
			++it;
			++_proof_scoped_kept;
		}
	}
	/* Also test the views themselves: one may be on screen without a cached image after eviction. */
	bool direct = _proof_layer_active && ProofDirectMode();
	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr) continue;
		const Viewport &vp = *w->viewport;
		if (!sees(ViewportTerrainCamera(vp))) continue;
		if (direct) return;
		SetDirtyBlocks(vp.left, vp.top, vp.left + vp.width, vp.top + vp.height);
	}
}

/** Upload the embedded layers with mipmaps; false keeps the flat-colour ground. */
static bool CreateGroundTextures()
{
	if (p_glTexImage3D == nullptr || p_glGenerateMipmap == nullptr) return false;
	_glGetError();
	_glActiveTexture(GL_TEXTURE1);
	_glGenTextures(1, &_ground_textures);
	_glBindTexture(GL_TEXTURE_2D_ARRAY, _ground_textures);
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 4); _glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	p_glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, Ground3D::TEXTURE_SIZE, Ground3D::TEXTURE_SIZE, Ground3D::TEXTURE_LAYERS, 0, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, Ground3D::texels);
	p_glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
	_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
	_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
	_glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
	_glActiveTexture(GL_TEXTURE2);
	_glGenTextures(1, &_ground_map_texture);
	_glBindTexture(GL_TEXTURE_2D, _ground_map_texture);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glActiveTexture(GL_TEXTURE0);
	_ground_map_generation = 0;
	GLenum error = _glGetError();
	if (error == GL_NO_ERROR) return true;
	Debug(driver, 0, "3D ground textures unavailable (OpenGL error {}); using flat colours", error);
	return false;
}

/** Copy changed tiles of the layer map to the GPU. Maps larger than the texture limit draw without neighbour blending. */
static void UploadGroundMap()
{
	_glActiveTexture(GL_TEXTURE2);
	_glBindTexture(GL_TEXTURE_2D, _ground_map_texture);
	/* The classic driver leaves GL_UNPACK_ROW_LENGTH at the screen pitch; a full upload with it reads past the map. */
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	bool full = _ground_map_generation != _terrain_generation;
	if (full) {
		GLint limit = 0;
		_glGetIntegerv(GL_MAX_TEXTURE_SIZE, &limit);
		bool fits = _terrain_map_x <= limit && _terrain_map_y <= limit;
		_ground_map_width = fits ? _terrain_map_x : 0;
		_ground_map_height = fits ? _terrain_map_y : 0;
		if (fits) _glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, _terrain_map_x, _terrain_map_y, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, _ground_map.data());
		_ground_map_generation = _terrain_generation;
		if (fits) _ground_map_upload_bytes += _ground_map.size();
	} else if (_ground_map_width > 0 && _ground_dirty_x1 >= _ground_dirty_x0) {
		int w = _ground_dirty_x1 - _ground_dirty_x0 + 1, h = _ground_dirty_y1 - _ground_dirty_y0 + 1;
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, _terrain_map_x);
		_glTexSubImage2D(GL_TEXTURE_2D, 0, _ground_dirty_x0, _ground_dirty_y0, w, h, GL_RED_INTEGER, GL_UNSIGNED_BYTE, _ground_map.data() + TileXY(_ground_dirty_x0, _ground_dirty_y0).base());
		_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		_ground_map_upload_bytes += static_cast<uint64_t>(w) * h;
	}
	_ground_dirty_x0 = _ground_dirty_y0 = 0;
	_ground_dirty_x1 = _ground_dirty_y1 = -1;
	_glActiveTexture(GL_TEXTURE1);
	_glBindTexture(GL_TEXTURE_2D_ARRAY, _ground_textures);
	_glActiveTexture(GL_TEXTURE0);
}

/** Keep all live window viewports plus any view requested this frame. */
static void TrimTerrainMeshes()
{
	if (!_proof_deferred) return;
	size_t total = 0;
	for (const auto &chunk : _terrain_chunks) total += chunk.faces.capacity() * sizeof(TerrainFace) + chunk.spans.capacity() * sizeof(Terrain3D::TileMeshSpan) + chunk.instances.capacity() * sizeof(Static3DInstance) + chunk.instance_spans.capacity() * sizeof(Terrain3D::TileMeshSpan);
	for (const auto &gpu : _proof_chunks) total += gpu.count * sizeof(ProofStaticVertex);
	/* Nothing can be over the budget below its floor; skip finding the views. */
	if (total <= Terrain3D::MeshBudget(0)) return;
	std::vector<Terrain3D::Camera> cameras;
	for (const Window *w : Window::Iterate()) if (w->viewport != nullptr) {
		const auto &vp = *w->viewport;
		cameras.push_back(ViewportTerrainCamera(vp));
	}
	std::vector<Terrain3D::MeshCacheEntry> entries;
	size_t pinned_bytes = 0;
	for (size_t i = 0; i < _terrain_chunks.size(); ++i) {
		const auto &chunk = _terrain_chunks[i];
		bool pinned = chunk.used == _proof_frame_serial || std::any_of(cameras.begin(), cameras.end(), [&](const auto &c) { return Terrain3D::Visible(chunk.low, chunk.high, c); });
		size_t bytes = chunk.faces.capacity() * sizeof(TerrainFace) + chunk.spans.capacity() * sizeof(Terrain3D::TileMeshSpan) + chunk.instances.capacity() * sizeof(Static3DInstance) + chunk.instance_spans.capacity() * sizeof(Terrain3D::TileMeshSpan);
		if (i < _proof_chunks.size()) bytes += _proof_chunks[i].count * sizeof(ProofStaticVertex);
		entries.push_back({bytes, chunk.used, pinned});
		if (pinned) pinned_bytes += bytes;
	}
	// Combined CPU geometry + retained GPU storage budget, scaled to the visible set. Snapshots are kept independently.
	for (size_t i : Terrain3D::MeshEvictions(entries, Terrain3D::MeshBudget(pinned_bytes))) {
		auto &chunk = _terrain_chunks[i];
		std::vector<TerrainFace>().swap(chunk.faces);
		std::vector<Static3DInstance>().swap(chunk.instances);
		std::vector<Terrain3D::TileMeshSpan>().swap(chunk.instance_spans);
		std::vector<Terrain3D::TileMeshSpan>().swap(chunk.spans);
		chunk.resident = false;
		if (i < _proof_chunks.size()) {
			auto &gpu = _proof_chunks[i];
			if (gpu.buffer) _glDeleteBuffers(1, &gpu.buffer);
			gpu = {};
		}
		++_proof_evictions;
	}
}

/** All inputs are copied numbers. No game state is written and no GPU work is queued past this call. */
/** Upload every Art3D and Asset3D model once; the metadata slot carries the local face normal. */
static void LoadProofModels()
{
	static bool loaded_lod = false;
	if (_proof_models != 0 && loaded_lod == _vehicle3d_lod) return;
	if (_proof_models != 0) _glDeleteBuffers(1, &_proof_models);
	loaded_lod = _vehicle3d_lod;
	std::vector<ProofModelVertex> upload;
	size_t total = FullVehicleModelCount3D() + (_vehicle3d_lod ? std::size(ArtLOD3D::models) : 0);
	_proof_model_ranges.assign(total, {});
	for (size_t index = 0; index < total; ++index) {
		const auto &model = VehicleModel3D(index);
		auto &range = _proof_model_ranges[index];
		range.first = static_cast<GLint>(upload.size());
		range.low = 1e30f; range.high = -1e30f;
		range.footprint = {1e30f, -1e30f, 1e30f, -1e30f};
		for (size_t i = 0; i < model.count; ++i) {
			const auto &p = model.faces[i].p;
			const auto &c = model.faces[i].colour;
			float ax = p[1][0]-p[0][0], ay = p[1][1]-p[0][1], az = p[1][2]-p[0][2];
			float bx = p[2][0]-p[0][0], by = p[2][1]-p[0][1], bz = p[2][2]-p[0][2];
			float nx = ay*bz-az*by, ny = az*bx-ax*bz, nz = ax*by-ay*bx;
			if (std::sqrt(nx*nx+ny*ny+nz*nz) < 0.0001f) continue;
			for (const auto &v : p) {
				upload.push_back({v[0], v[1], v[2], c[0], c[1], c[2], nx, ny, nz});
				range.radius = std::max(range.radius, std::sqrt(v[0]*v[0]+v[1]*v[1]));
				range.low = std::min(range.low, v[2]); range.high = std::max(range.high, v[2]);
				range.footprint = {std::min(range.footprint[0], v[0]), std::max(range.footprint[1], v[0]), std::min(range.footprint[2], v[1]), std::max(range.footprint[3], v[1])};
			}
		}
		range.count = static_cast<GLsizei>(upload.size()) - range.first;
	}
	_glGenBuffers(1, &_proof_models);
	_glBindBuffer(GL_ARRAY_BUFFER, _proof_models);
	_glBufferData(GL_ARRAY_BUFFER, upload.size() * sizeof(ProofModelVertex), upload.data(), GL_STATIC_DRAW);
	_proof_model_bytes = upload.size() * sizeof(ProofModelVertex);
	_proof_upload_bytes += upload.size() * sizeof(ProofModelVertex);
	_proof_static_upload_bytes += upload.size() * sizeof(ProofModelVertex);
}

/** Keep instanced attributes out of the terrain program, which processes most vertices on dense maps. */
static bool InitProofInstanceProgram()
{
	if (_proof_instance_failed) return false;
	if (_proof_instance_program != 0) return true;
	const char *vertex = R"GLSL(#version 150
in vec3 position, colour, metadata;
in vec4 instanceModelA, instanceModelB, instanceCompanyA, instanceCompanyB;
uniform vec4 cameraA, cameraB, cameraC, selection, selectedTile, visibility;
uniform mat4 viewProjection; uniform vec4 eye, fog;
out vec3 tint; out float stipple; flat out int hidden; out float haze;
out vec3 shadowPlace; out float shadowReceive; flat out float obstruction;
void main() {
 hidden=0; haze=0.0; obstruction=0.0;
 vec4 a=instanceModelA, b=instanceModelB;
 vec3 scale=vec3(a.w,1.0,b.z);
 vec3 p=position*scale, n=metadata/scale;
 vec3 place=vec3(a.x+p.x*b.x-p.y*b.y,a.y+p.x*b.y+p.y*b.x,a.z+p.z);
 n=vec3(n.x*b.x-n.y*b.y,n.x*b.y+n.y*b.x,n.z);
 if(n.z<0.0) n=-n;
 float light=0.62+0.38*max(0.0,dot(vec3(-0.4,-0.5,0.768),n)/length(n));
 tint=colour.r>1.5?colour-2.0:(colour.r<-1.5?instanceCompanyB.rgb:colour.r<0.0?instanceCompanyA.rgb:colour)*light;
 shadowPlace=place; shadowReceive=colour.r>1.5?0.0:1.0;
 stipple=b.w;
  if(cameraC.w>2.5) {
   vec2 tile=vec2(instanceCompanyA.w,instanceCompanyB.w);
   int option=int(instanceModelB.w); int mask=1<<option;
   obstruction=option>=1 && option<=6 && option!=5 && (int(visibility.z)&mask)==0?1.0:0.0;
   stipple=(int(visibility.x)&mask)!=0?1.0:0.0;
   hidden=(int(visibility.y)&mask)!=0?1:0;
   bool selected=all(equal(tile,selectedTile.xy));
   if(selectedTile.z>1.5) {
    vec2 at=vec2(tile.x+tile.y,tile.x-tile.y);
    vec2 a=vec2(selection.x+selection.y,selection.x-selection.y), b=vec2(selection.z+selection.w,selection.z-selection.w);
    if(all(greaterThanEqual(at,min(a,b))) && all(lessThanEqual(at,max(a,b)))) selected=true;
   } else if(selectedTile.z>0.5 && tile.x>=selection.x && tile.y>=selection.y && tile.x<selection.z && tile.y<selection.w) selected=true;
   if(selected) { tint=selectedTile.w>0.5?vec3(0.9,0.2,0.15):vec3(0.95,0.80,0.22); shadowReceive=0.0; }
  }
 if(eye.w>0.0) {
  gl_Position=viewProjection*vec4(place,1);
  haze=smoothstep(fog.w,eye.w*0.98,distance(place,eye.xyz));
  return;
 }
 vec2 delta=place.xy-cameraA.xy;
 float x=delta.x*cameraB.x-delta.y*cameraB.y;
 float y=delta.x*cameraB.y+delta.y*cameraB.x;
 gl_Position=vec4(4.0*(y-x)*cameraA.z/cameraA.w,
   -2.0*((x+y)*cameraB.z-(place.z-cameraC.y))*cameraA.z/cameraB.w,
   -(x+y+2.0*cameraB.z*place.z)/cameraC.x,1);
})GLSL";
	const char *fragment = R"GLSL(
in vec3 tint; in float stipple; flat in int hidden; in float haze; in vec3 shadowPlace; in float shadowReceive; flat in float obstruction;
uniform vec4 autoFocus;
uniform vec4 fog; out vec4 pixel;
void main() {
 vec3 sx=dFdx(shadowPlace), sy=dFdy(shadowPlace);
 if(hidden!=0) discard;
 if(stipple>0.5 && mod(floor(gl_FragCoord.x)+floor(gl_FragCoord.y),4.0)>0.5) discard;
 // Only obstructing fragments nearer than the target fade. A soft border and a fixed screen pattern
 // avoid whole-object popping; depth and picking remain independent of presentation.
 if(shadowInfo.w<0.5 && obstruction>0.5 && autoFocus.w>0.0 && gl_FragCoord.z<autoFocus.z-0.000001) {
  float fade=0.84*(1.0-smoothstep(0.65,1.0,length(gl_FragCoord.xy-autoFocus.xy)/autoFocus.w));
  int x=int(gl_FragCoord.x)&3, y=int(gl_FragCoord.y)&3;
  int b=((x&1)*2+(y&1))*4+((x>>1)*2+(y>>1));
  if((float(b)+0.5)/16.0<fade) discard;
 }
 if(shadowInfo.w>0.5){pixel=vec4(1);return;} // Sun shadow depth pass.
 float sun=shadowReceive>0.5?sunLight(shadowPlace,sx,sy):1.0;
 pixel=vec4(mix(tint*sun,fog.rgb,haze),1);
})GLSL";
	_proof_instance_program = ProofCompileProgram({vertex}, {"#version 150\n", PROOF_SUN_GLSL, fragment});
	if (_proof_instance_program == 0) {
		_proof_instance_failed = true;
		Debug(driver, 0, "3D instance program unavailable; using uniform draws");
		return false;
	}
	_proof_instance_locations.selection = _glGetUniformLocation(_proof_instance_program, "selection");
	_proof_instance_locations.selectedTile = _glGetUniformLocation(_proof_instance_program, "selectedTile");
	_proof_instance_locations.visibility = _glGetUniformLocation(_proof_instance_program, "visibility");
	_proof_instance_locations.autoFocus = _glGetUniformLocation(_proof_instance_program, "autoFocus");
	_proof_instance_locations.cameraA = _glGetUniformLocation(_proof_instance_program, "cameraA");
	_proof_instance_locations.cameraB = _glGetUniformLocation(_proof_instance_program, "cameraB");
	_proof_instance_locations.cameraC = _glGetUniformLocation(_proof_instance_program, "cameraC");
	_proof_instance_locations.viewProjection = _glGetUniformLocation(_proof_instance_program, "viewProjection");
	_proof_instance_locations.eye = _glGetUniformLocation(_proof_instance_program, "eye");
	_proof_instance_locations.fog = _glGetUniformLocation(_proof_instance_program, "fog");
	_proof_instance_locations.shadowMap = _glGetUniformLocation(_proof_instance_program, "shadowMap");
	_proof_instance_locations.shadowMatrix = _glGetUniformLocation(_proof_instance_program, "shadowMatrix");
	_proof_instance_locations.shadowInfo = _glGetUniformLocation(_proof_instance_program, "shadowInfo");
	_proof_instance_locations.shadowFade = _glGetUniformLocation(_proof_instance_program, "shadowFade");
	const char *vertices[] = {"position", "colour", "transparent", "metadata", "world", "layer"};
	const char *instances[] = {"instanceModelA", "instanceModelB", "instanceCompanyA", "instanceCompanyB"};
	for (size_t i = 0; i < _proof_instance_locations.vertex.size(); ++i) _proof_instance_locations.vertex[i] = _glGetAttribLocation(_proof_instance_program, vertices[i]);
	for (size_t i = 0; i < _proof_instance_locations.instance.size(); ++i) _proof_instance_locations.instance[i] = _glGetAttribLocation(_proof_instance_program, instances[i]);
	return true;
}

/** Submit an ordered model stream. All instance attributes are restored before returning. */
static void SubmitProofInstances(const Terrain3D::Camera &camera, float extent, int width, int height, float mode, uint64_t &draws, const ProofStaticStream *retained = nullptr)
{
	if (retained != nullptr ? retained->count == 0 : _proof_instance_upload.empty()) return;
	_glUseProgram(_proof_instance_program);
	ProofUniform(_proof_instance_locations.cameraA, camera.cx, camera.cy, camera.scale, width);
	ProofUniform(_proof_instance_locations.cameraB, camera.cosine, camera.sine, camera.tilt, height);
	ProofUniform(_proof_instance_locations.cameraC, extent * 4, camera.pz, 0, mode);
	ProofPerspectiveUniforms(_proof_instance_locations, camera);
	ProofShadowUniforms(_proof_instance_locations);
	ProofUniform(_proof_instance_locations.autoFocus, _proof_focus[0], _proof_focus[1], _proof_focus[2], _proof_focus[3]);
	ProofUniform(_proof_instance_locations.selection, _thd.pos.x, _thd.pos.y, _thd.pos.x + _thd.size.x, _thd.pos.y + _thd.size.y);
	bool rail_selection = (_thd.drawstyle & HT_DRAG_MASK) == HT_LINE || (_thd.drawstyle & HT_RAIL);
	ProofUniform(_proof_instance_locations.selectedTile, _terrain_highlight < 0 ? -1.0f : float(TileX(TileIndex(_terrain_highlight)) * TILE_SIZE),
		_terrain_highlight < 0 ? -1.0f : float(TileY(TileIndex(_terrain_highlight)) * TILE_SIZE), !rail_selection && _thd.drawstyle != HT_NONE ? (_thd.diagonal ? 2 : 1) : 0, false);
	ProofUniform(_proof_instance_locations.visibility, _game_mode == GameMode::Menu ? 0 : _transparency_opt.base(), _game_mode == GameMode::Menu ? 0 : (_transparency_opt & _invisibility_opt).base(), _transparency_lock.base(), 0);
	BindProofModelVertices(_proof_instance_locations);
	if (retained != nullptr) {
		_glBindBuffer(GL_ARRAY_BUFFER, retained->buffer);
	} else {
		if (_proof_instances == 0) _glGenBuffers(1, &_proof_instances);
		_glBindBuffer(GL_ARRAY_BUFFER, _proof_instances);
		/* Orphan the moving stream: a previous view may still be reading it. */
		auto stream_start = std::chrono::steady_clock::now();
		size_t bytes = _proof_instance_upload.size() * sizeof(ProofInstance);
		_glBufferData(GL_ARRAY_BUFFER, bytes, _proof_instance_upload.data(), GL_STREAM_DRAW);
		_proof_instance_stream_us += ProofMicros(stream_start);
		_proof_upload_bytes += bytes;
		_proof_instance_upload_bytes += bytes;
	}
	for (GLint location : _proof_instance_locations.instance) {
		_glEnableVertexAttribArray(location);
		p_glVertexAttribDivisor(location, 1);
	}
	for (const auto &batch : retained != nullptr ? retained->batches : _proof_instance_batches) {
		for (size_t i = 0; i < _proof_instance_locations.instance.size(); ++i) {
			GLint location = _proof_instance_locations.instance[i];
			_glVertexAttribPointer(location, 4, GL_FLOAT, GL_FALSE, sizeof(ProofInstance), reinterpret_cast<void *>(batch.first * sizeof(ProofInstance) + i * sizeof(std::array<float, 4>)));
		}
		const auto &range = _proof_model_ranges[batch.model];
		p_glDrawArraysInstanced(GL_TRIANGLES, range.first, range.count, batch.count);
		++draws;
	}
	/* This VAO also draws terrain and CPU previews; do not leave per-instance fetches enabled. */
	for (GLint location : _proof_instance_locations.instance) {
		_glDisableVertexAttribArray(location);
		p_glVertexAttribDivisor(location, 0);
	}
	_glUseProgram(_proof_program);
}

static ProofInstance ProofStaticPlacement(const Static3DInstance &v)
{
	return {{v.origin.x, v.origin.y, v.origin.z, v.length}, {v.dx, v.dy, v.height, float(v.transparency)},
		{v.company[0][0], v.company[0][1], v.company[0][2], float(TileX(v.tile) * TILE_SIZE)},
		{v.company[1][0], v.company[1][1], v.company[1][2], float(TileY(v.tile) * TILE_SIZE)}};
}

/** Retain exact model-grouped placements per visible chunk set. Chunk changes are
 * compared per model, then only affected model slots are gathered and uploaded.
 * Variable counts use the same bounded slot allocator as terrain vertices. */
static bool DrawProofStaticStream(const Terrain3D::Camera &camera, float extent, int width, int height)
{
	auto group_start = std::chrono::steady_clock::now();
	if (_proof_stream_generation != _terrain_generation) {
		ClearProofStaticStreams();
		_proof_stream_generation = _terrain_generation;
	}
	std::vector<ProofStaticChunkStream> chunks;
	for (size_t i = 0; i < _terrain_chunks.size(); ++i) {
		const auto &chunk = _terrain_chunks[i];
		if (chunk.resident && Terrain3D::Visible(chunk.low, chunk.high, camera)) chunks.push_back({i, chunk.instance_revision, {}, {}});
	}
	auto found = std::find_if(_proof_static_streams.begin(), _proof_static_streams.end(), [&](const auto &s) {
		return s.chunks.size() == chunks.size() && std::equal(s.chunks.begin(), s.chunks.end(), chunks.begin(),
			[](const auto &a, const auto &b) { return a.index == b.index; });
	});
	bool fresh = found == _proof_static_streams.end();
	ProofStaticStream stream;
	if (!fresh) {
		stream = std::move(*found);
		_proof_static_streams.erase(found);
	}
	const size_t models = FullVehicleModelCount3D();
	std::vector<size_t> counts(models, 0);
	std::vector<bool> changed_models(models, fresh);
	if (!fresh) for (size_t m = 0; m < models; ++m) counts[m] = stream.slots.count[m];
	static std::vector<std::vector<const Static3DInstance *>> grouped;
	grouped.resize(models);
	for (size_t i = 0; i < chunks.size(); ++i) {
		if (!fresh && stream.chunks[i].revision == chunks[i].revision) continue;
		for (auto &group : grouped) group.clear();
		for (const auto &v : _terrain_chunks[chunks[i].index].instances) grouped[v.model_index].push_back(&v);
		auto &next = chunks[i];
		for (size_t m = 0; m < models; ++m) {
			if (grouped[m].empty()) continue;
			next.spans.push_back({m, next.data.size(), static_cast<GLsizei>(grouped[m].size())});
			for (const auto *v : grouped[m]) next.data.push_back(ProofStaticPlacement(*v));
		}
		/* Exact comparisons avoid invalidating every tree model in a chunk when
		 * only one tree grows. Hash matches are not treated as proof of equality. */
		size_t old_span = 0, new_span = 0;
		for (size_t m = 0; m < models; ++m) {
			const ProofInstanceBatch *before = !fresh && old_span < stream.chunks[i].spans.size() && stream.chunks[i].spans[old_span].model == m ? &stream.chunks[i].spans[old_span++] : nullptr;
			const ProofInstanceBatch *after = new_span < next.spans.size() && next.spans[new_span].model == m ? &next.spans[new_span++] : nullptr;
			if (before == nullptr && after == nullptr) continue;
			size_t old_count = before == nullptr ? 0 : before->count, new_count = after == nullptr ? 0 : after->count;
			counts[m] = counts[m] - old_count + new_count;
			if (old_count != new_count || (old_count != 0 && !std::equal(stream.chunks[i].data.begin() + before->first,
					stream.chunks[i].data.begin() + before->first + old_count, next.data.begin() + after->first))) changed_models[m] = true;
		}
		if (!fresh) stream.chunks[i] = std::move(next);
	}
	if (fresh) stream.chunks = std::move(chunks);
	std::vector<size_t> writes;
	bool whole = stream.slots.Update(counts, fresh ? std::vector<bool>{} : changed_models, writes);
	stream.batches.clear();
	stream.count = 0;
	for (size_t m = 0; m < models; ++m) if (counts[m] != 0) {
		stream.batches.push_back({m, static_cast<size_t>(stream.slots.first[m]), stream.slots.count[m]});
		stream.count += counts[m];
	}
	stream.bytes = stream.slots.size * sizeof(ProofInstance) + sizeof(ProofStaticStream) + stream.chunks.capacity() * sizeof(ProofStaticChunkStream) + stream.batches.capacity() * sizeof(ProofInstanceBatch);
	stream.bytes += (stream.slots.first.capacity() + stream.slots.count.capacity() + stream.slots.capacity.capacity()) * sizeof(int);
	for (const auto &chunk : stream.chunks) stream.bytes += chunk.spans.capacity() * sizeof(ProofInstanceBatch) + chunk.data.capacity() * sizeof(ProofInstance);
	/* Account both CPU placements and GPU slot capacity. Oversized views use
	 * transient submission, and no retained stream borrows evictable mesh memory. */
	if (stream.bytes > PROOF_STREAM_BUDGET) {
		if (stream.buffer) _glDeleteBuffers(1, &stream.buffer);
		_proof_static_group_us += ProofMicros(group_start);
		return false;
	}
	while (!_proof_static_streams.empty() && (_proof_static_streams.size() >= 16 || ProofStaticStreamBytes() + stream.bytes > PROOF_STREAM_BUDGET)) {
		auto oldest = std::min_element(_proof_static_streams.begin(), _proof_static_streams.end(), [](const auto &a, const auto &b) { return a.used < b.used; });
		if (oldest->buffer) _glDeleteBuffers(1, &oldest->buffer);
		_proof_static_streams.erase(oldest);
		++_proof_stream_evictions;
	}
	/* Copy only written model ranges, in original chunk/placement order. */
	static std::vector<std::vector<ProofInstance>> upload;
	upload.resize(models);
	std::vector<bool> write_model(models, false);
	for (size_t m : writes) { upload[m].clear(); write_model[m] = true; }
	for (const auto &chunk : stream.chunks) for (const auto &span : chunk.spans) if (write_model[span.model]) {
		upload[span.model].insert(upload[span.model].end(), chunk.data.begin() + span.first, chunk.data.begin() + span.first + span.count);
	}
	_proof_static_group_us += ProofMicros(group_start);
	auto upload_start = std::chrono::steady_clock::now();
	if (stream.slots.size != 0 && (whole || !writes.empty())) {
		if (stream.buffer == 0) _glGenBuffers(1, &stream.buffer);
		_glBindBuffer(GL_ARRAY_BUFFER, stream.buffer);
		if (whole) _glBufferData(GL_ARRAY_BUFFER, stream.slots.size * sizeof(ProofInstance), nullptr, GL_DYNAMIC_DRAW);
		for (size_t m : writes) {
			size_t bytes = upload[m].size() * sizeof(ProofInstance);
			if (bytes == 0) continue;
			_glBufferSubData(GL_ARRAY_BUFFER, stream.slots.first[m] * sizeof(ProofInstance), bytes, upload[m].data());
			_proof_upload_bytes += bytes;
			_proof_instance_upload_bytes += bytes;
		}
	}
	_proof_instance_stream_us += ProofMicros(upload_start);
	bool patched = !whole && !writes.empty();
	if (whole) ++_proof_stream_builds; else if (patched) ++_proof_stream_patches; else ++_proof_stream_hits;
	stream.used = ++_proof_stream_serial;
	_proof_static_instances += stream.count;
	_proof_static_streams.push_back(std::move(stream));
	if (_proof_verify_gpu) {
		Debug(driver, 1, "PHASE7 static-view instances={}", _proof_static_streams.back().count);
		Debug(driver, 1, "PHASE7 stream-view rebuild={} patched={} bytes={} entries={}", whole, patched, ProofStaticStreamBytes(), _proof_static_streams.size());
	}
	SubmitProofInstances(camera, extent, width, height, 3, _proof_static_draws, &_proof_static_streams.back());
	return true;
}

/** Draw shared static library placements, with per-tile selection and transparency.
 * @param subset The chunks a partial render draws, or nullptr for all.
 * @return The retained or partial stream drawn, valid until the next draw; nullptr when they were drawn from a transient upload. */
static const ProofStaticStream *DrawProofStaticModels(const Terrain3D::Camera &camera, float extent, int width, int height, const std::vector<bool> *subset = nullptr)
{
	if (!_terrain_static_instances) return nullptr;
	LoadProofModels();
	bool instanced = _proof_instanced && p_glDrawArraysInstanced != nullptr && p_glVertexAttribDivisor != nullptr && InitProofInstanceProgram();
	if (instanced && _proof_retain_streams && subset == nullptr && DrawProofStaticStream(camera, extent, width, height)) return &_proof_static_streams.back();
	auto group_start = std::chrono::steady_clock::now();
	static std::vector<std::vector<const Static3DInstance *>> visible;
	visible.resize(FullVehicleModelCount3D());
	for (auto &group : visible) group.clear();
	for (size_t c = 0; c < _terrain_chunks.size(); ++c) {
		const auto &chunk = _terrain_chunks[c];
		if (subset != nullptr && !(*subset)[c]) continue;
		if (!chunk.resident || !Terrain3D::Visible(chunk.low, chunk.high, camera)) continue;
		/* Match baked geometry: cull chunks once, then let GPU clipping handle their placements. */
		for (const auto &v : chunk.instances) visible[v.model_index].push_back(&v);
	}
	/* Group opaque placements in linear time; retain placement order within each library model. */
	_proof_instance_batches.clear();
	_proof_instance_upload.clear();
	if (!instanced) {
		ProofUniform(_proof_locations.cameraC, extent * 4, camera.pz, 0, 3);
		BindProofModelVertices();
	}
	for (size_t model = 0; model < visible.size(); ++model) for (const auto *v : visible[model]) {
		ProofInstance data = ProofStaticPlacement(*v);
		++_proof_static_instances;
		if (instanced) {
			if (_proof_instance_batches.empty() || _proof_instance_batches.back().model != model) _proof_instance_batches.push_back({model, _proof_instance_upload.size(), 0});
			++_proof_instance_batches.back().count;
			_proof_instance_upload.push_back(data);
		} else {
			p_glUniform4f(_proof_locations.modelA, data.modelA[0], data.modelA[1], data.modelA[2], data.modelA[3]);
			p_glUniform4f(_proof_locations.modelB, data.modelB[0], data.modelB[1], data.modelB[2], data.modelB[3]);
			p_glUniform4f(_proof_locations.companyA, data.companyA[0], data.companyA[1], data.companyA[2], data.companyA[3]);
			p_glUniform4f(_proof_locations.companyB, data.companyB[0], data.companyB[1], data.companyB[2], data.companyB[3]);
			const auto &range = _proof_model_ranges[model];
			_glDrawArrays(GL_TRIANGLES, range.first, range.count);
			++_proof_static_draws;
		}
	}
	if (_proof_verify_gpu) {
		size_t count = 0;
		for (const auto &group : visible) count += group.size();
		Debug(driver, 1, "PHASE7 static-view instances={}", count);
	}
	_proof_static_group_us += ProofMicros(group_start);
	if (instanced && subset != nullptr) {
		/* The same model-major order as a retained stream, in a buffer of its own so contact shadows can draw it too. */
		_proof_partial_statics.batches = _proof_instance_batches;
		_proof_partial_statics.count = _proof_instance_upload.size();
		if (_proof_partial_statics.buffer == 0) _glGenBuffers(1, &_proof_partial_statics.buffer);
		_glBindBuffer(GL_ARRAY_BUFFER, _proof_partial_statics.buffer);
		size_t bytes = _proof_instance_upload.size() * sizeof(ProofInstance);
		_glBufferData(GL_ARRAY_BUFFER, bytes, _proof_instance_upload.data(), GL_STREAM_DRAW);
		_proof_upload_bytes += bytes;
		_proof_instance_upload_bytes += bytes;
		SubmitProofInstances(camera, extent, width, height, 3, _proof_static_draws, &_proof_partial_statics);
		return &_proof_partial_statics;
	}
	if (instanced) SubmitProofInstances(camera, extent, width, height, 3, _proof_static_draws);
	return nullptr;
}

/** Draw visible vehicles, effects and industry parts in batches sharing a library model.
 * @param effects Whether to draw effects too; sun shadows leave out smoke and sparks.
 * @return How many leading instanced batches are vehicles rather than effects; 0 without instancing. */
static size_t DrawProofVehicles(const Terrain3D::Camera &camera, float extent, int width, int height, bool effects = true)
{
	if (_vehicle3d_instances.empty() && _effect3d_instances.empty()) return 0;
	LoadProofModels();
	bool instanced = _proof_instanced && p_glDrawArraysInstanced != nullptr && p_glVertexAttribDivisor != nullptr && InitProofInstanceProgram();
	if (!instanced) {
		ProofUniform(_proof_locations.cameraC, extent * 4, camera.pz, 0, 2);
		BindProofModelVertices();
	}
	_proof_instance_batches.clear();
	_proof_instance_upload.clear();
	size_t reduced_instances = 0, hidden = 0, vehicle_batches = 0;
	for (const auto *list : {&_vehicle3d_instances, &_effect3d_instances}) {
		if (list == &_effect3d_instances) {
			/* Vehicle and effect models differ, so no batch spans both lists. */
			vehicle_batches = _proof_instance_batches.size();
			if (!effects) break;
		}
		for (const auto &v : *list) {
			const auto &full = _proof_model_ranges[v.model];
			if (list == &_vehicle3d_instances && HiddenVehicle3D(v, camera)) { ++hidden; continue; }
			if (full.count == 0) continue;
			float r = full.radius * std::max(1.0f, v.length);
			if (!Terrain3D::Visible({v.origin.x-r, v.origin.y-r, v.origin.z+full.low}, {v.origin.x+r, v.origin.y+r, v.origin.z+full.high}, camera)) continue;
			size_t model = VehicleLODModel3D(v, camera);
			const auto &range = _proof_model_ranges[model];
			_proof_model_triangles += range.count / 3;
			_proof_full_model_triangles += full.count / 3;
			if (model != v.model) { ++_proof_lod_instances; ++reduced_instances; }
			ProofInstance data{{v.origin.x, v.origin.y, v.origin.z, v.length}, {v.dx, v.dy, 1, v.stipple},
				{v.company[0][0], v.company[0][1], v.company[0][2], 0}, {v.company[1][0], v.company[1][1], v.company[1][2], 0}};
			++_proof_vehicle_instances;
			if (instanced) {
				/* Keep scene order: different models can have coplanar surfaces and GL_LESS keeps the first.
				 * Merge consecutive visible instances of the same model, including across the two lists. */
				if (_proof_instance_batches.empty() || _proof_instance_batches.back().model != model) {
					_proof_instance_batches.push_back({model, _proof_instance_upload.size(), 0});
				}
				++_proof_instance_batches.back().count;
				_proof_instance_upload.push_back(data);
			} else {
				p_glUniform4f(_proof_locations.modelA, data.modelA[0], data.modelA[1], data.modelA[2], data.modelA[3]);
				p_glUniform4f(_proof_locations.modelB, data.modelB[0], data.modelB[1], data.modelB[2], data.modelB[3]);
				p_glUniform4f(_proof_locations.companyA, data.companyA[0], data.companyA[1], data.companyA[2], data.companyA[3]);
				p_glUniform4f(_proof_locations.companyB, data.companyB[0], data.companyB[1], data.companyB[2], data.companyB[3]);
				_glDrawArrays(GL_TRIANGLES, range.first, range.count);
				++_proof_vehicle_draws;
			}
		}
	}
	if (_proof_verify_gpu && _vehicle3d_lod) Debug(driver, 1, "PHASE7 lod-view reduced={}", reduced_instances);
	if (_chase_log && camera.hides) Debug(driver, 0, "PHASE9 CABDRAW hidden={}", hidden);
	if (!instanced) return 0;
	SubmitProofInstances(camera, extent, width, height, 2, _proof_vehicle_draws);
	return vehicle_batches;
}

/** The shadows this render draws: the preference, unless the driver lacks what they need or the reference comparison runs. */
static int ProofShadowMode()
{
	if (_proof_shadows == 0 || _proof_shadow_failed || _proof_verify_gpu) return 0;
	if (p_glBlitFramebuffer == nullptr || p_glPolygonOffset == nullptr || p_glDrawBuffer == nullptr || p_glReadBuffer == nullptr || p_glBlendEquation == nullptr) return 0;
	if (!_proof_instanced || p_glDrawArraysInstanced == nullptr || p_glVertexAttribDivisor == nullptr || !InitProofInstanceProgram()) return 0;
	return _proof_shadows;
}

/** Column-major matrix from world positions to a view's clip space: a perspective camera's own, or the shaders' orthographic projection, which is affine. */
static std::array<float, 16> ProofViewMatrix(const Terrain3D::Camera &camera, float extent, int width, int height)
{
	if (camera.Perspective()) return camera.matrix;
	const float c = camera.cosine, n = camera.sine, t = camera.tilt, e = extent * 4;
	const float sx = 4 * camera.scale / width, sy = 2 * camera.scale / height;
	/* x'+y' and y'-x' of the rotated position, less these constants. */
	const float k0 = camera.cx * (c + n) + camera.cy * (c - n), k1 = camera.cx * (n - c) + camera.cy * (c + n);
	std::array<float, 16> m{};
	auto row = [&](int i, float x, float y, float z, float w) { m[i] = x; m[4 + i] = y; m[8 + i] = z; m[12 + i] = w; };
	row(0, sx * (n - c), sx * (c + n), 0, -sx * k1);
	row(1, -sy * t * (c + n), -sy * t * (c - n), sy, sy * (t * k0 - camera.pz));
	row(2, -(c + n) / e, -(c - n) / e, -2 * t / e, k0 / e);
	row(3, 0, 0, 0, 1);
	return m;
}

/** Inverse of a 4x4 matrix by cofactors; the identity if it is singular. */
static std::array<float, 16> ProofInvert(const std::array<float, 16> &m)
{
	std::array<float, 16> r;
	r[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	r[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	r[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	r[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	r[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	r[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	r[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	r[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	r[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
	r[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
	r[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
	r[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
	r[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
	r[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
	r[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
	r[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
	float det = m[0] * r[0] + m[1] * r[4] + m[2] * r[8] + m[3] * r[12];
	if (std::abs(det) < 1e-30f) return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
	for (float &v : r) v /= det;
	return r;
}

/** Column-major matrix times (x, y, z, w). */
static std::array<float, 4> ProofTransform(const std::array<float, 16> &m, float x, float y, float z, float w)
{
	return {m[0]*x + m[4]*y + m[8]*z + m[12]*w, m[1]*x + m[5]*y + m[9]*z + m[13]*w, m[2]*x + m[6]*y + m[10]*z + m[14]*w, m[3]*x + m[7]*y + m[11]*z + m[15]*w};
}

/**
 * Render the sun's depth over everything the view can see and set up its sampling. One orthographic map along the sun
 * covers the view's visible chunks, clipped to what the view frames; anything that can shade them lies further
 * towards the sun along the same lines, so only the depth range grows for casters. A view so wide that its texels
 * would be coarse fades the shadows out and skips the pass.
 * @param draw_terrain Draws the retained terrain chunks a camera can see.
 * @return Whether shadows are sampled in this render; the view's framebuffer and uniforms need restoring if so.
 */
template <typename DrawTerrain>
static bool RenderProofSunShadow(const Terrain3D::Camera &camera, float extent, DrawTerrain &&draw_terrain)
{
	auto start = std::chrono::steady_clock::now();
	using Terrain3D::Vec;
	auto dot = [](Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
	auto unit = [&](Vec a) { float l = std::sqrt(dot(a, a)); return Vec{a.x / l, a.y / l, a.z / l}; };
	const Vec sun = unit(PROOF_SUN), right = unit({sun.y, -sun.x, 0});
	const Vec up{right.y * sun.z - right.z * sun.y, right.z * sun.x - right.x * sun.z, right.x * sun.y - right.y * sun.x};
	/* Light space: across (right), along (up) and towards the sun (d). */
	float a0 = 1e30f, a1 = -1e30f, b0 = 1e30f, b1 = -1e30f, d0 = 1e30f, d1 = -1e30f, z0 = 1e30f, z1 = -1e30f;
	for (const auto &chunk : _terrain_chunks) {
		if (chunk.faces.empty() || !Terrain3D::Visible(chunk.low, chunk.high, camera)) continue;
		z0 = std::min(z0, chunk.low.z); z1 = std::max(z1, chunk.high.z);
		for (int i = 0; i < 8; ++i) {
			Vec p{(i & 1) ? chunk.high.x : chunk.low.x, (i & 2) ? chunk.high.y : chunk.low.y, (i & 4) ? chunk.high.z : chunk.low.z};
			float a = dot(p, right), b = dot(p, up), d = dot(p, sun);
			a0 = std::min(a0, a); a1 = std::max(a1, a); b0 = std::min(b0, b); b1 = std::max(b1, b); d0 = std::min(d0, d); d1 = std::max(d1, d);
		}
	}
	if (a0 >= a1 || b0 >= b1) return false;
	/* What the view frames: its clip volume, cut to the visible heights; a perspective view only to its draw distance. */
	const int width = int(camera.width), height = int(camera.height);
	auto inverse = ProofInvert(ProofViewMatrix(camera, extent, width, height));
	float va0 = 1e30f, va1 = -1e30f, vb0 = 1e30f, vb1 = -1e30f;
	for (int i = 0; i < 4; ++i) {
		float x = (i & 1) ? 1.0f : -1.0f, y = (i & 2) ? 1.0f : -1.0f;
		auto n = ProofTransform(inverse, x, y, -1, 1), f = ProofTransform(inverse, x, y, 1, 1);
		Vec near_point{n[0] / n[3], n[1] / n[3], n[2] / n[3]}, far_point{f[0] / f[3], f[1] / f[3], f[2] / f[3]};
		std::array<Vec, 2> ends;
		if (camera.Perspective()) {
			/* Shadows fade out well inside the draw distance, so only nearby casters are drawn. */
			Vec ray{far_point.x - camera.eye.x, far_point.y - camera.eye.y, far_point.z - camera.eye.z};
			float k = std::min(1.0f, std::min(camera.distance, PROOF_SUN_RANGE) / std::max(std::sqrt(dot(ray, ray)), 1e-3f));
			ends = {near_point, Vec{camera.eye.x + ray.x * k, camera.eye.y + ray.y * k, camera.eye.z + ray.z * k}};
		} else {
			/* The corner's line of sight at the lowest and highest visible ground. */
			float dz = far_point.z - near_point.z;
			auto at = [&](float z) {
				float k = std::abs(dz) < 1e-6f ? 0 : (z - near_point.z) / dz;
				return Vec{near_point.x + (far_point.x - near_point.x) * k, near_point.y + (far_point.y - near_point.y) * k, z};
			};
			ends = {at(z0), at(z1)};
		}
		for (const Vec &p : ends) {
			float a = dot(p, right), b = dot(p, up);
			va0 = std::min(va0, a); va1 = std::max(va1, a); vb0 = std::min(vb0, b); vb1 = std::max(vb1, b);
		}
	}
	a0 = std::max(a0, va0 - 8); a1 = std::min(a1, va1 + 8); b0 = std::max(b0, vb0 - 8); b1 = std::min(b1, vb1 + 8);
	if (a0 >= a1 || b0 >= b1) return false;
	const float texel = std::max(a1 - a0, b1 - b0) / PROOF_SHADOW_SIZE;
	/* World units per texel: past 1.5 the shadows soften away, and past 3 they are not drawn. */
	float fade = std::clamp((3.0f - texel) / 1.5f, 0.0f, 1.0f);
	fade = fade * fade * (3 - 2 * fade);
	if (fade <= 0) return false;
	d1 += 64; // Static models above the tallest ground of the chunks.

	if (_proof_shadow_fbo == 0) {
		_glGetError();
		p_glGenFramebuffers(1, &_proof_shadow_fbo);
		_glGenTextures(1, &_proof_shadow_depth);
		_glActiveTexture(GL_TEXTURE5);
		_glBindTexture(GL_TEXTURE_2D, _proof_shadow_depth);
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, PROOF_SHADOW_SIZE, PROOF_SHADOW_SIZE, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
		p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_shadow_fbo);
		p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _proof_shadow_depth, 0);
		p_glDrawBuffer(GL_NONE);
		p_glReadBuffer(GL_NONE);
		bool complete = p_glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		GLenum error = _glGetError();
		if (!complete || error != GL_NO_ERROR) {
			Debug(driver, 0, "3D sun shadows unavailable (complete={}, OpenGL error {}); shadows stay off", complete, error);
			_proof_shadow_failed = true;
			return false;
		}
	}
	/* Never sample the map while drawing into it. */
	_glActiveTexture(GL_TEXTURE5);
	_glBindTexture(GL_TEXTURE_2D, 0);
	_glActiveTexture(GL_TEXTURE0);

	std::array<float, 16> light{};
	auto row = [&](std::array<float, 16> &m, int i, Vec v, float scale, float offset) {
		m[i] = v.x * scale; m[4 + i] = v.y * scale; m[8 + i] = v.z * scale; m[12 + i] = offset;
	};
	row(light, 0, right, 2 / (a1 - a0), -(a1 + a0) / (a1 - a0));
	row(light, 1, up, 2 / (b1 - b0), -(b1 + b0) / (b1 - b0));
	row(light, 2, sun, -2 / (d1 - d0), 1 + 2 * d0 / (d1 - d0)); // Nearer the sun is nearer the light.
	light[15] = 1;
	Terrain3D::Camera view{};
	view.width = view.height = PROOF_SHADOW_SIZE;
	view.matrix = light;
	view.distance = 1e9f; // Drawn through the perspective path with the light's matrix; no haze.

	p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_shadow_fbo);
	_glViewport(0, 0, PROOF_SHADOW_SIZE, PROOF_SHADOW_SIZE);
	_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_LESS); p_glDepthMask(GL_TRUE);
	_glClear(GL_DEPTH_BUFFER_BIT);
	p_glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	_glEnable(GL_POLYGON_OFFSET_FILL);
	p_glPolygonOffset(1.5f, 4.0f);
	_proof_shadow_info = {0, 0, 0, 1};
	_glUseProgram(_proof_program);
	ProofPerspectiveUniforms(_proof_locations, view);
	ProofShadowUniforms(_proof_locations);
	ProofUniform(_proof_locations.cameraC, extent * 4, 0, 0, 1);
	draw_terrain(view);
	DrawProofStaticModels(view, extent, PROOF_SHADOW_SIZE, PROOF_SHADOW_SIZE);
	DrawProofVehicles(view, extent, PROOF_SHADOW_SIZE, PROOF_SHADOW_SIZE, false);
	_glDisable(GL_POLYGON_OFFSET_FILL);
	p_glPolygonMode(GL_FRONT_AND_BACK, _terrain_wireframe ? GL_LINE : GL_FILL);

	/* Sampling takes texture coordinates and a normal offset of one and a half texels against acne. */
	_proof_shadow_matrix = light;
	for (int c = 0; c < 4; ++c) for (int r = 0; r < 3; ++r) _proof_shadow_matrix[c * 4 + r] = 0.5f * light[c * 4 + r] + 0.5f * light[c * 4 + 3];
	_proof_shadow_info = {1, PROOF_SUN_STRENGTH * fade, texel * 1.5f, 0};
	_proof_shadow_fade = camera.Perspective() ? std::array<float, 4>{camera.eye.x, camera.eye.y, camera.eye.z, std::min(camera.distance, PROOF_SUN_RANGE)} : std::array<float, 4>{};
	_glActiveTexture(GL_TEXTURE5);
	_glBindTexture(GL_TEXTURE_2D, _proof_shadow_depth);
	_glActiveTexture(GL_TEXTURE0);
	++_proof_shadow_passes;
	_proof_shadow_us += ProofMicros(start);
	return true;
}

/** Contact shadows: a soft darkening of whatever lies at the foot of each opaque model, from the view's own depth. */
static bool InitProofContact()
{
	if (_proof_contact_program != 0) return true;
	if (_proof_shadow_failed) return false;
	/* One box per instance, from gl_VertexID: its footprint plus a margin, from just below its base to a reach above it.
	 * Only the faces turned away from the viewer are kept, so each pixel is shaded once even from inside a box. */
	const char *vertex = R"GLSL(#version 150
in vec4 instanceModelA, instanceModelB, instanceCompanyA, instanceCompanyB;
uniform mat4 viewProjection; uniform vec4 viewer, bounds, heights, visibility;
flat out vec4 frameA, frameB, frameC;
const int corners[36]=int[36](0,2,6,0,6,4, 1,5,7,1,7,3, 0,4,5,0,5,1, 2,3,7,2,7,6, 0,1,3,0,3,2, 4,6,7,4,7,5);
const vec3 normals[6]=vec3[6](vec3(-1,0,0),vec3(1,0,0),vec3(0,-1,0),vec3(0,1,0),vec3(0,0,-1),vec3(0,0,1));
void main() {
 vec4 a=instanceModelA, b=instanceModelB;
 gl_Position=vec4(2,2,2,1);
 bool skip=heights.z>2.5?(int(visibility.x)&(1<<int(b.w)))!=0:b.w>0.5;
 float low=heights.x*b.z, tall=(heights.y-heights.x)*b.z;
 vec4 rect=vec4(bounds.x*a.w,bounds.y*a.w,bounds.z,bounds.w);
 if(skip || tall<1.5) return;
 float reach=clamp(0.35*tall,1.5,6.0);
 float margin=clamp(0.25*min(rect.y-rect.x,rect.w-rect.z)+0.75,1.0,4.0);
 int c=corners[gl_VertexID];
 vec3 lo=vec3(rect.x-margin,rect.z-margin,a.z+low-2.0), hi=vec3(rect.y+margin,rect.w+margin,a.z+low+reach);
 vec3 l=vec3((c&1)!=0?hi.x:lo.x,(c&2)!=0?hi.y:lo.y,(c&4)!=0?hi.z:lo.z);
 vec3 place=vec3(a.x+l.x*b.x-l.y*b.y,a.y+l.x*b.y+l.y*b.x,l.z);
 vec3 n=normals[gl_VertexID/6]; n=vec3(n.x*b.x-n.y*b.y,n.x*b.y+n.y*b.x,n.z);
 if((viewer.w>0.5?dot(n,place-viewer.xyz):dot(n,viewer.xyz))<=0.0) return;
 gl_Position=viewProjection*vec4(place,1);
 frameA=vec4(a.xy,a.z+low,reach); frameB=vec4(b.xy,margin,0); frameC=rect;
})GLSL";
	const char *fragment = R"GLSL(#version 150
flat in vec4 frameA, frameB, frameC;
uniform sampler2D sceneDepth; uniform mat4 inverseView; uniform vec4 screen, eye, fog; out vec4 pixel;
void main() {
 float depth=texelFetch(sceneDepth,ivec2(gl_FragCoord.xy),0).r;
 if(depth>=1.0) discard;
 vec4 w=inverseView*vec4(gl_FragCoord.xy/screen.xy*2.0-1.0,depth*2.0-1.0,1.0);
 vec3 p=w.xyz/w.w;
 vec2 q=p.xy-frameA.xy;
 vec2 l=vec2(q.x*frameB.x+q.y*frameB.y,-q.x*frameB.y+q.y*frameB.x);
 vec2 d=abs(l-0.5*vec2(frameC.x+frameC.y,frameC.z+frameC.w))-0.5*vec2(frameC.y-frameC.x,frameC.w-frameC.z);
 float edge=length(max(d,0.0))+min(max(d.x,d.y),0.0);
 float dz=p.z-frameA.z;
 float shade=(1.0-smoothstep(-frameB.z,frameB.z,edge))*(1.0-smoothstep(0.0,frameA.w,dz))*smoothstep(-2.0,-0.5,dz);
 if(eye.w>0.0) shade*=1.0-smoothstep(fog.w,eye.w*0.98,distance(p,eye.xyz));
 if(shade<=0.0) discard;
 pixel=vec4(shade);
})GLSL";
	/* The darkest decal over each pixel, kept by a max blend, then one multiply over the view. */
	const char *apply_vertex = R"GLSL(#version 150
void main() {
 vec2 corner=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));
 gl_Position=vec4(corner*2.0-1.0,0,1);
})GLSL";
	const char *apply_fragment = R"GLSL(#version 150
uniform sampler2D occlusion; uniform float strength; out vec4 pixel;
void main() { pixel=vec4(vec3(1.0-strength*texelFetch(occlusion,ivec2(gl_FragCoord.xy),0).r),1); })GLSL";
	_proof_contact_program = ProofCompileProgram({vertex}, {fragment});
	_proof_occlusion_program = ProofCompileProgram({apply_vertex}, {apply_fragment});
	if (_proof_contact_program == 0 || _proof_occlusion_program == 0) {
		Debug(driver, 0, "3D contact shadow program unavailable; shadows stay off");
		_proof_shadow_failed = true;
		return false;
	}
	_proof_contact_view = _glGetUniformLocation(_proof_contact_program, "viewProjection");
	_proof_contact_inverse = _glGetUniformLocation(_proof_contact_program, "inverseView");
	_proof_contact_viewer = _glGetUniformLocation(_proof_contact_program, "viewer");
	_proof_contact_screen = _glGetUniformLocation(_proof_contact_program, "screen");
	_proof_contact_bounds = _glGetUniformLocation(_proof_contact_program, "bounds");
	_proof_contact_heights = _glGetUniformLocation(_proof_contact_program, "heights");
	_proof_contact_visibility = _glGetUniformLocation(_proof_contact_program, "visibility");
	_proof_contact_eye = _glGetUniformLocation(_proof_contact_program, "eye");
	_proof_contact_fog = _glGetUniformLocation(_proof_contact_program, "fog");
	_proof_contact_depth_map = _glGetUniformLocation(_proof_contact_program, "sceneDepth");
	_proof_occlusion_map = _glGetUniformLocation(_proof_occlusion_program, "occlusion");
	_proof_occlusion_strength = _glGetUniformLocation(_proof_occlusion_program, "strength");
	const char *instances[] = {"instanceModelA", "instanceModelB", "instanceCompanyA", "instanceCompanyB"};
	for (size_t i = 0; i < _proof_contact_instance.size(); ++i) _proof_contact_instance[i] = _glGetAttribLocation(_proof_contact_program, instances[i]);
	_glGenVertexArrays(1, &_proof_contact_vao);
	_glBindVertexArray(_proof_contact_vao);
	for (GLint location : _proof_contact_instance) {
		if (location < 0) continue;
		_glEnableVertexAttribArray(location);
		p_glVertexAttribDivisor(location, 1);
	}
	_glGenVertexArrays(1, &_proof_occlusion_vao);
	_glBindVertexArray(_proof_vao);
	p_glGenFramebuffers(1, &_proof_contact_fbo);
	_glGenTextures(1, &_proof_contact_depth);
	p_glGenFramebuffers(1, &_proof_occlusion_fbo);
	_glGenTextures(1, &_proof_occlusion);
	return true;
}

/** Darken the feet of the static models and vehicles just drawn, from a copy of the view's depth. Overlapping
 * shadows keep the darkest, so dense forests and terraces do not darken step by step. Effects cast none. */
static void DrawProofContact(const Terrain3D::Camera &camera, float extent, int width, int height, const ProofStaticStream *statics, size_t vehicle_batches)
{
	if ((statics == nullptr || statics->count == 0) && vehicle_batches == 0) return;
	if (!InitProofContact()) return;
	auto start = std::chrono::steady_clock::now();
	_glActiveTexture(GL_TEXTURE6);
	_glBindTexture(GL_TEXTURE_2D, _proof_contact_depth);
	if (_proof_contact_width < width || _proof_contact_height < height) {
		_proof_contact_width = std::max(width, _proof_contact_width);
		_proof_contact_height = std::max(height, _proof_contact_height);
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, _proof_contact_width, _proof_contact_height, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, nullptr);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_contact_fbo);
		p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _proof_contact_depth, 0);
		p_glDrawBuffer(GL_NONE);
		p_glReadBuffer(GL_NONE);
		_glActiveTexture(GL_TEXTURE7);
		_glBindTexture(GL_TEXTURE_2D, _proof_occlusion);
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, _proof_contact_width, _proof_contact_height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_occlusion_fbo);
		p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _proof_occlusion, 0);
		_glActiveTexture(GL_TEXTURE6);
	}
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, _proof_fbo);
	p_glBindFramebuffer(GL_DRAW_FRAMEBUFFER, _proof_contact_fbo);
	p_glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
	/* Unbind the occlusion before drawing into it; the decals then keep the strongest value per pixel. */
	_glActiveTexture(GL_TEXTURE7);
	_glBindTexture(GL_TEXTURE_2D, 0);
	_glActiveTexture(GL_TEXTURE0);
	p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_occlusion_fbo);
	_glClearColor(0, 0, 0, 0);
	_glClear(GL_COLOR_BUFFER_BIT);

	auto view = ProofViewMatrix(camera, extent, width, height);
	auto inverse = ProofInvert(view);
	std::array<float, 4> viewer{camera.eye.x, camera.eye.y, camera.eye.z, 1};
	if (!camera.Perspective()) {
		/* An orthographic view looks along the direction its depth grows. */
		auto f = ProofTransform(inverse, 0, 0, 1, 0);
		viewer = {f[0], f[1], f[2], 0};
	}
	p_glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	_glDisable(GL_DEPTH_TEST);
	p_glDepthMask(GL_FALSE);
	GLint src_rgb, dst_rgb, src_alpha, dst_alpha;
	_glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb); _glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
	_glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha); _glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
	_glEnable(GL_BLEND);
	p_glBlendEquation(GL_MAX);
	_glUseProgram(_proof_contact_program);
	_glBindVertexArray(_proof_contact_vao);
	p_glUniformMatrix4fv(_proof_contact_view, 1, GL_FALSE, view.data());
	p_glUniformMatrix4fv(_proof_contact_inverse, 1, GL_FALSE, inverse.data());
	ProofUniform(_proof_contact_viewer, viewer[0], viewer[1], viewer[2], viewer[3]);
	ProofUniform(_proof_contact_screen, width, height, 0, 0);
	ProofUniform(_proof_contact_visibility, _game_mode == GameMode::Menu ? 0 : _transparency_opt.base(), _game_mode == GameMode::Menu ? 0 : (_transparency_opt & _invisibility_opt).base(), _transparency_lock.base(), 0);
	ProofUniform(_proof_contact_eye, camera.eye.x, camera.eye.y, camera.eye.z, camera.distance);
	ProofUniform(_proof_contact_fog, PROOF_FOG[0], PROOF_FOG[1], PROOF_FOG[2], camera.distance * 0.6f);
	_glUniform1i(_proof_contact_depth_map, 6);
	auto submit = [&](GLuint buffer, const std::vector<ProofInstanceBatch> &batches, size_t count, float mode) {
		_glBindBuffer(GL_ARRAY_BUFFER, buffer);
		for (size_t b = 0; b < count; ++b) {
			const auto &batch = batches[b];
			const auto &range = _proof_model_ranges[batch.model];
			if (range.count == 0) continue;
			ProofUniform(_proof_contact_bounds, range.footprint[0], range.footprint[1], range.footprint[2], range.footprint[3]);
			ProofUniform(_proof_contact_heights, range.low, range.high, mode, 0);
			for (size_t i = 0; i < _proof_contact_instance.size(); ++i) {
				if (_proof_contact_instance[i] < 0) continue;
				_glVertexAttribPointer(_proof_contact_instance[i], 4, GL_FLOAT, GL_FALSE, sizeof(ProofInstance), reinterpret_cast<void *>(batch.first * sizeof(ProofInstance) + i * sizeof(std::array<float, 4>)));
			}
			p_glDrawArraysInstanced(GL_TRIANGLES, 0, 36, batch.count);
		}
	};
	if (statics != nullptr) submit(statics->buffer, statics->batches, statics->batches.size(), 3);
	if (vehicle_batches != 0) submit(_proof_instances, _proof_instance_batches, vehicle_batches, 2);
	p_glBlendEquation(GL_FUNC_ADD);
	p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_fbo);
	_glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	_glUseProgram(_proof_occlusion_program);
	_glBindVertexArray(_proof_occlusion_vao);
	_glActiveTexture(GL_TEXTURE7);
	_glBindTexture(GL_TEXTURE_2D, _proof_occlusion);
	_glUniform1i(_proof_occlusion_map, 7);
	_glUniform1f(_proof_occlusion_strength, PROOF_CONTACT_STRENGTH);
	_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	_glDisable(GL_BLEND);
	if (p_glBlendFuncSeparate != nullptr) {
		p_glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
	} else {
		_glBlendFunc(src_rgb, dst_rgb);
	}
	_glEnable(GL_DEPTH_TEST);
	p_glDepthMask(GL_TRUE);
	p_glPolygonMode(GL_FRONT_AND_BACK, _terrain_wireframe ? GL_LINE : GL_FILL);
	_glActiveTexture(GL_TEXTURE0);
	_glBindVertexArray(_proof_vao);
	_glUseProgram(_proof_program);
	++_proof_contact_passes;
	_proof_contact_us += ProofMicros(start);
}

/** Draw only the outside edge of the focus's exact model/ground silhouette. The interior is
 * stencil-masked, so this also works behind structures without painting over the selected object.
 * Nine small draws share the view's packed depth/stencil attachment; no readback or extra image. */
static void DrawProofFocusOutline(const ProofImage &image, const Terrain3D::Camera &camera)
{
	if (image.key[22]==0 || image.key[22]==3 || _proof_focus[3]<=0 || p_glColorMask==nullptr) return;
	if (_proof_outline_program==0) {
		const char *vertex=R"GLSL(#version 150
in vec3 position; uniform vec4 offset;
void main(){gl_Position=vec4(position.xy+offset.xy,0,1);})GLSL";
		const char *fragment=R"GLSL(#version 150
uniform vec4 colour; out vec4 pixel; void main(){pixel=colour;})GLSL";
		_proof_outline_program=ProofCompileProgram({vertex},{fragment});
		if (_proof_outline_program==0) return;
		_proof_outline_offset=_glGetUniformLocation(_proof_outline_program,"offset");
		_proof_outline_colour=_glGetUniformLocation(_proof_outline_program,"colour");
		_glGenVertexArrays(1,&_proof_outline_vao); _glGenBuffers(1,&_proof_outline_vbo);
	}
	Vec target{std::bit_cast<float>(image.key[23]),std::bit_cast<float>(image.key[24]),std::bit_cast<float>(image.key[25])};
	std::vector<TerrainFace> faces;
	if (image.key[22]==2) {
		for (const auto &v : _vehicle3d_instances) {
			if (v.origin!=target || HiddenVehicle3D(v,camera)) continue;
			AppendModel3D(faces,VehicleModel3D(v.model),v.tile,v.origin,v.dx,v.dy,v.length,1,v.company);
		}
	} else {
		int x=int(target.x)/TILE_SIZE*TILE_SIZE, y=int(target.y)/TILE_SIZE*TILE_SIZE;
		auto point=[&](int a,int b){return Vec{float(a),float(b),float(GetSlopePixelZ(a,b))+0.5f};};
		Vec a=point(x,y), b=point(x+TILE_SIZE,y), c=point(x+TILE_SIZE,y+TILE_SIZE), d=point(x,y+TILE_SIZE);
		faces.push_back({{a,b,c},{1,1,1},TileVirtXY(x,y)});
		faces.push_back({{a,c,d},{1,1,1},TileVirtXY(x,y)});
	}
	if (faces.empty()) return;
	std::vector<Vec> vertices; vertices.reserve(faces.size()*3);
	for (const auto &face : faces) for (Vec v : face.p) {
		// Clip on the GPU: perspective vertices use homogeneous coordinates in the other passes;
		// skip a silhouette intersecting the near plane rather than projecting it across the screen.
		if (camera.Perspective()) { auto r=camera.Row(3); if (r[0]*v.x+r[1]*v.y+r[2]*v.z+r[3]<=0.1f) return; }
		auto p=camera.Project(v); vertices.push_back({p.x*2/image.key[0]-1,1-p.y*2/image.key[1],0});
	}
	// This helper runs on the private viewport FBO. Restore global stencil and colour state locally.
	bool stencil=p_glIsEnabled(GL_STENCIL_TEST), depth=p_glIsEnabled(GL_DEPTH_TEST);
	GLint func,ref,mask,write,fail,zfail,zpass,clear,colour_mask[4];
	_glGetIntegerv(GL_STENCIL_FUNC,&func); _glGetIntegerv(GL_STENCIL_REF,&ref);
	_glGetIntegerv(GL_STENCIL_VALUE_MASK,&mask); _glGetIntegerv(GL_STENCIL_WRITEMASK,&write);
	_glGetIntegerv(GL_STENCIL_FAIL,&fail); _glGetIntegerv(GL_STENCIL_PASS_DEPTH_FAIL,&zfail); _glGetIntegerv(GL_STENCIL_PASS_DEPTH_PASS,&zpass);
	_glGetIntegerv(GL_STENCIL_CLEAR_VALUE,&clear); _glGetIntegerv(GL_COLOR_WRITEMASK,colour_mask);
	_glDisable(GL_DEPTH_TEST); _glEnable(GL_STENCIL_TEST); p_glPolygonMode(GL_FRONT_AND_BACK,GL_FILL);
	p_glStencilMask(0xFF); p_glClearStencil(0); _glClear(GL_STENCIL_BUFFER_BIT);
	_glUseProgram(_proof_outline_program); _glBindVertexArray(_proof_outline_vao);
	_glBindBuffer(GL_ARRAY_BUFFER,_proof_outline_vbo);
	_glBufferData(GL_ARRAY_BUFFER,vertices.size()*sizeof(Vec),vertices.data(),GL_STREAM_DRAW);
	GLint location=_glGetAttribLocation(_proof_outline_program,"position");
	_glEnableVertexAttribArray(location); _glVertexAttribPointer(location,3,GL_FLOAT,GL_FALSE,sizeof(Vec),nullptr);
	ProofUniform(_proof_outline_colour,1.0f,false && image.key[22]==1?0.18f:0.83f,0.12f,1);
	ProofUniform(_proof_outline_offset,0,0,0,0);
	p_glColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
	p_glStencilFunc(GL_ALWAYS,1,0xFF); p_glStencilOp(GL_KEEP,GL_KEEP,GL_REPLACE);
	_glDrawArrays(GL_TRIANGLES,0,GLsizei(vertices.size()));
	p_glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
	p_glStencilMask(0); p_glStencilFunc(GL_EQUAL,0,0xFF); p_glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
	float pixels=std::max(1.0f,float(ScaleGUITrad(1)));
	for (int y=-1;y<=1;++y) for (int x=-1;x<=1;++x) {
		if (x==0 && y==0) continue;
		ProofUniform(_proof_outline_offset,2*pixels*x/image.key[0],2*pixels*y/image.key[1],0,0);
		_glDrawArrays(GL_TRIANGLES,0,GLsizei(vertices.size()));
	}
	p_glColorMask(colour_mask[0],colour_mask[1],colour_mask[2],colour_mask[3]);
	p_glStencilMask(write); p_glStencilFunc(func,ref,mask); p_glStencilOp(fail,zfail,zpass); p_glClearStencil(clear);
	(stencil?_glEnable:_glDisable)(GL_STENCIL_TEST); (depth?_glEnable:_glDisable)(GL_DEPTH_TEST);
}

static bool DrawProofFade(float dark);
static bool FinishProofImage(ProofImage &image, bool direct, bool measure_gpu, size_t query);

static bool RenderProofImage(ProofImage &image)
{
	if (!LoadProofGL()) return false;
	ProofGLState restore;
	if (!InitProofProgram()) return false;
	const int width = image.key[0], height = image.key[1], map_x = image.key[5], map_y = image.key[6];
	const bool direct = image.key[7] != 0;
	p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_fbo);
	_glActiveTexture(GL_TEXTURE0);
	_glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
	if (direct && image.texture == 0) {
		auto reuse = std::ranges::find_if(_proof_free_textures, [&](const ProofTexture &t) { return t.width == width && t.height == height; });
		if (reuse != _proof_free_textures.end()) {
			image.texture = reuse->texture;
			_proof_free_textures.erase(reuse);
		} else {
			_glGenTextures(1, &image.texture);
			_glBindTexture(GL_TEXTURE_2D, image.texture);
			_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
			_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		}
	}
	_glBindTexture(GL_TEXTURE_2D, _proof_colour);
	if (_proof_optimized ? (_proof_width < width || _proof_height < height) : (_proof_width != width || _proof_height != height)) {
		int storage_width = _proof_optimized ? std::max(width, _proof_width) : width;
		int storage_height = _proof_optimized ? std::max(height, _proof_height) : height;
		_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, storage_width, storage_height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		p_glBindRenderbuffer(GL_RENDERBUFFER, _proof_depth);
		p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, storage_width, storage_height);
		p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _proof_depth);
		_proof_width = storage_width; _proof_height = storage_height;
	}
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, direct ? image.texture : _proof_colour, 0);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
	_glViewport(0, 0, width, height);
	_glDisable(GL_BLEND); _glDisable(GL_CULL_FACE); _glDisable(GL_SCISSOR_TEST);
	_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_LESS); p_glDepthMask(GL_TRUE); p_glClearDepth(1);
	auto camera = KeyTerrainCamera(image.key);
	_proof_focus = {};
	if (image.key[22] != 0) {
		Vec target{std::bit_cast<float>(image.key[23]), std::bit_cast<float>(image.key[24]), std::bit_cast<float>(image.key[25])};
		auto p = camera.Project(target);
		float depth = camera.Perspective() ? (1-p.z)*0.5f : (1-p.z/float((map_x+map_y)*TILE_SIZE+1024)/4)*0.5f;
		float radius = std::bit_cast<float>(image.key[26]);
		float pixels = camera.Perspective() ? std::abs(camera.Project({target.x,target.y,target.z+radius}).y-p.y) : radius*camera.scale;
		if (depth>0 && depth<1 && p.x>-128 && p.y>-128 && p.x<width+128 && p.y<height+128) _proof_focus={p.x,height-p.y,depth,std::clamp(pixels,28.0f,128.0f)};
	}
	bool black = camera.Perspective() && camera.dark >= 1;
	if (black) {
		_glClearColor(0, 0, 0, 1);
	} else if (camera.Perspective()) {
		_glClearColor(PROOF_FOG[0], PROOF_FOG[1], PROOF_FOG[2], 1);
	} else {
		_glClearColor(0.10f, 0.17f, 0.23f, 1);
	}
	/* A kept image with stale cells clears and redraws only those: the stencil marks them with 1, and every
	 * pass below keeps to it. Only chunks that can reach a stale cell are drawn. */
	const bool partial = direct && image.texture != 0 && !image.stale.Empty();
	std::vector<bool> reaches;
	if (partial) {
		_glEnable(GL_STENCIL_TEST);
		p_glStencilMask(0xFF);
		p_glClearStencil(0);
		_glClear(GL_STENCIL_BUFFER_BIT);
		p_glClearStencil(1);
		_glEnable(GL_SCISSOR_TEST);
		for (const auto &[x, y, w, h] : image.stale.Rects()) {
			p_glScissor(x, height - y - h, w, h);
			_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
			_proof_partial_pixels += static_cast<uint64_t>(w) * h;
		}
		_glDisable(GL_SCISSOR_TEST);
		p_glClearStencil(0);
		p_glStencilFunc(GL_EQUAL, 1, 0xFF);
		p_glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		p_glStencilMask(0);
		reaches.resize(_terrain_chunks.size());
		/* Contact shadows of a chunk's placements reach past its bounds, as InvalidateProof3DRegions widens changes. */
		float reach = _proof_shadows != 0 ? 4.0f : 0.0f;
		for (size_t i = 0; i < _terrain_chunks.size(); ++i) {
			const auto &chunk = _terrain_chunks[i];
			if (chunk.faces.empty() && chunk.instances.empty()) continue;
			auto [x0, y0, x1, y1] = ProofScreenBounds(camera, {chunk.low.x - reach, chunk.low.y - reach, chunk.low.z - 2 * reach},
				{chunk.high.x + reach, chunk.high.y + reach, chunk.high.z});
			reaches[i] = image.stale.Overlaps(x0, y0, x1, y1);
		}
	} else {
		_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
	}
	/* A view faded to black in a tunnel draws nothing. */
	if (black) return FinishProofImage(image, direct, false, 0);
	auto prepare_start = std::chrono::steady_clock::now();
	std::vector<ProofVertex> vertices;
	const float extent = static_cast<float>((map_x + map_y) * TILE_SIZE + 1024);
	/* CPU vertices: projected here for orthographic views; perspective views keep world space and the shader projects them. */
	auto clip = [&](Vec world) -> std::array<float, 3> {
		if (camera.Perspective()) return {world.x, world.y, world.z};
		auto p = camera.Project(world);
		return {p.x * 2 / width - 1, 1 - p.y * 2 / height, -p.z / (extent * 4)};
	};
	vertices.clear();
	int visible_chunks=0;
	std::vector<TerrainFace> reference_faces;
	for (size_t index = 0; index < _terrain_chunks.size(); ++index) {
		const auto &chunk = _terrain_chunks[index];
		const auto *faces = &chunk.faces;
		if (!_proof_retained && _proof_verify_gpu && (_terrain_static_instances || _proof_deferred)) {
			/* The sampled reference regenerates the whole scene from snapshots,
			 * bypassing residency, pending masks and cached culling bounds. */
			int nx = (_terrain_map_x + _terrain_chunk_size - 1) / _terrain_chunk_size;
			int cx = int(index % nx), cy = int(index / nx);
			int cw = std::min(_terrain_chunk_size, _terrain_map_x - cx * _terrain_chunk_size);
			reference_faces.clear();
			for (size_t i = 0; i < chunk.tiles.size(); ++i) BuildTileGeometry(reference_faces, chunk.tiles[i], cx * _terrain_chunk_size + int(i % cw), cy * _terrain_chunk_size + int(i / cw));
			faces = &reference_faces;
		} else if (chunk.faces.empty() || !Terrain3D::Visible(chunk.low,chunk.high,camera)) continue;
		++visible_chunks;
		if (_proof_retained) continue;
		for (const auto &face : *faces) {
		if (face.transparency != TransparencyOption::Invalid && IsInvisibilitySet(face.transparency)) continue;
		float transparent = face.transparency != TransparencyOption::Invalid && IsTransparencySet(face.transparency) ? 1.0f : 0.0f;
		if (face.transparency>=TransparencyOption::Trees && face.transparency<=TransparencyOption::Structures && face.transparency!=TransparencyOption::Bridges && !_transparency_lock.Test(face.transparency)) transparent+=2;
		bool selected = int(face.tile.base()) == _terrain_highlight;
		int tx = TileX(face.tile) * TILE_SIZE, ty = TileY(face.tile) * TILE_SIZE;
		bool rail_selection = (_thd.drawstyle & HT_DRAG_MASK) == HT_LINE || (_thd.drawstyle & HT_RAIL);
		if (!rail_selection && _thd.drawstyle != HT_NONE && (_thd.diagonal ? IsInsideRotatedRectangle(tx, ty) :
			tx >= _thd.pos.x && ty >= _thd.pos.y && tx < _thd.pos.x + _thd.size.x && ty < _thd.pos.y + _thd.size.y)) selected = true;
		for (const auto &world : face.p) {
			auto p = clip(world);
			auto c = selected ? std::array<float,3>{0.95f,0.80f,0.22f} : face.colour;
			if (selected && false) c = {0.9f,0.2f,0.15f};
			vertices.push_back({p[0], p[1], p[2], c[0], c[1], c[2], transparent,
				world.x, world.y, float(tx), float(ty), selected ? -1.0f : float(face.layer)});
		}
	}
	}
	/* Track-piece previews use the same direction/parity convention as DrawTileSelection. */
	if ((_thd.drawstyle & HT_DRAG_MASK) == HT_LINE || (_thd.drawstyle & HT_RAIL)) {
		int direction = _thd.drawstyle & HT_DIR_MASK;
		static constexpr Track pieces[6][2] = {{TRACK_X,TRACK_X},{TRACK_Y,TRACK_Y},{TRACK_UPPER,TRACK_LOWER},{TRACK_LOWER,TRACK_UPPER},{TRACK_LEFT,TRACK_RIGHT},{TRACK_RIGHT,TRACK_LEFT}};
		if (direction < 6) for (int y=std::max(0,_thd.pos.y/int(TILE_SIZE));y<std::min(int(Map::MaxY()),(_thd.pos.y+_thd.size.y)/int(TILE_SIZE));++y) {
			for (int x=std::max(0,_thd.pos.x/int(TILE_SIZE));x<std::min(int(Map::MaxX()),(_thd.pos.x+_thd.size.x)/int(TILE_SIZE));++x) {
				if ((_thd.drawstyle & HT_DRAG_MASK)==HT_LINE && !IsPartOfAutoLine(x*TILE_SIZE,y*TILE_SIZE)) continue;
				int side=0;
				if ((_thd.drawstyle & HT_DRAG_MASK)==HT_LINE && direction>=2) side=std::abs(std::abs(x-_thd.selstart.x/int(TILE_SIZE))-std::abs(y-_thd.selstart.y/int(TILE_SIZE)));
				if (side>1) continue;
				TileIndex tile=TileXY(x,y);
				auto [slope,z]=GetFoundationPixelSlope(tile);
				TerrainTile preview{slope,slope,z,z,TileType::Clear};
				preview.tracks=TrackToTrackBits(pieces[direction][side]);
				std::vector<TerrainFace> faces;
				BuildRail3D(faces,preview,tile);
				for (const auto &face : faces) for (auto world : face.p) {
					world.z+=1;
					auto p=clip(world);
					vertices.push_back({p[0],p[1],p[2],1.0f,false?0.15f:0.85f,0.1f});
				}
			}
		}
	}
	/* Library vehicles are drawn on the GPU in retained mode; the reference path expands them here. */
	static std::vector<TerrainFace> expanded;
	expanded.clear();
	if (!_proof_retained) {
		AppendVehicleInstances3D(expanded, camera);
		AppendEffectInstances3D(expanded, camera);
	}
	for (const auto *list : {&_train3d_faces, &expanded}) for (const auto &face : *list) for (const auto &world : face.p) {
		auto p = clip(world);
		float transparent = face.transparency != TransparencyOption::Invalid && IsTransparencySet(face.transparency) ? 1.0f : 0.0f;
		if (face.transparency>=TransparencyOption::Trees && face.transparency<=TransparencyOption::Structures && face.transparency!=TransparencyOption::Bridges && !_transparency_lock.Test(face.transparency)) transparent+=2;
		vertices.push_back({p[0],p[1],p[2],face.colour[0],face.colour[1],face.colour[2],transparent});
	}
	Debug(driver, 2, "PHASE7 geometry retained={} visible={} cpu_vertices={}", _proof_retained, visible_chunks, vertices.size());
	_proof_projection_us += ProofMicros(prepare_start);
	p_glPolygonMode(GL_FRONT_AND_BACK, _terrain_wireframe ? GL_LINE : GL_FILL);
	_glUseProgram(_proof_program); _glBindVertexArray(_proof_vao);
	ProofPerspectiveUniforms(_proof_locations, camera);
	ProofUniform(_proof_locations.autoFocus, _proof_focus[0], _proof_focus[1], _proof_focus[2], _proof_focus[3]);
	_proof_shadow_info = {};
	ProofShadowUniforms(_proof_locations);
	bool textured = _ground_textured && !_ground_flat_forced;
	if (textured) UploadGroundMap();
	_glUniform1i(_proof_locations.groundTextures, 1);
	_glUniform1i(_proof_locations.groundMap, 2);
	ProofUniform(_proof_locations.groundCrisp, Ground3D::FIELD_0, Ground3D::FIELD_8, Ground3D::CONCRETE, 0);
	ProofUniform(_proof_locations.groundInfo, textured ? 1.0f : 0.0f, float(_ground_map_width), float(_ground_map_height), float(std::bit_width(unsigned(Ground3D::TEXTURE_SIZE)) - 1));
	bool measure_gpu = _proof_queries[0] != 0 && GetDebugLevel(DebugLevelID::driver) >= 2;
	size_t query = _proof_query_next;
	if (measure_gpu) {
		/* Reusing a query collects its earlier result, normally finished renders ago. */
		_proof_query_next = (query + 1) % _proof_queries.size();
		if (_proof_query_pending[query]) {
			GLuint64 elapsed = 0;
			p_glGetQueryObjectui64v(_proof_queries[query], GL_QUERY_RESULT, &elapsed);
			_proof_gpu_ns += elapsed;
			_proof_query_pending[query] = false;
		}
		p_glBeginQuery(GL_TIME_ELAPSED, _proof_queries[query]);
	}
	if (_proof_retained) {
		if (_proof_generation != _terrain_generation) {
			for (auto &gpu : _proof_chunks) if (gpu.buffer) _glDeleteBuffers(1, &gpu.buffer);
			_proof_chunks.clear();
			_proof_generation = _terrain_generation;
		}
		while (_proof_chunks.size() > _terrain_chunks.size()) {
			if (_proof_chunks.back().buffer) _glDeleteBuffers(1, &_proof_chunks.back().buffer);
			_proof_chunks.pop_back();
		}
		_proof_chunks.resize(_terrain_chunks.size());
		ProofUniform(_proof_locations.cameraA, camera.cx, camera.cy, camera.scale, width);
		ProofUniform(_proof_locations.cameraB, camera.cosine, camera.sine, camera.tilt, height);
		ProofUniform(_proof_locations.cameraC, extent * 4, camera.pz, 0, 1);
		ProofUniform(_proof_locations.selection, _thd.pos.x, _thd.pos.y, _thd.pos.x + _thd.size.x, _thd.pos.y + _thd.size.y);
		bool rail_selection = (_thd.drawstyle & HT_DRAG_MASK) == HT_LINE || (_thd.drawstyle & HT_RAIL);
		ProofUniform(_proof_locations.selectedTile, _terrain_highlight < 0 ? -1.0f : float(TileX(TileIndex(_terrain_highlight)) * TILE_SIZE),
			_terrain_highlight < 0 ? -1.0f : float(TileY(TileIndex(_terrain_highlight)) * TILE_SIZE), !rail_selection && _thd.drawstyle != HT_NONE ? (_thd.diagonal ? 2 : 1) : 0, false);
		ProofUniform(_proof_locations.visibility, _game_mode == GameMode::Menu ? 0 : _transparency_opt.base(), _game_mode == GameMode::Menu ? 0 : (_transparency_opt & _invisibility_opt).base(), _transparency_lock.base(), 0);
		std::vector<ProofStaticVertex> upload;
		std::vector<size_t> sizes, writes;
		/* Upload a chunk's retained geometry if it changed, then draw it; the sun shadow pass draws chunks its own view does not. */
		auto draw_chunk = [&](size_t i) {
			auto &chunk = _terrain_chunks[i];
			auto &gpu = _proof_chunks[i];
			if (!gpu.buffer) _glGenBuffers(1, &gpu.buffer);
			BindProofVertices(gpu.buffer, true);
			if (gpu.revision != chunk.revision) {
				/* Rewrite only the slots of tiles rebuilt since the last write; a fresh or outgrown buffer is written whole. */
				auto upload_start = std::chrono::steady_clock::now();
				sizes.resize(chunk.spans.size());
				for (size_t t = 0; t < chunk.spans.size(); ++t) sizes[t] = 3 * (chunk.spans[t].end - (t == 0 ? 0 : chunk.spans[t - 1].end));
				auto changed = chunk.gpu_changes.Take(chunk.spans.size(), false);
				if (gpu.count == 0 || !_proof_tile_meshes) changed.clear(); // Whole buffer; baseline keeps the old full uploads.
				bool fresh = changed.empty() || gpu.slots.first.size() != sizes.size();
				bool whole = gpu.slots.Update(sizes, changed, writes);
				_proof_slot_relayouts += whole && !fresh;
				auto convert = [&](size_t t) {
					size_t offset = upload.size();
					for (size_t f = t == 0 ? 0 : chunk.spans[t - 1].end; f < chunk.spans[t].end; ++f) {
						const auto &face = chunk.faces[f];
						for (const auto &p : face.p) {
							upload.push_back({p.x, p.y, p.z, face.colour[0], face.colour[1], face.colour[2],
								static_cast<uint16_t>(TileX(face.tile) * TILE_SIZE), static_cast<uint16_t>(TileY(face.tile) * TILE_SIZE),
								static_cast<uint16_t>(face.transparency), static_cast<int16_t>(face.layer)});
						}
					}
					return offset;
				};
				upload.clear();
				size_t bytes = 0;
				if (whole) {
					for (size_t t : writes) {
						upload.resize(gpu.slots.first[t]);
						convert(t);
					}
					upload.resize(gpu.slots.size);
					_glBufferData(GL_ARRAY_BUFFER, upload.size()*sizeof(ProofStaticVertex), upload.data(), GL_STATIC_DRAW);
					bytes = upload.size()*sizeof(ProofStaticVertex);
					gpu.count = static_cast<GLsizei>(gpu.slots.size);
				} else {
					for (size_t t : writes) {
						if (gpu.slots.count[t] == 0) continue;
						size_t offset = convert(t);
						_glBufferSubData(GL_ARRAY_BUFFER, gpu.slots.first[t]*sizeof(ProofStaticVertex), (upload.size()-offset)*sizeof(ProofStaticVertex), upload.data()+offset);
					}
					bytes = upload.size()*sizeof(ProofStaticVertex);
					++_proof_slot_writes;
				}
				_proof_upload_bytes += bytes;
				_proof_static_upload_bytes += bytes;
				_proof_upload_us += ProofMicros(upload_start);
				gpu.revision = chunk.revision;
				gpu.slots.Runs(gpu.run_first, gpu.run_count);
			}
			p_glMultiDrawArrays(GL_TRIANGLES, gpu.run_first.data(), gpu.run_count.data(), static_cast<GLsizei>(gpu.run_first.size()));
			_proof_terrain_runs += gpu.run_first.size();
		};
		auto draw_terrain = [&](const Terrain3D::Camera &view) {
			for (size_t i = 0; i < _terrain_chunks.size(); ++i) {
				const auto &chunk = _terrain_chunks[i];
				if (!chunk.faces.empty() && (!partial || reaches[i]) && Terrain3D::Visible(chunk.low, chunk.high, view)) draw_chunk(i);
			}
		};
		int shadows = ProofShadowMode();
		if (shadows == 2 && RenderProofSunShadow(camera, extent, draw_terrain)) {
			/* Back to this view's target and uniforms; the sun pass used the perspective path with the light's matrix. */
			p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_fbo);
			_glViewport(0, 0, width, height);
			_glUseProgram(_proof_program);
			ProofPerspectiveUniforms(_proof_locations, camera);
			ProofUniform(_proof_locations.cameraA, camera.cx, camera.cy, camera.scale, width);
			ProofUniform(_proof_locations.cameraB, camera.cosine, camera.sine, camera.tilt, height);
			ProofUniform(_proof_locations.cameraC, extent * 4, camera.pz, 0, 1);
		}
		ProofShadowUniforms(_proof_locations);
		draw_terrain(camera);
		const ProofStaticStream *statics = DrawProofStaticModels(camera, extent, width, height, partial ? &reaches : nullptr);
		size_t vehicle_batches = DrawProofVehicles(camera, extent, width, height);
		if (shadows >= 1) DrawProofContact(camera, extent, width, height, statics, vehicle_batches);
	}
	ProofUniform(_proof_locations.cameraC, 0, 0, 0, 0);
	BindProofVertices(_proof_vbo);
	auto upload_start = std::chrono::steady_clock::now();
	_glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(ProofVertex), vertices.data(), GL_STREAM_DRAW);
	_proof_upload_us += ProofMicros(upload_start);
	_proof_upload_bytes += vertices.size() * sizeof(ProofVertex);
	_proof_prepare_us += ProofMicros(prepare_start);
	_glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices.size()));
	DrawProofFocusOutline(image,camera);
	if (partial) {
		_glDisable(GL_STENCIL_TEST);
		p_glStencilMask(0xFF);
	}
	if (camera.Perspective() && camera.dark > 0 && !DrawProofFade(camera.dark)) return false;
	if (measure_gpu) p_glEndQuery(GL_TIME_ELAPSED);
	return FinishProofImage(image, direct, measure_gpu, query);
}

/** Darken the whole rendered view towards black by `dark`, for a view fading out in a tunnel. Its colours are scaled,
 * so the image keeps its alpha. */
static bool DrawProofFade(float dark)
{
	if (_proof_fade_program == 0) {
		const char *vertex = R"GLSL(#version 150
void main() {
 vec2 corner=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));
 gl_Position=vec4(corner*2.0-1.0,0,1);
})GLSL";
		const char *fragment = R"GLSL(#version 150
uniform float dark; out vec4 pixel;
void main() { pixel=vec4(vec3(1.0-dark),1); })GLSL";
		GLuint vs = _glCreateShader(GL_VERTEX_SHADER), fs = _glCreateShader(GL_FRAGMENT_SHADER);
		_glShaderSource(vs, 1, &vertex, nullptr); _glCompileShader(vs);
		_glShaderSource(fs, 1, &fragment, nullptr); _glCompileShader(fs);
		bool valid = VerifyShader(vs) && VerifyShader(fs);
		if (valid) {
			_proof_fade_program = _glCreateProgram();
			_glAttachShader(_proof_fade_program, vs); _glAttachShader(_proof_fade_program, fs);
			_glBindFragDataLocation(_proof_fade_program, 0, "pixel");
			_glLinkProgram(_proof_fade_program);
			valid = VerifyProgram(_proof_fade_program);
		}
		_glDeleteShader(vs); _glDeleteShader(fs);
		if (!valid) return false;
		_proof_fade_dark = _glGetUniformLocation(_proof_fade_program, "dark");
		_glGenVertexArrays(1, &_proof_fade_vao);
	}
	p_glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
	_glDisable(GL_DEPTH_TEST);
	GLint src_rgb, dst_rgb, src_alpha, dst_alpha;
	_glGetIntegerv(GL_BLEND_SRC_RGB, &src_rgb); _glGetIntegerv(GL_BLEND_DST_RGB, &dst_rgb);
	_glGetIntegerv(GL_BLEND_SRC_ALPHA, &src_alpha); _glGetIntegerv(GL_BLEND_DST_ALPHA, &dst_alpha);
	_glEnable(GL_BLEND);
	_glBlendFunc(GL_ZERO, GL_SRC_COLOR);
	_glUseProgram(_proof_fade_program);
	_glBindVertexArray(_proof_fade_vao);
	_glUniform1f(_proof_fade_dark, dark);
	_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	_glDisable(GL_BLEND);
	if (p_glBlendFuncSeparate != nullptr) {
		p_glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
	} else {
		_glBlendFunc(src_rgb, dst_rgb);
	}
	return true;
}

/** Present a rendered view directly or read it back. */
static bool FinishProofImage(ProofImage &image, bool direct, bool measure_gpu, size_t query)
{
	const int width = image.key[0], height = image.key[1];
	if (direct) {
		if (measure_gpu) _proof_query_pending[query] = true;
		TrimTerrainMeshes();
		GLenum error = _glGetError();
		if (error != GL_NO_ERROR) Debug(driver, 0, "3D proof OpenGL error: {}", error);
		return error == GL_NO_ERROR;
	}
	image.pixels.resize(static_cast<size_t>(width) * height);
	_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	_glPixelStorei(GL_PACK_ALIGNMENT, 4); _glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	_glPixelStorei(GL_PACK_SKIP_ROWS, 0); _glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	auto read_start = std::chrono::steady_clock::now();
	p_glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, image.pixels.data());
	_proof_read_us += ProofMicros(read_start);
	if (measure_gpu) {
		GLuint64 elapsed = 0;
		p_glGetQueryObjectui64v(_proof_queries[query], GL_QUERY_RESULT, &elapsed);
		_proof_gpu_ns += elapsed;
	}
	TrimTerrainMeshes();
	GLenum error = _glGetError();
	if (error != GL_NO_ERROR) Debug(driver, 0, "3D proof OpenGL error: {}", error);
	return error == GL_NO_ERROR;
}

static bool ProofViewDrawable(const Viewport &vp)
{
	return vp.width > 0 && vp.height > 0 && vp.width <= 4096 && vp.height <= 4096 && static_cast<size_t>(vp.width) * vp.height <= 8 * 1024 * 1024;
}

/** A view's image identity; the exact camera angle is part of it, so views with their own cameras never share images.
 * A perspective view does not depend on the scroll position, so the followed vehicle moving alone keeps its key. */
static std::array<int, 28> ProofViewKey(const Viewport &vp, bool direct)
{
	auto view = ViewTerrainAngle(vp);
	bool perspective = view.fov > 0;
	int kind=0; Vec target{}; float radius=24;
	if (_proof_auto_visibility && _game_mode != GameMode::Menu) {
		const Vehicle *vehicle=nullptr;
		for (const Window *w : Window::IterateFromFront()) {
			if (w->viewport==&vp) {
				vehicle=Vehicle::GetIfValid(w->viewport->follow_vehicle);
				break;
			}
		}
		if (vp.camera_3d == ViewportCamera3D::Fixed) vehicle = nullptr;
		/* Construction takes priority in ordinary map views; ride views focus only on their own vehicle. */
		if (!perspective && (_thd.drawstyle!=HT_NONE || _terrain_highlight>=0)) {
			int x=_thd.pos.x+_thd.size.x/2, y=_thd.pos.y+_thd.size.y/2;
			if ((_thd.drawstyle&HT_DRAG_MASK)==HT_LINE) { x=_thd.selend.x; y=_thd.selend.y; }
			if (_terrain_highlight>=0) { TileIndex tile(_terrain_highlight); x=TileX(tile)*TILE_SIZE+8; y=TileY(tile)*TILE_SIZE+8; }
			x=std::clamp(x,0,int(Map::MaxX()*TILE_SIZE)-1); y=std::clamp(y,0,int(Map::MaxY()*TILE_SIZE)-1);
			target={float(x),float(y),float(GetSlopePixelZ(x,y))+1}; kind=1;
		} else {
			if (vp.camera_3d==ViewportCamera3D::Cab || vp.camera_3d==ViewportCamera3D::Chase) vehicle=Vehicle::GetIfValid(vp.camera_vehicle);
			if (vehicle==nullptr && !perspective) for (const Window *w : Window::IterateFromFront()) {
				if (w->window_class==WindowClass::VehicleView) { vehicle=Vehicle::GetIfValid(VehicleID(w->window_number)); break; }
			}
			if (vehicle!=nullptr && !vehicle->vehstatus.Test(VehState::Hidden) && !RideEnded3D(*vehicle)) {
				target={float(vehicle->x_pos),float(vehicle->y_pos),float(vehicle->z_pos)};
				kind=vp.camera_3d==ViewportCamera3D::Cab?3:2;
				radius=vehicle->type==VehicleType::Aircraft || vehicle->type==VehicleType::Ship?40.0f:24.0f;
				if (kind==3) {
					auto forward=Terrain3D::HeadingForward(view.heading);
					float pitch=view.pitch*3.14159265f/180;
					target={view.eye.x+forward[0]*24*std::cos(pitch),view.eye.y+forward[1]*24*std::cos(pitch),view.eye.z+24*std::sin(pitch)};
				}
			}
		}
	}
	if (kind != 0 && !Terrain3D::Visible({target.x-radius,target.y-radius,target.z-radius}, {target.x+radius,target.y+radius,target.z+radius}, ViewportTerrainCamera(vp))) {
		kind = 0; target = {}; radius = 24;
	}
	return {vp.width, vp.height, perspective ? 0 : vp.virtual_left & ScaleByZoom(-1, vp.zoom), perspective ? 0 : vp.virtual_top & ScaleByZoom(-1, vp.zoom), static_cast<int>(vp.zoom), static_cast<int>(Map::SizeX()), static_cast<int>(Map::SizeY()), int(direct),
		std::bit_cast<int>(view.yaw), std::bit_cast<int>(view.tilt), std::bit_cast<int>(view.pz),
		std::bit_cast<int>(view.eye.x), std::bit_cast<int>(view.eye.y), std::bit_cast<int>(view.eye.z),
		std::bit_cast<int>(view.heading), std::bit_cast<int>(view.pitch), std::bit_cast<int>(view.fov),
		int(view.hides), std::bit_cast<int>(view.hidden.x), std::bit_cast<int>(view.hidden.y), std::bit_cast<int>(view.hidden.z), std::bit_cast<int>(view.dark), kind, std::bit_cast<int>(target.x), std::bit_cast<int>(target.y), std::bit_cast<int>(target.z), std::bit_cast<int>(radius), std::bit_cast<int>(view.magnify)};
}

/** Refresh the authoritative snapshots once per drawing pass (every call in the baseline mode). */
static void RefreshProof3DScene()
{
	if (!_proof_optimized || !_proof_refreshed) {
		auto start = std::chrono::steady_clock::now();
		RefreshTerrain();
		RefreshTrains3D();
		RefreshEffects3D();
		_proof_scan_us += ProofMicros(start);
		_proof_refreshed = true;
	}
	static std::array<uint, 4> last_visibility{};
	std::array<uint, 4> visibility{_transparency_opt.base(), _invisibility_opt.base(), uint(_game_mode), _transparency_lock.base()};
	if (visibility != last_visibility) { last_visibility = visibility; InvalidateProof3DImages(); }
	static std::array<int, 9> last_selection{};
	std::array<int, 9> selection{_thd.pos.x,_thd.pos.y,_thd.size.x,_thd.size.y,int(_thd.drawstyle),int(false),_thd.selstart.x,_thd.selstart.y,int(_thd.diagonal)};
	if (selection != last_selection) { last_selection = selection; InvalidateProof3DImages(); }
}

/** Pixels whose colour differs by more than rounding between two renders of the same view. */
static size_t CountDifferingPixels(const std::vector<Colour> &image, const std::vector<Colour> &reference)
{
	size_t differing = 0;
	for (size_t i = 0; i < image.size() && i < reference.size(); ++i) {
		const auto &a = image[i], &b = reference[i];
		if (std::abs(int(a.r)-int(b.r)) > 2 || std::abs(int(a.g)-int(b.g)) > 2 || std::abs(int(a.b)-int(b.b)) > 2) ++differing;
	}
	return differing;
}

/** Read a view texture back, rows from the bottom (diagnostics only). */
static std::vector<Colour> ReadProofTexture(GLuint texture, int width, int height)
{
	ProofGLState restore;
	GLuint fbo = 0;
	p_glGenFramebuffers(1, &fbo);
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
	p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	std::vector<Colour> pixels(static_cast<size_t>(width) * height);
	_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	_glPixelStorei(GL_PACK_ALIGNMENT, 4); _glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	_glPixelStorei(GL_PACK_SKIP_ROWS, 0); _glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	if (p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) p_glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
	p_glDeleteFramebuffers(1, &fbo);
	return pixels;
}

/** Partial render reference: a kept image drawn again in part must match the same view rendered whole. */
static void VerifyProofPartial(const ProofImage &image)
{
	const int width = image.key[0], height = image.key[1];
	ProofImage reference{image.key, {}};
	bool valid = RenderProofImage(reference);
	size_t differing = 0, exact = 0;
	if (valid) {
		auto kept = ReadProofTexture(image.texture, width, height), whole = ReadProofTexture(reference.texture, width, height);
		differing = CountDifferingPixels(kept, whole);
		for (size_t i = 0; i < kept.size(); ++i) exact += kept[i].r != whole[i].r || kept[i].g != whole[i].g || kept[i].b != whole[i].b;
	}
	ReleaseProofImage(reference);
	Debug(driver, 0, "PHASE95G partial-reference valid={} differing={} exact={} pixels={}", valid, differing, exact, static_cast<size_t>(width) * height);
}

/** The cached image of a view, rendering it when missing; nullptr after a failure has reverted to classic 2D. */
static ProofImage *FindProofImage(const Viewport &vp, bool direct)
{
	auto key = ProofViewKey(vp, direct);
	auto found = std::ranges::find(_proof_images, key, &ProofImage::key);
	if (found != _proof_images.end()) {
		/* Scoped invalidation reference: an image kept through changes must match a fresh render. */
		if (!direct && _proof_verify_gpu && _proof_scoped && _proof_frame_serial % 16 == 1 && found->verified != _proof_frame_serial) {
			found->verified = _proof_frame_serial;
			auto camera = KeyTerrainCamera(key);
			if (_proof_deferred) PrepareTerrainMeshes(&camera);
			ProofImage reference{key, {}};
			bool valid = RenderProofImage(reference);
			Debug(driver, 1, "PHASE7 kept-reference valid={} differing={} pixels={} perspective={}", valid, valid ? CountDifferingPixels(found->pixels, reference.pixels) : 0, found->pixels.size(), KeyTerrainCamera(key).Perspective());
			ReleaseProofImage(reference);
		}
		if (!found->stale.Empty()) {
			auto start = std::chrono::steady_clock::now();
			auto camera = KeyTerrainCamera(key);
			if (_proof_deferred) PrepareTerrainMeshes(&camera);
			_proof_scan_us += ProofMicros(start);
			auto render_start = std::chrono::steady_clock::now();
			bool valid = RenderProofImage(*found);
			found->stale = {};
			if (!valid) {
				Debug(driver, 0, "3D proof failed; reverting to classic 2D");
				DestroyProof3D();
				MarkWholeScreenDirty();
				return nullptr;
			}
			++_proof_partial_renders;
			_proof_render_us += ProofMicros(render_start);
			found->revision = ++_proof_image_revision;
			if (_proof_verify_partial) VerifyProofPartial(*found);
		}
		return &*found;
	}
	if (_proof_deferred) {
		auto start = std::chrono::steady_clock::now();
		auto camera = KeyTerrainCamera(key);
		auto rebuilt = _proof_chunks_rebuilt, cold = _proof_cold_chunks;
		PrepareTerrainMeshes(&camera);
		auto elapsed = ProofMicros(start);
		_proof_scan_us += elapsed;
		Debug(driver, 2, "PHASE7 view width={} height={} left={} top={} zoom={} prepare_us={} chunks_rebuilt={} cold_chunks={}", vp.width, vp.height, key[2], key[3], int(vp.zoom), elapsed, _proof_chunks_rebuilt - rebuilt, _proof_cold_chunks - cold);
	}
	/* Bound retained image memory (read-back pixels or GPU textures) as well as viewport count.
	 * A frame may be split into many clips by overlapping windows; retain all ordinary views. */
	size_t bytes = static_cast<size_t>(vp.width) * vp.height * sizeof(Colour);
	for (const auto &cached : _proof_images) bytes += static_cast<size_t>(cached.key[0]) * cached.key[1] * sizeof(Colour);
	while (!_proof_images.empty() && (_proof_images.size() >= (_proof_optimized ? 32U : 2U) || bytes > 128U * 1024 * 1024)) {
		bytes -= static_cast<size_t>(_proof_images.front().key[0]) * _proof_images.front().key[1] * sizeof(Colour);
		ReleaseProofImage(_proof_images.front());
		_proof_images.erase(_proof_images.begin());
	}
	ProofImage image{key, {}};
	auto start = std::chrono::steady_clock::now();
	if (!RenderProofImage(image)) {
		Debug(driver, 0, "3D proof failed; reverting to classic 2D");
		ReleaseProofImage(image);
		DestroyProof3D();
		MarkWholeScreenDirty();
		return nullptr;
	}
	if (!direct && _proof_verify_gpu && _proof_retained && _proof_frame_serial % 16 == 1) {
		if (_proof_instanced && _proof_instance_program != 0 && p_glVertexAttribDivisor != nullptr && p_glDrawArraysInstanced != nullptr) {
			ProofImage reference{key, {}};
			_proof_instanced = false;
			bool valid = RenderProofImage(reference);
			_proof_instanced = true;
			Debug(driver, 1, "PHASE7 instance-reference valid={} differing={} pixels={} perspective={}", valid, valid ? CountDifferingPixels(image.pixels, reference.pixels) : 0, image.pixels.size(), KeyTerrainCamera(key).Perspective());
		}
		ProofImage reference{key, {}};
		_proof_retained = false;
		bool valid = RenderProofImage(reference);
		_proof_retained = true;
		auto eye = KeyTerrainCamera(key).eye;
		Debug(driver, 1, "PHASE7 gpu-reference valid={} differing={} pixels={} perspective={} eye={},{},{}", valid, valid ? CountDifferingPixels(image.pixels, reference.pixels) : 0, image.pixels.size(), KeyTerrainCamera(key).Perspective(), eye.x, eye.y, eye.z);
	}
	Debug(driver, 1, "3D proof {}x{} render{}: {} us", vp.width, vp.height, direct ? "" : "/readback", ProofMicros(start));
	++_proof_renders;
	_proof_render_us += ProofMicros(start);
	image.revision = ++_proof_image_revision;
	_proof_images.push_back(std::move(image));
	return &_proof_images.back();
}

/** Stencil value for a view's pixels in the layer. */
static uint8_t ProofLayerId(const Viewport &vp)
{
	for (size_t i = 1; i < _proof_layer_ids.size(); ++i) if (_proof_layer_ids[i] == &vp) return uint8_t(i);
	for (size_t i = 1; i < _proof_layer_ids.size(); ++i) {
		if (_proof_layer_ids[i] == nullptr) { _proof_layer_ids[i] = &vp; _proof_layer_revisions[i] = 0; return uint8_t(i); }
	}
	/* Every value is taken: start again, and the whole screen redraws to reclaim its pixels. */
	_proof_layer_ids.fill(nullptr);
	_proof_layer_revisions.fill(0);
	_proof_layer_ids[1] = &vp;
	MarkWholeScreenDirty();
	return 1;
}

/** CPU equivalent of the layer shader, used for readback/giant screenshots and
 * the independent GPU compositing check. Coordinates start at the view's top left;
 * source rows are bottom-up. Blocks stay aligned across dirty clips and windows. */
static Colour SampleProofRetro(const std::vector<Colour> &pixels, int width, int height, int x, int y)
{
	const auto &gui = _settings_client.gui;
	int size = gui.renderer_3d_retro ? std::clamp<int>(gui.renderer_3d_pixel_size, 1, 4) : 1;
	int sx = std::min(x / size * size + size / 2, width - 1);
	int sy = std::min(y / size * size + size / 2, height - 1);
	Colour colour = pixels[static_cast<size_t>(height - 1 - sy) * width + sx];
	if (!gui.renderer_3d_retro || !gui.renderer_3d_retro_colours) return colour;
	static constexpr int bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	float threshold = gui.renderer_3d_retro_dither ? ((bayer[((y / size) % 4) * 4 + (x / size) % 4] + 0.5f) / 16.0f - 0.5f) * 0.65f : 0;
	auto quantize = [threshold](uint8_t c) -> uint8_t {
		return uint8_t(std::round(std::clamp(std::floor(float(c) / 255.0f * 7.0f + 0.5f + threshold), 0.0f, 7.0f) / 7.0f * 255.0f));
	};
	colour.r = quantize(colour.r); colour.g = quantize(colour.g); colour.b = quantize(colour.b);
	return colour;
}

/** Size the layer to the screen; a new layer is black and owned by no view. */
static bool EnsureProofLayer()
{
	int width = _screen.width, height = _screen.height;
	if (_proof_layer_fbo != 0 && _proof_layer_width == width && _proof_layer_height == height) return true;
	if (_proof_layer_program == 0) {
		const char *vertex = R"GLSL(#version 150
uniform vec4 rect; uniform vec2 screen;
void main() {
 vec2 corner=vec2(float(gl_VertexID&1),float(gl_VertexID>>1));
 gl_Position=vec4(mix(rect.xy,rect.zw,corner)/screen*2.0-1.0,0,1);
})GLSL";
		/* Layer rows are screen rows; a view texture is bottom-up, as rendered. */
		const char *fragment = R"GLSL(#version 150
uniform sampler2D view; uniform vec4 origin;
uniform vec4 retro; // block size, colour reduction, dithering, reserved
out vec4 pixel;
const int bayer[16]=int[16](0,8,2,10,12,4,14,6,3,11,1,9,15,7,13,5);
void main() {
 ivec2 p=ivec2(gl_FragCoord.xy)-ivec2(origin.xy);
 int size=int(retro.x);
 ivec2 cell=p/size;
 ivec2 sampleAt=min(cell*size+size/2,textureSize(view,0)-1);
 vec3 colour=texelFetch(view,ivec2(sampleAt.x,int(origin.z)-1-sampleAt.y),0).rgb;
 if(retro.y>0.5) {
  float threshold=retro.z>0.5 ? ((float(bayer[(cell.y%4)*4+cell.x%4])+0.5)/16.0-0.5)*0.65 : 0.0;
  colour=clamp(floor(colour*7.0+0.5+threshold),0.0,7.0)/7.0;
 }
 pixel=vec4(colour,1);
})GLSL";
		GLuint vs = _glCreateShader(GL_VERTEX_SHADER), fs = _glCreateShader(GL_FRAGMENT_SHADER);
		_glShaderSource(vs, 1, &vertex, nullptr); _glCompileShader(vs);
		_glShaderSource(fs, 1, &fragment, nullptr); _glCompileShader(fs);
		bool valid = VerifyShader(vs) && VerifyShader(fs);
		if (valid) {
			_proof_layer_program = _glCreateProgram();
			_glAttachShader(_proof_layer_program, vs); _glAttachShader(_proof_layer_program, fs);
			_glBindFragDataLocation(_proof_layer_program, 0, "pixel");
			_glLinkProgram(_proof_layer_program);
			valid = VerifyProgram(_proof_layer_program);
		}
		_glDeleteShader(vs); _glDeleteShader(fs);
		if (!valid) return false;
		_proof_layer_view = _glGetUniformLocation(_proof_layer_program, "view");
		_proof_layer_screen = _glGetUniformLocation(_proof_layer_program, "screen");
		_proof_layer_rect = _glGetUniformLocation(_proof_layer_program, "rect");
		_proof_layer_origin = _glGetUniformLocation(_proof_layer_program, "origin");
		_proof_layer_retro = _glGetUniformLocation(_proof_layer_program, "retro");
		_glGenVertexArrays(1, &_proof_layer_vao);
	}
	if (_proof_layer_fbo == 0) {
		p_glGenFramebuffers(1, &_proof_layer_fbo);
		_glGenTextures(1, &_proof_layer_colour);
		p_glGenRenderbuffers(1, &_proof_layer_stencil);
	}
	_glActiveTexture(GL_TEXTURE0);
	_glBindTexture(GL_TEXTURE_2D, _proof_layer_colour);
	_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	p_glBindRenderbuffer(GL_RENDERBUFFER, _proof_layer_stencil);
	p_glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
	p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_layer_fbo);
	p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _proof_layer_colour, 0);
	p_glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, _proof_layer_stencil);
	if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return false;
	_glViewport(0, 0, width, height);
	_glDisable(GL_SCISSOR_TEST);
	_glClearColor(0, 0, 0, 1);
	p_glClearStencil(0);
	p_glStencilMask(0xFF);
	_glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
	_proof_layer_width = width;
	_proof_layer_height = height;
	_proof_layer_revisions.fill(0);
	return true;
}

/** Check actual compositing, which the mesh/readback references do not exercise. Diagnostic only. */
static void VerifyProof3DLayer()
{
	if (!_proof_verify_layer || !_proof_layer_active || !ProofDirectMode() || _proof_frame_serial % 16 != 1) return;
	ProofGLState restore;
	size_t size = static_cast<size_t>(_proof_layer_width) * _proof_layer_height;
	std::vector<Colour> layer(size);
	std::vector<uint8_t> owners(size);
	_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
	_glPixelStorei(GL_PACK_ALIGNMENT, 1); _glPixelStorei(GL_PACK_ROW_LENGTH, 0);
	_glPixelStorei(GL_PACK_SKIP_ROWS, 0); _glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, _proof_layer_fbo);
	p_glReadPixels(0, 0, _proof_layer_width, _proof_layer_height, GL_BGRA, GL_UNSIGNED_BYTE, layer.data());
	p_glReadPixels(0, 0, _proof_layer_width, _proof_layer_height, GL_STENCIL_INDEX, GL_UNSIGNED_BYTE, owners.data());
	GLuint reference = 0;
	p_glGenFramebuffers(1, &reference);
	p_glBindFramebuffer(GL_READ_FRAMEBUFFER, reference);
	const auto *screen = static_cast<const Colour *>(_screen.dst_ptr);
	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr || w->IsShaded()) continue;
		const Viewport &vp = *w->viewport;
		auto id = std::ranges::find(_proof_layer_ids, &vp);
		if (id == _proof_layer_ids.end() || id == _proof_layer_ids.begin()) continue;
		auto found = std::ranges::find(_proof_images, ProofViewKey(vp, true), &ProofImage::key);
		if (found == _proof_images.end() || found->texture == 0) continue;
		p_glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, found->texture, 0);
		bool valid = p_glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		std::vector<Colour> pixels(static_cast<size_t>(vp.width) * vp.height);
		if (valid) p_glReadPixels(0, 0, vp.width, vp.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
		size_t checked = 0, differing = 0;
		if (valid) for (int y = std::max(vp.top, 0); y < std::min(vp.top + vp.height, _proof_layer_height); ++y) {
			for (int x = std::max(vp.left, 0); x < std::min(vp.left + vp.width, _proof_layer_width); ++x) {
				size_t offset = static_cast<size_t>(y) * _proof_layer_width + x;
				if (owners[offset] != id - _proof_layer_ids.begin() || screen[static_cast<size_t>(y) * _screen.pitch + x].a != 0) continue;
				const auto &a = layer[offset];
				Colour b = SampleProofRetro(pixels, vp.width, vp.height, x - vp.left, y - vp.top);
				++checked;
				if (std::abs(int(a.r) - int(b.r)) > 2 || std::abs(int(a.g) - int(b.g)) > 2 || std::abs(int(a.b) - int(b.b)) > 2) ++differing;
			}
		}
		Debug(driver, 1, "PHASE7 layer-reference valid={} differing={} pixels={} view={}", valid, differing, checked, id - _proof_layer_ids.begin());
	}
	p_glDeleteFramebuffers(1, &reference);
	GLenum error = _glGetError();
	if (error != GL_NO_ERROR) Debug(driver, 0, "3D layer reference OpenGL error: {}", error);
}

/** Run the queued layer draws; called as the 2D buffer is uploaded, so both present the same frame. */
static void FlushProof3DLayer()
{
	if (!_proof_layer_queue.empty() && LoadProofGL()) {
		ProofGLState restore;
		if (EnsureProofLayer()) {
			p_glBindFramebuffer(GL_FRAMEBUFFER, _proof_layer_fbo);
			_glViewport(0, 0, _proof_layer_width, _proof_layer_height);
			_glDisable(GL_BLEND); _glDisable(GL_DEPTH_TEST); _glDisable(GL_CULL_FACE); _glDisable(GL_SCISSOR_TEST);
			p_glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			_glEnable(GL_STENCIL_TEST);
			p_glStencilMask(0xFF);
			_glUseProgram(_proof_layer_program);
			_glBindVertexArray(_proof_layer_vao);
			_glActiveTexture(GL_TEXTURE0);
			_glUniform1i(_proof_layer_view, 0);
			_glUniform2f(_proof_layer_screen, float(_proof_layer_width), float(_proof_layer_height));
			const auto &gui = _settings_client.gui;
			p_glUniform4f(_proof_layer_retro, gui.renderer_3d_retro ? float(std::clamp<int>(gui.renderer_3d_pixel_size, 1, 4)) : 1.0f,
				gui.renderer_3d_retro && gui.renderer_3d_retro_colours ? 1.0f : 0.0f,
				gui.renderer_3d_retro_dither ? 1.0f : 0.0f, 0);
			GLint rect = _proof_layer_rect, origin = _proof_layer_origin;
			for (const auto &draw : _proof_layer_queue) {
				int x0 = std::max(draw.x0, 0), y0 = std::max(draw.y0, 0);
				int x1 = std::min(draw.x1, _proof_layer_width), y1 = std::min(draw.y1, _proof_layer_height);
				if (x0 >= x1 || y0 >= y1) continue;
				_glBindTexture(GL_TEXTURE_2D, draw.texture);
				p_glUniform4f(rect, float(x0), float(y0), float(x1), float(y1));
				p_glUniform4f(origin, float(draw.left), float(draw.top), float(draw.height), 0);
				/* A drawn clip claims its pixels for the view; a refresh repaints only pixels the view owns. */
				p_glStencilFunc(draw.claim ? GL_ALWAYS : GL_EQUAL, draw.id, 0xFF);
				p_glStencilOp(GL_KEEP, GL_KEEP, draw.claim ? GL_REPLACE : GL_KEEP);
				_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
			}
			_glDisable(GL_STENCIL_TEST);
		}
		GLenum error = _glGetError();
		if (error != GL_NO_ERROR) Debug(driver, 0, "3D layer OpenGL error: {}", error);
	}
	_proof_layer_queue.clear();
	if (_proof_layer_fbo != 0) VerifyProof3DLayer();
	_proof_free_textures.insert(_proof_free_textures.end(), _proof_released_textures.begin(), _proof_released_textures.end());
	_proof_released_textures.clear();
	/* Keep a few spare textures for re-rendered views; view sizes change rarely. */
	while (_proof_free_textures.size() > 8) {
		_glDeleteTextures(1, &_proof_free_textures.front().texture);
		_proof_free_textures.erase(_proof_free_textures.begin());
	}
}

/** Draw the layer as the backdrop of the 2D UI and set blending for the UI; false when there is none. */
static bool DrawProof3DLayer(GLuint program, GLuint vao)
{
	if (!IsProof3DEnabled() || !_proof_layer_active) return false;
	/* The retained layer persists across clean frames, so it must also stop
	 * presenting when every view has left the 3D zoom range; otherwise it would
	 * show through the transparency of classic 2D pixels. */
	bool any_active = false;
	for (const Window *w : Window::Iterate()) {
		if (w->viewport != nullptr && IsProof3DZoomActive(*w->viewport)) { any_active = true; break; }
	}
	if (!any_active) return false;
	if (_proof_layer_colour == 0 || _proof_layer_width != _screen.width || _proof_layer_height != _screen.height || p_glBlendFuncSeparate == nullptr) return false;
	_glActiveTexture(GL_TEXTURE0);
	_glBindTexture(GL_TEXTURE_2D, _proof_layer_colour);
	_glUseProgram(program);
	_glBindVertexArray(vao);
	_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	/* Holes in the UI (alpha 0) show the layer; the window keeps the layer's opaque alpha. */
	p_glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
	return true;
}

/** Re-render changed views into the pixels they own, without redrawing the 2D UI. */
static void RefreshProof3DLayer()
{
	/* The tile-loop warm-up clears _generating_world before generation is done. */
	if (HasModalProgress()) return;
	RefreshProof3DScene();
	if (!_proof_layer_active || !ProofDirectMode()) return;
	std::array<bool, 256> live{};
	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr) continue;
		const Viewport &vp = *w->viewport;
		auto id = std::ranges::find(_proof_layer_ids, &vp);
		if (id == _proof_layer_ids.end() || id == _proof_layer_ids.begin()) continue;
		live[id - _proof_layer_ids.begin()] = true;
		if (w->IsShaded() || !ProofViewDrawable(vp)) continue;
		/* The camera and rendered vehicles must use the same game state, including when an image is reused. */
		if (GetDebugLevel(DebugLevelID::driver) >= 3 && vp.camera_3d == ViewportCamera3D::Normal) {
			if (const Vehicle *v = Vehicle::GetIfValid(w->viewport->follow_vehicle); v != nullptr) {
				Point target = RemapCoords(v->x_pos, v->y_pos, v->z_pos);
				Debug(driver, 3, "3D FOLLOW vehicle={} type={} position={},{},{} lag={},{}", v->index.base(), int(v->type), v->x_pos, v->y_pos, v->z_pos,
					vp.virtual_left - (target.x - vp.virtual_width / 2), vp.virtual_top - (target.y - vp.virtual_height / 2));
			}
		}
		auto *image = FindProofImage(vp, true);
		if (image == nullptr) return;
		size_t index = id - _proof_layer_ids.begin();
		/* A dirty clip may already have rendered this image earlier in the frame. It still
		 * needs to replace every pixel owned by this view, not just that clip. */
		if (_proof_layer_revisions[index] == image->revision) continue;
		_proof_layer_queue.push_back({image->texture, vp.left, vp.top, vp.height, vp.left, vp.top, vp.left + vp.width, vp.top + vp.height, uint8_t(index), false});
		_proof_layer_revisions[index] = image->revision;
		++_proof_layer_refreshes;
	}
	/* A closed view's pixels were redrawn by whatever lies beneath; its value is free again. */
	for (size_t i = 1; i < live.size(); ++i) if (!live[i]) _proof_layer_ids[i] = nullptr;
}

static void DestroyProofLayer()
{
	ClearProofImages();
	_proof_layer_queue.clear();
	for (const auto *list : {&_proof_free_textures, &_proof_released_textures}) for (const auto &t : *list) _glDeleteTextures(1, &t.texture);
	_proof_free_textures.clear();
	_proof_released_textures.clear();
	if (_proof_layer_fbo) p_glDeleteFramebuffers(1, &_proof_layer_fbo);
	if (_proof_layer_stencil) p_glDeleteRenderbuffers(1, &_proof_layer_stencil);
	if (_proof_layer_colour) _glDeleteTextures(1, &_proof_layer_colour);
	if (_proof_outline_program) _glDeleteProgram(_proof_outline_program);
	if (_proof_outline_vao) _glDeleteVertexArrays(1,&_proof_outline_vao);
	if (_proof_outline_vbo) _glDeleteBuffers(1,&_proof_outline_vbo);
	_proof_outline_program=_proof_outline_vao=_proof_outline_vbo=0;
	if (_proof_fade_program) _glDeleteProgram(_proof_fade_program);
	if (_proof_fade_vao) _glDeleteVertexArrays(1, &_proof_fade_vao);
	_proof_fade_program = _proof_fade_vao = 0;
	if (_proof_layer_program) _glDeleteProgram(_proof_layer_program);
	if (_proof_layer_vao) _glDeleteVertexArrays(1, &_proof_layer_vao);
	_proof_layer_fbo = _proof_layer_stencil = _proof_layer_colour = _proof_layer_program = _proof_layer_vao = 0;
	_proof_layer_width = _proof_layer_height = 0;
	_proof_layer_ids.fill(nullptr);
	_proof_layer_revisions.fill(0);
	_proof_layer_active = false;
}
} // namespace

void SetProof3DGPUVerification(bool enabled)
{
	_proof_verify_gpu = enabled;
	/* The reference comparison reads back on-screen views; redraw them in the matching mode. */
	_proof_layer_active = false;
	ClearProofImages();
	MarkWholeScreenDirty();
}

void SetProof3DLayerVerification(bool enabled)
{
	_proof_verify_layer = enabled;
}

void SetTerrain3DGroundTextures(bool enabled)
{
	_ground_flat_forced = !enabled;
	ClearProofImages();
	MarkWholeScreenDirty();
}

bool AreTerrain3DGroundTexturesActive() { return _ground_textured && !_ground_flat_forced; }

void CheckTerrain3DVisibility(std::string_view tag)
{
	for (const Window *w : Window::Iterate()) {
		if (w->viewport==nullptr || w->IsShaded()) continue;
		const auto &vp=*w->viewport;
		auto key=ProofViewKey(vp,true);
		uint64_t hash=0;
		for (int y : {1,2,3}) for (int x : {1,2,3}) {
			auto p=PickTerrain3D(vp,vp.left+vp.width*x/4,vp.top+vp.height*y/4,false);
			hash=hash*1000003+uint32_t(p.x); hash=hash*1000003+uint32_t(p.y);
		}
		Debug(driver,0,"PHASE95D tag={} view={}:{} enabled={} kind={} target={},{},{} picks={} locks={} manual={} invisible={}",
			tag,int(w->window_class),static_cast<int32_t>(w->window_number),_proof_auto_visibility,key[22],
			std::bit_cast<float>(key[23]),std::bit_cast<float>(key[24]),std::bit_cast<float>(key[25]),hash,_transparency_lock,_transparency_opt,_invisibility_opt);
	}
}

void SetTerrain3DAutoVisibility(bool enabled)
{
	_proof_auto_visibility=enabled;
	ClearProofImages();
	MarkWholeScreenDirty();
}

void SetTerrain3DShadows(int mode)
{
	_proof_shadows = std::clamp(mode, 0, 2);
	ClearProofImages();
	MarkWholeScreenDirty();
}

int GetTerrain3DShadows() { return _proof_shadow_failed ? 0 : _proof_shadows; }

std::string GetTerrain3DShadowStats()
{
	return fmt::format("mode={} failed={} sun_passes={} sun_us={} contact_passes={} contact_us={} sun_map_bytes={} contact_bytes={}", _proof_shadows, _proof_shadow_failed,
		_proof_shadow_passes, _proof_shadow_us, _proof_contact_passes, _proof_contact_us, _proof_shadow_depth ? size_t(PROOF_SHADOW_SIZE) * PROOF_SHADOW_SIZE * 4 : 0,
		size_t(_proof_contact_width) * _proof_contact_height * 5);
}

bool IsProof3DEnabled() { return _proof_enabled.load(); }

void SetProof3DInstancing(bool enabled)
{
	_proof_instanced = enabled;
	InvalidateProof3DImages();
}

void SetProof3DStaticInstances(bool enabled)
{
	_terrain_static_instances = enabled;
	Debug(driver, 1, "PHASE7 static-sharing enabled={}", enabled);
	ResetTerrain3D();
	InvalidateProof3DImages();
}

void SetProof3DInstanceStreams(bool enabled)
{
	_proof_retain_streams = enabled;
	Debug(driver, 1, "PHASE7 retained-streams enabled={}", enabled);
	InvalidateProof3DImages();
}

void SetProof3DModelLOD(bool enabled)
{
	_vehicle3d_lod = enabled;
	Debug(driver, 1, "PHASE7 lod enabled={}", enabled);
	InvalidateProof3DImages();
}

void SetProof3DScopedInvalidation(bool enabled)
{
	_proof_scoped = enabled;
	ClearProofImages();
	MarkWholeScreenDirty();
}

void SetProof3DPartialRender(bool enabled)
{
	_proof_partial = enabled;
	ClearProofImages();
	MarkWholeScreenDirty();
}

void SetProof3DPartialVerification(bool enabled)
{
	_proof_verify_partial = enabled;
}

void SetProof3DPerformanceMode(bool optimized, bool retained, bool tile_meshes, bool dirty_tiles, bool deferred, bool direct)
{
	_proof_optimized = optimized;
	_proof_retained = retained;
	_proof_tile_meshes = tile_meshes;
	_proof_dirty_tiles = dirty_tiles;
	_proof_deferred = deferred;
	_proof_direct = direct;
	_proof_layer_active = false;
	ResetTerrain3D();
	ClearProofImages();
	MarkWholeScreenDirty();
}

void BeginProof3DFrame()
{
	if (!IsProof3DEnabled()) return;
	++_proof_frame_serial;
	_proof_refreshed = false;
	_proof_draws = _proof_renders = _proof_scan_us = _proof_render_us = _proof_read_us = 0;
	_proof_gpu_ns = 0;
	_proof_mesh_us = _proof_chunks_rebuilt = _proof_evictions = 0;
	_proof_cold_chunks = 0;
	_proof_tiles_rebuilt = _proof_tiles_reused = 0;
	_proof_tiles_scan_candidates = 0;
	_proof_upload_bytes = _proof_prepare_us = 0;
	_proof_static_upload_bytes = _proof_upload_us = _proof_projection_us = _proof_tiles_scanned = 0;
	_proof_slot_writes = _proof_slot_relayouts = 0;
	_proof_terrain_runs = 0;
	_ground_map_upload_bytes = 0;
	_proof_vehicle_draws = _proof_vehicle_instances = _proof_instance_upload_bytes = 0;
	_proof_static_draws = _proof_static_instances = 0;
	_proof_static_group_us = _proof_instance_stream_us = 0;
	_proof_stream_hits = _proof_stream_builds = _proof_stream_patches = _proof_stream_evictions = 0;
	_proof_model_triangles = _proof_full_model_triangles = _proof_lod_instances = 0;
	_proof_layer_draws = _proof_layer_refreshes = 0;
	_proof_scoped_boxes = _proof_scoped_drops = _proof_scoped_kept = 0;
	_proof_partial_renders = _proof_partial_pixels = _proof_partial_kept = 0;
	_proof_frame_start = std::chrono::steady_clock::now();
}

void EndProof3DFrame()
{
	if (IsProof3DEnabled() && _proof_layer_active && OpenGLBackend::Get() != nullptr) RefreshProof3DLayer();
	if (!IsProof3DEnabled() || (_proof_draws == 0 && _proof_layer_refreshes == 0)) return;
	size_t retained_bytes = 0, cpu_mesh_bytes = 0, pending_chunks = 0, resident_chunks = 0;
	for (const auto &chunk : _terrain_chunks) {
		cpu_mesh_bytes += chunk.faces.capacity() * sizeof(TerrainFace) + chunk.spans.capacity() * sizeof(Terrain3D::TileMeshSpan) + chunk.instances.capacity() * sizeof(Static3DInstance) + chunk.instance_spans.capacity() * sizeof(Terrain3D::TileMeshSpan);
		pending_chunks += !chunk.mesh_changes.Empty();
		resident_chunks += chunk.resident;
	}
	for (const auto &gpu : _proof_chunks) retained_bytes += gpu.count * sizeof(ProofStaticVertex);
	Debug(driver, 2, "PHASE7 streams static_group_us={} instance_stream_us={} stream_hits={} stream_builds={} stream_patches={} stream_evictions={} stream_retained_bytes={} stream_entries={} streams_enabled={}",
		_proof_static_group_us, _proof_instance_stream_us, _proof_stream_hits, _proof_stream_builds, _proof_stream_patches, _proof_stream_evictions, ProofStaticStreamBytes(), _proof_static_streams.size(), _proof_retain_streams);
	Debug(driver, 2, "PHASE7 frame optimized={} draws={} renders={} scan_us={} render_us={} read_us={} draw_us={} gpu_ns={} gpu_timer={} prepare_us={} upload_bytes={} retained_bytes={} static_upload_bytes={} upload_us={} projection_us={} tiles_scanned={} tiles_rebuilt={} tiles_reused={} tiles_scan_candidates={} mesh_us={} chunks_rebuilt={} pending_chunks={} resident_chunks={} cpu_mesh_bytes={} evictions={} deferred={} cold_chunks={} ground_textured={} ground_map_upload_bytes={} vehicle_draws={} direct={} layer_draws={} layer_refreshes={} slot_writes={} slot_relayouts={} scoped_boxes={} scoped_drops={} scoped_kept={} vehicle_instances={} instance_upload_bytes={} instanced={} model_triangles={} full_model_triangles={} lod_instances={} lod={} model_library_bytes={} static_instances={} static_draws={} static_shared={} terrain_runs={} partial_renders={} partial_pixels={} partial_kept={}",
		_proof_optimized, _proof_draws, _proof_renders, _proof_scan_us, _proof_render_us, _proof_read_us, ProofMicros(_proof_frame_start), _proof_gpu_ns, _proof_queries[0] != 0, _proof_prepare_us, _proof_upload_bytes, retained_bytes, _proof_static_upload_bytes, _proof_upload_us, _proof_projection_us, _proof_tiles_scanned, _proof_tiles_rebuilt, _proof_tiles_reused, _proof_tiles_scan_candidates, _proof_mesh_us, _proof_chunks_rebuilt, pending_chunks, resident_chunks, cpu_mesh_bytes, _proof_evictions, _proof_deferred, _proof_cold_chunks, int(_ground_textured && !_ground_flat_forced), _ground_map_upload_bytes, _proof_vehicle_draws, ProofDirectMode(), _proof_layer_draws, _proof_layer_refreshes, _proof_slot_writes, _proof_slot_relayouts, _proof_scoped_boxes, _proof_scoped_drops, _proof_scoped_kept, _proof_vehicle_instances, _proof_instance_upload_bytes, _proof_instanced && _proof_instance_program != 0 && p_glVertexAttribDivisor != nullptr && p_glDrawArraysInstanced != nullptr, _proof_model_triangles, _proof_full_model_triangles, _proof_lod_instances, _vehicle3d_lod, _proof_model_bytes, _proof_static_instances, _proof_static_draws, _terrain_static_instances, _proof_terrain_runs, _proof_partial_renders, _proof_partial_pixels, _proof_partial_kept);
}

/* Game ticks run on the game thread; the draw tick collects them under the game-state lock. */
static std::atomic<uint64_t> _proof_game_us = 0, _proof_game_ticks = 0;

void NoteProof3DGameTick(std::chrono::steady_clock::time_point start)
{
	if (!_proof_enabled) return;
	_proof_game_us += ProofMicros(start);
	++_proof_game_ticks;
}

void NoteProof3DPresent(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point paint_start)
{
	static std::chrono::steady_clock::time_point last{};
	if (!IsProof3DEnabled()) { last = {}; return; }
	auto now = std::chrono::steady_clock::now();
	uint64_t interval = last == std::chrono::steady_clock::time_point{} ? 0 : std::chrono::duration_cast<std::chrono::microseconds>(now - last).count();
	last = now;
	/* work_us covers input, window drawing (including 3D), painting and presentation; interval_us is the presented frame time. */
	Debug(driver, 2, "PHASE7 present work_us={} paint_us={} interval_us={} draws={} renders={} game_us={} game_ticks={}",
		ProofMicros(start), ProofMicros(paint_start), interval, _proof_draws, _proof_renders, _proof_game_us.exchange(0), _proof_game_ticks.exchange(0));
}

bool IsProof3DSupported()
{
	std::string_view driver = VideoDriver::GetInstance() == nullptr ? "" : VideoDriver::GetInstance()->GetName();
	if (OpenGLBackend::Get() == nullptr || (driver != "win32-opengl" && driver != "sdl-opengl")) return false;
	/* Any 32bpp screen buffer can host the composited layer; the palette blitters are usable too. */
	return true;
}

bool SetProof3DEnabled(bool enabled)
{
	if (enabled && !IsProof3DSupported()) return false;
	if (enabled && !_proof_enabled) ResetTerrain3D();
	if (!enabled && _proof_enabled) {
		/* Zoomed-out 2D views fill undrawn areas with transparency; kill the stale 3D image underneath. */
		Blitter *blitter = BlitterFactory::GetCurrentBlitter();
		if (blitter != nullptr && _screen.dst_ptr != nullptr) {
			GfxFillRect(blitter, &_screen, 0, 0, _screen.width - 1, _screen.height - 1, 0, FillRectMode::Opaque);
		}
	}
	_proof_enabled = enabled;
	_proof_layer_active = false;
	MarkWholeScreenDirty();
	return true;
}

bool IsProof3DZoomActive(const Viewport &vp)
{
	return _proof_enabled && vp.zoom <= ZoomLevel::Out4x;
}

void DestroyProof3D()
{
	_proof_enabled = false;
	ClearProofStaticStreams();
	DestroyProofLayer();
	if (_proof_shadow_fbo) p_glDeleteFramebuffers(1, &_proof_shadow_fbo);
	if (_proof_shadow_depth) _glDeleteTextures(1, &_proof_shadow_depth);
	if (_proof_contact_fbo) p_glDeleteFramebuffers(1, &_proof_contact_fbo);
	if (_proof_contact_depth) _glDeleteTextures(1, &_proof_contact_depth);
	if (_proof_contact_program) _glDeleteProgram(_proof_contact_program);
	if (_proof_contact_vao) _glDeleteVertexArrays(1, &_proof_contact_vao);
	if (_proof_occlusion_fbo) p_glDeleteFramebuffers(1, &_proof_occlusion_fbo);
	if (_proof_occlusion) _glDeleteTextures(1, &_proof_occlusion);
	if (_proof_occlusion_program) _glDeleteProgram(_proof_occlusion_program);
	if (_proof_occlusion_vao) _glDeleteVertexArrays(1, &_proof_occlusion_vao);
	_proof_shadow_fbo = _proof_shadow_depth = _proof_contact_fbo = _proof_contact_depth = _proof_contact_program = _proof_contact_vao = 0;
	_proof_occlusion_fbo = _proof_occlusion = _proof_occlusion_program = _proof_occlusion_vao = 0;
	_proof_contact_width = _proof_contact_height = 0;
	_proof_shadow_failed = false;
	for (auto &chunk : _proof_chunks) if (chunk.buffer) _glDeleteBuffers(1, &chunk.buffer);
	_proof_chunks.clear();
	_terrain_chunks.clear();
	ResetTerrain3D();
	_train3d_snapshots.clear();
	_train3d_faces.clear();
	_vehicle3d_instances.clear();
	_effect3d_instances.clear();
	_infrastructure_observations.clear();
	_infrastructure_audit=false;
	if (_proof_fbo) p_glDeleteFramebuffers(1, &_proof_fbo);
	if (_proof_depth) p_glDeleteRenderbuffers(1, &_proof_depth);
	if (_proof_colour) _glDeleteTextures(1, &_proof_colour);
	if (_proof_program) _glDeleteProgram(_proof_program);
	if (_proof_instance_program) _glDeleteProgram(_proof_instance_program);
	_proof_instance_program = 0;
	_proof_instance_failed = false;
	if (_proof_vao) _glDeleteVertexArrays(1, &_proof_vao);
	if (_proof_vbo) _glDeleteBuffers(1, &_proof_vbo);
	if (_proof_models) _glDeleteBuffers(1, &_proof_models);
	if (_proof_instances) _glDeleteBuffers(1, &_proof_instances);
	_proof_instances = 0;
	_proof_instance_batches.clear();
	_proof_instance_upload.clear();
	_proof_models = 0;
	_proof_model_ranges.clear();
	if (_proof_queries[0]) p_glDeleteQueries(GLsizei(_proof_queries.size()), _proof_queries.data());
	if (_ground_textures) _glDeleteTextures(1, &_ground_textures);
	if (_ground_map_texture) _glDeleteTextures(1, &_ground_map_texture);
	_ground_textures = _ground_map_texture = 0;
	_ground_textured = false;
	_ground_map_generation = 0;
	_proof_queries.fill(0);
	_proof_query_pending.fill(false);
	_proof_fbo = _proof_colour = _proof_depth = _proof_program = _proof_vao = _proof_vbo = 0;
	_proof_width = _proof_height = 0;
}

bool DrawProof3D(const Viewport &vp, const DrawPixelInfo &dpi, int screen_x, int screen_y)
{
	if (!IsProof3DEnabled() || OpenGLBackend::Get() == nullptr) return false;
	/* Progress redraws must not cache partially generated terrain. */
	if (HasModalProgress()) return false;
	auto name = BlitterFactory::GetCurrentBlitter()->GetName();
	if (name != "40bpp-anim" && name != "32bpp-optimized") return false;
	if (!ProofViewDrawable(vp)) return false;
	int width = UnScaleByZoom(dpi.width, dpi.zoom), height = UnScaleByZoom(dpi.height, dpi.zoom);
	int ox = screen_x - vp.left, oy = screen_y - vp.top;
	if (ox < 0 || oy < 0 || ox + width > vp.width || oy + height > vp.height) return false;
	++_proof_draws;
	RefreshProof3DScene();
	auto *dst = static_cast<Colour *>(dpi.dst_ptr);
	const auto *screen = static_cast<const Colour *>(_screen.dst_ptr);
	/* Giant screenshots draw into their own buffer and keep the read-back path. */
	bool on_screen = screen != nullptr && dst >= screen && dst < screen + static_cast<size_t>(_screen.pitch) * _screen.height;
	bool direct = on_screen && ProofDirectMode();
	auto *image = FindProofImage(vp, direct);
	if (image == nullptr) return false;
	uint8_t *anim = nullptr;
	if (name == "40bpp-anim" && !_screen_disable_anim && on_screen) {
		anim = VideoDriver::GetInstance()->GetAnimBuffer();
		if (anim == nullptr) return false;
		anim += dst - screen;
	}
	if (direct) {
		_proof_layer_queue.push_back({image->texture, vp.left, vp.top, vp.height, screen_x, screen_y, screen_x + width, screen_y + height, ProofLayerId(vp), true});
		_proof_layer_active = true;
		++_proof_layer_draws;
	}
	for (int row = 0; row < height; ++row) {
		if (direct) {
			/* A transparent hole: the layer beneath shows through. */
			std::fill_n(dst + row * dpi.pitch, width, Colour(0));
		} else if (_settings_client.gui.renderer_3d_retro) {
			for (int col = 0; col < width; ++col) {
				dst[row * dpi.pitch + col] = SampleProofRetro(image->pixels, vp.width, vp.height, ox + col, oy + row);
			}
		} else {
			const Colour *src = image->pixels.data() + static_cast<size_t>(vp.height - 1 - oy - row) * vp.width + ox;
			std::copy_n(src, width, dst + row * dpi.pitch);
		}
		if (anim != nullptr) std::fill_n(anim + row * dpi.pitch, width, 0);
	}
	return true;
}

void ResolveProof3DScreenshot(void *buf, uint y, uint pitch, uint n)
{
	if (!IsProof3DEnabled() || _screen.dst_ptr == nullptr || BlitterFactory::GetCurrentBlitter()->GetScreenDepth() != 32) return;
	/* The screen buffer is newer than the presented layer until the queue runs. */
	FlushProof3DLayer();
	if (_proof_layer_colour == 0 || _proof_layer_width != _screen.width || _proof_layer_height != _screen.height) return;
	std::vector<Colour> layer(static_cast<size_t>(_screen.width) * n);
	{
		ProofGLState restore;
		p_glBindFramebuffer(GL_READ_FRAMEBUFFER, _proof_layer_fbo);
		_glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		_glPixelStorei(GL_PACK_ALIGNMENT, 4); _glPixelStorei(GL_PACK_ROW_LENGTH, 0);
		_glPixelStorei(GL_PACK_SKIP_ROWS, 0); _glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
		p_glReadPixels(0, y, _screen.width, n, GL_BGRA, GL_UNSIGNED_BYTE, layer.data());
	}
	const auto *src = static_cast<const Colour *>(_screen.dst_ptr) + static_cast<size_t>(y) * _screen.pitch;
	auto *dst = static_cast<Colour *>(buf);
	for (uint row = 0; row < n; ++row) {
		for (int x = 0; x < _screen.width; ++x) {
			if (src[row * _screen.pitch + x].a != 0) continue;
			Colour c = layer[static_cast<size_t>(row) * _screen.width + x];
			c.a = 0xFF;
			dst[row * pitch + x] = c;
		}
	}
}
