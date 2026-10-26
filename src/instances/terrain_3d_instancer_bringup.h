// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_INSTANCER_BRINGUP_H
#define TERRAIN3D_INSTANCER_BRINGUP_H

// RenderingDevice instancer geometry backend.
//
// RenderingDevice instance/HLOD integration remains active. P11 promotes the
// retained shared static-mesh arenas to the production geometry backend while
// preserving the CPU reference path through surface-local array views.
#define T3D_INSTANCER_RD_PORT 1

// P11 geometry backend. One vertex arena plus uint16/uint32 index arenas own
// imported regular-instance and HLOD geometry. Per-surface CPU-reference views
// reference those arenas without duplicating vertex/index buffers.
#define T3D_INSTANCER_SHARED_MESH_ARENA 1

// P10 production visibility backend. This promotes the P9 compute path to the
// normal Forward+ instance/HLOD visibility path without changing geometry
// ownership. The CPU packet/frustum builder remains available as an explicit
// direct-draw/debug reference and as the parity oracle.
#define T3D_INSTANCER_GPU_CULL_PRODUCTION 1

// UWR-P08: instance/HLOD occlusion consumes Terrain3DWorldHiZ through the
// unified world visibility path. Keep the legacy instance pyramid compiled out
// while its reference culler remains available through the P11 migration.
#define T3D_INSTANCER_HIZ_OCCLUSION 0

// Production publication: compile an immutable RD instance scene directly
// from resident authoritative Terrain3DInstanceStore data and asset policy.
// P5 selects one authored visual LOD per source cell, P6 owns HLOD handoff,
// P7 owns independent occlusion metadata, and P8 incrementally republishes
// unchanged transform records.
#define T3D_INSTANCER_AUTHORITATIVE_RD_SNAPSHOT 1

// The old correctness bring-up switch forced LOD0, disabled streaming and
// rendered magenta diagnostic geometry. It is no longer the active runtime.
#define T3D_INSTANCER_MINIMAL_RENDER_PATH 0

#endif // TERRAIN3D_INSTANCER_BRINGUP_H
