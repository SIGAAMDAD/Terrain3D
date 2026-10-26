// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "rendering/material/terrain_3d_shader_specializer.h"

bool t3dTerrainShaderPipelineKeyIsValid( const t3dTerrainShaderPipelineKey_t &rKey ) {
	if ( rKey.m_nPassClass >= T3D_TERRAIN_SHADER_PASS_COUNT || rKey.m_nTier >= 3u
	        || rKey.m_nShaderPath >= T3D_TERRAIN_SHADER_PATH_COUNT || rKey.m_nMaterialBackend >= T3D_TERRAIN_MATERIAL_BACKEND_COUNT ) {
		return false;
	}
	const bool bColorPass = rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_COLOR_GENERAL
	        || rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_COLOR_ONE_SUN;
	if ( rKey.m_nShaderPath == T3D_TERRAIN_SHADER_PATH_SHIPPING_CANONICAL ) {
		if ( rKey.m_nMaterialBackend != T3D_TERRAIN_MATERIAL_BACKEND_CANONICAL_VT_V3
		        || ( rKey.m_nShippingFeatureClass & ~T3D_TERRAIN_SHIPPING_FEATURE_MASK ) != 0u ) {
			return false;
		}
		const bool bDnrPass = rKey.m_nPassClass == T3D_TERRAIN_SHADER_PASS_DEPTH_NORMAL_ROUGHNESS;
		const bool bDnrValid = bDnrPass ? rKey.m_nDnrOutputClass < T3D_TERRAIN_DNR_OUTPUT_CLASS_COUNT
		                               : rKey.m_nDnrOutputClass == T3D_TERRAIN_DNR_OUTPUT_RUNTIME;
		const bool bMiddleValid = rKey.m_nTier == 1u ? rKey.m_nMiddleSurfaceClass < T3D_TERRAIN_MIDDLE_SURFACE_CLASS_COUNT
		                                          : rKey.m_nMiddleSurfaceClass == T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
		if ( !bDnrValid || !bMiddleValid ) {
			return false;
		}
		if ( bColorPass ) {
			return rKey.m_nLightingModelClass < T3D_TERRAIN_LIGHTING_MODEL_CLASS_COUNT
			        && rKey.m_nSceneIntegrationClass < T3D_TERRAIN_SCENE_INTEGRATION_CLASS_COUNT;
		}
		return rKey.m_nLightingModelClass == T3D_TERRAIN_LIGHTING_MODEL_RUNTIME
		        && rKey.m_nSceneIntegrationClass == T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME;
	}
	return rKey.m_nMaterialBackend == T3D_TERRAIN_MATERIAL_BACKEND_COMPATIBILITY_MIXED
	        && rKey.m_nShippingFeatureClass == T3D_TERRAIN_SHIPPING_FEATURE_RUNTIME
	        && rKey.m_nLightingModelClass == T3D_TERRAIN_LIGHTING_MODEL_RUNTIME
	        && rKey.m_nSceneIntegrationClass == T3D_TERRAIN_SCENE_INTEGRATION_RUNTIME
	        && rKey.m_nDnrOutputClass == T3D_TERRAIN_DNR_OUTPUT_RUNTIME
	        && rKey.m_nMiddleSurfaceClass == T3D_TERRAIN_MIDDLE_SURFACE_RUNTIME;
}
