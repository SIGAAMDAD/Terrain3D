// Build-aware memory limits. No scene serialization or Godot dependencies.
#ifndef TERRAIN3D_MEMORY_PROFILE_H
#define TERRAIN3D_MEMORY_PROFILE_H

#include "core/config/terrain_3d_native_build.h"
#include <stdint.h>

#define T3D_EDITOR_AUTHORING_TOTAL_BUDGET_MB		  ( 8192 )
#define T3D_EDITOR_AUTHORING_TRANSIENT_RESERVE_MB	  ( 256 )
#define T3D_EDITOR_AUTHORING_COOKING_BUDGET_MB		  ( 2048 )
#define T3D_EDITOR_AUTHORING_GPU_REGION_MAX_BUDGET_MB ( 1024 )
#define T3D_EDITOR_AUTHORING_GPU_UPLOAD_LAYERS		  ( 16 )
#define T3D_EDITOR_AUTHORING_GPU_UPLOAD_BUDGET_MB	  ( 16 )
#define T3D_EDITOR_AUTHORING_GPU_HYSTERESIS_TICKS	  ( 180 )
#define T3D_EDITOR_AUTHORING_GPU_SPARE_PAGES		  ( 24 )

inline int t3dEditorGpuUploadLayers(
	const bool bAuthoring,
	const int  nRuntimeLayers ) {
	return bAuthoring && nRuntimeLayers < T3D_EDITOR_AUTHORING_GPU_UPLOAD_LAYERS
			   ? T3D_EDITOR_AUTHORING_GPU_UPLOAD_LAYERS
			   : nRuntimeLayers;
}
inline int t3dEditorGpuUploadBudgetMb(
	const bool bAuthoring,
	const int  nRuntimeBudgetMb ) {
	return bAuthoring && nRuntimeBudgetMb < T3D_EDITOR_AUTHORING_GPU_UPLOAD_BUDGET_MB
			   ? T3D_EDITOR_AUTHORING_GPU_UPLOAD_BUDGET_MB
			   : nRuntimeBudgetMb;
}
inline int t3dEditorGpuHysteresisTicks(
	const bool bAuthoring,
	const int  nRuntimeTicks ) {
	return bAuthoring && nRuntimeTicks < T3D_EDITOR_AUTHORING_GPU_HYSTERESIS_TICKS
			   ? T3D_EDITOR_AUTHORING_GPU_HYSTERESIS_TICKS
			   : nRuntimeTicks;
}
inline int t3dEditorGpuSparePages(
	const bool bAuthoring,
	const int  nRuntimePages ) {
	return bAuthoring && nRuntimePages < T3D_EDITOR_AUTHORING_GPU_SPARE_PAGES ? T3D_EDITOR_AUTHORING_GPU_SPARE_PAGES
																			  : nRuntimePages;
}

struct t3dMemoryProfileLimits_t
{
	uint32_t m_nTotalBudgetMb = 0u;
	uint32_t m_nTransientReserveMb = 0u;
	bool	 m_bEditorAuthoring = false;
};

// Explicit inputs permit profile regression testing independent of the host.
inline t3dMemoryProfileLimits_t t3dResolveMemoryProfileLimits(
	const bool bEditorBinary,
	const bool bEditorHost,
	const bool bAuthoringMode,
	const int  nRuntimeBudgetMb,
	const int  nRuntimeTransientMb,
	const int  nEditorBudgetMb,
	const int  nEditorTransientMb ) {
	t3dMemoryProfileLimits_t limits;
	limits.m_bEditorAuthoring = bEditorBinary && bEditorHost && bAuthoringMode;
	limits.m_nTotalBudgetMb = uint32_t( limits.m_bEditorAuthoring ? nEditorBudgetMb : nRuntimeBudgetMb );
	limits.m_nTransientReserveMb = uint32_t( limits.m_bEditorAuthoring ? nEditorTransientMb : nRuntimeTransientMb );
	return limits;
}

inline bool t3dNativeEditorMemoryProfileAvailable(
	void ) {
#if defined( T3D_EDITOR_NATIVE )
	return true;
#else
	return false;
#endif
}

#endif // TERRAIN3D_MEMORY_PROFILE_H
