// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCER_BRINGUP_H
#define TERRAIN3D_INSTANCER_BRINGUP_H

// RenderingDevice instancer port.
//
// The full fork keeps its authoritative Terrain3DInstanceStore/editor/streaming
// state, but the presentation backend is the direct-RD instancer port:
// exact per-surface GPU geometry, CPU packet/visibility construction, and
// ordinary indexed instanced draws. This deliberately does not use the old
// MultiMesh/RenderingServer presentation path or the experimental GPU-cull /
// shared-arena / indexed-indirect instance backend.
#define T3D_INSTANCER_RD_PORT 1

// Diagnostic/production isolation: publish the RD instance scene directly
// from resident authoritative Terrain3DInstanceStore data instead of the
// derived m_renderCells cache. This deliberately bypasses render-cell
// materialization, instance-cell streaming/HLOD suppression, cached render
// cell AABBs, and CPU LOD transition state while retaining the production RD
// mesh/material/shader/depth/Forward+ renderer.
#define T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT 1

// The old correctness bring-up switch forced LOD0, disabled streaming and
// rendered magenta diagnostic geometry. It is no longer the active runtime.
#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0

#endif // TERRAIN3D_INSTANCER_BRINGUP_H
