// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "Terrain3DConstants.h"
#include "rendering/terrain_3d_material.h"
#include "core/terrain_3d_eastl_helpers.h"

#include "authoring/terrain_3d_editor.h"
#include "core/logger.h"
#include "core/terrain_3d_compiler.h"
#include "core/terrain_3d_owner.h"
#include "core/terrain_3d_util.h"
#include "data/terrain_3d_region_codec.h"
#include "rendering/terrain_3d_shader_specializer.h"
#include "streaming/terrain_3d_performance_profile.h"

#include <string.h>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/fast_noise_lite.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/gradient.hpp>
#include <godot_cpp/classes/image_texture.hpp>
#include <godot_cpp/classes/noise_texture2d.hpp>
#include <godot_cpp/classes/reg_ex.hpp>
#include <godot_cpp/classes/reg_ex_match.hpp>
#include <godot_cpp/classes/resource_saver.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/time.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

EA_FORCE_INLINE static void t3dMaterialMixShaderSignature( uint64_t &nSignature, const uint64_t nValue ) {
	nSignature ^= nValue;
	nSignature *= 1099511628211ULL;
}

static String t3dMaterialApplyRendererShaderCompatibility( const String &rShader ) {
	return rShader;
}


EA_FORCE_INLINE static void t3dMaterialStoreShaderPackString( const Ref<FileAccess> &rFile, const String &rValue ) {
	const CharString utf8 = rValue.utf8();
	const uint32_t bytes = ( uint32_t )MAX( 0, utf8.length() );
	rFile->store_32( bytes );
	if ( bytes > 0u ) {
		rFile->store_buffer( ( const uint8_t * )( utf8.get_data() ), bytes );
	}
}

static bool t3dMaterialReadShaderPackString( const Ref<FileAccess> &rFile, String &rValue ) {
	if ( rFile.is_null() || rFile->get_position() + 4u > rFile->get_length() ) {
		return false;
	}
	const uint32_t bytes = rFile->get_32();
	if ( bytes > 16u * 1024u * 1024u || rFile->get_position() + bytes > rFile->get_length() ) {
		return false;
	}
	const PackedByteArray data = rFile->get_buffer( bytes );
	if ( data.size() != bytes ) {
		return false;
	}
	rValue = bytes > 0u ? String::utf8( ( const char * )( data.ptr() ), ( int )bytes ) : String();
	return true;
}


static String t3dMaterialInjectShaderDefines( const String &rShader, const PackedStringArray &rDefineLines ) {
	if ( rDefineLines.is_empty() ) {
		return rShader;
	}
	const String defines = String( "\n" ).join( rDefineLines ) + String( "\n" );
	int declaration_end = -1;
	const int render_mode_pos = rShader.find( "render_mode" );
	if ( render_mode_pos >= 0 ) {
		declaration_end = rShader.find( ";", render_mode_pos );
	}
	if ( declaration_end < 0 ) {
		const int shader_type_pos = rShader.find( "shader_type" );
		if ( shader_type_pos >= 0 ) {
			declaration_end = rShader.find( ";", shader_type_pos );
		}
	}
	if ( declaration_end < 0 ) {
		UtilityFunctions::push_error( "Terrain3DMaterial: Shader source is missing shader_type/render_mode declaration" );
		return rShader;
	}
	return rShader.insert( declaration_end + 1, String( "\n" ) + defines );
}

static PackedColorArray t3dMaterialMakeTextureColorUniforms( const PackedColorArray &rSource ) {
	PackedColorArray colors;
	colors.resize( s_nMaxTextureAssets );
	Color *pWrite = colors.ptrw();
	for ( int index = 0; index < s_nMaxTextureAssets; ++index ) {
		pWrite[index] = s_colorWhite;
	}
	const int count = MIN( rSource.size(), s_nMaxTextureAssets );
	for ( int index = 0; index < count; ++index ) {
		pWrite[index] = rSource[index];
	}
	return colors;
}

EA_FORCE_INLINE static void t3dMaterialAppendDefine( PackedStringArray &rLines, const String &strName, const String &strValue ) {
	rLines.push_back( String( "#define " ) + strName + String( " " ) + strValue );
}

EA_FORCE_INLINE static char32_t t3dMaterialCommentPeek( const String &rCode, const int iIndex ) {
	return iIndex < rCode.length() ? rCode[iIndex] : 0;
}

static bool t3dMaterialCommentAdvance( const String &rCode, int &iIndex, int &nLine, Vector<char32_t> &rStripped, const char32_t target ) {
	while ( iIndex < rCode.length() ) {
		const char32_t character = rCode[iIndex++];
		if ( character == '\n' ) {
			++nLine;
			rStripped.push_back( '\n' );
		}
		if ( character == target ) {
			return true;
		}
	}
	return false;
}

static bool t3dMaterialShaderParameterListHas( const Array &rParameterList, const StringName &strName ) {
	for ( int index = 0; index < rParameterList.size(); ++index ) {
		const Dictionary parameter = rParameterList[index];
		const StringName parameter_name = parameter.get( "name", StringName() );
		if ( parameter_name == strName ) {
			return true;
		}
	}
	return false;
}

static bool t3dMaterialRequireShaderParameter( const Array &rParameterList, const char *pszName, String &rFailureSummary ) {
	const StringName name( pszName );
	if ( t3dMaterialShaderParameterListHas( rParameterList, name ) ) {
		return true;
	}
	rFailureSummary = String( "Missing required reflected parameter: " ) + String( name );
	return false;
}

static String t3dMaterialCharVectorToString( const Vector<char32_t> &rCharacters, const int nStart = 0, const int nEnd = -1 ) {
	const int stop = nEnd == -1 ? rCharacters.size() : nEnd;
	const int count = stop - nStart;
	String result;
	result.resize( count + 1 );
	for ( int index = 0; index < count; ++index ) {
		result[index] = rCharacters[nStart + index];
	}
	result[count] = 0;
	return result;
}

typedef struct t3dShaderTemplate_s {
	const char *m_pszName;
	const char *m_pszPath;
} t3dShaderTemplate_t;

static const t3dShaderTemplate_t s_szShaderTemplates[] = {
	{ "main", "res://addons/terrain_3d/shaders/main.gdshaderinc" },
	{ "main_middle", "res://addons/terrain_3d/shaders/main_middle.gdshaderinc" },
	{ "main_far", "res://addons/terrain_3d/shaders/main_far.gdshaderinc" },
	{ "main_shadow", "res://addons/terrain_3d/shaders/main_shadow.gdshaderinc" },
	{ "displacement_buffer", "res://addons/terrain_3d/shaders/displacement_buffer.gdshaderinc" },
};

void Terrain3DMaterial::preloadShaders( void ) {
	m_shaderConfig.m_shaderTemplates.clear();

	for ( size_t iTemplate = 0u; iTemplate < T3D_ARRAY_COUNT( s_szShaderTemplates ); iTemplate++ ) {
		const t3dShaderTemplate_t &shaderTemplate = ( s_szShaderTemplates )[iTemplate];
		String source;
		String failure_summary;
		if ( !t3dExpandShaderIncludes( shaderTemplate.m_pszPath, source, failure_summary ) ) {
			UtilityFunctions::push_error(
			        "Terrain3DMaterial: Failed to assemble shader include graph for ", shaderTemplate.m_pszPath, ": ", failure_summary );
			continue;
		}
		m_shaderConfig.m_shaderTemplates[shaderTemplate.m_pszName] = source;
		LOG( DEBUG, "Expanded shader template: ", shaderTemplate.m_pszPath );
	}
}

PackedStringArray Terrain3DMaterial::getEditorShaderFeatures( void ) const {
	PackedStringArray features;
	if ( m_pTerrain && m_pTerrain->getEditor() ) {
		features.push_back( "EDITOR_DECAL_SETUP" );
		features.push_back( "EDITOR_DECAL_RENDER" );
	}
	if ( m_authoringState.m_bDebugViewHeightmap ) {
		features.push_back( "DEBUG_HEIGHTMAP_SETUP" );
		features.push_back( "DEBUG_HEIGHTMAP" );
	}
	if ( m_authoringState.m_bShowContours ) {
		features.push_back( "OVERLAY_CONTOURS_SETUP" );
		features.push_back( "OVERLAY_CONTOURS_RENDER" );
	}
	if ( m_authoringState.m_bShowInstancerGrid ) {
		features.push_back( "OVERLAY_INSTANCER_GRID_SETUP" );
		features.push_back( "OVERLAY_INSTANCER_GRID" );
	}
	if ( m_authoringState.m_bDebugViewCheckered ) {
		features.push_back( "DEBUG_CHECKERED" );
	}
	if ( m_authoringState.m_bDebugViewGrey ) {
		features.push_back( "DEBUG_GREY" );
	}
	if ( m_authoringState.m_bDebugViewJaggedness ) {
		features.push_back( "DEBUG_JAGGEDNESS" );
	}
	if ( m_authoringState.m_bDebugViewAutoshader ) {
		features.push_back( "DEBUG_AUTOSHADER" );
	}
	if ( m_authoringState.m_bDebugViewControlTexture ) {
		features.push_back( "DEBUG_CONTROL_TEXTURE" );
	}
	if ( m_authoringState.m_bDebugViewControlBlend ) {
		features.push_back( "DEBUG_CONTROL_BLEND" );
	}
	if ( m_authoringState.m_bDebugViewControlAngle ) {
		features.push_back( "DEBUG_CONTROL_ANGLE" );
	}
	if ( m_authoringState.m_bDebugViewControlScale ) {
		features.push_back( "DEBUG_CONTROL_SCALE" );
	}
	if ( m_authoringState.m_bDebugViewColormap ) {
		features.push_back( "DEBUG_COLORMAP" );
	}
	if ( m_authoringState.m_bDebugViewRoughmap ) {
		features.push_back( "DEBUG_ROUGHMAP" );
	}
	if ( m_authoringState.m_bPbrViewTexAlbedo ) {
		features.push_back( "PBR_TEXTURE_ALBEDO" );
	}
	if ( m_authoringState.m_bPbrViewTexHeight ) {
		features.push_back( "PBR_TEXTURE_HEIGHT" );
	}
	if ( m_authoringState.m_bPbrViewTexNormal ) {
		features.push_back( "PBR_TEXTURE_NORMAL" );
	}
	if ( m_authoringState.m_bPbrViewTexAo ) {
		features.push_back( "PBR_TEXTURE_AO" );
	}
	if ( m_authoringState.m_bPbrViewTexRough ) {
		features.push_back( "PBR_TEXTURE_ROUGHNESS" );
	}
	if ( m_authoringState.m_bDebugViewDisplacementBuffer ) {
		features.push_back( "DEBUG_DISPLACEMENT_BUFFER" );
	}
	if ( m_authoringState.m_bShowVertexGrid ) {
		features.push_back( "OVERLAY_VERTEX_GRID" );
	}
	if ( m_authoringState.m_bShowNavigation
	        || ( m_pTerrain && m_pTerrain->getEditor() && m_pTerrain->getEditor()->getTool() == NAVIGATION ) ) {
		features.push_back( "EDITOR_NAVIGATION" );
	}
	if ( m_authoringState.m_bShowRegionGrid || ( m_pTerrain && m_pTerrain->getEditor() && m_pTerrain->getEditor()->getTool() == REGION ) ) {
		features.push_back( "EDITOR_REGION_GRID" );
	}
	return features;
}

String Terrain3DMaterial::prepareShaderSource( const String &rShader,
        const t3dShaderVariantState_t &eState,
        const Array &rExcludes,
        const t3dShaderTier_t tier,
        const bool bIncludeEditorFeatures ) const {
	const bool sample_surface_data = samplesSurfaceData( eState );

	t3dShaderStaticState_t static_state;
	static_state.m_nTerrainSamplingMode = ( int )( eState.m_terrainSamplingMode );
	static_state.m_nTerrainNormalMode = ( int )( eState.m_terrainNormalMode );
	static_state.m_nColorSamplingMode = ( int )( eState.m_colorSamplingMode );
	static_state.m_nProjectionMode = ( int )( eState.m_projectionMode );
	static_state.m_nTextureSurfaceMode = ( int )( eState.m_textureSurfaceMode );
	static_state.m_nTerrainShadingMode = ( int )( eState.m_terrainShadingMode );
	static_state.m_nDetilingMode = ( int )( eState.m_detilingMode );
	static_state.m_nWorldNoiseShaderMode = ( int )( eState.m_worldNoiseShaderMode );
	static_state.m_bSampleSurfaceData = sample_surface_data;
	static_state.m_bOutputRoughness = eState.m_bOutputRoughnessEnabled;
	static_state.m_bOutputSpecular = eState.m_bOutputSpecularEnabled;
	static_state.m_bOutputNormalMap = eState.m_bOutputNormalMapEnabled;
	static_state.m_bOutputAo = eState.m_bOutputAmbientOcclusionEnabled;
	static_state.m_bFastBlendCurve = eState.m_bFastBlendCurve;
	static_state.m_bControlMapsImplicit = eState.m_bControlMapsImplicit;
	static_state.m_bColorMapsImplicit = eState.m_bColorMapsImplicit;
	static_state.m_bAutoShaderEnabled = eState.m_bAutoShaderEnabled;

	PackedStringArray enabled_features;
	for ( size_t iFeature = 0u; iFeature < T3D_ARRAY_COUNT( s_pszShaderFeatures ); iFeature++ ) {
		const char *pszFeature = ( s_pszShaderFeatures )[iFeature];
		const StringName feature( pszFeature );
		if ( !rExcludes.has( feature ) ) {
			enabled_features.push_back( String( feature ) );
		}
	}
	if ( bIncludeEditorFeatures ) {
		const PackedStringArray editor_features = getEditorShaderFeatures();
		for ( int index = 0; index < editor_features.size(); ++index ) {
			enabled_features.push_back( editor_features[index] );
		}
	}

	String assembled_shader;
	String failure_summary;
	if ( !t3dAssembleShaderSource( rShader, static_state, enabled_features, assembled_shader, failure_summary ) ) {
		UtilityFunctions::push_error(
		        "Terrain3DMaterial: Built-in shader assembly failed for tier ", ( int )( tier ), ": ", failure_summary );
		return String();
	}
	return t3dMaterialApplyRendererShaderCompatibility( assembled_shader );
}

bool Terrain3DMaterial::samplesSurfaceData( const t3dShaderVariantState_t &eState ) const {
	if ( eState.m_textureSurfaceMode == TEXTURE_SURFACE_SILHOUETTE ) {
		return false;
	}
	if ( eState.m_surfaceDataMode == SURFACE_DATA_FULL ) {
		return true;
	}
	if ( eState.m_surfaceDataMode != SURFACE_DATA_AUTO ) {
		return false;
	}
	return eState.m_bOutputNormalMapEnabled || eState.m_bOutputRoughnessEnabled || eState.m_bOutputSpecularEnabled
	        || eState.m_bOutputAmbientOcclusionEnabled;
}

t3dShaderVariantState_t Terrain3DMaterial::captureVariantState( void ) const {
	t3dShaderVariantState_t state;
	state.m_worldBackground = m_authoringState.m_worldBackground;
	state.m_projectionMode = m_authoringState.m_projectionMode;
	state.m_tessellationMaterialMode = m_authoringState.m_tessellationMaterialMode;
	state.m_worldNoiseShaderMode = m_authoringState.m_worldNoiseShaderMode;
	state.m_bAutoShaderEnabled = m_authoringState.m_bAutoShaderEnabled;
	state.m_bDualScalingEnabled = m_authoringState.m_bDualScalingEnabled;
	state.m_bMacroVariationEnabled = m_authoringState.m_bMacroVariationEnabled;
	state.m_bOutputAlbedoEnabled = m_authoringState.m_bOutputAlbedoEnabled;
	state.m_bOutputRoughnessEnabled = m_authoringState.m_bOutputRoughnessEnabled;
	state.m_bOutputSpecularEnabled = m_authoringState.m_bOutputSpecularEnabled;
	state.m_bOutputNormalMapEnabled = m_authoringState.m_bOutputNormalMapEnabled;
	state.m_bOutputAmbientOcclusionEnabled = m_authoringState.m_bOutputAmbientOcclusionEnabled;
	state.m_bFastBlendCurve = m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_RUNTIME_360
	        || m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_XBOX_360_AUTHENTIC
	        || m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_LOW
	        || m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_MINIMUM;
	state.m_bTessellationEnabled = m_pTerrain && m_pTerrain->getTessellationLevel() > 0;
	const Ref<Terrain3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	state.m_bSurfacePacked = pAssets.is_valid() && pAssets->usesPackedPbr();
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
	state.m_bControlMapsImplicit = pData && pData->areControlMapsImplicit();
	state.m_bColorMapsImplicit = pData && !pData->isGpuRegionColorChannelEnabled();
	applyTierQualityToState( state, m_authoringState.m_nearQuality, SHADER_TIER_NEAR );
	return state;
}

void Terrain3DMaterial::applyXboxProfileManifestToState( t3dShaderVariantState_t &rState ) const {
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		if ( setting.m_owner != T3D_PROFILE_OWNER_MATERIAL ) {
			continue;
		}
		const char *property = setting.m_pszProperty;
		if ( strcmp( property, "projection_mode" ) == 0 ) {
			rState.m_projectionMode = ( t3dProjectionMode_t )( setting.m_nIntValue );
		} else if ( strcmp( property, "tessellation_material_mode" ) == 0 ) {
			rState.m_tessellationMaterialMode = ( t3dTessellationMaterialMode_t )( setting.m_nIntValue );
		} else if ( strcmp( property, "world_noise_shader_mode" ) == 0 ) {
			rState.m_worldNoiseShaderMode = ( t3dWorldNoiseShaderMode_t )( setting.m_nIntValue );
		} else if ( strcmp( property, "auto_shader_enabled" ) == 0 ) {
			rState.m_bAutoShaderEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "dual_scaling_enabled" ) == 0 ) {
			rState.m_bDualScalingEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "macro_variation_enabled" ) == 0 ) {
			rState.m_bMacroVariationEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_albedo" ) == 0 ) {
			rState.m_bOutputAlbedoEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_roughness" ) == 0 ) {
			rState.m_bOutputRoughnessEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_specular" ) == 0 ) {
			rState.m_bOutputSpecularEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_normal_map" ) == 0 ) {
			rState.m_bOutputNormalMapEnabled = setting.m_bBoolValue;
		} else if ( strcmp( property, "output_ambient_occlusion" ) == 0 ) {
			rState.m_bOutputAmbientOcclusionEnabled = setting.m_bBoolValue;
		}
	}
}

void Terrain3DMaterial::applyXboxProfileManifestToTiers( void ) {
	for ( size_t iSetting = 0u; iSetting < T3D_ARRAY_COUNT( g_szXbox360ExperimentalSettings ); iSetting++ ) {
		const t3dPerformanceProfileSetting_t &setting = ( g_szXbox360ExperimentalSettings )[iSetting];
		if ( setting.m_owner != T3D_PROFILE_OWNER_MATERIAL ) {
			continue;
		}
		t3dTierQualityConfig_t *pQuality = NULL;
		const char *suffix = NULL;
		if ( strncmp( setting.m_pszProperty, "near_", 5 ) == 0 ) {
			pQuality = &m_authoringState.m_nearQuality;
			suffix = setting.m_pszProperty + 5;
		} else if ( strncmp( setting.m_pszProperty, "middle_", 7 ) == 0 ) {
			pQuality = &m_authoringState.m_middleQuality;
			suffix = setting.m_pszProperty + 7;
		} else if ( strncmp( setting.m_pszProperty, "far_", 4 ) == 0 ) {
			pQuality = &m_authoringState.m_farQuality;
			suffix = setting.m_pszProperty + 4;
		} else {
			continue;
		}

		if ( strcmp( suffix, "texture_filtering" ) == 0 ) {
			pQuality->m_textureFiltering = ( t3dTextureFiltering_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "material_sampling_quality" ) == 0 ) {
			pQuality->m_materialSamplingQuality = ( t3dMaterialSamplingQuality_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "terrain_normal_quality" ) == 0 ) {
			pQuality->m_terrainNormalQuality = ( t3dTerrainNormalQuality_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "terrain_color_quality" ) == 0 ) {
			pQuality->m_terrainColorQuality = ( t3dTerrainColorQuality_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "surface_response_quality" ) == 0 ) {
			pQuality->m_surfaceResponseQuality = ( t3dSurfaceResponseQuality_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "texture_surface_mode" ) == 0 ) {
			pQuality->m_textureSurfaceMode = ( t3dTextureSurfaceMode_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "lighting_model" ) == 0 ) {
			pQuality->m_lightingModel = ( t3dTierLightingModel_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "detiling_quality" ) == 0 ) {
			pQuality->m_detilingQuality = ( t3dTierDetilingQuality_t )( setting.m_nIntValue );
		} else if ( strcmp( suffix, "authored_color_contribution" ) == 0 ) {
			pQuality->m_fAuthoredColorContribution = ( real_t )( setting.m_fFloatValue );
		} else if ( strcmp( suffix, "wetness_contribution" ) == 0 ) {
			pQuality->m_fWetnessContribution = ( real_t )( setting.m_fFloatValue );
		} else if ( strcmp( suffix, "material_lod_bias" ) == 0 ) {
			pQuality->m_fMaterialLodBias = ( real_t )( setting.m_fFloatValue );
		}
	}
}

void Terrain3DMaterial::applyPerformancePresetToState( t3dShaderVariantState_t &rState, const t3dShaderPerformancePreset_t ePreset ) const {
	switch ( ePreset ) {
		case SHADER_PRESET_CINEMATIC:
			rState.m_textureFiltering = LINEAR_ANISOTROPIC;
			rState.m_terrainSamplingMode = TERRAIN_SAMPLING_ALWAYS_SMOOTH;
			rState.m_terrainNormalMode = TERRAIN_NORMAL_HEIGHT_8_TAP;
			rState.m_colorSamplingMode = COLOR_SAMPLING_EXACT_4_TAP;
			rState.m_projectionMode = PROJECTION_EXACT_PER_CORNER;
			rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT;
			rState.m_surfaceDataMode = SURFACE_DATA_FULL;
			rState.m_terrainShadingMode = SHADING_BURLEY_GGX;
			rState.m_bOutputAlbedoEnabled = true;
			rState.m_bOutputNormalMapEnabled = true;
			rState.m_bOutputRoughnessEnabled = true;
			rState.m_bOutputSpecularEnabled = true;
			rState.m_bOutputAmbientOcclusionEnabled = true;
			rState.m_detilingMode = DETILING_CONTINUOUS;
			rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_FRAGMENT_NORMALS;
			rState.m_bFastBlendCurve = false;
			break;
		case SHADER_PRESET_HIGH:
			rState.m_textureFiltering = LINEAR_ANISOTROPIC;
			rState.m_terrainSamplingMode = TERRAIN_SAMPLING_ADAPTIVE;
			rState.m_terrainNormalMode = TERRAIN_NORMAL_DERIVED_TEXTURE;
			rState.m_colorSamplingMode = COLOR_SAMPLING_AUTO;
			rState.m_projectionMode = PROJECTION_SHARED_FRAGMENT;
			rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
			rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
			rState.m_terrainShadingMode = SHADING_BURLEY_GGX;
			rState.m_bOutputAlbedoEnabled = true;
			rState.m_bOutputNormalMapEnabled = true;
			rState.m_bOutputRoughnessEnabled = true;
			rState.m_bOutputSpecularEnabled = true;
			rState.m_bOutputAmbientOcclusionEnabled = true;
			rState.m_detilingMode = DETILING_DISCRETE_8;
			rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
			rState.m_bFastBlendCurve = false;
			break;
		case SHADER_PRESET_RUNTIME_360:
			// Stylized/performative 360 mode: modern cached normals and discrete
			// detiling, but no expensive PBR specular or multi-scale layers.
			rState.m_textureFiltering = LINEAR;
			rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
			rState.m_terrainNormalMode = TERRAIN_NORMAL_DERIVED_TEXTURE;
			rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
			rState.m_projectionMode = PROJECTION_DISABLED;
			rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
			rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
			rState.m_terrainShadingMode = SHADING_LAMBERT_DIFFUSE_ONLY;
			rState.m_bOutputAlbedoEnabled = true;
			rState.m_bOutputNormalMapEnabled = true;
			rState.m_bOutputRoughnessEnabled = false;
			rState.m_bOutputSpecularEnabled = false;
			rState.m_bOutputAmbientOcclusionEnabled = true;
			rState.m_detilingMode = DETILING_DISCRETE_8;
			rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
			rState.m_bAutoShaderEnabled = false;
			rState.m_bDualScalingEnabled = false;
			rState.m_bMacroVariationEnabled = false;
			rState.m_bFastBlendCurve = true;
			break;
		case SHADER_PRESET_LOW:
			rState.m_textureFiltering = LINEAR;
			rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
			rState.m_terrainNormalMode = TERRAIN_NORMAL_HEIGHT_3_TAP;
			rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
			rState.m_projectionMode = PROJECTION_DISABLED;
			rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
			rState.m_surfaceDataMode = SURFACE_DATA_AUTO;
			rState.m_terrainShadingMode = SHADING_LAMBERT_DIFFUSE_ONLY;
			rState.m_bOutputAlbedoEnabled = true;
			rState.m_bOutputNormalMapEnabled = true;
			rState.m_bOutputRoughnessEnabled = false;
			rState.m_bOutputSpecularEnabled = false;
			rState.m_bOutputAmbientOcclusionEnabled = true;
			rState.m_detilingMode = DETILING_DISABLED;
			rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
			rState.m_bFastBlendCurve = true;
			break;
		case SHADER_PRESET_MINIMUM:
			rState.m_textureFiltering = LINEAR;
			rState.m_terrainSamplingMode = TERRAIN_SAMPLING_SINGLE_SAMPLE;
			rState.m_terrainNormalMode = TERRAIN_NORMAL_GEOMETRY_DERIVATIVE;
			rState.m_colorSamplingMode = COLOR_SAMPLING_HARDWARE_FILTERED;
			rState.m_projectionMode = PROJECTION_DISABLED;
			rState.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
			rState.m_surfaceDataMode = SURFACE_DATA_ALBEDO_ONLY;
			rState.m_terrainShadingMode = SHADING_UNSHADED;
			rState.m_bOutputAlbedoEnabled = true;
			rState.m_bOutputNormalMapEnabled = false;
			rState.m_bOutputRoughnessEnabled = false;
			rState.m_bOutputSpecularEnabled = false;
			rState.m_bOutputAmbientOcclusionEnabled = false;
			rState.m_detilingMode = DETILING_DISABLED;
			rState.m_worldNoiseShaderMode = WORLD_NOISE_CACHED_VERTEX_NORMALS;
			rState.m_bFastBlendCurve = true;
			break;
		case SHADER_PRESET_XBOX_360_AUTHENTIC:
			applyXboxProfileManifestToState( rState );
			rState.m_bFastBlendCurve = true;
			break;
		case SHADER_PRESET_CUSTOM:
		default:
			break;
	}
}

void Terrain3DMaterial::applyPerformancePresetToTiers( const t3dShaderPerformancePreset_t ePreset ) {
	// Presets seed the same semantic rows for all tiers. The authentic Xbox
	// profile reads its owned values from the authoritative native manifest;
	// the other material-only presets retain their local definitions.
	m_authoringState.m_middleQuality.m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	m_authoringState.m_middleQuality.m_fAuthoredColorContribution = 1.0f;
	m_authoringState.m_farQuality.m_textureSurfaceMode = TEXTURE_SURFACE_TEXTURED;
	m_authoringState.m_farQuality.m_fAuthoredColorContribution = 1.0f;

	switch ( ePreset ) {
		case SHADER_PRESET_CINEMATIC:
			m_authoringState.m_middleQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
			m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
			m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
			m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
			m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_PBR;
			m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_ROTATION;
			m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_middleQuality.m_fMaterialLodBias = -0.5f;
			m_authoringState.m_farQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
			m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
			m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
			m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
			m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_PBR;
			m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_ROTATION;
			m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_farQuality.m_fMaterialLodBias = -1.5f;
			break;
		case SHADER_PRESET_HIGH:
			m_authoringState.m_middleQuality.m_textureFiltering = LINEAR_ANISOTROPIC;
			m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
			m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
			m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
			m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_ROTATION;
			m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_middleQuality.m_fMaterialLodBias = 0.0f;
			m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
			m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
			m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_EXACT;
			m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_DETAILED;
			m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_ROTATION;
			m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_farQuality.m_fMaterialLodBias = -0.5f;
			break;
		case SHADER_PRESET_RUNTIME_360:
			m_authoringState.m_middleQuality.m_textureFiltering = LINEAR;
			m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
			m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_middleQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_middleQuality.m_fMaterialLodBias = 0.75f;
			m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
			m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FULL;
			m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_DETAILED;
			m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_farQuality.m_fWetnessContribution = 1.0f;
			m_authoringState.m_farQuality.m_fMaterialLodBias = 0.0f;
			break;
		case SHADER_PRESET_XBOX_360_AUTHENTIC:
			applyXboxProfileManifestToTiers();
			break;
		case SHADER_PRESET_LOW:
			m_authoringState.m_middleQuality.m_textureFiltering = LINEAR;
			m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
			m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
			m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_middleQuality.m_fWetnessContribution = 0.5f;
			m_authoringState.m_middleQuality.m_fMaterialLodBias = 1.5f;
			m_authoringState.m_farQuality.m_textureFiltering = LINEAR;
			m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
			m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
			m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_LAMBERT;
			m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_farQuality.m_fWetnessContribution = 0.0f;
			m_authoringState.m_farQuality.m_fMaterialLodBias = 1.0f;
			break;
		case SHADER_PRESET_MINIMUM:
			m_authoringState.m_middleQuality.m_textureFiltering = NEAREST;
			m_authoringState.m_middleQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
			m_authoringState.m_middleQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
			m_authoringState.m_middleQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_middleQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_middleQuality.m_lightingModel = TIER_LIGHTING_UNSHADED;
			m_authoringState.m_middleQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_middleQuality.m_fWetnessContribution = 0.0f;
			m_authoringState.m_middleQuality.m_fMaterialLodBias = 2.5f;
			m_authoringState.m_farQuality.m_textureFiltering = NEAREST;
			m_authoringState.m_farQuality.m_materialSamplingQuality = MATERIAL_SAMPLING_FAST;
			m_authoringState.m_farQuality.m_terrainNormalQuality = TERRAIN_NORMAL_QUALITY_GEOMETRY;
			m_authoringState.m_farQuality.m_terrainColorQuality = TERRAIN_COLOR_QUALITY_FAST;
			m_authoringState.m_farQuality.m_surfaceResponseQuality = SURFACE_RESPONSE_SIMPLE;
			m_authoringState.m_farQuality.m_lightingModel = TIER_LIGHTING_UNSHADED;
			m_authoringState.m_farQuality.m_detilingQuality = TIER_DETILING_DISABLED;
			m_authoringState.m_farQuality.m_fWetnessContribution = 0.0f;
			m_authoringState.m_farQuality.m_fMaterialLodBias = 2.0f;
			break;
		case SHADER_PRESET_CUSTOM:
		default:
			break;
	}
}

void Terrain3DMaterial::applyVariantState( const t3dShaderVariantState_t &eState ) {
	m_authoringState.m_worldBackground = eState.m_worldBackground;
	m_authoringState.m_projectionMode = eState.m_projectionMode;
	m_authoringState.m_tessellationMaterialMode = eState.m_tessellationMaterialMode;
	m_authoringState.m_worldNoiseShaderMode = eState.m_worldNoiseShaderMode;
	m_authoringState.m_bAutoShaderEnabled = eState.m_bAutoShaderEnabled;
	m_authoringState.m_bDualScalingEnabled = eState.m_bDualScalingEnabled;
	m_authoringState.m_bMacroVariationEnabled = eState.m_bMacroVariationEnabled;
	m_authoringState.m_bOutputAlbedoEnabled = eState.m_bOutputAlbedoEnabled;
	m_authoringState.m_bOutputRoughnessEnabled = eState.m_bOutputRoughnessEnabled;
	m_authoringState.m_bOutputSpecularEnabled = eState.m_bOutputSpecularEnabled;
	m_authoringState.m_bOutputNormalMapEnabled = eState.m_bOutputNormalMapEnabled;
	m_authoringState.m_bOutputAmbientOcclusionEnabled = eState.m_bOutputAmbientOcclusionEnabled;
	m_authoringState.m_nearQuality.m_textureFiltering = eState.m_textureFiltering;
	m_authoringState.m_nearQuality.m_materialSamplingQuality =
	        eState.m_terrainSamplingMode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST : MATERIAL_SAMPLING_FULL;
	m_authoringState.m_nearQuality.m_terrainNormalQuality = eState.m_terrainNormalMode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
	        ? TERRAIN_NORMAL_QUALITY_GEOMETRY
	        : TERRAIN_NORMAL_QUALITY_DETAILED;
	m_authoringState.m_nearQuality.m_terrainColorQuality =
	        eState.m_colorSamplingMode == COLOR_SAMPLING_EXACT_4_TAP ? TERRAIN_COLOR_QUALITY_EXACT : TERRAIN_COLOR_QUALITY_FAST;
	m_authoringState.m_nearQuality.m_surfaceResponseQuality =
	        eState.m_surfaceDataMode == SURFACE_DATA_ALBEDO_ONLY ? SURFACE_RESPONSE_SIMPLE : SURFACE_RESPONSE_DETAILED;
	m_authoringState.m_nearQuality.m_textureSurfaceMode = eState.m_textureSurfaceMode;
	m_authoringState.m_nearQuality.m_lightingModel = eState.m_terrainShadingMode == SHADING_UNSHADED
	        ? TIER_LIGHTING_UNSHADED
	        : ( eState.m_terrainShadingMode == SHADING_XBOX_360_BLINN_PHONG
	                          ? TIER_LIGHTING_BLINN_PHONG
	                          : ( eState.m_terrainShadingMode == SHADING_BURLEY_GGX || eState.m_terrainShadingMode == SHADING_LAMBERT_GGX
	                                            ? TIER_LIGHTING_PBR
	                                            : TIER_LIGHTING_LAMBERT ) );
	m_authoringState.m_nearQuality.m_detilingQuality =
	        eState.m_detilingMode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION;
}

t3dShaderVariantKey_t Terrain3DMaterial::makeVariantCacheKey( const String &rCode ) const {
	t3dShaderVariantKey_t key;
	key.m_nPrimaryHash = ( uint64_t )( rCode.hash() );
	key.m_nSourceLength = ( uint32_t )( rCode.length() );

	// A second full-source fingerprint avoids retaining duplicate shader source
	// strings solely to guard the compiled variant cache against hash collisions.
	uint64_t fingerprint = 1469598103934665603ULL;
	for ( int32_t i = 0; i < rCode.length(); ++i ) {
		fingerprint ^= ( uint64_t )( rCode.unicode_at( i ) );
		fingerprint *= 1099511628211ULL;
	}
	key.m_nFingerprint = fingerprint;
	return key;
}

t3dShaderVariantEntry_t *Terrain3DMaterial::getShaderVariantEntries( const t3dShaderVariantDomain_t domain ) {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return m_shaderConfig.m_editorShaderVariantEntries.data();
		case SHADER_DOMAIN_BUFFER:
			return m_shaderConfig.m_bufferShaderVariantEntries.data();
		case SHADER_DOMAIN_RUNTIME:
		default:
			return m_shaderConfig.m_runtimeShaderVariantEntries.data();
	}
}

const t3dShaderVariantEntry_t *Terrain3DMaterial::getShaderVariantEntries( const t3dShaderVariantDomain_t domain ) const {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return m_shaderConfig.m_editorShaderVariantEntries.data();
		case SHADER_DOMAIN_BUFFER:
			return m_shaderConfig.m_bufferShaderVariantEntries.data();
		case SHADER_DOMAIN_RUNTIME:
		default:
			return m_shaderConfig.m_runtimeShaderVariantEntries.data();
	}
}

int Terrain3DMaterial::getShaderVariantCountForDomain( const t3dShaderVariantDomain_t domain ) const {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return ( int )( m_shaderConfig.m_editorShaderVariantEntries.size() );
		case SHADER_DOMAIN_BUFFER:
			return ( int )( m_shaderConfig.m_bufferShaderVariantEntries.size() );
		case SHADER_DOMAIN_RUNTIME:
		default:
			return ( int )( m_shaderConfig.m_runtimeShaderVariantEntries.size() );
	}
}

uint64_t &Terrain3DMaterial::getShaderVariantDomainSourceBytes( const t3dShaderVariantDomain_t domain ) {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return m_shaderConfig.m_nEditorShaderVariantSourceBytes;
		case SHADER_DOMAIN_BUFFER:
			return m_shaderConfig.m_nBufferShaderVariantSourceBytes;
		case SHADER_DOMAIN_RUNTIME:
		default:
			return m_shaderConfig.m_nRuntimeShaderVariantSourceBytes;
	}
}

uint64_t Terrain3DMaterial::getShaderVariantDomainSourceBytes( const t3dShaderVariantDomain_t domain ) const {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return m_shaderConfig.m_nEditorShaderVariantSourceBytes;
		case SHADER_DOMAIN_BUFFER:
			return m_shaderConfig.m_nBufferShaderVariantSourceBytes;
		case SHADER_DOMAIN_RUNTIME:
		default:
			return m_shaderConfig.m_nRuntimeShaderVariantSourceBytes;
	}
}

int Terrain3DMaterial::getVariantPhysicalCapacity( const t3dShaderVariantDomain_t domain ) const {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return s_nMaxEditorShaderVariantCacheLimit;
		case SHADER_DOMAIN_BUFFER:
			return s_nMaxBufferShaderVariantCacheLimit;
		case SHADER_DOMAIN_RUNTIME:
		default:
			return s_nMaxShaderVariantCacheLimit;
	}
}

int Terrain3DMaterial::getVariantCacheLimit( const t3dShaderVariantDomain_t domain ) const {
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			return MAX( s_nMinShaderVariantCacheLimit, m_shaderConfig.m_nShaderVariantCacheLimit / 4 );
		case SHADER_DOMAIN_BUFFER:
			return MAX( 16, m_shaderConfig.m_nShaderVariantCacheLimit / 2 );
		case SHADER_DOMAIN_RUNTIME:
		default:
			return m_shaderConfig.m_nShaderVariantCacheLimit;
	}
}

uint64_t Terrain3DMaterial::getShaderVariantSourceBytesRaw( void ) const {
	return m_shaderConfig.m_nRuntimeShaderVariantSourceBytes + m_shaderConfig.m_nEditorShaderVariantSourceBytes
	        + m_shaderConfig.m_nBufferShaderVariantSourceBytes;
}

bool Terrain3DMaterial::isActiveShader( const Ref<Shader> &rShader ) const {
	if ( !rShader.is_valid() ) {
		return false;
	}
	if ( rShader.ptr() == m_shaderConfig.m_shader.ptr() || rShader.ptr() == m_shaderConfig.m_bufferShader.ptr() ) {
		return true;
	}
	for ( int tier = SHADER_TIER_MIDDLE; tier < SHADER_TIER_COUNT; ++tier ) {
		if ( m_shaderConfig.m_szTierShaders[tier].is_valid() && rShader.ptr() == m_shaderConfig.m_szTierShaders[tier].ptr() ) {
			return true;
		}
	}
	return false;
}

int Terrain3DMaterial::findShaderVariant( const t3dShaderVariantDomain_t domain, const t3dShaderVariantKey_t &rKey ) const {
	const t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
	const int count = getShaderVariantCountForDomain( domain );
	for ( int index = 0; index < count; ++index ) {
		if ( pEntries[index].m_key == rKey ) {
			return index;
		}
	}
	return -1;
}

void Terrain3DMaterial::eraseShaderVariant( const t3dShaderVariantDomain_t domain, const int iIndex ) {
	t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
	const int count = getShaderVariantCountForDomain( domain );
	uint64_t &retained_source_bytes = getShaderVariantDomainSourceBytes( domain );
	if ( iIndex < 0 || iIndex >= count ) {
		return;
	}
	const uint32_t source_bytes = pEntries[iIndex].m_nSourceBytes;
	for ( int index = iIndex; index + 1 < count; ++index ) {
		pEntries[index] = eastl::move( pEntries[index + 1] );
	}
	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			m_shaderConfig.m_editorShaderVariantEntries.pop_back();
			break;
		case SHADER_DOMAIN_BUFFER:
			m_shaderConfig.m_bufferShaderVariantEntries.pop_back();
			break;
		case SHADER_DOMAIN_RUNTIME:
		default:
			m_shaderConfig.m_runtimeShaderVariantEntries.pop_back();
			break;
	}
	if ( ( uint64_t )( source_bytes ) > retained_source_bytes ) {
		LOG( ERROR,
		        "Shader variant source-byte accounting underflow in domain ",
		        ( int )( domain ),
		        ": entry ",
		        source_bytes,
		        ", retained ",
		        retained_source_bytes );
		retained_source_bytes = 0u;
	} else {
		retained_source_bytes -= ( uint64_t )( source_bytes );
	}
	++m_shaderConfig.m_nVariantRemovalsTotal;
}

void Terrain3DMaterial::touchShaderVariant( const t3dShaderVariantDomain_t domain, const int iIndex, const bool bActualUse ) {
	t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
	const int count = getShaderVariantCountForDomain( domain );
	if ( iIndex < 0 || iIndex >= count ) {
		return;
	}
	if ( bActualUse ) {
		pEntries[iIndex].m_bSpeculative = false;
	}
	if ( iIndex == 0 ) {
		return;
	}
	t3dShaderVariantEntry_t touched = eastl::move( pEntries[iIndex] );
	for ( int index = iIndex; index > 0; --index ) {
		pEntries[index] = eastl::move( pEntries[index - 1] );
	}
	pEntries[0] = eastl::move( touched );
}

bool Terrain3DMaterial::evictLruShaderVariant( const t3dShaderVariantDomain_t domain, const bool bSpeculativeOnly ) {
	const t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
	const int count = getShaderVariantCountForDomain( domain );
	for ( int index = count - 1; index >= 0; --index ) {
		const t3dShaderVariantEntry_t &entry = pEntries[index];
		if ( isActiveShader( entry.m_shader ) ) {
			continue;
		}
		if ( bSpeculativeOnly && !entry.m_bSpeculative ) {
			continue;
		}
		eraseShaderVariant( domain, index );
		return true;
	}
	return false;
}

bool Terrain3DMaterial::evictBudgetLruShaderVariant( const t3dShaderVariantDomain_t requestDomain, const bool bSpeculativeOnly ) {
	if ( evictLruShaderVariant( SHADER_DOMAIN_EDITOR, bSpeculativeOnly ) ) {
		return true;
	}
	if ( requestDomain != SHADER_DOMAIN_EDITOR && evictLruShaderVariant( SHADER_DOMAIN_BUFFER, bSpeculativeOnly ) ) {
		return true;
	}
	if ( requestDomain == SHADER_DOMAIN_RUNTIME && evictLruShaderVariant( SHADER_DOMAIN_RUNTIME, bSpeculativeOnly ) ) {
		return true;
	}
	return false;
}

void Terrain3DMaterial::trimShaderVariantCaches( void ) {
	const t3dShaderVariantDomain_t domains[] = { SHADER_DOMAIN_RUNTIME, SHADER_DOMAIN_EDITOR, SHADER_DOMAIN_BUFFER };
	for ( size_t iDomain = 0u; iDomain < T3D_ARRAY_COUNT( domains ); iDomain++ ) {
		const t3dShaderVariantDomain_t domain = ( domains )[iDomain];
		const int limit = getVariantCacheLimit( domain );
		while ( getShaderVariantCountForDomain( domain ) > limit ) {
			if ( !evictLruShaderVariant( domain, true ) && !evictLruShaderVariant( domain, false ) ) {
				break;
			}
		}
	}

	const uint64_t budget_bytes = ( uint64_t )( m_shaderConfig.m_nShaderVariantEffectiveSourceBudgetMb ) * 1024ULL * 1024ULL;
	while ( getShaderVariantSourceBytesRaw() > budget_bytes ) {
		if ( !evictBudgetLruShaderVariant( SHADER_DOMAIN_RUNTIME, true ) && !evictBudgetLruShaderVariant( SHADER_DOMAIN_RUNTIME, false ) ) {
			break;
		}
	}
}

void Terrain3DMaterial::submitShaderSource( const Ref<Shader> &rShader, const String &rCode ) {
	if ( rShader.is_null() ) {
		return;
	}
	Time *const pTime = Time::get_singleton();
	const uint64_t started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	rShader->set_code( rCode );
	const uint64_t finished_usec = pTime ? pTime->get_ticks_usec() : started_usec;
	const uint64_t elapsed_usec = finished_usec - started_usec;
	m_shaderConfig.m_nVariantSourceSubmissionUsecLast = elapsed_usec;
	m_shaderConfig.m_nVariantSourceSubmissionUsecTotal += elapsed_usec;
	m_shaderConfig.m_nVariantSourceSubmissionUsecPeak = MAX( m_shaderConfig.m_nVariantSourceSubmissionUsecPeak, elapsed_usec );
}

t3dShaderValidationResult_t Terrain3DMaterial::validateShaderVariant( const Ref<Shader> &rShader, const t3dShaderTier_t tier ) {
	t3dShaderValidationResult_t result;
	if ( rShader.is_null() ) {
		result.m_failureSummary = "Shader resource is null";
		return result;
	}
	const RID shader_rid = rShader->get_rid();
	result.m_nShaderRidId = shader_rid.get_id();
	if ( !shader_rid.is_valid() ) {
		result.m_failureSummary = "Shader RID is invalid";
		return result;
	}

	const Array parameter_list = rShader->get_shader_uniform_list( true );
	++m_shaderConfig.m_nVariantValidationReflectionQueriesTotal;
	result.m_nReflectedParameterCount = parameter_list.size();
	if ( parameter_list.is_empty() ) {
		result.m_failureSummary = "Godot returned zero reflected shader parameters";
		return result;
	}

	if ( !t3dMaterialRequireShaderParameter( parameter_list, "_target_pos", result.m_failureSummary )
	        || !t3dMaterialRequireShaderParameter( parameter_list, "_far_height_map", result.m_failureSummary )
	        || !t3dMaterialRequireShaderParameter( parameter_list, "_far_color_map", result.m_failureSummary )
	        || !t3dMaterialRequireShaderParameter( parameter_list, "_clipmap_lod_states", result.m_failureSummary ) ) {
		return result;
	}
	if ( tier == SHADER_TIER_NEAR || tier == SHADER_TIER_MIDDLE ) {
		if ( !t3dMaterialRequireShaderParameter( parameter_list, "_region_map_texture", result.m_failureSummary )
		        || !t3dMaterialRequireShaderParameter( parameter_list, "_logical_region_map_texture", result.m_failureSummary ) ) {
			return result;
		}
	}
	if ( tier == SHADER_TIER_NEAR && !t3dMaterialRequireShaderParameter( parameter_list, "_height_maps", result.m_failureSummary ) ) {
		return result;
	}
	if ( tier == SHADER_TIER_MIDDLE
	        && ( !t3dMaterialRequireShaderParameter( parameter_list, "_derived_maps", result.m_failureSummary )
	                || !t3dMaterialRequireShaderParameter( parameter_list, "_middle_height_region_map_texture", result.m_failureSummary )
	                || !t3dMaterialRequireShaderParameter( parameter_list, "_middle_height_maps", result.m_failureSummary )
	                || ( m_authoringState.m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST
	                        && !t3dMaterialRequireShaderParameter(
	                                parameter_list, "_middle_composite_maps", result.m_failureSummary ) ) ) ) {
		return result;
	}
	const bool bBakedMiddle =
	        tier == SHADER_TIER_MIDDLE && m_authoringState.m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST;
	const char *const albedo_parameter = tier == SHADER_TIER_FAR ? "_far_albedo_array" : "_texture_array_albedo";
	if ( !bBakedMiddle && !t3dMaterialRequireShaderParameter( parameter_list, albedo_parameter, result.m_failureSummary ) ) {
		return result;
	}
	if ( tier == SHADER_TIER_FAR ) {
		const StringName forbidden[] = { StringName( "_logical_region_map_texture" ),
			StringName( "_region_map_texture" ),
			StringName( "_height_maps" ),
			StringName( "_control_maps" ),
			StringName( "_color_maps" ),
			StringName( "_derived_maps" ) };
		for ( size_t iForbidden = 0u; iForbidden < T3D_ARRAY_COUNT( forbidden ); iForbidden++ ) {
			const StringName &name = ( forbidden )[iForbidden];
			if ( t3dMaterialShaderParameterListHas( parameter_list, name ) ) {
				result.m_failureSummary = String( "Far shader reflected forbidden streamed dependency: " ) + String( name );
				return result;
			}
		}
	}
	result.m_bValid = true;
	return result;
}

void Terrain3DMaterial::recordShaderTierValidation( const t3dShaderTier_t tier,
        const String &rCode,
        const t3dShaderValidationResult_t &rValidation,
        const uint64_t nActiveShaderRidId,
        const bool bFallbackToNear,
        const bool bCompactTierActive ) {
	if ( tier < SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT ) {
		return;
	}
	t3dShaderTierValidationState_t &state = m_shaderConfig.m_szShaderTierValidation[tier];
	const t3dShaderVariantKey_t next_key = makeVariantCacheKey( rCode );
	const bool bStateChanged = state.m_key.m_nPrimaryHash != next_key.m_nPrimaryHash
	        || state.m_key.m_nFingerprint != next_key.m_nFingerprint || state.m_key.m_nSourceLength != next_key.m_nSourceLength
	        || state.m_nActiveShaderRidId != nActiveShaderRidId || state.m_bCompileValid != rValidation.m_bValid
	        || state.m_nReflectedParameterCount != rValidation.m_nReflectedParameterCount
	        || state.m_failureSummary != rValidation.m_failureSummary || state.m_bFallbackToNear != bFallbackToNear
	        || state.m_bCompactTierActive != bCompactTierActive;
	state.m_key = next_key;
	state.m_nRequestedShaderRidId = rValidation.m_nShaderRidId;
	state.m_nActiveShaderRidId = nActiveShaderRidId;
	state.m_bCompileValid = rValidation.m_bValid;
	state.m_nReflectedParameterCount = rValidation.m_nReflectedParameterCount;
	state.m_failureSummary = rValidation.m_failureSummary;
	state.m_bFallbackToNear = bFallbackToNear;
	state.m_bCompactTierActive = bCompactTierActive;
	if ( bStateChanged ) {
		emit_signal( "shader_tier_state_changed",
		        ( int )( tier ),
		        String::num_uint64( state.m_key.m_nPrimaryHash, 16, true ),
		        state.m_bCompileValid,
		        state.m_bFallbackToNear );
	}
}

Ref<Shader> Terrain3DMaterial::getOrCreateShaderVariant( const String &rCode,
        const t3dShaderVariantDomain_t domain,
        const bool bPrewarm,
        const t3dShaderTier_t tier,
        t3dShaderValidationResult_t *pValidation ) {
	const uint32_t source_bytes = ( uint32_t )( rCode.to_utf8_buffer().size() );
	m_shaderConfig.m_nLastVariantRequestDomain = ( int )( domain );
	m_shaderConfig.m_bLastVariantRequestHit = false;
	m_shaderConfig.m_bLastVariantRequestPrewarm = bPrewarm;
	m_shaderConfig.m_nLastVariantRequestSourceBytes = source_bytes;
	if ( pValidation ) {
		*pValidation = t3dShaderValidationResult_t{};
	}
	if ( m_authoringState.m_shaderVariantMode == SHADER_VARIANTS_DISABLED ) {
		// Even with caching disabled, shader replacement must remain transactional.
		// Mutating the currently active Shader resource before validation can leave
		// the native shader compiler with a pre-raster stage that failed to compile.
		Ref<Shader> shader;
		shader.instantiate();
		++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
		submitShaderSource( shader, rCode );
		if ( domain != SHADER_DOMAIN_BUFFER ) {
			const t3dShaderValidationResult_t validation = validateShaderVariant( shader, tier );
			if ( pValidation ) {
				*pValidation = validation;
			}
			if ( !validation.m_bValid ) {
				++m_shaderConfig.m_nVariantValidationFailuresTotal;
				return Ref<Shader>();
			}
		} else if ( pValidation ) {
			pValidation->m_bValid = true;
			pValidation->m_nShaderRidId = shader->get_rid().get_id();
		}
		return shader;
	}

	t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
	uint64_t &retained_source_bytes = getShaderVariantDomainSourceBytes( domain );
	const t3dShaderVariantKey_t key = makeVariantCacheKey( rCode );
	const int cached_index = findShaderVariant( domain, key );
	if ( cached_index >= 0 ) {
		if ( pEntries[cached_index].m_shader.is_valid() && pEntries[cached_index].m_shader->get_rid().is_valid() ) {
			++m_shaderConfig.m_nVariantCacheHitsTotal;
			m_shaderConfig.m_bLastVariantRequestHit = true;
			const Ref<Shader> pCachedShader = pEntries[cached_index].m_shader;
			if ( pValidation ) {
				pValidation->m_bValid = true;
				pValidation->m_nReflectedParameterCount = pEntries[cached_index].m_nReflectedParameterCount;
				pValidation->m_nShaderRidId = pCachedShader->get_rid().get_id();
			}
			if ( !bPrewarm ) {
				touchShaderVariant( domain, cached_index, true );
			}
			return pCachedShader;
		}
		eraseShaderVariant( domain, cached_index );
	}
	++m_shaderConfig.m_nVariantCacheMissesTotal;

	const int domain_limit = getVariantCacheLimit( domain );
	while ( getShaderVariantCountForDomain( domain ) >= domain_limit ) {
		bool bEvicted = evictLruShaderVariant( domain, true );
		if ( !bEvicted && !bPrewarm ) {
			bEvicted = evictLruShaderVariant( domain, false );
		}
		if ( !bEvicted ) {
			++m_shaderConfig.m_nVariantAllocationFailuresTotal;
			return Ref<Shader>();
		}
	}

	const uint64_t budget_bytes = ( uint64_t )( m_shaderConfig.m_nShaderVariantEffectiveSourceBudgetMb ) * 1024ULL * 1024ULL;
	while ( getShaderVariantSourceBytesRaw() + source_bytes > budget_bytes ) {
		bool bEvicted = evictBudgetLruShaderVariant( domain, true );
		if ( !bEvicted && !bPrewarm ) {
			bEvicted = evictBudgetLruShaderVariant( domain, false );
		}
		if ( !bEvicted ) {
			if ( bPrewarm ) {
				++m_shaderConfig.m_nVariantAllocationFailuresTotal;
				return Ref<Shader>();
			}
			LOG( WARN,
			        "Shader variant source budget is smaller than the active working set; retaining the selected "
			        "shader and exceeding the soft budget." );
			break;
		}
	}

	const int physical_capacity = getVariantPhysicalCapacity( domain );
	if ( getShaderVariantCountForDomain( domain ) >= physical_capacity ) {
		LOG( ERROR, "Shader variant cache exceeded its fixed physical capacity of ", physical_capacity );
		++m_shaderConfig.m_nVariantAllocationFailuresTotal;
		return Ref<Shader>();
	}

	Ref<Shader> pVariant;
	pVariant.instantiate();
	submitShaderSource( pVariant, rCode );
	t3dShaderValidationResult_t validation;
	if ( domain != SHADER_DOMAIN_BUFFER ) {
		validation = validateShaderVariant( pVariant, tier );
		if ( !validation.m_bValid ) {
			++m_shaderConfig.m_nVariantValidationFailuresTotal;
			if ( pValidation ) {
				*pValidation = validation;
			}
			return Ref<Shader>();
		}
	} else {
		validation.m_bValid = true;
		validation.m_nShaderRidId = pVariant->get_rid().get_id();
	}
	if ( pValidation ) {
		*pValidation = validation;
	}

	switch ( domain ) {
		case SHADER_DOMAIN_EDITOR:
			m_shaderConfig.m_editorShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );
			break;
		case SHADER_DOMAIN_BUFFER:
			m_shaderConfig.m_bufferShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );
			break;
		case SHADER_DOMAIN_RUNTIME:
		default:
			m_shaderConfig.m_runtimeShaderVariantEntries.push_back( t3dShaderVariantEntry_t{} );
			break;
	}
	pEntries = getShaderVariantEntries( domain );
	const int count = getShaderVariantCountForDomain( domain );
	for ( int index = count - 1; index > 0; --index ) {
		pEntries[index] = eastl::move( pEntries[index - 1] );
	}
	t3dShaderVariantEntry_t &entry = pEntries[0];
	entry.m_key = key;
	entry.m_shader = pVariant;
	entry.m_nSourceBytes = source_bytes;
	entry.m_nReflectedParameterCount = validation.m_nReflectedParameterCount;
	entry.m_bSpeculative = bPrewarm;
	retained_source_bytes += source_bytes;
	++m_shaderConfig.m_nVariantCreationsTotal;
	if ( bPrewarm ) {
		++m_shaderConfig.m_nVariantPrewarmCreationsTotal;
	}
	return pVariant;
}

uint64_t Terrain3DMaterial::getChangedVariantDimensions(
        const t3dShaderVariantState_t &rPrevious, const t3dShaderVariantState_t &rCurrent ) const {
	uint64_t changed = 0u;
	if ( rPrevious.m_worldBackground != rCurrent.m_worldBackground ) {
		changed |= 1ULL << VARIANT_DIM_WORLD_BACKGROUND;
	}
	if ( rPrevious.m_textureFiltering != rCurrent.m_textureFiltering ) {
		changed |= 1ULL << VARIANT_DIM_TEXTURE_FILTERING;
	}
	if ( rPrevious.m_terrainSamplingMode != rCurrent.m_terrainSamplingMode ) {
		changed |= 1ULL << VARIANT_DIM_TERRAIN_SAMPLING;
	}
	if ( rPrevious.m_terrainNormalMode != rCurrent.m_terrainNormalMode ) {
		changed |= 1ULL << VARIANT_DIM_TERRAIN_NORMAL;
	}
	if ( rPrevious.m_colorSamplingMode != rCurrent.m_colorSamplingMode ) {
		changed |= 1ULL << VARIANT_DIM_COLOR_SAMPLING;
	}
	if ( rPrevious.m_projectionMode != rCurrent.m_projectionMode ) {
		changed |= 1ULL << VARIANT_DIM_PROJECTION;
	}
	if ( rPrevious.m_tessellationMaterialMode != rCurrent.m_tessellationMaterialMode ) {
		changed |= 1ULL << VARIANT_DIM_TESSELLATION_MATERIAL;
	}
	if ( rPrevious.m_surfaceDataMode != rCurrent.m_surfaceDataMode ) {
		changed |= 1ULL << VARIANT_DIM_SURFACE_DATA;
	}
	if ( rPrevious.m_terrainShadingMode != rCurrent.m_terrainShadingMode ) {
		changed |= 1ULL << VARIANT_DIM_SHADING;
	}
	if ( rPrevious.m_detilingMode != rCurrent.m_detilingMode ) {
		changed |= 1ULL << VARIANT_DIM_DETILING;
	}
	if ( rPrevious.m_textureSurfaceMode != rCurrent.m_textureSurfaceMode ) {
		changed |= 1ULL << VARIANT_DIM_TEXTURE_SURFACE;
	}
	if ( rPrevious.m_worldNoiseShaderMode != rCurrent.m_worldNoiseShaderMode ) {
		changed |= 1ULL << VARIANT_DIM_WORLD_NOISE;
	}
	if ( rPrevious.m_bAutoShaderEnabled != rCurrent.m_bAutoShaderEnabled ) {
		changed |= 1ULL << VARIANT_DIM_AUTO_SHADER;
	}
	if ( rPrevious.m_bDualScalingEnabled != rCurrent.m_bDualScalingEnabled ) {
		changed |= 1ULL << VARIANT_DIM_DUAL_SCALING;
	}
	if ( rPrevious.m_bMacroVariationEnabled != rCurrent.m_bMacroVariationEnabled ) {
		changed |= 1ULL << VARIANT_DIM_MACRO_VARIATION;
	}
	if ( rPrevious.m_bOutputAlbedoEnabled != rCurrent.m_bOutputAlbedoEnabled ) {
		changed |= 1ULL << VARIANT_DIM_OUTPUT_ALBEDO;
	}
	if ( rPrevious.m_bOutputRoughnessEnabled != rCurrent.m_bOutputRoughnessEnabled ) {
		changed |= 1ULL << VARIANT_DIM_OUTPUT_ROUGHNESS;
	}
	if ( rPrevious.m_bOutputSpecularEnabled != rCurrent.m_bOutputSpecularEnabled ) {
		changed |= 1ULL << VARIANT_DIM_OUTPUT_SPECULAR;
	}
	if ( rPrevious.m_bOutputNormalMapEnabled != rCurrent.m_bOutputNormalMapEnabled ) {
		changed |= 1ULL << VARIANT_DIM_OUTPUT_NORMAL;
	}
	if ( rPrevious.m_bOutputAmbientOcclusionEnabled != rCurrent.m_bOutputAmbientOcclusionEnabled ) {
		changed |= 1ULL << VARIANT_DIM_OUTPUT_AO;
	}
	return changed;
}

uint64_t Terrain3DMaterial::getInferredPotentialDimensions( const t3dShaderVariantState_t &eState ) const {
	uint64_t potential = 0u;
	if ( eState.m_projectionMode != PROJECTION_DISABLED ) {
		potential |= 1ULL << VARIANT_DIM_PROJECTION;
	}
	if ( eState.m_bAutoShaderEnabled ) {
		potential |= 1ULL << VARIANT_DIM_AUTO_SHADER;
	}
	if ( eState.m_bDualScalingEnabled ) {
		potential |= 1ULL << VARIANT_DIM_DUAL_SCALING;
	}
	if ( eState.m_bMacroVariationEnabled ) {
		potential |= 1ULL << VARIANT_DIM_MACRO_VARIATION;
	}
	if ( eState.m_bTessellationEnabled ) {
		potential |= 1ULL << VARIANT_DIM_TESSELLATION_MATERIAL;
	}
	if ( eState.m_worldBackground == NOISE ) {
		potential |= 1ULL << VARIANT_DIM_WORLD_NOISE;
	}
	return potential;
}

void Terrain3DMaterial::recordVariantTransition( const t3dShaderVariantState_t &eState ) {
	uint64_t changed = getInferredPotentialDimensions( eState );
	if ( m_shaderConfig.m_bHasLastBoundVariantState ) {
		changed |= getChangedVariantDimensions( m_shaderConfig.m_lastBoundVariantState, eState );
	}

	for ( int dimension = 0; dimension < VARIANT_DIM_COUNT; ++dimension ) {
		const uint64_t bit = 1ULL << ( uint64_t )( dimension );
		if ( ( changed & bit ) == 0 ) {
			continue;
		}
		m_shaderConfig.m_nObservedVariantDimensions |= bit;
		m_shaderConfig.m_szVariantDimensionLastUsed[dimension] = ++m_shaderConfig.m_nVariantTransitionClock;
	}
	m_shaderConfig.m_lastBoundVariantState = eState;
	m_shaderConfig.m_bHasLastBoundVariantState = true;
}

bool Terrain3DMaterial::appendVariantState( eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> &rStates,
        const t3dShaderVariantState_t &eState ) const {
	const int capacity = MIN( m_shaderConfig.m_nShaderVariantPrewarmLimit, s_nMaxShaderVariantPrewarmLimit );
	if ( ( int )( rStates.size() ) >= capacity ) {
		return false;
	}
	rStates.push_back( eState );
	return true;
}

void Terrain3DMaterial::appendDimensionStates(
        eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> &rStates,
        const t3dShaderVariantState_t &rBase,
        const t3dShaderVariantDimension_t dimension ) const {
	t3dShaderVariantState_t state;
	switch ( dimension ) {
		case VARIANT_DIM_WORLD_BACKGROUND:
			for ( int value = NONE; value <= NOISE; ++value ) {
				if ( value != rBase.m_worldBackground ) {
					state = rBase;
					state.m_worldBackground = ( t3dWorldBackground_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_TEXTURE_FILTERING:
			for ( int value = LINEAR_ANISOTROPIC; value <= NEAREST; ++value ) {
				if ( value != rBase.m_textureFiltering ) {
					state = rBase;
					state.m_textureFiltering = ( t3dTextureFiltering_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_TERRAIN_SAMPLING:
			for ( int value = TERRAIN_SAMPLING_ADAPTIVE; value <= TERRAIN_SAMPLING_ADAPTIVE_2_CORNER; ++value ) {
				if ( value != rBase.m_terrainSamplingMode ) {
					state = rBase;
					state.m_terrainSamplingMode = ( t3dTerrainSamplingMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_TERRAIN_NORMAL:
			for ( int value = TERRAIN_NORMAL_ADAPTIVE; value <= TERRAIN_NORMAL_HEIGHT_3_TAP; ++value ) {
				if ( value != rBase.m_terrainNormalMode ) {
					state = rBase;
					state.m_terrainNormalMode = ( t3dTerrainNormalMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_COLOR_SAMPLING:
			for ( int value = COLOR_SAMPLING_AUTO; value <= COLOR_SAMPLING_EXACT_4_TAP; ++value ) {
				if ( value != rBase.m_colorSamplingMode ) {
					state = rBase;
					state.m_colorSamplingMode = ( t3dColorSamplingMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_PROJECTION:
			for ( int value = PROJECTION_DISABLED; value <= PROJECTION_EXACT_PER_CORNER; ++value ) {
				if ( value != rBase.m_projectionMode ) {
					state = rBase;
					state.m_projectionMode = ( t3dProjectionMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_TESSELLATION_MATERIAL:
			if ( rBase.m_bTessellationEnabled ) {
				state = rBase;
				state.m_tessellationMaterialMode = rBase.m_tessellationMaterialMode == TESSELLATION_GEOMETRY_ONLY
				        ? TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT
				        : TESSELLATION_GEOMETRY_ONLY;
				appendVariantState( rStates, state );
			}
			break;
		case VARIANT_DIM_SURFACE_DATA:
			for ( int value = SURFACE_DATA_AUTO; value <= SURFACE_DATA_ALBEDO_ONLY; ++value ) {
				if ( value != rBase.m_surfaceDataMode ) {
					state = rBase;
					state.m_surfaceDataMode = ( t3dSurfaceDataMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_SHADING:
			for ( int value = SHADING_BURLEY_GGX; value <= SHADING_XBOX_360_BLINN_PHONG; ++value ) {
				if ( value != rBase.m_terrainShadingMode ) {
					state = rBase;
					state.m_terrainShadingMode = ( t3dTerrainShadingMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_DETILING:
			for ( int value = DETILING_CONTINUOUS; value <= DETILING_DISCRETE_16; ++value ) {
				if ( value != rBase.m_detilingMode ) {
					state = rBase;
					state.m_detilingMode = ( t3dDetilingMode_t )( value );
					if ( !appendVariantState( rStates, state ) ) {
						return;
					}
				}
			}
			break;
		case VARIANT_DIM_TEXTURE_SURFACE:
			state = rBase;
			state.m_textureSurfaceMode =
			        rBase.m_textureSurfaceMode == TEXTURE_SURFACE_TEXTURED ? TEXTURE_SURFACE_SILHOUETTE : TEXTURE_SURFACE_TEXTURED;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_WORLD_NOISE:
			if ( rBase.m_worldBackground == NOISE ) {
				for ( int value = WORLD_NOISE_DYNAMIC; value <= WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS; ++value ) {
					if ( value != rBase.m_worldNoiseShaderMode ) {
						state = rBase;
						state.m_worldNoiseShaderMode = ( t3dWorldNoiseShaderMode_t )( value );
						if ( !appendVariantState( rStates, state ) ) {
							return;
						}
					}
				}
			}
			break;
		case VARIANT_DIM_AUTO_SHADER:
			state = rBase;
			state.m_bAutoShaderEnabled = !state.m_bAutoShaderEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_DUAL_SCALING:
			state = rBase;
			state.m_bDualScalingEnabled = !state.m_bDualScalingEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_MACRO_VARIATION:
			state = rBase;
			state.m_bMacroVariationEnabled = !state.m_bMacroVariationEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_OUTPUT_ALBEDO:
			state = rBase;
			state.m_bOutputAlbedoEnabled = !state.m_bOutputAlbedoEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_OUTPUT_ROUGHNESS:
			state = rBase;
			state.m_bOutputRoughnessEnabled = !state.m_bOutputRoughnessEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_OUTPUT_SPECULAR:
			state = rBase;
			state.m_bOutputSpecularEnabled = !state.m_bOutputSpecularEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_OUTPUT_NORMAL:
			state = rBase;
			state.m_bOutputNormalMapEnabled = !state.m_bOutputNormalMapEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_OUTPUT_AO:
			state = rBase;
			state.m_bOutputAmbientOcclusionEnabled = !state.m_bOutputAmbientOcclusionEnabled;
			appendVariantState( rStates, state );
			break;
		case VARIANT_DIM_COUNT:
		default:
			break;
	}
}

void Terrain3DMaterial::appendPotentialBranchStates(
        eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> &rStates,
        const t3dShaderVariantState_t &rBase ) const {
	appendVariantState( rStates, rBase );

	// Quality presets are the most likely menu transitions. Only neighboring tiers
	// are speculative; selecting a distant tier may stall once and then joins the LRU.
	if ( m_authoringState.m_shaderPerformancePreset > SHADER_PRESET_CUSTOM
	        && m_authoringState.m_shaderPerformancePreset <= SHADER_PRESET_MINIMUM ) {
		for ( int offset = -1; offset <= 1; offset += 2 ) {
			const int preset = ( int )( m_authoringState.m_shaderPerformancePreset ) + offset;
			if ( preset < SHADER_PRESET_CINEMATIC || preset > SHADER_PRESET_MINIMUM ) {
				continue;
			}
			t3dShaderVariantState_t state = rBase;
			applyPerformancePresetToState( state, ( t3dShaderPerformancePreset_t )( preset ) );
			appendVariantState( rStates, state );
		}
	} else if ( m_authoringState.m_shaderPerformancePreset == SHADER_PRESET_XBOX_360_AUTHENTIC ) {
		t3dShaderVariantState_t styled_state = rBase;
		applyPerformancePresetToState( styled_state, SHADER_PRESET_RUNTIME_360 );
		appendVariantState( rStates, styled_state );
		t3dShaderVariantState_t high_state = rBase;
		applyPerformancePresetToState( high_state, SHADER_PRESET_HIGH );
		appendVariantState( rStates, high_state );
	} else {
		t3dShaderVariantState_t styled_state = rBase;
		applyPerformancePresetToState( styled_state, SHADER_PRESET_RUNTIME_360 );
		appendVariantState( rStates, styled_state );
		t3dShaderVariantState_t authentic_state = rBase;
		applyPerformancePresetToState( authentic_state, SHADER_PRESET_XBOX_360_AUTHENTIC );
		appendVariantState( rStates, authentic_state );
	}

	uint64_t dimensions = getInferredPotentialDimensions( rBase );
	if ( m_authoringState.m_shaderVariantMode == SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT
	        || m_authoringState.m_shaderVariantMode == SHADER_VARIANTS_PREWARM_FEATURE_MATRIX ) {
		dimensions |= m_shaderConfig.m_nObservedVariantDimensions;
	}

	// Most recently adjusted dimensions are prewarmed first. The configurable cap
	// keeps speculative branches from taking over the 64-entry working set.
	while ( dimensions != 0
	        && ( int )( rStates.size() ) < MIN( m_shaderConfig.m_nShaderVariantPrewarmLimit, s_nMaxShaderVariantPrewarmLimit ) ) {
		int selected = -1;
		uint64_t selected_clock = 0;
		for ( int dimension = 0; dimension < VARIANT_DIM_COUNT; ++dimension ) {
			const uint64_t bit = 1ULL << ( uint64_t )( dimension );
			if ( ( dimensions & bit ) == 0 ) {
				continue;
			}
			if ( selected < 0 || m_shaderConfig.m_szVariantDimensionLastUsed[dimension] > selected_clock ) {
				selected = dimension;
				selected_clock = m_shaderConfig.m_szVariantDimensionLastUsed[dimension];
			}
		}
		if ( selected < 0 ) {
			break;
		}
		dimensions &= ~( 1ULL << ( uint64_t )( selected ) );
		appendDimensionStates( rStates, rBase, ( t3dShaderVariantDimension_t )( selected ) );
	}
}

uint64_t Terrain3DMaterial::getVariantStateSignature( const t3dShaderVariantState_t &state ) const {
	uint64_t signature = 1469598103934665603ULL;
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_worldBackground );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_textureFiltering );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_terrainSamplingMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_terrainNormalMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_colorSamplingMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_projectionMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_tessellationMaterialMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_surfaceDataMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_terrainShadingMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_detilingMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_textureSurfaceMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_worldNoiseShaderMode );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bAutoShaderEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bDualScalingEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bMacroVariationEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bOutputAlbedoEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bOutputRoughnessEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bOutputSpecularEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bOutputNormalMapEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bOutputAmbientOcclusionEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bFastBlendCurve );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bTessellationEnabled );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bSurfacePacked );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bControlMapsImplicit );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )state.m_bColorMapsImplicit );
	return signature;
}

uint64_t Terrain3DMaterial::getPrewarmSignature( void ) const {
	const t3dShaderVariantState_t state = captureVariantState();
	uint64_t signature = 1469598103934665603ULL;
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( m_authoringState.m_shaderVariantMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( m_shaderConfig.m_nShaderVariantCacheLimit ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( m_shaderConfig.m_nShaderVariantPrewarmLimit ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( m_shaderConfig.m_nShaderVariantSourceBudgetMb ) );
	t3dMaterialMixShaderSignature( signature, m_shaderConfig.m_nObservedVariantDimensions );
	for ( int dimension = 0; dimension < VARIANT_DIM_COUNT; ++dimension ) {
		t3dMaterialMixShaderSignature( signature, m_shaderConfig.m_szVariantDimensionLastUsed[dimension] );
	}
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_worldBackground ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_textureFiltering ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_terrainSamplingMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_terrainNormalMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_colorSamplingMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_projectionMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_tessellationMaterialMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_surfaceDataMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_terrainShadingMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_detilingMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_textureSurfaceMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_worldNoiseShaderMode ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bAutoShaderEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bDualScalingEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bMacroVariationEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bOutputAlbedoEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bOutputRoughnessEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bOutputSpecularEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bOutputNormalMapEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bOutputAmbientOcclusionEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bFastBlendCurve ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bTessellationEnabled ) );
	t3dMaterialMixShaderSignature( signature, ( uint64_t )( state.m_bSurfacePacked ) );
	return signature;
}

void Terrain3DMaterial::prewarmShaderVariantsInternal( const bool bForce ) {
	if ( m_authoringState.m_shaderVariantMode != SHADER_VARIANTS_PREWARM_ACTIVE_BRANCHES
	        && m_authoringState.m_shaderVariantMode != SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT ) {
		return;
	}
	if ( m_authoringState.m_bShaderOverrideEnabled || !m_pTerrain || ( IS_EDITOR && !bForce ) ) {
		return;
	}

	const bool strict_cooked_runtime = m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode();
	if ( strict_cooked_runtime ) {
		const uint64_t state_signature = getVariantStateSignature( captureVariantState() );
		if ( !m_shaderConfig.m_bRuntimeShaderPackLoaded || m_shaderConfig.m_nRuntimeShaderPackStateSignature != state_signature ) {
			if ( !loadRuntimeShaderPack() ) {
				m_shaderConfig.m_nLastPrewarmVariantCount = 0;
				m_shaderConfig.m_nLastPrewarmSkippedCount = 0;
				LOG( ERROR, "Runtime shader prewarm requires a valid terrain3d_shaders.t3dshaderpack for the current material state." );
			}
		}
		// Strict cooked runtime never regenerates speculative branches from authoring
		// source. loadRuntimeShaderPack() validates the required tier working set and
		// prewarms any additional packed entries directly from cooked source.
		return;
	}

	const uint64_t signature = getPrewarmSignature();
	if ( !bForce && signature == m_shaderConfig.m_nPrewarmSignature ) {
		return;
	}
	m_shaderConfig.m_nPrewarmSignature = signature;
	m_shaderConfig.m_nLastPrewarmVariantCount = 0;
	m_shaderConfig.m_nLastPrewarmSkippedCount = 0;

	const t3dShaderVariantState_t base = captureVariantState();
	eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> states;
	appendPotentialBranchStates( states, base );

	eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit, false> seen_main;
	eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit, false> seen_buffer;
	for ( size_t state_index = 0u; state_index < states.size(); ++state_index ) {
		const t3dShaderVariantState_t &state = states[state_index];
		const String main_code = finalizeRuntimeShaderCode( generateShaderCode( state ) );
		const t3dShaderVariantKey_t main_key = makeVariantCacheKey( main_code );
		bool bMainSeen = false;
		for ( size_t seen_index = 0u; seen_index < seen_main.size(); ++seen_index ) {
			if ( seen_main[seen_index] == main_key ) {
				bMainSeen = true;
				break;
			}
		}
		if ( !bMainSeen ) {
			if ( seen_main.size() >= s_nMaxShaderVariantPrewarmLimit ) {
				LOG( ERROR, "Main shader prewarm key set exceeded fixed capacity" );
				break;
			}
			seen_main.push_back( main_key );
			if ( getOrCreateShaderVariant( main_code, SHADER_DOMAIN_RUNTIME, true ).is_valid() ) {
				m_shaderConfig.m_nLastPrewarmVariantCount++;
			} else {
				m_shaderConfig.m_nLastPrewarmSkippedCount++;
			}
		}

		if ( state.m_bTessellationEnabled && !m_authoringState.m_bBufferShaderOverrideEnabled ) {
			const String buffer_code = generateBufferShaderCode( state );
			const t3dShaderVariantKey_t buffer_key = makeVariantCacheKey( buffer_code );
			bool bBufferSeen = false;
			for ( size_t seen_index = 0u; seen_index < seen_buffer.size(); ++seen_index ) {
				if ( seen_buffer[seen_index] == buffer_key ) {
					bBufferSeen = true;
					break;
				}
			}
			if ( !bBufferSeen ) {
				if ( seen_buffer.size() >= s_nMaxShaderVariantPrewarmLimit ) {
					LOG( ERROR, "Buffer shader prewarm key set exceeded fixed capacity" );
					break;
				}
				seen_buffer.push_back( buffer_key );
				if ( getOrCreateShaderVariant( buffer_code, SHADER_DOMAIN_BUFFER, true ).is_valid() ) {
					m_shaderConfig.m_nLastPrewarmVariantCount++;
				} else {
					m_shaderConfig.m_nLastPrewarmSkippedCount++;
				}
			}
		}
	}

	// Remove speculative entries from older branch neighborhoods. The cache is
	// already in MRU-to-LRU order, so deletion requires only one bounded shift.
	for ( int cache_index = getRuntimeShaderVariantCount() - 1; cache_index >= 0; --cache_index ) {
		const t3dShaderVariantEntry_t &entry = m_shaderConfig.m_runtimeShaderVariantEntries[cache_index];
		bool bStillPredicted = false;
		for ( size_t seen_index = 0u; seen_index < seen_main.size(); ++seen_index ) {
			if ( seen_main[seen_index] == entry.m_key ) {
				bStillPredicted = true;
				break;
			}
		}
		if ( entry.m_bSpeculative && !isActiveShader( entry.m_shader ) && !bStillPredicted ) {
			eraseShaderVariant( SHADER_DOMAIN_RUNTIME, cache_index );
		}
	}
	for ( int cache_index = getBufferShaderVariantCount() - 1; cache_index >= 0; --cache_index ) {
		const t3dShaderVariantEntry_t &entry = m_shaderConfig.m_bufferShaderVariantEntries[cache_index];
		bool bStillPredicted = false;
		for ( size_t seen_index = 0u; seen_index < seen_buffer.size(); ++seen_index ) {
			if ( seen_buffer[seen_index] == entry.m_key ) {
				bStillPredicted = true;
				break;
			}
		}
		if ( entry.m_bSpeculative && !isActiveShader( entry.m_shader ) && !bStillPredicted ) {
			eraseShaderVariant( SHADER_DOMAIN_BUFFER, cache_index );
		}
	}

	if ( m_shaderConfig.m_nLastPrewarmSkippedCount > 0 ) {
		LOG( WARN,
		        "Shader prewarm retained ",
		        m_shaderConfig.m_nLastPrewarmVariantCount,
		        " variants and skipped ",
		        m_shaderConfig.m_nLastPrewarmSkippedCount,
		        " because one or more variants failed validation or could not fit the cache/source-memory limits." );
	} else {
		LOG( INFO,
		        "Shader prewarm retained ",
		        m_shaderConfig.m_nLastPrewarmVariantCount,
		        " unique runtime/buffer variants using ",
		        getShaderVariantSourceBytesRaw(),
		        " source bytes." );
	}
}
String Terrain3DMaterial::generateShaderCode( void ) const {
	return generateShaderCode( captureVariantState(), SHADER_TIER_NEAR );
}

String Terrain3DMaterial::generateShaderCode( const t3dShaderVariantState_t &eState ) const {
	return generateShaderCode( eState, SHADER_TIER_NEAR );
}

void Terrain3DMaterial::applyTierQualityToState(
        t3dShaderVariantState_t &rState, const t3dTierQualityConfig_t &rQuality, const t3dShaderTier_t tier ) const {
	rState.m_textureFiltering = rQuality.m_textureFiltering;
	rState.m_terrainSamplingMode = rQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST ? TERRAIN_SAMPLING_SINGLE_SAMPLE
	                                                                                            : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
	rState.m_terrainNormalMode = rQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY ? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
	                                                                                                : TERRAIN_NORMAL_DERIVED_TEXTURE;
	rState.m_colorSamplingMode =
	        rQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_EXACT ? COLOR_SAMPLING_EXACT_4_TAP : COLOR_SAMPLING_HARDWARE_FILTERED;
	rState.m_surfaceDataMode =
	        rQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_DETAILED ? SURFACE_DATA_FULL : SURFACE_DATA_ALBEDO_ONLY;
	rState.m_textureSurfaceMode = rQuality.m_textureSurfaceMode;
	rState.m_terrainShadingMode = rQuality.m_lightingModel == TIER_LIGHTING_UNSHADED
	        ? SHADING_UNSHADED
	        : ( rQuality.m_lightingModel == TIER_LIGHTING_BLINN_PHONG
	                          ? SHADING_XBOX_360_BLINN_PHONG
	                          : ( rQuality.m_lightingModel == TIER_LIGHTING_PBR ? SHADING_BURLEY_GGX : SHADING_LAMBERT_DIFFUSE_ONLY ) );
	rState.m_detilingMode = rQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED : DETILING_DISCRETE_8;
	// Auto-material classification is shared across quality tiers. Far rendering does
	// not consume the detailed control-map autoshader path.
	if ( tier == SHADER_TIER_FAR ) {
		rState.m_bAutoShaderEnabled = false;
	}
	rState.m_bDualScalingEnabled = false;
	rState.m_bMacroVariationEnabled = false;
	if ( tier != SHADER_TIER_NEAR ) {
		rState.m_bOutputAlbedoEnabled = true;
		rState.m_bOutputRoughnessEnabled = false;
		rState.m_bOutputSpecularEnabled = false;
		rState.m_bOutputNormalMapEnabled = false;
		rState.m_bOutputAmbientOcclusionEnabled = false;
	}
}

t3dShaderVariantState_t Terrain3DMaterial::makeTierState( const t3dShaderVariantState_t &eState, const t3dShaderTier_t tier ) const {
	t3dShaderVariantState_t state = eState;
	if ( tier == SHADER_TIER_NEAR ) {
		applyTierQualityToState( state, m_authoringState.m_nearQuality, tier );
	} else if ( tier == SHADER_TIER_MIDDLE ) {
		applyTierQualityToState( state, m_authoringState.m_middleQuality, tier );
	} else if ( tier == SHADER_TIER_FAR ) {
		applyTierQualityToState( state, m_authoringState.m_farQuality, tier );
	}
	return state;
}

String Terrain3DMaterial::generateShaderCode(
        const t3dShaderVariantState_t &eState, const t3dShaderTier_t tier, const bool bIncludeEditorFeatures ) const {
	if ( !bIncludeEditorFeatures ) {
		const bool strict_cooked_builtin = m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode()
		        && !m_authoringState.m_bShaderOverrideEnabled;
		if ( m_shaderConfig.m_bRuntimeShaderPackLoaded
		        && getVariantStateSignature( eState ) == m_shaderConfig.m_nRuntimeShaderPackStateSignature && tier >= SHADER_TIER_NEAR
		        && tier < SHADER_TIER_COUNT && !m_shaderConfig.m_szRuntimeShaderPackTierSource[tier].is_empty() ) {
			return m_shaderConfig.m_szRuntimeShaderPackTierSource[tier];
		}
		if ( strict_cooked_builtin ) {
			ERR_PRINT_ONCE( "Terrain3D strict cooked runtime refused to regenerate built-in terrain shader source; "
			                "recook terrain3d_shaders.t3dshaderpack." );
			return String();
		}
	}
	const t3dShaderVariantState_t tier_state = makeTierState( eState, tier );
	LOG( INFO, "Generating terrain shader tier ", ( int )( tier ) );
	Array excludes;
	if ( tier_state.m_worldBackground != NONE ) {
		excludes.push_back( "NONE_FUNCTIONS" );
		excludes.push_back( "NONE_CHECK" );
		excludes.push_back( "MIDDLE_NONE_FRAGMENT" );
		excludes.push_back( "FAR_NONE_FUNCTIONS" );
		excludes.push_back( "FAR_NONE_FRAGMENT" );
	}
	if ( tier_state.m_worldBackground == NONE ) {
		excludes.push_back( "FLAT_UNIFORMS" );
		excludes.push_back( "FLAT_FUNCTIONS" );
		excludes.push_back( "FAR_FLAT_FUNCTIONS" );
		excludes.push_back( "FLAT_VERTEX" );
		excludes.push_back( "FLAT_FRAGMENT" );
	}
	if ( tier_state.m_worldBackground != NOISE ) {
		excludes.push_back( "WORLD_NOISE_UNIFORMS" );
		excludes.push_back( "WORLD_NOISE_FUNCTIONS" );
		excludes.push_back( "WORLD_NOISE_VERTEX" );
		excludes.push_back( "WORLD_NOISE_FRAGMENT" );
	}
	if ( tier_state.m_bSurfacePacked ) {
		excludes.push_back( "LEGACY_PBR_UNIFORMS" );
		excludes.push_back( "LEGACY_PBR_MATERIAL_FUNCTIONS" );
		excludes.push_back( "LEGACY_PBR_DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_DISPLACEMENT_FUNCTIONS" );
		if ( !tier_state.m_bTessellationEnabled || tier_state.m_tessellationMaterialMode == TESSELLATION_GEOMETRY_ONLY ) {
			excludes.push_back( "SURFACE_DISPLACEMENT_UNIFORM" );
		}
	} else {
		if ( !samplesSurfaceData( tier_state ) ) {
			excludes.push_back( "LEGACY_PBR_UNIFORMS" );
		}
		excludes.push_back( "SURFACE_PBR_UNIFORMS" );
		excludes.push_back( "SURFACE_DISPLACEMENT_UNIFORM" );
		excludes.push_back( "SURFACE_PBR_CORE_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_MATERIAL_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "LEGACY_PBR_DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "OUTPUT_METALLIC" );
	}

	switch ( tier_state.m_textureFiltering ) {
		case LINEAR_ANISOTROPIC:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			break;
		case LINEAR:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
		case NEAREST_ANISOTROPIC:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
		case NEAREST:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
	}
	if ( !tier_state.m_bAutoShaderEnabled ) {
		excludes.push_back( "AUTO_SHADER_UNIFORMS" );
		excludes.push_back( "AUTO_SHADER" );
	}
	if ( !tier_state.m_bDualScalingEnabled ) {
		excludes.push_back( "DUAL_SCALING_UNIFORMS" );
		excludes.push_back( "DUAL_SCALING_FACTOR" );
		excludes.push_back( "DUAL_SCALING" );
		excludes.push_back( "DUAL_SCALING_CONDITION_0" );
		excludes.push_back( "DUAL_SCALING_CONDITION_1" );
		excludes.push_back( "DUAL_SCALING_MIX" );
	}
	if ( !tier_state.m_bMacroVariationEnabled ) {
		excludes.push_back( "MACRO_VARIATION_UNIFORMS" );
		excludes.push_back( "MACRO_VARIATION" );
	}
	if ( tier_state.m_projectionMode == PROJECTION_DISABLED ) {
		excludes.push_back( "PROJECTION" );
	}
	if ( !tier_state.m_bTessellationEnabled ) {
		excludes.push_back( "DISPLACEMENT_UNIFORMS" );
		excludes.push_back( "DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "DISPLACEMENT_VERTEX" );
	}
	if ( !tier_state.m_bOutputAlbedoEnabled ) {
		excludes.push_back( "OUTPUT_ALBEDO" );
	} else {
		excludes.push_back( "OUTPUT_ALBEDO_GREY" );
	}
	if ( !tier_state.m_bOutputRoughnessEnabled ) {
		excludes.push_back( "OUTPUT_ROUGHNESS" );
	}
	if ( !tier_state.m_bOutputSpecularEnabled ) {
		excludes.push_back( "OUTPUT_SPECULAR" );
	} else {
		excludes.push_back( "OUTPUT_SPECULAR_NONE" );
	}
	if ( !tier_state.m_bOutputNormalMapEnabled ) {
		excludes.push_back( "OUTPUT_NORMAL_MAP" );
	}
	if ( !tier_state.m_bOutputAmbientOcclusionEnabled ) {
		excludes.push_back( "OUTPUT_AMBIENT_OCCLUSION" );
	}
	if ( tier_state.m_terrainShadingMode == SHADING_XBOX_360_BLINN_PHONG ) {
		// The authentic preset uses its custom non-metallic light loop. Keeping the
		// metallic output would reintroduce a modern PBR reflection response.
		excludes.push_back( "OUTPUT_METALLIC" );
	}
	StringName shader_template = "main";
	if ( tier == SHADER_TIER_MIDDLE ) {
		shader_template = "main_middle";
	} else if ( tier == SHADER_TIER_FAR ) {
		shader_template = "main_far";
	}
	String shader =
	        prepareShaderSource( m_shaderConfig.m_shaderTemplates[shader_template], tier_state, excludes, tier, bIncludeEditorFeatures );
	if ( shader.is_empty() ) {
		return String();
	}
	String render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, specular_schlick_ggx, "
	                     "skip_vertex_transform;";
	switch ( tier_state.m_terrainShadingMode ) {
		case SHADING_LAMBERT_GGX:
			render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_lambert, specular_schlick_ggx, "
			              "skip_vertex_transform;";
			break;
		case SHADING_LAMBERT_DIFFUSE_ONLY:
			render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_lambert, specular_disabled, "
			              "skip_vertex_transform;";
			break;
		case SHADING_UNSHADED:
			render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, unshaded, skip_vertex_transform;";
			break;
		case SHADING_LAMBERT_VERTEX:
			render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_lambert, specular_disabled, "
			              "vertex_lighting, skip_vertex_transform;";
			break;
		case SHADING_XBOX_360_BLINN_PHONG:
			// The custom light() function replaces the built-in direct-light model.
			// Keep specular output enabled so the custom loop can write SPECULAR_LIGHT.
			render_mode = "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_lambert, "
			              "specular_schlick_ggx, skip_vertex_transform;";
			break;
		case SHADING_BURLEY_GGX:
		default:
			break;
	}
	shader = shader.replace( "render_mode blend_mix, depth_draw_opaque, cull_back, diffuse_burley, "
	                         "specular_schlick_ggx, skip_vertex_transform;",
	        render_mode );
	return shader;
}

// Ripped from ShaderPreprocessor::CommentRemover::strip()
String Terrain3DMaterial::stripComments( const String &rShader ) const {
	Vector<char32_t> stripped;
	String code = rShader;
	int index = 0;
	int line = 0;
	int strings_open = 0;
	const char32_t CURSOR = 0xFFFF;

	while ( index < code.length() ) {
		const char32_t c = code[index++];
		if ( c == CURSOR ) {
			// Cursor. Maintain.
			stripped.push_back( c );
		} else if ( c == '"' ) {
			if ( strings_open <= 0 ) {
				strings_open++;
			} else {
				strings_open--;
			}
			stripped.push_back( c );
		} else if ( c == '/' && strings_open == 0 ) {
			const char32_t p = t3dMaterialCommentPeek( code, index );
			if ( p == '/' ) { // Single line comment.
				t3dMaterialCommentAdvance( code, index, line, stripped, '\n' );
			} else if ( p == '*' ) { // Start of a block comment.
				index++;
				while ( t3dMaterialCommentAdvance( code, index, line, stripped, '*' ) ) {
					if ( t3dMaterialCommentPeek( code, index ) == '/' ) { // End of a block comment.
						index++;
						break;
					}
				}
			} else {
				stripped.push_back( c );
			}
		} else if ( c == '*' && strings_open == 0 ) {
			if ( t3dMaterialCommentPeek( code, index ) == '/' ) { // Unmatched end of a block comment.
			} else {
				stripped.push_back( c );
			}
		} else if ( c == '\n' ) {
			line++;
			stripped.push_back( c );
		} else {
			stripped.push_back( c );
		}
	}
	return t3dMaterialCharVectorToString( stripped );
}

String Terrain3DMaterial::generateBufferShaderCode( void ) const {
	return generateBufferShaderCode( captureVariantState() );
}

String Terrain3DMaterial::generateBufferShaderCode( const t3dShaderVariantState_t &eState ) const {
	if ( m_shaderConfig.m_bRuntimeShaderPackLoaded
	        && getVariantStateSignature( eState ) == m_shaderConfig.m_nRuntimeShaderPackStateSignature
	        && !m_shaderConfig.m_runtimeShaderPackBufferSource.is_empty() ) {
		return m_shaderConfig.m_runtimeShaderPackBufferSource;
	}
	const bool strict_cooked_builtin = m_pTerrain && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode()
	        && !m_authoringState.m_bBufferShaderOverrideEnabled;
	if ( strict_cooked_builtin ) {
		ERR_PRINT_ONCE( "Terrain3D strict cooked runtime refused to regenerate displacement shader source; recook "
		                "terrain3d_shaders.t3dshaderpack." );
		return String();
	}
	LOG( INFO, "Generating default displacement buffer shader code" );
	// The displacement buffer does not consume terrain lighting, color sampling,
	// material PBR outputs, dual scaling, or macro variation. Normalize those
	// dimensions before emitting defines so unrelated Inspector changes map to
	// the same buffer shader instead of compiling duplicate programs.
	t3dShaderVariantState_t buffer_state = eState;
	buffer_state.m_terrainNormalMode = TERRAIN_NORMAL_ADAPTIVE;
	buffer_state.m_colorSamplingMode = COLOR_SAMPLING_AUTO;
	buffer_state.m_tessellationMaterialMode = TESSELLATION_GEOMETRY_ONLY;
	buffer_state.m_surfaceDataMode = SURFACE_DATA_ALBEDO_ONLY;
	buffer_state.m_terrainShadingMode = SHADING_UNSHADED;
	buffer_state.m_bDualScalingEnabled = false;
	buffer_state.m_bMacroVariationEnabled = false;
	buffer_state.m_bOutputAlbedoEnabled = false;
	buffer_state.m_bOutputRoughnessEnabled = false;
	buffer_state.m_bOutputSpecularEnabled = false;
	buffer_state.m_bOutputNormalMapEnabled = false;
	buffer_state.m_bOutputAmbientOcclusionEnabled = false;
	if ( buffer_state.m_projectionMode != PROJECTION_DISABLED ) {
		buffer_state.m_projectionMode = PROJECTION_SHARED_FRAGMENT;
	}
	Array excludes;
	if ( buffer_state.m_worldBackground != NONE ) {
		excludes.push_back( "NONE_FUNCTIONS" );
		excludes.push_back( "NONE_CHECK" );
	}
	if ( buffer_state.m_worldBackground == NONE ) {
		excludes.push_back( "FLAT_UNIFORMS" );
		excludes.push_back( "FLAT_FUNCTIONS" );
		excludes.push_back( "FLAT_FRAGMENT" );
	}
	if ( buffer_state.m_bSurfacePacked ) {
		excludes.push_back( "LEGACY_PBR_UNIFORMS" );
		excludes.push_back( "LEGACY_PBR_MATERIAL_FUNCTIONS" );
		excludes.push_back( "LEGACY_PBR_DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_MATERIAL_FUNCTIONS" );
	} else {
		excludes.push_back( "SURFACE_PBR_UNIFORMS" );
		excludes.push_back( "SURFACE_DISPLACEMENT_UNIFORM" );
		excludes.push_back( "SURFACE_PBR_CORE_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_MATERIAL_FUNCTIONS" );
		excludes.push_back( "SURFACE_PBR_DISPLACEMENT_FUNCTIONS" );
		excludes.push_back( "LEGACY_PBR_MATERIAL_FUNCTIONS" );
	}

	switch ( buffer_state.m_textureFiltering ) {
		case LINEAR_ANISOTROPIC:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			break;
		case LINEAR:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
		case NEAREST_ANISOTROPIC:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
		case NEAREST:
			excludes.push_back( "TEXTURE_SAMPLERS_NEAREST_ANISOTROPIC" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR" );
			excludes.push_back( "TEXTURE_SAMPLERS_LINEAR_ANISOTROPIC" );
			break;
	}
	if ( !buffer_state.m_bAutoShaderEnabled ) {
		excludes.push_back( "AUTO_SHADER_UNIFORMS" );
		excludes.push_back( "AUTO_SHADER" );
	}
	if ( buffer_state.m_projectionMode == PROJECTION_DISABLED ) {
		excludes.push_back( "PROJECTION" );
	}
	String shader = prepareShaderSource( m_shaderConfig.m_shaderTemplates["displacement_buffer"], buffer_state, excludes );
	return finalizeRuntimeShaderCode( shader );
}

String Terrain3DMaterial::finalizeRuntimeShaderCode( const String &rShader ) const {
	// Built-in shaders are already flattened and specialized. Preserve source text
	// so Godot diagnostics still refer to the assembled line structure.
	return rShader;
}

String Terrain3DMaterial::injectEditorCode( const String &rShader ) const {
	// Custom shader overrides still use Godot include preprocessing. Built-in
	// shaders never enter this path; they receive editor features during the
	// Terrain3D assembly pass before Shader::set_code().
	const PackedStringArray editor_features = getEditorShaderFeatures();
	PackedStringArray editor_defines;
	for ( int index = 0; index < editor_features.size(); ++index ) {
		t3dMaterialAppendDefine( editor_defines, String( "T3D_ENABLE_" ) + editor_features[index], "1" );
	}

	String shader = t3dMaterialInjectShaderDefines( rShader, editor_defines );
	const String setup_include = "#include \"res://addons/terrain_3d/shaders/includes/editor_setup.gdshaderinc\"";
	const String fragment_include = "#include \"res://addons/terrain_3d/shaders/includes/editor_fragment.gdshaderinc\"";
	if ( shader.find( setup_include ) >= 0 && shader.find( fragment_include ) >= 0 ) {
		return stripComments( shader );
	}

	// Legacy custom overrides do not contain Terrain3D's include hooks. Keep
	// them compatible by inserting the standard include modules at the same
	// vertex/fragment boundaries used by the built-in shaders.
	shader = stripComments( shader );
	Ref<RegEx> pRegex;
	pRegex.instantiate();
	Ref<RegExMatch> pMatch;
	int idx = -1;

	pRegex->compile( "void\\s+vertex\\s*\\(" );
	pMatch = pRegex->search( shader );
	idx = pMatch.is_valid() ? pMatch->get_start() - 1 : -1;
	if ( idx < 0 ) {
		LOG( DEBUG, "No void vertex(); cannot inject editor code" );
		return shader;
	}
	shader = shader.insert( idx, setup_include + String( "\n" ) );

	// Locate the end of fragment() only for legacy custom shaders. The built-in
	// shader never takes this path, so inactive uber branches cannot confuse it.
	pRegex->compile( "void\\s*fragment\\s*\\(\\s*\\)\\s*{" );
	pMatch = pRegex->search( shader );
	idx = -1;
	if ( pMatch.is_valid() ) {
		const int start_idx = pMatch->get_end() - 1;
		int pair = 0;
		for ( int i = start_idx; i < shader.length(); i++ ) {
			if ( shader[i] == '{' ) {
				pair++;
			} else if ( shader[i] == '}' ) {
				pair--;
			}
			if ( pair == 0 ) {
				idx = i;
				break;
			}
		}
	}
	if ( idx < 0 ) {
		LOG( DEBUG, "No ending bracket; cannot inject editor code into custom shader" );
		return shader;
	}
	return shader.insert( idx, String( "\n" ) + fragment_include + String( "\n" ) );
}

void Terrain3DMaterial::ensureWorldNoiseCache( void ) {
	if ( m_runtimeState.m_worldNoiseCache.is_null() ) {
		LOG( INFO, "Generating cached world-background noise texture" );
		Ref<FastNoiseLite> pNoise;
		pNoise.instantiate();
		pNoise->set_noise_type( FastNoiseLite::TYPE_SIMPLEX_SMOOTH );
		pNoise->set_seed( 1337 );
		pNoise->set_frequency( 0.0125f );
		pNoise->set_fractal_type( FastNoiseLite::FRACTAL_FBM );
		pNoise->set_fractal_octaves( 10 );
		pNoise->set_fractal_lacunarity( 2.0f );
		pNoise->set_fractal_gain( 0.5f );
		pNoise->set_fractal_weighted_strength( 0.35f );
		pNoise->set_domain_warp_enabled( true );
		pNoise->set_domain_warp_type( FastNoiseLite::DOMAIN_WARP_SIMPLEX_REDUCED );
		pNoise->set_domain_warp_amplitude( 24.0f );
		pNoise->set_domain_warp_fractal_type( FastNoiseLite::DOMAIN_WARP_FRACTAL_PROGRESSIVE );
		pNoise->set_domain_warp_fractal_octaves( 4 );
		pNoise->set_domain_warp_fractal_lacunarity( 2.0f );
		pNoise->set_domain_warp_fractal_gain( 0.5f );

		m_runtimeState.m_worldNoiseCache.instantiate();
		m_runtimeState.m_worldNoiseCache->set_width( 1024 );
		m_runtimeState.m_worldNoiseCache->set_height( 1024 );
		m_runtimeState.m_worldNoiseCache->set_generate_mipmaps( true );
		m_runtimeState.m_worldNoiseCache->set_seamless( true );
		m_runtimeState.m_worldNoiseCache->set_seamless_blend_skirt( 0.1f );
		m_runtimeState.m_worldNoiseCache->set_normalize( true );
		m_runtimeState.m_worldNoiseCache->set_noise( pNoise );
	}
}

void Terrain3DMaterial::disconnectShaderOverrideSignals( void ) {
	const Callable callback = callable_mp( this, &Terrain3DMaterial::updateShader );
	if ( m_shaderConfig.m_connectedShaderOverride.is_valid()
	        && m_shaderConfig.m_connectedShaderOverride->is_connected( "changed", callback ) ) {
		m_shaderConfig.m_connectedShaderOverride->disconnect( "changed", callback );
	}
	if ( m_shaderConfig.m_connectedBufferShaderOverride.is_valid()
	        && m_shaderConfig.m_connectedBufferShaderOverride->is_connected( "changed", callback ) ) {
		m_shaderConfig.m_connectedBufferShaderOverride->disconnect( "changed", callback );
	}
	m_shaderConfig.m_connectedShaderOverride.unref();
	m_shaderConfig.m_connectedBufferShaderOverride.unref();
}

void Terrain3DMaterial::connectShaderOverrideSignals( void ) {
	const Callable callback = callable_mp( this, &Terrain3DMaterial::updateShader );
	if ( m_authoringState.m_bShaderOverrideEnabled && m_authoringState.m_shaderOverride.is_valid() ) {
		if ( !m_authoringState.m_shaderOverride->is_connected( "changed", callback ) ) {
			m_authoringState.m_shaderOverride->connect( "changed", callback );
		}
		m_shaderConfig.m_connectedShaderOverride = m_authoringState.m_shaderOverride;
	}
	if ( m_authoringState.m_bBufferShaderOverrideEnabled && m_authoringState.m_bufferShaderOverride.is_valid() && m_pTerrain
	        && m_pTerrain->getTessellationLevel() > 0 ) {
		if ( !m_authoringState.m_bufferShaderOverride->is_connected( "changed", callback ) ) {
			m_authoringState.m_bufferShaderOverride->connect( "changed", callback );
		}
		m_shaderConfig.m_connectedBufferShaderOverride = m_authoringState.m_bufferShaderOverride;
	}
}

void Terrain3DMaterial::setRuntimeMaterialParam( const StringName &strName, const Variant &rValue ) {
	const Variant current = m_runtimeState.m_runtimeMaterialParams.get( strName, Variant() );
	if ( current == rValue ) {
		return;
	}
	m_runtimeState.m_runtimeMaterialParams[strName] = rValue;
	m_runtimeState.m_nRdPublicationGeneration =
	        m_runtimeState.m_nRdPublicationGeneration == UINT64_MAX ? 1u : m_runtimeState.m_nRdPublicationGeneration + 1u;
}

void Terrain3DMaterial::updateShader( void ) {
	IS_INIT( VOID );
	Time *const pTime = Time::get_singleton();
	const uint64_t update_started_usec = pTime ? pTime->get_ticks_usec() : 0u;
	disconnectShaderOverrideSignals();
	const bool bRuntimeShaderExecution = m_pTerrain && !m_pTerrain->isEditorAuthoringMode();
	const bool bEditorShaderFeatures = IS_EDITOR && !bRuntimeShaderExecution;
	if ( bEditorShaderFeatures && !m_authoringState.m_bShaderOverrideEnabled ) {
		// Built-in shader templates are external authoring assets. Refresh their
		// flattened dependency graph on editor recompiles so root/include edits do
		// not remain hidden behind a stale raw-text cache.
		preloadShaders();
	}
	LOG( INFO, "Updating shader" );
	const t3dShaderVariantState_t variant_state = captureVariantState();
	const bool strict_cooked_runtime =
	        bRuntimeShaderExecution && m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_authoringState.m_bShaderOverrideEnabled;
	if ( strict_cooked_runtime
	        && ( !m_shaderConfig.m_bRuntimeShaderPackLoaded
	                || m_shaderConfig.m_nRuntimeShaderPackStateSignature != getVariantStateSignature( variant_state ) ) ) {
		if ( !loadRuntimeShaderPack() ) {
			LOG( ERROR,
			        "Runtime Profile could not load a valid terrain3d_shaders.t3dshaderpack for the current material state; "
			        "retaining the last valid terrain shader where possible." );
		}
	}
	String code;
	t3dShaderValidationResult_t near_validation;
	const Ref<Shader> previous_near_shader = m_shaderConfig.m_shader;
	const bool previous_near_ready =
	        m_shaderConfig.m_bNearShaderReady && previous_near_shader.is_valid() && previous_near_shader->get_rid().is_valid();
	Ref<Shader> candidate_near_shader;

	// Build and validate a candidate before replacing the authoring shader state.
	// A failed compile retains the last valid shader resource.
	if ( m_authoringState.m_bShaderOverrideEnabled && m_authoringState.m_shaderOverride.is_valid() ) {
		const bool seeded_from_builtin = m_authoringState.m_shaderOverride->get_code().is_empty();
		if ( seeded_from_builtin ) {
			++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
			submitShaderSource( m_authoringState.m_shaderOverride, generateShaderCode( variant_state, SHADER_TIER_NEAR, false ) );
		}
		code = m_authoringState.m_shaderOverride->get_code();
		candidate_near_shader.instantiate();
		++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
		const String candidate_code = seeded_from_builtin ? generateShaderCode( variant_state, SHADER_TIER_NEAR, bEditorShaderFeatures )
		                                                  : ( bEditorShaderFeatures ? injectEditorCode( code ) : code );
		submitShaderSource( candidate_near_shader, candidate_code );
		near_validation = validateShaderVariant( candidate_near_shader, SHADER_TIER_NEAR );
	} else {
		code = generateShaderCode( variant_state, SHADER_TIER_NEAR, bEditorShaderFeatures );
#ifdef DEBUG_ENABLED
		if ( m_shaderConfig.m_nShaderRegressionInvalidTier == SHADER_TIER_NEAR ) {
			code += "\nvoid patch_a22_forced_invalid_near_shader() { float x = PATCH_A22_UNDEFINED_IDENTIFIER; }\n";
		}
#endif
		const t3dShaderVariantDomain_t domain = bRuntimeShaderExecution ? SHADER_DOMAIN_RUNTIME : SHADER_DOMAIN_EDITOR;
		if ( code.is_empty() ) {
			near_validation.m_failureSummary =
			        "Runtime terrain shader source is unavailable; the cooked shader pack is missing or incompatible";
		} else {
			candidate_near_shader = getOrCreateShaderVariant( code, domain, false, SHADER_TIER_NEAR, &near_validation );
		}
	}

	if ( candidate_near_shader.is_valid() && near_validation.m_bValid ) {
		m_shaderConfig.m_shader = candidate_near_shader;
		m_shaderConfig.m_bNearShaderReady = true;
	} else if ( previous_near_ready ) {
		m_shaderConfig.m_shader = previous_near_shader;
		m_shaderConfig.m_bNearShaderReady = true;
		LOG( ERROR,
		        "Terrain Near shader candidate failed validation; retaining the last valid shader. ",
		        near_validation.m_failureSummary );
	} else {
		m_shaderConfig.m_shader.unref();
		m_shaderConfig.m_bNearShaderReady = false;
		LOG( ERROR,
		        "Terrain Near shader candidate failed validation and no previously valid shader exists. ",
		        near_validation.m_failureSummary );
	}

	m_shaderConfig.m_szTierShaders[SHADER_TIER_NEAR] = m_shaderConfig.m_shader;
	recordShaderTierValidation( SHADER_TIER_NEAR,
	        code,
	        near_validation,
	        m_shaderConfig.m_bNearShaderReady ? getShaderRid().get_id() : 0u,
	        previous_near_ready && !near_validation.m_bValid,
	        false );
	LOG( DEBUG, "Near shader rid: ", getShaderRid() );

	if ( !m_shaderConfig.m_bNearShaderReady ) {
		// Initialization will stop before terrain meshes are created. Avoid querying
		// shader metadata or publishing tier materials from an invalid candidate.
		connectShaderOverrideSignals();
		const uint64_t update_finished_usec = pTime ? pTime->get_ticks_usec() : update_started_usec;
		const uint64_t update_elapsed_usec = update_finished_usec - update_started_usec;
		m_shaderConfig.m_nShaderUpdateUsecLast = update_elapsed_usec;
		m_shaderConfig.m_nShaderUpdateUsecTotal += update_elapsed_usec;
		m_shaderConfig.m_nShaderUpdateUsecPeak = MAX( m_shaderConfig.m_nShaderUpdateUsecPeak, update_elapsed_usec );
		++m_shaderConfig.m_nShaderUpdateCount;
		return;
	}

	const bool bTieringActive = !m_authoringState.m_bShaderOverrideEnabled && m_pTerrain->isRuntimeShaderTieringActive();
	for ( int tier_value = SHADER_TIER_MIDDLE; tier_value < SHADER_TIER_COUNT; ++tier_value ) {
		const t3dShaderTier_t tier = ( t3dShaderTier_t )( tier_value );
		if ( bTieringActive ) {
			// Compact tiers intentionally omit the Near shader's editor/debug contract.
			// Runtime Profile is semantically runtime even though Engine::is_editor_hint()
			// remains true, so compact tiers always consume the runtime source/domain.
			String tier_code = finalizeRuntimeShaderCode( generateShaderCode( variant_state, tier, false ) );
#ifdef DEBUG_ENABLED
			if ( m_shaderConfig.m_nShaderRegressionInvalidTier == tier_value ) {
				tier_code += "\nvoid patch01_forced_invalid_compact_shader() { float x = PATCH01_UNDEFINED_IDENTIFIER; }\n";
			}
#endif
			t3dShaderValidationResult_t validation;
			Ref<Shader> pCompactShader;
			if ( tier_code.is_empty() ) {
				validation.m_failureSummary =
				        "Runtime compact shader source is unavailable; the cooked shader pack is missing or incompatible";
			} else if ( m_authoringState.m_shaderVariantMode == SHADER_VARIANTS_DISABLED ) {
				// Compile into a detached candidate. Mutating the currently bound tier
				// Shader before validation can expose a failed vertex stage to the render thread.
				pCompactShader.instantiate();
				++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
				submitShaderSource( pCompactShader, tier_code );
				validation = validateShaderVariant( pCompactShader, tier );
				if ( !validation.m_bValid ) {
					++m_shaderConfig.m_nVariantValidationFailuresTotal;
					pCompactShader.unref();
				}
			} else {
				const t3dShaderVariantDomain_t tier_domain = SHADER_DOMAIN_RUNTIME;
				pCompactShader = getOrCreateShaderVariant( tier_code, tier_domain, false, tier, &validation );
			}

			if ( pCompactShader.is_valid() && validation.m_bValid ) {
				m_shaderConfig.m_szTierShaders[tier] = pCompactShader;
				recordShaderTierValidation( tier, tier_code, validation, pCompactShader->get_rid().get_id(), false, true );
			} else {
				if ( validation.m_failureSummary.is_empty() ) {
					validation.m_failureSummary = "Compact shader variant was unavailable before validation";
				}
				m_shaderConfig.m_szTierShaders[tier] = m_shaderConfig.m_shader;
				recordShaderTierValidation( tier, tier_code, validation, getShaderRid().get_id(), true, true );
				LOG( ERROR,
				        "Compact terrain shader tier ",
				        tier_value,
				        " failed validation; using Near fallback. ",
				        validation.m_failureSummary );
			}
		} else {
			m_shaderConfig.m_szTierShaders[tier] = m_shaderConfig.m_shader;
			recordShaderTierValidation( tier, code, near_validation, getShaderRid().get_id(), false, false );
		}
	}

	// Displacement Buffer
	if ( m_pTerrain->getTessellationLevel() > 0 ) {
		if ( m_authoringState.m_bBufferShaderOverrideEnabled && m_authoringState.m_bufferShaderOverride.is_valid() ) {
			if ( m_authoringState.m_bufferShaderOverride->get_code().is_empty() ) {
				++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
				submitShaderSource( m_authoringState.m_bufferShaderOverride, generateBufferShaderCode( variant_state ) );
			}
			code = m_authoringState.m_bufferShaderOverride->get_code();
			m_shaderConfig.m_bufferShader.instantiate();
			++m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
			submitShaderSource( m_shaderConfig.m_bufferShader, code );
		} else {
			code = generateBufferShaderCode( variant_state );
			if ( code.is_empty() ) {
				LOG( ERROR, "Runtime displacement shader source is unavailable; the cooked shader pack is missing or incompatible." );
			} else {
				const Ref<Shader> buffer_shader = getOrCreateShaderVariant( code, SHADER_DOMAIN_BUFFER );
				if ( buffer_shader.is_valid() && buffer_shader->get_rid().is_valid() ) {
					m_shaderConfig.m_bufferShader = buffer_shader;
				} else {
					LOG( ERROR, "Displacement buffer shader candidate was invalid; retaining the last valid shader." );
				}
			}
		}
		LOG( DEBUG, "Buffer shader rid: ", getBufferShaderRid() );
	}

	connectShaderOverrideSignals();
	recordVariantTransition( variant_state );
	prewarmShaderVariants();

	// Rebuild the inspector-facing shader metadata once per shader change. Godot
	// can ask for property values many times per second while the inspector is
	// idle; those reads must not query renderer shader metadata again.
	refreshShaderPropertyCache();
	LOG( EXTREME, "_active_params: ", m_runtimeState.m_activeParams );
	Terrain3DUtil::printDict( "_shader_params", m_authoringState.m_shaderParams, EXTREME );

	// Preserve resource objects in runtime authoring state. The RD coordinator
	// resolves the direct texture resources it owns when publishing snapshots;
	// native texture RIDs are not a material-publication channel.
	for ( size_t iParam = 0u; iParam < ( size_t )( ( m_runtimeState.m_activeParams ).size() ); iParam++ ) {
		const StringName param = ( m_runtimeState.m_activeParams )[iParam];
		const Variant value = m_authoringState.m_shaderParams[param];
		if ( value.get_type() == Variant::OBJECT ) {
			const Ref<Texture> pTex = value;
			setRuntimeMaterialParam( param, pTex.is_valid() ? Variant( pTex ) : Variant() );
		} else {
			setRuntimeMaterialParam( param, value );
		}
	}

	// Control-map rotation is encoded to 16 fixed angles. Upload the exact
	// sine/cosine pairs once per shader rebuild instead of evaluating two
	// transcendental functions for every blended material corner.
	PackedVector2Array control_rotations;
	control_rotations.resize( 16 );
	for ( int i = 0; i < 16; i++ ) {
		const real_t fAngle = ( real_t )( i ) * -0.392699081698724;
		control_rotations.set( i, Vector2( Math::cos( fAngle ), Math::sin( fAngle ) ) );
	}
	setRuntimeMaterialParam( "_control_rotation_cs", control_rotations );

	// Sixteen fixed detiling rotations back the 4/8/16-way modes. Keep this as a
	// uniform table rather than a local shader array: older/compatibility Godot
	// shader parsers are inconsistent about local constant-array constructors.
	PackedVector2Array detile_rotations;
	detile_rotations.resize( 16 );
	for ( int i = 0; i < 16; i++ ) {
		const real_t fAngle = ( real_t )( i )*Math_TAU / 16.0;
		detile_rotations.set( i, Vector2( Math::cos( fAngle ), Math::sin( fAngle ) ) );
	}
	setRuntimeMaterialParam( "_detile_rotation_cs", detile_rotations );

	// Precompute uniform-only rotations on the CPU. This avoids evaluating sin/cos
	// for every terrain fragment while preserving the public angle parameter.
	if ( m_runtimeState.m_activeParams.has( "noise1_angle" ) ) {
		const Variant angle_value = m_authoringState.m_shaderParams.get( StringName( "noise1_angle" ),
		        m_authoringState.m_shaderParamDefaults.get( StringName( "noise1_angle" ), ( real_t )( 0.0f ) ) );
		const real_t fAngle = angle_value;
		const Vector2 rotation( Math::cos( fAngle ), Math::sin( fAngle ) );
		setRuntimeMaterialParam( "_noise1_rotation", rotation );
	}

	// Set specific shader parameters
	setRuntimeMaterialParam( "_background_mode", m_authoringState.m_worldBackground );
	if ( m_authoringState.m_worldBackground == NOISE
	        && ( m_authoringState.m_worldNoiseShaderMode == WORLD_NOISE_DYNAMIC
	                || m_authoringState.m_worldNoiseShaderMode == WORLD_NOISE_CACHED_VERTEX_NORMALS
	                || m_authoringState.m_worldNoiseShaderMode == WORLD_NOISE_CACHED_FRAGMENT_NORMALS ) ) {
		ensureWorldNoiseCache();
	}

	// If no noise texture, generate one
	if ( m_runtimeState.m_activeParams.has( "noise_texture" )
	        && m_authoringState.m_shaderParams.get( StringName( "noise_texture" ), Variant() ).get_type() == Variant::NIL ) {
		LOG( INFO, "Generating default noise_texture for shader" );
		Ref<FastNoiseLite> pFnoise;
		pFnoise.instantiate();
		pFnoise->set_noise_type( FastNoiseLite::TYPE_CELLULAR );
		pFnoise->set_frequency( 0.03f );
		pFnoise->set_cellular_jitter( 3.0f );
		pFnoise->set_cellular_return_type( FastNoiseLite::RETURN_CELL_VALUE );
		pFnoise->set_domain_warp_enabled( true );
		pFnoise->set_domain_warp_type( FastNoiseLite::DOMAIN_WARP_SIMPLEX_REDUCED );
		pFnoise->set_domain_warp_amplitude( 50.f );
		pFnoise->set_domain_warp_fractal_type( FastNoiseLite::DOMAIN_WARP_FRACTAL_INDEPENDENT );
		pFnoise->set_domain_warp_fractal_lacunarity( 1.5f );
		pFnoise->set_domain_warp_fractal_gain( 1.f );

		Ref<Gradient> pCurve;
		pCurve.instantiate();
		PackedFloat32Array pfa;
		pfa.push_back( 0.2f );
		pfa.push_back( 1.0f );
		pCurve->set_offsets( pfa );
		PackedColorArray pca;
		pca.push_back( Color( 1.f, 1.f, 1.f, 1.f ) );
		pca.push_back( Color( 0.f, 0.f, 0.f, 1.f ) );
		pCurve->set_colors( pca );

		Ref<NoiseTexture2D> pNoiseTex;
		pNoiseTex.instantiate();
		pNoiseTex->set_seamless( true );
		pNoiseTex->set_generate_mipmaps( true );
		pNoiseTex->set_noise( pFnoise );
		pNoiseTex->set_color_ramp( pCurve );
		_set( "noise_texture", pNoiseTex );
	}

	notify_property_list_changed();
	const uint64_t update_finished_usec = pTime ? pTime->get_ticks_usec() : update_started_usec;
	const uint64_t update_elapsed_usec = update_finished_usec - update_started_usec;
	m_shaderConfig.m_nShaderUpdateUsecLast = update_elapsed_usec;
	m_shaderConfig.m_nShaderUpdateUsecTotal += update_elapsed_usec;
	m_shaderConfig.m_nShaderUpdateUsecPeak = MAX( m_shaderConfig.m_nShaderUpdateUsecPeak, update_elapsed_usec );
	++m_shaderConfig.m_nShaderUpdateCount;
}

void Terrain3DMaterial::invalidateRegionPageTables( void ) {
	m_vtBinding.m_generatedRegionMap.clear();
	m_vtBinding.m_nRegionMapTextureHash = 0u;
	m_vtBinding.m_nRegionMapTextureSize = 0;
	m_vtBinding.m_generatedLogicalRegionMap.clear();
	m_vtBinding.m_nLogicalRegionMapTextureHash = 0u;
	m_vtBinding.m_nLogicalRegionMapTextureSize = 0;
	m_vtBinding.m_nRegionPageTableOwnerGeneration = 0u;
}

bool Terrain3DMaterial::updateRegionMapTexture( const PackedInt32Array &rRegionMap, const int nRegionMapSize ) {
	if ( nRegionMapSize <= 0 || nRegionMapSize > s_nRegionMapSize || rRegionMap.size() != nRegionMapSize * nRegionMapSize ) {
		return false;
	}
	uint64_t hash = 1469598103934665603ULL;
	t3dMaterialMixShaderSignature( hash, ( uint64_t )( nRegionMapSize ) );
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		t3dMaterialMixShaderSignature( hash, ( uint64_t )( ( uint32_t )( rRegionMap[index] ) ) );
	}
	if ( m_vtBinding.m_generatedRegionMap.getRid().is_valid() && m_vtBinding.m_nRegionMapTextureHash == hash
	        && m_vtBinding.m_nRegionMapTextureSize == nRegionMapSize ) {
		return true;
	}

	PackedByteArray bytes;
	bytes.resize( rRegionMap.size() * ( int )( sizeof( uint16_t ) ) );
	uint8_t *const pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		const uint16_t value = ( uint16_t )( CLAMP( rRegionMap[index], 0, ( int )( UINT16_MAX ) ) );
		memcpy( pWrite + ( size_t )( index ) * sizeof( uint16_t ), &value, sizeof( uint16_t ) );
	}
	const Ref<Image> pImage = Image::create_from_data( nRegionMapSize, nRegionMapSize, false, Image::FORMAT_R16, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bSameSizeTexture =
	        m_vtBinding.m_generatedRegionMap.getRid().is_valid() && m_vtBinding.m_nRegionMapTextureSize == nRegionMapSize;
	bool bUploaded = bSameSizeTexture ? m_vtBinding.m_generatedRegionMap.update2d( pImage )
	                                  : m_vtBinding.m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !bUploaded && bSameSizeTexture ) {
		// A World3D/owner transition can leave a renderer-side RID unusable even
		// though the cached wrapper still describes the same dimensions. Discard
		// that identity and create a fresh page table instead of treating it as
		// authoritative forever.
		m_vtBinding.m_generatedRegionMap.clear();
		bUploaded = m_vtBinding.m_generatedRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	}
	if ( !bUploaded ) {
		m_vtBinding.m_nRegionMapTextureHash = 0u;
		m_vtBinding.m_nRegionMapTextureSize = 0;
		return false;
	}
	m_vtBinding.m_nRegionMapTextureHash = hash;
	m_vtBinding.m_nRegionMapTextureSize = nRegionMapSize;
	return true;
}

bool Terrain3DMaterial::updateLogicalRegionMapTexture( const PackedInt32Array &rRegionMap, const int nRegionMapSize ) {
	if ( nRegionMapSize <= 0 || nRegionMapSize > s_nRegionMapSize || rRegionMap.size() != nRegionMapSize * nRegionMapSize ) {
		return false;
	}
	uint64_t hash = 1469598103934665603ULL;
	t3dMaterialMixShaderSignature( hash, ( uint64_t )( nRegionMapSize ) );
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		t3dMaterialMixShaderSignature( hash, ( uint64_t )( ( uint32_t )( rRegionMap[index] ) ) );
	}
	if ( m_vtBinding.m_generatedLogicalRegionMap.getRid().is_valid() && m_vtBinding.m_nLogicalRegionMapTextureHash == hash
	        && m_vtBinding.m_nLogicalRegionMapTextureSize == nRegionMapSize ) {
		return true;
	}
	PackedByteArray bytes;
	bytes.resize( rRegionMap.size() );
	uint8_t *pWrite = bytes.ptrw();
	if ( !pWrite ) {
		return false;
	}
	for ( int index = 0; index < rRegionMap.size(); ++index ) {
		pWrite[index] = rRegionMap[index] != 0 ? 255u : 0u;
	}
	const Ref<Image> pImage = Image::create_from_data( nRegionMapSize, nRegionMapSize, false, Image::FORMAT_R8, bytes );
	if ( pImage.is_null() || pImage->is_empty() ) {
		return false;
	}
	const bool bSameSizeTexture =
	        m_vtBinding.m_generatedLogicalRegionMap.getRid().is_valid() && m_vtBinding.m_nLogicalRegionMapTextureSize == nRegionMapSize;
	bool bUploaded = bSameSizeTexture ? m_vtBinding.m_generatedLogicalRegionMap.update2d( pImage )
	                                  : m_vtBinding.m_generatedLogicalRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	if ( !bUploaded && bSameSizeTexture ) {
		m_vtBinding.m_generatedLogicalRegionMap.clear();
		bUploaded = m_vtBinding.m_generatedLogicalRegionMap.create( pImage, T3D_RD_TEXTURE_LINEAR_DATA ).is_valid();
	}
	if ( !bUploaded ) {
		m_vtBinding.m_nLogicalRegionMapTextureHash = 0u;
		m_vtBinding.m_nLogicalRegionMapTextureSize = 0;
		return false;
	}
	m_vtBinding.m_nLogicalRegionMapTextureHash = hash;
	m_vtBinding.m_nLogicalRegionMapTextureSize = nRegionMapSize;
	return true;
}

t3dPageTableBuildResult_t Terrain3DMaterial::prepareRegionPageTables( void ) {
	if ( !m_pTerrain || !m_pTerrain->getData() ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	Terrain3DData *const pData = m_pTerrain->getData();
	if ( !pData->isRuntimeReady() ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	const uint32_t owner_generation = pData->getActiveStorageGeneration();
	if ( owner_generation == 0u ) {
		return PAGE_TABLE_INVALID_TERRAIN_STATE;
	}
	if ( m_vtBinding.m_nRegionPageTableOwnerGeneration != owner_generation ) {
		invalidateRegionPageTables();
	}

	const PackedInt32Array region_map = pData->getRegionMap();
	const int region_map_size = pData->getRegionMapSizeInternal();
	const PackedInt32Array logical_region_map = pData->getLogicalRegionMap();
	if ( region_map_size <= 0 || region_map_size > s_nRegionMapSize || region_map.size() != region_map_size * region_map_size
	        || logical_region_map.size() != region_map_size * region_map_size ) {
		return PAGE_TABLE_INVALID_DIMENSIONS;
	}
	if ( !updateRegionMapTexture( region_map, region_map_size ) ) {
		return PAGE_TABLE_TEXTURE_ALLOCATION_FAILED;
	}
	if ( !updateLogicalRegionMapTexture( logical_region_map, region_map_size ) ) {
		return PAGE_TABLE_TEXTURE_ALLOCATION_FAILED;
	}
	m_vtBinding.m_nRegionPageTableOwnerGeneration = owner_generation;
	return PAGE_TABLE_READY;
}

void Terrain3DMaterial::setShaderParameters( const Dictionary &rDict ) {
	SET_IF_DIFF( m_authoringState.m_shaderParams, rDict );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting shader params dictionary: ", rDict.size() );
}

// -----------------------------------------------------------------------------
// Public functions

// This function serves as the constructor which is initialized by the class Terrain3D.
// Godot likes to create resource objects at startup, so this prevents it from creating
// uninitialized materials.
void Terrain3DMaterial::initialize( Terrain3D *pTerrain ) {
	if ( pTerrain ) {
		Terrain3D *previous_owner = t3dTransferOwner( m_pTerrain, pTerrain );
		if ( previous_owner != pTerrain ) {
			invalidateRegionPageTables();
		}
		if ( previous_owner && previous_owner != pTerrain ) {
			LOG( INFO, "Transferring material runtime ownership from ", previous_owner, " to ", pTerrain );
		}
	} else {
		LOG( ERROR, "Initialization failed, p_terrain is null" );
		return;
	}
	LOG( INFO, "Initializing material" );
	const bool strict_cooked_builtin = m_pTerrain->isGpuRegionCookedPagesEnabled() && !m_pTerrain->isEditorAuthoringMode()
	        && !m_authoringState.m_bShaderOverrideEnabled;
	if ( strict_cooked_builtin ) {
		if ( !loadRuntimeShaderPack() ) {
			ERR_PRINT( "Terrain3D strict cooked runtime requires a valid terrain3d_shaders.t3dshaderpack." );
		}
	} else {
		preloadShaders();
	}

	if ( m_shaderConfig.m_shader.is_null() ) {
		m_shaderConfig.m_shader.instantiate();
	}
	if ( m_shaderConfig.m_bufferShader.is_null() ) {
		m_shaderConfig.m_bufferShader.instantiate();
	}
	{ // Create dummy texture arrays to avoid empty sampler2DArrays
		const Ref<Image> pImg = Image::create( 1, 1, false, Image::FORMAT_RF );
		const TypedArray<Image> ia = { pImg };
		m_runtimeState.m_generatedDummy.create( ia, T3D_RD_TEXTURE_LINEAR_DATA );

		// Far atlases are sampler2D, not sampler2DArray. Binding the layered
		// dummy to them causes GL_INVALID_OPERATION on the compatibility renderer
		// and repeated driver validation work during material updates.
		const uint16_t encoded_zero_height = t3dRegionCodecEncodeHeightU16( 0.0f );
		PackedByteArray far_height_bytes;
		far_height_bytes.resize( 2 );
		memcpy( far_height_bytes.ptrw(), &encoded_zero_height, sizeof( encoded_zero_height ) );
		const Ref<Image> pFarHeightImg = Image::create_from_data( 1, 1, false, Image::FORMAT_R16, far_height_bytes );
		m_runtimeState.m_generatedFarHeightDummy.create( pFarHeightImg, T3D_RD_TEXTURE_LINEAR_DATA );
		Ref<Image> pFarColorImg = Image::create( 1, 1, false, Image::FORMAT_RGBA8 );
		// Alpha is packed far-surface metadata. A fallback texture must describe
		// empty terrain; 0.5 rounds to bit 7 and therefore means solid terrain.
		pFarColorImg->fill( Color( 1.0, 1.0, 1.0, 0.0 ) );
		m_runtimeState.m_generatedFarColorDummy.create( pFarColorImg, T3D_RD_TEXTURE_SOURCE_COLOR );

		const Ref<Image> pSurfaceImg = Terrain3DUtil::getDefaultSurfaceImage( V2I( 1 ), false );
		const TypedArray<Image> surface_array = { pSurfaceImg };
		m_runtimeState.m_generatedSurfaceDummy.create( surface_array, T3D_RD_TEXTURE_LINEAR_DATA );
		const Ref<Image> pDisplacementImg = Terrain3DUtil::getDefaultDisplacementImage( V2I( 1 ), false );
		const TypedArray<Image> displacement_array = { pDisplacementImg };
		m_runtimeState.m_generatedDisplacementDummy.create( displacement_array, T3D_RD_TEXTURE_LINEAR_DATA );
	}

	update( FULL_REBUILD );
	m_authoringState.m_bPersistenceDirty = false;
}

void Terrain3DMaterial::uninitialize( Terrain3D *pTerrain ) {
	if ( !t3dReleaseOwner( m_pTerrain, pTerrain ) ) {
		LOG( DEBUG, "Ignoring stale material uninitialize request from ", pTerrain, "; current owner: ", m_pTerrain );
		return;
	}
	LOG( INFO, "Uninitializing material" );
	disconnectShaderOverrideSignals();
	invalidateRegionPageTables();
	t3dVectorRelease( m_authoringState.m_shaderPropertyCache );
}

void Terrain3DMaterial::destroy( void ) {
	LOG( INFO, "Destroying material" );
	disconnectShaderOverrideSignals();
	m_pTerrain = NULL;
	m_shaderConfig.m_shader.unref();
	m_shaderConfig.m_bNearShaderReady = false;
	m_shaderConfig.m_bufferShader.unref();
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		m_shaderConfig.m_szTierShaders[tier].unref();
	}
	m_shaderConfig.m_shadowShader.unref();
	m_shaderConfig.m_shaderTemplates.clear();
	m_shaderConfig.m_bRuntimeShaderPackLoaded = false;
	m_shaderConfig.m_nRuntimeShaderPackStateSignature = 0u;
	m_shaderConfig.m_runtimeShaderPackBufferSource = String();
	for ( size_t iTier = 0u; iTier < T3D_ARRAY_COUNT( m_shaderConfig.m_szRuntimeShaderPackTierSource ); iTier++ ) {
		String &source = ( m_shaderConfig.m_szRuntimeShaderPackTierSource )[iTier];
		source = String();
	}
	m_shaderConfig.m_runtimeShaderVariantEntries.clear();
	m_shaderConfig.m_nRuntimeShaderVariantSourceBytes = 0;
	m_shaderConfig.m_editorShaderVariantEntries.clear();
	m_shaderConfig.m_nEditorShaderVariantSourceBytes = 0;
	m_shaderConfig.m_bufferShaderVariantEntries.clear();
	m_shaderConfig.m_nBufferShaderVariantSourceBytes = 0;
	m_shaderConfig.m_bHasLastBoundVariantState = false;
	m_shaderConfig.m_nObservedVariantDimensions = 0;
	m_shaderConfig.m_nVariantTransitionClock = 0;
	for ( size_t iDimension = 0u; iDimension < T3D_ARRAY_COUNT( m_shaderConfig.m_szVariantDimensionLastUsed ); iDimension++ ) {
		uint64_t &last_used = ( m_shaderConfig.m_szVariantDimensionLastUsed )[iDimension];
		last_used = 0;
	}
	m_runtimeState.m_activeParams.clear();
	m_authoringState.m_shaderParams.clear();
	t3dVectorRelease( m_authoringState.m_shaderPropertyCache );
	m_authoringState.m_shaderParamDefaults.clear();
	m_runtimeState.m_runtimeMaterialParams.clear();
	m_runtimeState.m_generatedDummy.clear();
	m_runtimeState.m_generatedFarHeightDummy.clear();
	m_runtimeState.m_generatedFarColorDummy.clear();
	m_runtimeState.m_generatedSurfaceDummy.clear();
	m_runtimeState.m_generatedDisplacementDummy.clear();
	invalidateRegionPageTables();
	m_runtimeState.m_worldNoiseCache.unref();
}

void Terrain3DMaterial::update( const uint32_t nFlags ) {
	m_runtimeState.m_nRdPublicationGeneration =
	        m_runtimeState.m_nRdPublicationGeneration == UINT64_MAX ? 1u : m_runtimeState.m_nRdPublicationGeneration + 1u;
	bool bRebuildShader = ( nFlags & FULL_REBUILD ) != 0u;
	if ( !bRebuildShader && ( nFlags & REGION_ARRAYS ) != 0u && m_shaderConfig.m_bHasLastBoundVariantState ) {
		const t3dShaderVariantState_t current_state = captureVariantState();
		bRebuildShader = current_state.m_bControlMapsImplicit != m_shaderConfig.m_lastBoundVariantState.m_bControlMapsImplicit
		        || current_state.m_bColorMapsImplicit != m_shaderConfig.m_lastBoundVariantState.m_bColorMapsImplicit;
	}
	if ( bRebuildShader ) {
		updateShader();
	}
	if ( nFlags & REGION_ARRAYS ) {
		const t3dPageTableBuildResult_t page_table_result = prepareRegionPageTables();
		if ( page_table_result != PAGE_TABLE_READY ) {
			if ( page_table_result == PAGE_TABLE_INVALID_TERRAIN_STATE && m_pTerrain && m_pTerrain->getData() ) {
				LOG( ERROR,
				        "Cannot build terrain page tables while data runtime storage is inactive: ",
				        m_pTerrain->getData()->getRuntimeState() );
			} else if ( page_table_result == PAGE_TABLE_INVALID_DIMENSIONS ) {
				LOG( ERROR, "Cannot build terrain page tables: invalid region-map dimensions" );
			} else if ( page_table_result == PAGE_TABLE_TEXTURE_ALLOCATION_FAILED ) {
				LOG( ERROR,
				        "Cannot build terrain page tables: generated RenderingDevice texture allocation/update "
				        "failed" );
			}
			return;
		}
	}
	IS_INIT( VOID );
	if ( m_pTerrain->getTessellationLevel() > 0 ) {
		m_pTerrain->snap();
	}
}

void Terrain3DMaterial::updateEditorOverlay( void ) {
	if ( !m_pTerrain || !m_pTerrain->getData() || !m_pTerrain->getData()->isRuntimeReady() ) {
		return;
	}
	// Region/navigation editor modes alter shader defines, but showing the toolbar
	// must not force terrain arrays or page tables through a lifecycle transition.
	update( FULL_REBUILD & ~UPDATE_ARRAYS );
}

void Terrain3DMaterial::updateRegionPageTable( void ) {
	IS_DATA_INIT( VOID );
	const t3dPageTableBuildResult_t page_table_result = prepareRegionPageTables();
	if ( page_table_result != PAGE_TABLE_READY ) {
		if ( page_table_result == PAGE_TABLE_INVALID_TERRAIN_STATE ) {
			LOG( DEBUG, "Skipping region page-table publication while terrain runtime storage is inactive" );
		} else if ( page_table_result == PAGE_TABLE_INVALID_DIMENSIONS ) {
			LOG( ERROR, "Cannot publish terrain page tables: invalid region-map dimensions" );
		} else {
			LOG( ERROR, "Cannot publish terrain page tables: texture allocation/update failed" );
		}
	}
}

void Terrain3DMaterial::updateFarAtlasTextures( void ) {
	// Far-atlas RIDs are read directly from Terrain3DData by the RD coordinator.
}

void Terrain3DMaterial::setDisplacementScale( const real_t fDisplacementScale ) {
	SET_IF_DIFF( m_authoringState.m_fDisplacementScale, CLAMP( fDisplacementScale, 0.f, 2.f ) );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting displacement scale: ", fDisplacementScale );
	update();
}

void Terrain3DMaterial::setDisplacementSharpness( const real_t fDisplacementSharpness ) {
	SET_IF_DIFF( m_authoringState.m_fDisplacementSharpness, CLAMP( fDisplacementSharpness, 0.f, 1.f ) );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting displacement sharpness: ", fDisplacementSharpness );
	update();
	if ( m_pTerrain ) {
		m_pTerrain->snap();
	}
}

void Terrain3DMaterial::setWorldBackground( const t3dWorldBackground_t background ) {
	SET_IF_DIFF( m_authoringState.m_worldBackground, background );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable world background: ", background );
	update( FULL_REBUILD );
}

void Terrain3DMaterial::setShaderVariantMode( const t3dShaderVariantMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_shaderVariantMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting shader variant mode: ", mode );
	clearShaderVariantCache();
	update( FULL_REBUILD );
}

void Terrain3DMaterial::setShaderVariantCacheLimit( const int nLimit ) {
	const int limit = CLAMP( nLimit, s_nMinShaderVariantCacheLimit, s_nMaxShaderVariantCacheLimit );
	SET_IF_DIFF( m_shaderConfig.m_nShaderVariantCacheLimit, limit );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting shader variant cache limit: ", limit );
	trimShaderVariantCaches();
	m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
	prewarmShaderVariants();
	emit_changed();
}

void Terrain3DMaterial::setShaderVariantPrewarmLimit( const int nLimit ) {
	const int limit = CLAMP( nLimit, s_nMinShaderVariantPrewarmLimit, s_nMaxShaderVariantPrewarmLimit );
	SET_IF_DIFF( m_shaderConfig.m_nShaderVariantPrewarmLimit, limit );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting shader variant prewarm limit: ", limit );
	m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
	prewarmShaderVariants();
	emit_changed();
}

void Terrain3DMaterial::setShaderVariantSourceBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, s_nMinShaderVariantSourceBudgetMb, s_nMaxShaderVariantSourceBudgetMb );
	SET_IF_DIFF( m_shaderConfig.m_nShaderVariantSourceBudgetMb, budget_mb );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting shader variant source budget: ", budget_mb, " MiB" );
	if ( m_pTerrain == NULL ) {
		setEffectiveShaderVariantSourceBudgetMb( budget_mb );
	}
	m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
	prewarmShaderVariants();
	emit_changed();
}

void Terrain3DMaterial::setEffectiveShaderVariantSourceBudgetMb( const int nBudgetMb ) {
	const int budget_mb = CLAMP( nBudgetMb, s_nMinShaderVariantSourceBudgetMb, s_nMaxShaderVariantSourceBudgetMb );
	if ( m_shaderConfig.m_nShaderVariantEffectiveSourceBudgetMb == budget_mb ) {
		return;
	}
	m_shaderConfig.m_nShaderVariantEffectiveSourceBudgetMb = budget_mb;
	trimShaderVariantCaches();
	m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
	prewarmShaderVariants();
}

void Terrain3DMaterial::setShaderPerformancePreset( const t3dShaderPerformancePreset_t ePreset ) {
	if ( m_authoringState.m_shaderPerformancePreset == ePreset && ePreset != SHADER_PRESET_CUSTOM ) {
		return;
	}

	t3dShaderVariantState_t state = captureVariantState();
	applyPerformancePresetToState( state, ePreset );
	applyVariantState( state );
	applyPerformancePresetToTiers( ePreset );
	m_authoringState.m_shaderPerformancePreset = ePreset;
	LOG( INFO, "Setting shader performance preset: ", ePreset );
	// Changing the preset can replace the Middle/Far shader RIDs. Rebind every
	// sampler and compact-tier uniform in the same transaction so the new shader
	// never renders with default white sampler state or zeroed metadata arrays.
	update( FULL_REBUILD );
	emit_changed();
}

void Terrain3DMaterial::updateTierUniforms( const t3dShaderTier_t tier ) {
	if ( !m_pTerrain || tier < SHADER_TIER_NEAR || tier >= SHADER_TIER_COUNT ) {
		return;
	}
#ifdef DEBUG_ENABLED
	++m_runtimeState.m_szTierUniformPublicationCount[tier];
#endif
}

void Terrain3DMaterial::tierQualityChanged( const t3dShaderTier_t tier, const bool bRecompile ) {
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	if ( bRecompile ) {
		m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
		// Static tier dimensions replace shader RIDs. Rebind through the normal
		// material update transaction so samplers and generated uniforms remain coherent.
		update( FULL_REBUILD );
	} else {
		// P06 caches the direct-RD material image by this generation. Dynamic
		// tier values are part of that image even though they do not require a
		// shader rebuild, so they must invalidate the cached publication too.
		m_runtimeState.m_nRdPublicationGeneration =
		        m_runtimeState.m_nRdPublicationGeneration == UINT64_MAX ? 1u : m_runtimeState.m_nRdPublicationGeneration + 1u;
		updateTierUniforms( tier );
	}
	emit_changed();
}

void Terrain3DMaterial::refreshGpuRegionChannelPolicy( void ) {
	if ( !m_pTerrain || m_pTerrain->isEditorAuthoringMode() ) {
		return;
	}
	Terrain3DData *const pData = m_pTerrain->getData();
	if ( !pData ) {
		return;
	}
	const uint64_t effective_budget_bytes = m_pTerrain->getEffectiveGpuRegionCacheBudgetBytes();
	pData->configureGpuRegionCache( effective_budget_bytes > 0u,
	        effective_budget_bytes,
	        m_pTerrain->getGpuRegionUploadBudgetLayers(),
	        m_pTerrain->getGpuRegionUploadBudgetMb(),
	        m_pTerrain->getGpuRegionSparePages(),
	        m_pTerrain->getGpuRegionRetireFrames() );
}

void Terrain3DMaterial::setNearTextureFiltering( const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearMaterialSamplingQuality( const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTerrainNormalQuality( const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTerrainColorQuality( const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearSurfaceResponseQuality( const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearTextureSurfaceMode( const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearLightingModel( const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearDetilingQuality( const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR );
}

void Terrain3DMaterial::setNearAuthoredColorContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setNearWetnessContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setNearMaterialLodBias( const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_nearQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_NEAR, false );
}

void Terrain3DMaterial::setMiddleTextureFiltering( const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleMaterialSamplingQuality( const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTerrainNormalQuality( const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTerrainColorQuality( const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleSurfaceResponseQuality( const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleTextureSurfaceMode( const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleLightingModel( const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleDetilingQuality( const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE );
}

void Terrain3DMaterial::setMiddleAuthoredColorContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setMiddleWetnessContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	refreshGpuRegionChannelPolicy();
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setMiddleMaterialLodBias( const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_middleQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_MIDDLE, false );
}

void Terrain3DMaterial::setFarTextureFiltering( const t3dTextureFiltering_t filtering ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_textureFiltering, filtering );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarMaterialSamplingQuality( const t3dMaterialSamplingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_materialSamplingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTerrainNormalQuality( const t3dTerrainNormalQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_terrainNormalQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTerrainColorQuality( const t3dTerrainColorQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_terrainColorQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarSurfaceResponseQuality( const t3dSurfaceResponseQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_surfaceResponseQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarTextureSurfaceMode( const t3dTextureSurfaceMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_textureSurfaceMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarLightingModel( const t3dTierLightingModel_t model ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_lightingModel, model );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarDetilingQuality( const t3dTierDetilingQuality_t eQuality ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_detilingQuality, eQuality );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR );
}

void Terrain3DMaterial::setFarAuthoredColorContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fAuthoredColorContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

void Terrain3DMaterial::setFarWetnessContribution( const real_t fContribution ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fWetnessContribution, CLAMP( fContribution, 0.0f, 1.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

void Terrain3DMaterial::setFarMaterialLodBias( const real_t fBias ) {
	SET_IF_DIFF( m_authoringState.m_farQuality.m_fMaterialLodBias, CLAMP( fBias, -2.0f, 4.0f ) );
	m_authoringState.m_bPersistenceDirty = true;
	tierQualityChanged( SHADER_TIER_FAR, false );
}

// Hidden serialized aliases map old enum/value domains onto the canonical rows.
void Terrain3DMaterial::setTextureFiltering( const t3dTextureFiltering_t filtering ) {
	setNearTextureFiltering( filtering );
}

void Terrain3DMaterial::setMiddleTerrainSamplingMode( const t3dTerrainSamplingMode_t mode ) {
	setMiddleMaterialSamplingQuality( mode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST : MATERIAL_SAMPLING_FULL );
}

t3dTerrainSamplingMode_t Terrain3DMaterial::getMiddleTerrainSamplingMode( void ) const {
	return m_authoringState.m_middleQuality.m_materialSamplingQuality == MATERIAL_SAMPLING_FAST ? TERRAIN_SAMPLING_SINGLE_SAMPLE
	                                                                                            : TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED;
}

void Terrain3DMaterial::setMiddleTerrainNormalMode( const t3dTerrainNormalMode_t mode ) {
	setMiddleTerrainNormalQuality(
	        mode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE ? TERRAIN_NORMAL_QUALITY_GEOMETRY : TERRAIN_NORMAL_QUALITY_DETAILED );
}

t3dTerrainNormalMode_t Terrain3DMaterial::getMiddleTerrainNormalMode( void ) const {
	return m_authoringState.m_middleQuality.m_terrainNormalQuality == TERRAIN_NORMAL_QUALITY_GEOMETRY ? TERRAIN_NORMAL_GEOMETRY_DERIVATIVE
	                                                                                                  : TERRAIN_NORMAL_DERIVED_TEXTURE;
}

void Terrain3DMaterial::setMiddleColorSamplingMode( const t3dColorSamplingMode_t mode ) {
	setMiddleTerrainColorQuality( mode == COLOR_SAMPLING_EXACT_4_TAP ? TERRAIN_COLOR_QUALITY_EXACT : TERRAIN_COLOR_QUALITY_FAST );
}

t3dColorSamplingMode_t Terrain3DMaterial::getMiddleColorSamplingMode( void ) const {
	return m_authoringState.m_middleQuality.m_terrainColorQuality == TERRAIN_COLOR_QUALITY_EXACT ? COLOR_SAMPLING_EXACT_4_TAP
	                                                                                             : COLOR_SAMPLING_HARDWARE_FILTERED;
}

void Terrain3DMaterial::setMiddleSurfaceDataMode( const t3dSurfaceDataMode_t mode ) {
	setMiddleSurfaceResponseQuality( mode == SURFACE_DATA_FULL ? SURFACE_RESPONSE_DETAILED : SURFACE_RESPONSE_SIMPLE );
}

t3dSurfaceDataMode_t Terrain3DMaterial::getMiddleSurfaceDataMode( void ) const {
	return m_authoringState.m_middleQuality.m_surfaceResponseQuality == SURFACE_RESPONSE_DETAILED ? SURFACE_DATA_FULL
	                                                                                              : SURFACE_DATA_ALBEDO_ONLY;
}

void Terrain3DMaterial::setMiddleTerrainShadingMode( const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setMiddleLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setMiddleLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setMiddleLightingModel( TIER_LIGHTING_PBR );
	} else {
		setMiddleLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

t3dTerrainShadingMode_t Terrain3DMaterial::getMiddleTerrainShadingMode( void ) const {
	switch ( m_authoringState.m_middleQuality.m_lightingModel ) {
		case TIER_LIGHTING_UNSHADED:
			return SHADING_UNSHADED;
		case TIER_LIGHTING_BLINN_PHONG:
			return SHADING_XBOX_360_BLINN_PHONG;
		case TIER_LIGHTING_PBR:
			return SHADING_BURLEY_GGX;
		default:
			return SHADING_LAMBERT_DIFFUSE_ONLY;
	}
}

void Terrain3DMaterial::setMiddleDetilingMode( const t3dDetilingMode_t mode ) {
	setMiddleDetilingQuality( mode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION );
}

t3dDetilingMode_t Terrain3DMaterial::getMiddleDetilingMode( void ) const {
	return m_authoringState.m_middleQuality.m_detilingQuality == TIER_DETILING_DISABLED ? DETILING_DISABLED : DETILING_DISCRETE_8;
}

void Terrain3DMaterial::setMiddleAutoShaderEnabled( const bool bEnabled ) {
	setAutoShaderEnabled( bEnabled );
}

void Terrain3DMaterial::setMiddleAuthoredColorEnabled( const bool bEnabled ) {
	setMiddleAuthoredColorContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setMiddleWetnessEnabled( const bool bEnabled ) {
	setMiddleWetnessContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setFarTerrainShadingMode( const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setFarLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setFarLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setFarLightingModel( TIER_LIGHTING_PBR );
	} else {
		setFarLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

t3dTerrainShadingMode_t Terrain3DMaterial::getFarTerrainShadingMode( void ) const {
	switch ( m_authoringState.m_farQuality.m_lightingModel ) {
		case TIER_LIGHTING_UNSHADED:
			return SHADING_UNSHADED;
		case TIER_LIGHTING_BLINN_PHONG:
			return SHADING_XBOX_360_BLINN_PHONG;
		case TIER_LIGHTING_PBR:
			return SHADING_BURLEY_GGX;
		default:
			return SHADING_LAMBERT_DIFFUSE_ONLY;
	}
}

void Terrain3DMaterial::setFarMaterialLod( const real_t fLod ) {
	setFarMaterialLodBias( CLAMP( fLod, 2.0f, 8.0f ) - 4.0f );
}

void Terrain3DMaterial::setFarAuthoredColorEnabled( const bool bEnabled ) {
	setFarAuthoredColorContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setFarWetnessEnabled( const bool bEnabled ) {
	setFarWetnessContribution( bEnabled ? 1.0f : 0.0f );
}

void Terrain3DMaterial::setTerrainSamplingMode( const t3dTerrainSamplingMode_t mode ) {
	setNearMaterialSamplingQuality( mode == TERRAIN_SAMPLING_SINGLE_SAMPLE ? MATERIAL_SAMPLING_FAST : MATERIAL_SAMPLING_FULL );
}

void Terrain3DMaterial::setTerrainNormalMode( const t3dTerrainNormalMode_t mode ) {
	setNearTerrainNormalQuality(
	        mode == TERRAIN_NORMAL_GEOMETRY_DERIVATIVE ? TERRAIN_NORMAL_QUALITY_GEOMETRY : TERRAIN_NORMAL_QUALITY_DETAILED );
}

void Terrain3DMaterial::setColorSamplingMode( const t3dColorSamplingMode_t mode ) {
	setNearTerrainColorQuality( mode == COLOR_SAMPLING_EXACT_4_TAP ? TERRAIN_COLOR_QUALITY_EXACT : TERRAIN_COLOR_QUALITY_FAST );
}

void Terrain3DMaterial::setProjectionMode( const t3dProjectionMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_projectionMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setTessellationMaterialMode( const t3dTessellationMaterialMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_tessellationMaterialMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setSurfaceDataMode( const t3dSurfaceDataMode_t mode ) {
	setNearSurfaceResponseQuality( mode == SURFACE_DATA_FULL ? SURFACE_RESPONSE_DETAILED : SURFACE_RESPONSE_SIMPLE );
}

void Terrain3DMaterial::setTextureSurfaceMode( const t3dTextureSurfaceMode_t mode ) {
	setNearTextureSurfaceMode( mode );
}

void Terrain3DMaterial::setTerrainShadingMode( const t3dTerrainShadingMode_t mode ) {
	if ( mode == SHADING_UNSHADED ) {
		setNearLightingModel( TIER_LIGHTING_UNSHADED );
	} else if ( mode == SHADING_XBOX_360_BLINN_PHONG ) {
		setNearLightingModel( TIER_LIGHTING_BLINN_PHONG );
	} else if ( mode == SHADING_BURLEY_GGX || mode == SHADING_LAMBERT_GGX ) {
		setNearLightingModel( TIER_LIGHTING_PBR );
	} else {
		setNearLightingModel( TIER_LIGHTING_LAMBERT );
	}
}

void Terrain3DMaterial::setDetilingMode( const t3dDetilingMode_t mode ) {
	setNearDetilingQuality( mode == DETILING_DISABLED ? TIER_DETILING_DISABLED : TIER_DETILING_ROTATION );
}

void Terrain3DMaterial::setWorldNoiseShaderMode( const t3dWorldNoiseShaderMode_t mode ) {
	SET_IF_DIFF( m_authoringState.m_worldNoiseShaderMode, mode );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Setting world-noise shader mode: ", mode );
	update( FULL_REBUILD );
	emit_changed();
}

Dictionary Terrain3DMaterial::cookRuntimeShaderPack( void ) {
	Dictionary result;
	result["complete"] = false;
	if ( !m_pTerrain || m_pTerrain->getDataDirectory().is_empty() || m_authoringState.m_bShaderOverrideEnabled ) {
		result["error"] = "Terrain/material is unavailable or uses a custom shader override.";
		return result;
	}
	const bool old_loaded = m_shaderConfig.m_bRuntimeShaderPackLoaded;
	m_shaderConfig.m_bRuntimeShaderPackLoaded = false;
	const t3dShaderVariantState_t base = captureVariantState();
	const uint64_t state_signature = getVariantStateSignature( base );
	typedef struct t3dShaderPackSourceEntry_s {
		uint8_t m_nDomain;
		uint8_t m_nTier;
		String m_source;
	} t3dShaderPackSourceEntry_t;
	eastl::vector<t3dShaderPackSourceEntry_t, Terrain3DEastlAllocator> entries;
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		entries.push_back(
		        { 0u, ( uint8_t )tier, finalizeRuntimeShaderCode( generateShaderCode( base, ( t3dShaderTier_t )tier, false ) ) } );
	}
	if ( base.m_bTessellationEnabled ) {
		entries.push_back( { 1u, 0u, generateBufferShaderCode( base ) } );
	}
	eastl::fixed_vector<t3dShaderVariantState_t, s_nMaxShaderVariantPrewarmLimit, false> states;
	appendPotentialBranchStates( states, base );
	eastl::fixed_vector<t3dShaderVariantKey_t, s_nMaxShaderVariantPrewarmLimit * 2, false> seen;
	for ( size_t i = 0u; i < states.size() && ( int )entries.size() < m_shaderConfig.m_nShaderVariantPrewarmLimit + 4; ++i ) {
		const String source = finalizeRuntimeShaderCode( generateShaderCode( states[i], SHADER_TIER_NEAR, false ) );
		const t3dShaderVariantKey_t key = makeVariantCacheKey( source );
		bool duplicate = false;
		for ( size_t k = 0u; k < seen.size(); ++k ) {
			if ( seen[k] == key ) {
				duplicate = true;
				break;
			}
		}
		if ( !duplicate ) {
			seen.push_back( key );
			entries.push_back( { 0u, ( uint8_t )SHADER_TIER_NEAR, source } );
		}
		if ( states[i].m_bTessellationEnabled && ( int )entries.size() < m_shaderConfig.m_nShaderVariantPrewarmLimit + 4 ) {
			const String buffer = generateBufferShaderCode( states[i] );
			const t3dShaderVariantKey_t buffer_key = makeVariantCacheKey( buffer );
			duplicate = false;
			for ( size_t k = 0u; k < seen.size(); ++k ) {
				if ( seen[k] == buffer_key ) {
					duplicate = true;
					break;
				}
			}
			if ( !duplicate && seen.size() < s_nMaxShaderVariantPrewarmLimit * 2 ) {
				seen.push_back( buffer_key );
				entries.push_back( { 1u, 0u, buffer } );
			}
		}
	}
	m_shaderConfig.m_bRuntimeShaderPackLoaded = old_loaded;
	const String path = m_pTerrain->getDataDirectory().path_join( "terrain3d_shaders.t3dshaderpack" );
	const String temporary = path + String( ".tmp" );
	Ref<FileAccess> file = FileAccess::open( temporary, FileAccess::WRITE );
	if ( file.is_null() ) {
		result["error"] = "Unable to open runtime shader pack.";
		return result;
	}
	file->store_64( s_nShaderPackMagic );
	file->store_32( s_nShaderPackVersion );
	file->store_32( ( uint32_t )entries.size() );
	file->store_64( state_signature );
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( entries ).size() ); iEntry++ ) {
		const t3dShaderPackSourceEntry_t &entry = ( entries )[iEntry];
		file->store_8( entry.m_nDomain );
		file->store_8( entry.m_nTier );
		file->store_16( 0u );
		t3dMaterialStoreShaderPackString( file, entry.m_source );
	}
	const Error write_error = file->get_error();
	file->close();
	if ( write_error != OK ) {
		DirAccess::remove_absolute( temporary );
		result["error"] = "Runtime shader pack write failed.";
		return result;
	}
	const String backup = path + String( ".bak" );
	if ( FileAccess::file_exists( backup ) ) {
		DirAccess::remove_absolute( backup );
	}
	const bool had_previous = FileAccess::file_exists( path );
	if ( had_previous && DirAccess::rename_absolute( path, backup ) != OK ) {
		DirAccess::remove_absolute( temporary );
		result["error"] = "Runtime shader pack backup failed.";
		return result;
	}
	if ( DirAccess::rename_absolute( temporary, path ) != OK ) {
		if ( had_previous ) {
			DirAccess::rename_absolute( backup, path );
		}
		result["error"] = "Runtime shader pack publish failed.";
		return result;
	}
	if ( FileAccess::file_exists( backup ) ) {
		DirAccess::remove_absolute( backup );
	}
	result["complete"] = true;
	result["path"] = path;
	result["entry_count"] = ( int )entries.size();
	result["state_signature"] = String::num_uint64( state_signature, 16, true );
	return result;
}

bool Terrain3DMaterial::loadRuntimeShaderPack( void ) {
	m_shaderConfig.m_bRuntimeShaderPackLoaded = false;
	m_shaderConfig.m_nRuntimeShaderPackStateSignature = 0u;
	m_shaderConfig.m_runtimeShaderPackBufferSource = String();
	for ( size_t iTier = 0u; iTier < T3D_ARRAY_COUNT( m_shaderConfig.m_szRuntimeShaderPackTierSource ); iTier++ ) {
		String &source = ( m_shaderConfig.m_szRuntimeShaderPackTierSource )[iTier];
		source = String();
	}
	if ( !m_pTerrain || m_pTerrain->getDataDirectory().is_empty() ) {
		return false;
	}
	const String path = m_pTerrain->getDataDirectory().path_join( "terrain3d_shaders.t3dshaderpack" );
	Ref<FileAccess> file = FileAccess::open( path, FileAccess::READ );
	if ( file.is_null() || file->get_length() < 24u ) {
		return false;
	}
	if ( file->get_64() != s_nShaderPackMagic || file->get_32() != s_nShaderPackVersion ) {
		return false;
	}
	const uint32_t count = file->get_32();
	const uint64_t signature = file->get_64();
	if ( count < SHADER_TIER_COUNT || count > 128u || signature != getVariantStateSignature( captureVariantState() ) ) {
		return false;
	}
	typedef struct t3dShaderPackVerifyEntry_s {
		uint8_t m_nDomain;
		uint8_t m_nTier;
		String m_source;
	} t3dShaderPackVerifyEntry_t;
	eastl::vector<t3dShaderPackVerifyEntry_t, Terrain3DEastlAllocator> entries;
	entries.reserve( count );
	for ( uint32_t i = 0; i < count; ++i ) {
		if ( file->get_position() + 4u > file->get_length() ) {
			return false;
		}
		t3dShaderPackVerifyEntry_t entry;
		entry.m_nDomain = file->get_8();
		entry.m_nTier = file->get_8();
		file->get_16();
		if ( !t3dMaterialReadShaderPackString( file, entry.m_source ) || entry.m_source.is_empty() ) {
			return false;
		}
		entries.push_back( entry );
	}
	if ( file->get_position() != file->get_length() ) {
		return false;
	}
	for ( size_t i = 0; i < entries.size(); ++i ) {
		const t3dShaderPackVerifyEntry_t &entry = entries[i];
		if ( entry.m_nDomain == 0u && entry.m_nTier < SHADER_TIER_COUNT
		        && m_shaderConfig.m_szRuntimeShaderPackTierSource[entry.m_nTier].is_empty() ) {
			m_shaderConfig.m_szRuntimeShaderPackTierSource[entry.m_nTier] = entry.m_source;
		} else if ( entry.m_nDomain == 1u && m_shaderConfig.m_runtimeShaderPackBufferSource.is_empty() ) {
			m_shaderConfig.m_runtimeShaderPackBufferSource = entry.m_source;
		}
	}
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		if ( m_shaderConfig.m_szRuntimeShaderPackTierSource[tier].is_empty() ) {
			return false;
		}
	}
	m_shaderConfig.m_nRuntimeShaderPackStateSignature = signature;

	// A loaded pack must mean the required runtime working set is actually usable,
	// not merely that the serialized file parsed. Compile the authoritative Near,
	// Middle, and Far sources transactionally before publishing pack-loaded state.
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		t3dShaderValidationResult_t validation;
		const Ref<Shader> shader = getOrCreateShaderVariant( m_shaderConfig.m_szRuntimeShaderPackTierSource[tier],
		        SHADER_DOMAIN_RUNTIME,
		        false,
		        ( t3dShaderTier_t )( tier ),
		        &validation );
		if ( shader.is_null() || !shader->get_rid().is_valid() || !validation.m_bValid ) {
			LOG( ERROR, "Runtime shader pack tier ", tier, " failed validation during pack load. ", validation.m_failureSummary );
			m_shaderConfig.m_nRuntimeShaderPackStateSignature = 0u;
			return false;
		}
	}
	if ( !m_shaderConfig.m_runtimeShaderPackBufferSource.is_empty() ) {
		const Ref<Shader> buffer_shader =
		        getOrCreateShaderVariant( m_shaderConfig.m_runtimeShaderPackBufferSource, SHADER_DOMAIN_BUFFER, false );
		if ( buffer_shader.is_null() || !buffer_shader->get_rid().is_valid() ) {
			LOG( ERROR, "Runtime shader pack displacement buffer shader failed validation during pack load." );
			m_shaderConfig.m_nRuntimeShaderPackStateSignature = 0u;
			return false;
		}
	}

	m_shaderConfig.m_bRuntimeShaderPackLoaded = true;
	m_shaderConfig.m_nLastPrewarmVariantCount = 0;
	m_shaderConfig.m_nLastPrewarmSkippedCount = 0;
	uint64_t nPrewarmValidationFailures = 0u;
	uint64_t nPrewarmAllocationFailures = 0u;
	uint64_t nPrewarmOtherFailures = 0u;
	// Additional entries are speculative cooked variants. They are prewarmed
	// directly from the pack; strict runtime never regenerates them from templates.
	for ( size_t iEntry = 0u; iEntry < ( size_t )( ( entries ).size() ); iEntry++ ) {
		const t3dShaderPackVerifyEntry_t &entry = ( entries )[iEntry];
		if ( entry.m_nDomain == 0u && entry.m_nTier < SHADER_TIER_COUNT
		        && entry.m_source == m_shaderConfig.m_szRuntimeShaderPackTierSource[entry.m_nTier] ) {
			continue;
		}
		if ( entry.m_nDomain == 1u && entry.m_source == m_shaderConfig.m_runtimeShaderPackBufferSource ) {
			continue;
		}
		const uint64_t validation_before = m_shaderConfig.m_nVariantValidationFailuresTotal;
		const uint64_t allocation_before = m_shaderConfig.m_nVariantAllocationFailuresTotal;
		Ref<Shader> shader;
		if ( entry.m_nDomain == 0u ) {
			shader = getOrCreateShaderVariant( entry.m_source,
			        SHADER_DOMAIN_RUNTIME,
			        true,
			        ( t3dShaderTier_t )CLAMP( ( int )entry.m_nTier, 0, ( int )SHADER_TIER_FAR ) );
		} else if ( entry.m_nDomain == 1u ) {
			shader = getOrCreateShaderVariant( entry.m_source, SHADER_DOMAIN_BUFFER, true );
		} else {
			continue;
		}
		if ( shader.is_valid() && shader->get_rid().is_valid() ) {
			++m_shaderConfig.m_nLastPrewarmVariantCount;
		} else {
			++m_shaderConfig.m_nLastPrewarmSkippedCount;
			if ( m_shaderConfig.m_nVariantValidationFailuresTotal > validation_before ) {
				++nPrewarmValidationFailures;
			} else if ( m_shaderConfig.m_nVariantAllocationFailuresTotal > allocation_before ) {
				++nPrewarmAllocationFailures;
			} else {
				++nPrewarmOtherFailures;
			}
		}
	}
	if ( m_shaderConfig.m_nLastPrewarmSkippedCount > 0 ) {
		LOG( WARN,
		        "Runtime shader pack prewarm retained ",
		        m_shaderConfig.m_nLastPrewarmVariantCount,
		        " optional variants and skipped ",
		        m_shaderConfig.m_nLastPrewarmSkippedCount,
		        " (validation=",
		        nPrewarmValidationFailures,
		        ", allocation_or_budget=",
		        nPrewarmAllocationFailures,
		        ", other=",
		        nPrewarmOtherFailures,
		        ")." );
	}
	return true;
}

void Terrain3DMaterial::prewarmShaderVariants( void ) {
	prewarmShaderVariantsInternal( true );
}

void Terrain3DMaterial::clearShaderVariantCache( void ) {
	m_shaderConfig.m_runtimeShaderVariantEntries.clear();
	m_shaderConfig.m_nRuntimeShaderVariantSourceBytes = 0;
	m_shaderConfig.m_editorShaderVariantEntries.clear();
	m_shaderConfig.m_nEditorShaderVariantSourceBytes = 0;
	m_shaderConfig.m_bufferShaderVariantEntries.clear();
	m_shaderConfig.m_nBufferShaderVariantSourceBytes = 0;
	m_shaderConfig.m_bHasLastBoundVariantState = false;
	m_shaderConfig.m_nObservedVariantDimensions = 0;
	m_shaderConfig.m_nVariantTransitionClock = 0;
	for ( size_t iDimension = 0u; iDimension < T3D_ARRAY_COUNT( m_shaderConfig.m_szVariantDimensionLastUsed ); iDimension++ ) {
		uint64_t &last_used = ( m_shaderConfig.m_szVariantDimensionLastUsed )[iDimension];
		last_used = 0;
	}
	m_shaderConfig.m_nPrewarmSignature = UINT64_MAX;
	m_shaderConfig.m_nLastPrewarmVariantCount = 0;
	m_shaderConfig.m_nLastPrewarmSkippedCount = 0;
}

int Terrain3DMaterial::getSpeculativeShaderVariantCount( void ) const {
	int speculative_count = 0;
	const t3dShaderVariantDomain_t domains[] = { SHADER_DOMAIN_RUNTIME, SHADER_DOMAIN_EDITOR, SHADER_DOMAIN_BUFFER };
	for ( size_t iDomain = 0u; iDomain < T3D_ARRAY_COUNT( domains ); iDomain++ ) {
		const t3dShaderVariantDomain_t domain = ( domains )[iDomain];
		const t3dShaderVariantEntry_t *pEntries = getShaderVariantEntries( domain );
		const int entry_count = getShaderVariantCountForDomain( domain );
		for ( int index = 0; index < entry_count; ++index ) {
			if ( pEntries[index].m_bSpeculative ) {
				++speculative_count;
			}
		}
	}
	return speculative_count;
}

String Terrain3DMaterial::getShaderRegressionSource(
        const int nTier, const int nAssemblyPath, const bool bControlMapsImplicit, const bool bColorMapsImplicit ) {
	if ( m_shaderConfig.m_shaderTemplates.is_empty() ) {
		preloadShaders();
	}

	const t3dShaderTier_t requested_tier = ( t3dShaderTier_t )( CLAMP( nTier, ( int )( SHADER_TIER_NEAR ), ( int )( SHADER_TIER_FAR ) ) );
	t3dShaderVariantState_t state = captureVariantState();
	state.m_bControlMapsImplicit = bControlMapsImplicit;
	state.m_bColorMapsImplicit = bColorMapsImplicit;

	// Regression assembly paths mirror the production material update contract.
	// Authoring binds the complete Near shader to every clipmap tier. Runtime
	// Profile activates compact Middle/Far source while Near keeps the editor path.
	if ( nAssemblyPath == 0 || requested_tier == SHADER_TIER_NEAR ) {
		return generateShaderCode( state, SHADER_TIER_NEAR, nAssemblyPath == 0 );
	}
	return finalizeRuntimeShaderCode( generateShaderCode( state, requested_tier ) );
}

Dictionary Terrain3DMaterial::getShaderVariantDiagnostics( void ) const {
	Dictionary diagnostics;
	const uint64_t cached_requests = m_shaderConfig.m_nVariantCacheHitsTotal + m_shaderConfig.m_nVariantCacheMissesTotal;
	const double fHitRatio =
	        cached_requests > 0u ? ( double )( m_shaderConfig.m_nVariantCacheHitsTotal ) / ( double )( cached_requests ) : -1.0;
	const uint64_t submitted_sources = m_shaderConfig.m_nVariantCreationsTotal + m_shaderConfig.m_nVariantUncachedSubmissionsTotal;
	const uint64_t source_submission_average =
	        submitted_sources > 0u ? m_shaderConfig.m_nVariantSourceSubmissionUsecTotal / submitted_sources : 0u;
	const uint64_t shader_update_average =
	        m_shaderConfig.m_nShaderUpdateCount > 0u ? m_shaderConfig.m_nShaderUpdateUsecTotal / m_shaderConfig.m_nShaderUpdateCount : 0u;

	diagnostics["variant_mode"] = ( int )( m_authoringState.m_shaderVariantMode );
	diagnostics["cache_limit"] = m_shaderConfig.m_nShaderVariantCacheLimit;
	diagnostics["prewarm_limit"] = m_shaderConfig.m_nShaderVariantPrewarmLimit;
	diagnostics["source_budget_mb"] = m_shaderConfig.m_nShaderVariantSourceBudgetMb;
	diagnostics["effective_source_budget_mb"] = m_shaderConfig.m_nShaderVariantEffectiveSourceBudgetMb;
	diagnostics["variants_total"] = getShaderVariantCount();
	diagnostics["variants_runtime"] = getRuntimeShaderVariantCount();
	diagnostics["variants_editor"] = getEditorShaderVariantCount();
	diagnostics["variants_buffer"] = getBufferShaderVariantCount();
	diagnostics["variants_speculative"] = getSpeculativeShaderVariantCount();
	diagnostics["variant_source_bytes"] = ( int64_t )( getShaderVariantSourceBytesRaw() );
	diagnostics["cache_hits_total"] = ( int64_t )( m_shaderConfig.m_nVariantCacheHitsTotal );
	diagnostics["cache_misses_total"] = ( int64_t )( m_shaderConfig.m_nVariantCacheMissesTotal );
	diagnostics["cache_hit_ratio"] = fHitRatio;
	diagnostics["creations_total"] = ( int64_t )( m_shaderConfig.m_nVariantCreationsTotal );
	diagnostics["prewarm_creations_total"] = ( int64_t )( m_shaderConfig.m_nVariantPrewarmCreationsTotal );
	diagnostics["uncached_submissions_total"] = ( int64_t )( m_shaderConfig.m_nVariantUncachedSubmissionsTotal );
	diagnostics["allocation_failures_total"] = ( int64_t )( m_shaderConfig.m_nVariantAllocationFailuresTotal );
	diagnostics["removals_total"] = ( int64_t )( m_shaderConfig.m_nVariantRemovalsTotal );
	diagnostics["source_submission_usec_last"] = ( int64_t )( m_shaderConfig.m_nVariantSourceSubmissionUsecLast );
	diagnostics["source_submission_usec_average"] = ( int64_t )( source_submission_average );
	diagnostics["source_submission_usec_peak"] = ( int64_t )( m_shaderConfig.m_nVariantSourceSubmissionUsecPeak );
	diagnostics["shader_update_usec_last"] = ( int64_t )( m_shaderConfig.m_nShaderUpdateUsecLast );
	diagnostics["shader_update_usec_average"] = ( int64_t )( shader_update_average );
	diagnostics["shader_update_usec_peak"] = ( int64_t )( m_shaderConfig.m_nShaderUpdateUsecPeak );
	diagnostics["shader_update_count"] = ( int64_t )( m_shaderConfig.m_nShaderUpdateCount );
	diagnostics["shader_parameter_list_queries"] = ( int64_t )( m_runtimeState.m_nShaderParameterListQueryCount );
	diagnostics["shader_parameter_default_queries"] = ( int64_t )( m_runtimeState.m_nShaderParameterDefaultQueryCount );
	diagnostics["shader_inspector_local_reads"] = ( int64_t )( m_runtimeState.m_nShaderInspectorRuntimeReadCount );
	diagnostics["last_request_domain"] = m_shaderConfig.m_nLastVariantRequestDomain;
	diagnostics["last_request_hit"] = m_shaderConfig.m_bLastVariantRequestHit;
	diagnostics["last_request_prewarm"] = m_shaderConfig.m_bLastVariantRequestPrewarm;
	diagnostics["last_request_source_bytes"] = ( int64_t )( m_shaderConfig.m_nLastVariantRequestSourceBytes );
	diagnostics["last_prewarm_variants"] = m_shaderConfig.m_nLastPrewarmVariantCount;
	diagnostics["last_prewarm_skipped"] = m_shaderConfig.m_nLastPrewarmSkippedCount;
	diagnostics["theoretical_variants"] = getTheoreticalShaderVariantCount();
	diagnostics["near_shader_rid"] = ( int64_t )( getShaderRidForTier( SHADER_TIER_NEAR ).get_id() );
	diagnostics["middle_shader_rid"] = ( int64_t )( getShaderRidForTier( SHADER_TIER_MIDDLE ).get_id() );
	diagnostics["far_shader_rid"] = ( int64_t )( getShaderRidForTier( SHADER_TIER_FAR ).get_id() );
	diagnostics["buffer_shader_rid"] = ( int64_t )( getBufferShaderRid().get_id() );
	Ref<Terrain3DAssets> pAssets = m_pTerrain ? m_pTerrain->getAssets() : Ref<Terrain3DAssets>();
	Terrain3DData *const pData = m_pTerrain ? m_pTerrain->getData() : NULL;
	const bool bFarAssetsReady = pAssets.is_valid() && pAssets->isInitialized();
	const int far_albedo_layers = bFarAssetsReady ? pAssets->getFarAlbedoArraySize() : 0;
	const int far_albedo_max_lod = bFarAssetsReady ? pAssets->getFarAlbedoArrayMaxLod() : 0;
	const RID far_albedo_rid = bFarAssetsReady ? pAssets->getFarAlbedoArrayRid() : RID();
	const RID far_height_rid = pData ? pData->getFarHeightMapRid() : RID();
	const RID far_color_rid = pData ? pData->getFarColorMapRid() : RID();
	diagnostics["page_table_rid"] = ( int64_t )( m_vtBinding.m_generatedRegionMap.getRid().get_id() );
	diagnostics["logical_page_table_rid"] = ( int64_t )( m_vtBinding.m_generatedLogicalRegionMap.getRid().get_id() );
	diagnostics["page_table_valid"] = isRegionPageTableValid();
	diagnostics["page_table_region_map_size"] = m_vtBinding.m_nRegionMapTextureSize;
	diagnostics["page_table_owner_generation"] = ( int64_t )( m_vtBinding.m_nRegionPageTableOwnerGeneration );
	diagnostics["far_albedo_array_rid"] = ( int64_t )( far_albedo_rid.get_id() );
	diagnostics["far_albedo_layers"] = far_albedo_layers;
	diagnostics["far_albedo_max_lod"] = far_albedo_max_lod;
	diagnostics["far_requested_material_lod"] = m_authoringState.m_farQuality.m_fMaterialLodBias;
	diagnostics["far_material_resolution"] = m_pTerrain ? m_pTerrain->getFarMaterialResolution() : 128;
	diagnostics["far_material_lod_bias"] = m_authoringState.m_farQuality.m_fMaterialLodBias;
	diagnostics["far_quality_divisor"] = m_pTerrain ? m_pTerrain->getGpuFarQualityDivisor() : 1;
	diagnostics["far_height_atlas_side"] = m_pTerrain ? m_pTerrain->getGpuFarHeightAtlasSide() : s_nGpuCacheFarHeightSide;
	diagnostics["far_surface_atlas_side"] = m_pTerrain ? m_pTerrain->getGpuFarSurfaceAtlasSide() : s_nGpuCacheFarHeightSide;
	diagnostics["far_atlas_side"] = diagnostics["far_height_atlas_side"];
	diagnostics["far_texture_color_count"] = bFarAssetsReady ? MIN( pAssets->getTextureColors().size(), s_nMaxTextureAssets ) : 0;
	diagnostics["far_height_atlas_rid"] = ( int64_t )( far_height_rid.get_id() );
	diagnostics["far_color_atlas_rid"] = ( int64_t )( far_color_rid.get_id() );
	diagnostics["far_resources_ready"] =
	        far_albedo_rid.is_valid() && far_albedo_layers > 0 && far_height_rid.is_valid() && far_color_rid.is_valid();
	diagnostics["validation_reflection_queries_total"] = ( int64_t )( m_shaderConfig.m_nVariantValidationReflectionQueriesTotal );
	diagnostics["validation_failures_total"] = ( int64_t )( m_shaderConfig.m_nVariantValidationFailuresTotal );
	Array tier_diagnostics;
	for ( int tier = SHADER_TIER_NEAR; tier < SHADER_TIER_COUNT; ++tier ) {
		tier_diagnostics.push_back( getShaderTierDiagnostics( tier ) );
	}
	diagnostics["tier_diagnostics"] = tier_diagnostics;
	return diagnostics;
}

Dictionary Terrain3DMaterial::getShaderTierDiagnostics( const int nTier ) const {
	Dictionary diagnostics;
	const int tier = CLAMP( nTier, ( int )( SHADER_TIER_NEAR ), ( int )( SHADER_TIER_FAR ) );
	const t3dShaderTierValidationState_t &state = m_shaderConfig.m_szShaderTierValidation[tier];
	const t3dTierQualityConfig_t &quality = tier == SHADER_TIER_NEAR
	        ? m_authoringState.m_nearQuality
	        : ( tier == SHADER_TIER_MIDDLE ? m_authoringState.m_middleQuality : m_authoringState.m_farQuality );
	diagnostics["tier"] = tier;
	diagnostics["variant_signature"] = String::num_uint64( state.m_key.m_nPrimaryHash, 16, true );
	diagnostics["variant_fingerprint"] = String::num_uint64( state.m_key.m_nFingerprint, 16, true );
	diagnostics["variant_source_length"] = ( int64_t )( state.m_key.m_nSourceLength );
	diagnostics["requested_shader_rid"] = ( int64_t )( state.m_nRequestedShaderRidId );
	diagnostics["active_shader_rid"] = ( int64_t )( state.m_nActiveShaderRidId );
	diagnostics["compile_valid"] = state.m_bCompileValid;
	diagnostics["reflected_parameter_count"] = state.m_nReflectedParameterCount;
	diagnostics["last_failure_summary"] = state.m_failureSummary;
	diagnostics["fallback_to_near"] = state.m_bFallbackToNear;
	diagnostics["compact_tier_active"] = state.m_bCompactTierActive;
	diagnostics["texture_filtering"] = ( int )( quality.m_textureFiltering );
	diagnostics["material_sampling_quality"] = ( int )( quality.m_materialSamplingQuality );
	diagnostics["terrain_normal_quality"] = ( int )( quality.m_terrainNormalQuality );
	diagnostics["terrain_color_quality"] = ( int )( quality.m_terrainColorQuality );
	diagnostics["surface_response_quality"] = ( int )( quality.m_surfaceResponseQuality );
	diagnostics["texture_surface_mode"] = ( int )( quality.m_textureSurfaceMode );
	diagnostics["lighting_model"] = ( int )( quality.m_lightingModel );
	diagnostics["detiling_quality"] = ( int )( quality.m_detilingQuality );
	diagnostics["authored_color_contribution"] = quality.m_fAuthoredColorContribution;
	diagnostics["wetness_contribution"] = quality.m_fWetnessContribution;
	diagnostics["material_lod_bias"] = quality.m_fMaterialLodBias;
	diagnostics["range_context"] = getTierRangeContext( ( t3dShaderTier_t )( tier ) );
	diagnostics["shadow_context"] = getTierShadowContext( ( t3dShaderTier_t )( tier ) );
	return diagnostics;
}

String Terrain3DMaterial::getTierRangeContext( const t3dShaderTier_t tier ) const {
	if ( !m_pTerrain ) {
		return "Unavailable until attached to Terrain3D";
	}
	const real_t fNearEnd = m_pTerrain->getRuntimeNearRangeEndM();
	const real_t fMiddleEnd = m_pTerrain->getRuntimeMiddleRangeEndM();
	if ( tier == SHADER_TIER_NEAR ) {
		return vformat( "0 m - %.1f m", fNearEnd );
	}
	if ( tier == SHADER_TIER_MIDDLE ) {
		return vformat( "%.1f m - %.1f m", fNearEnd, fMiddleEnd );
	}
	return vformat( "%.1f m onward", fMiddleEnd );
}

String Terrain3DMaterial::getTierShadowContext( const t3dShaderTier_t tier ) const {
	if ( !m_pTerrain ) {
		return "Unavailable until attached to Terrain3D";
	}
	bool bEnabled = false;
	switch ( tier ) {
		case SHADER_TIER_NEAR:
			bEnabled = m_pTerrain->getNearShadowEnabled();
			break;
		case SHADER_TIER_MIDDLE:
			bEnabled = m_pTerrain->getMiddleShadowEnabled();
			break;
		case SHADER_TIER_FAR:
			bEnabled = m_pTerrain->getFarShadowEnabled();
			break;
		default:
			break;
	}
	if ( !bEnabled || m_pTerrain->getShadowLodCount() <= 0 ) {
		return "Disabled";
	}
	return vformat( "Enabled to %.1f m", m_pTerrain->getRuntimeShadowRangeEndM() );
}

String Terrain3DMaterial::getTierShaderStatus( const t3dShaderTier_t argTier ) const {
	const int tier = CLAMP( ( int )( argTier ), ( int )( SHADER_TIER_NEAR ), ( int )( SHADER_TIER_FAR ) );
	const t3dShaderTierValidationState_t &state = m_shaderConfig.m_szShaderTierValidation[tier];
	if ( state.m_bFallbackToNear ) {
		return vformat( "Fallback to Near; RID %d; %s", ( int64_t )( state.m_nActiveShaderRidId ), state.m_failureSummary );
	}
	if ( !state.m_bCompileValid ) {
		return state.m_failureSummary.is_empty() ? "Not compiled" : String( "Invalid: " ) + state.m_failureSummary;
	}
	return vformat( "Valid; RID %d; %d reflected parameters", ( int64_t )( state.m_nActiveShaderRidId ), state.m_nReflectedParameterCount );
}

void Terrain3DMaterial::resetShaderVariantDiagnostics( void ) {
	m_shaderConfig.m_nVariantCacheHitsTotal = 0u;
	m_shaderConfig.m_nVariantCacheMissesTotal = 0u;
	m_shaderConfig.m_nVariantCreationsTotal = 0u;
	m_shaderConfig.m_nVariantPrewarmCreationsTotal = 0u;
	m_shaderConfig.m_nVariantUncachedSubmissionsTotal = 0u;
	m_shaderConfig.m_nVariantAllocationFailuresTotal = 0u;
	m_shaderConfig.m_nVariantRemovalsTotal = 0u;
	m_shaderConfig.m_nVariantSourceSubmissionUsecTotal = 0u;
	m_shaderConfig.m_nVariantSourceSubmissionUsecPeak = 0u;
	m_shaderConfig.m_nVariantSourceSubmissionUsecLast = 0u;
	m_shaderConfig.m_nShaderUpdateUsecTotal = 0u;
	m_shaderConfig.m_nShaderUpdateUsecPeak = 0u;
	m_shaderConfig.m_nShaderUpdateUsecLast = 0u;
	m_shaderConfig.m_nShaderUpdateCount = 0u;
	m_runtimeState.m_nShaderParameterListQueryCount = 0u;
	m_runtimeState.m_nShaderParameterDefaultQueryCount = 0u;
	m_runtimeState.m_nShaderInspectorRuntimeReadCount = 0u;
	m_shaderConfig.m_nVariantValidationReflectionQueriesTotal = 0u;
	m_shaderConfig.m_nVariantValidationFailuresTotal = 0u;
	m_shaderConfig.m_nLastVariantRequestDomain = -1;
	m_shaderConfig.m_bLastVariantRequestHit = false;
	m_shaderConfig.m_bLastVariantRequestPrewarm = false;
	m_shaderConfig.m_nLastVariantRequestSourceBytes = 0u;
}

#ifdef DEBUG_ENABLED
void Terrain3DMaterial::setShaderRegressionInvalidTier( const int nTier ) {
	const int requested_tier = ( nTier >= SHADER_TIER_NEAR && nTier < SHADER_TIER_COUNT ) ? nTier : -1;
	if ( m_shaderConfig.m_nShaderRegressionInvalidTier == requested_tier ) {
		return;
	}
	m_shaderConfig.m_nShaderRegressionInvalidTier = requested_tier;
	if ( m_pTerrain && isInitialized() ) {
		updateShader();
	}
}

Variant Terrain3DMaterial::getTierRegressionMaterialParam( const int nTier, const StringName &strName ) const {
	const int tier = CLAMP( nTier, ( int )( SHADER_TIER_NEAR ), ( int )( SHADER_TIER_FAR ) );
	if ( m_runtimeState.m_szTierUniformPublicationCount[tier] == 0u ) {
		return Variant();
	}
	const t3dTierQualityConfig_t &quality = tier == SHADER_TIER_NEAR
	        ? m_authoringState.m_nearQuality
	        : ( tier == SHADER_TIER_MIDDLE ? m_authoringState.m_middleQuality : m_authoringState.m_farQuality );
	if ( strName == StringName( "_tier_authored_color_contribution" ) ) {
		return quality.m_fAuthoredColorContribution;
	}
	if ( strName == StringName( "_tier_wetness_contribution" ) ) {
		return quality.m_fWetnessContribution;
	}
	if ( strName == StringName( "_tier_material_lod_bias" ) ) {
		return quality.m_fMaterialLodBias;
	}
	return Variant();
}
#endif

void Terrain3DMaterial::setPackedPbrSingleFetch( const bool bEnabled ) {
	// Compatibility shim for scenes serialized by the experimental packed-PBR
	// branch. The current layout stores albedo and surface data in separate arrays,
	// so a true single-fetch path cannot be generated without changing the format.
	if ( bEnabled ) {
		LOG( WARN,
		        "packed_pbr_single_fetch is deprecated and has no valid path with the current texture layout; "
		        "ignoring it." );
	}
}

void Terrain3DMaterial::setAutoShaderEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bAutoShaderEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable auto shader: ", bEnabled );
	updateShader();
	if ( m_pTerrain && m_pTerrain->getData() ) {
		m_pTerrain->getData()->invalidateGpuFarSurface();
	}
	emit_changed();
}

void Terrain3DMaterial::setAutoShaderEnabledTransient( const bool bEnabled ) {
	if ( m_authoringState.m_bAutoShaderEnabled == bEnabled ) {
		return;
	}
	m_authoringState.m_bAutoShaderEnabled = bEnabled;
	LOG( INFO, "Temporarily enable auto shader: ", bEnabled );
	updateShader();
	if ( m_pTerrain && m_pTerrain->getData() ) {
		m_pTerrain->getData()->invalidateGpuFarSurface();
	}
}

void Terrain3DMaterial::setDualScalingEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDualScalingEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable dual scaling: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setMacroVariationEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bMacroVariationEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable macro variation: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setProjectionEnabled( const bool bEnabled ) {
	setProjectionMode( bEnabled ? PROJECTION_EXACT_PER_CORNER : PROJECTION_DISABLED );
}

void Terrain3DMaterial::setShaderOverrideEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShaderOverrideEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable shader override: ", bEnabled );
	if ( m_authoringState.m_bShaderOverrideEnabled && m_authoringState.m_shaderOverride.is_null() ) {
		LOG( DEBUG, "Instantiating new _shader_override" );
		m_authoringState.m_shaderOverride.instantiate();
	}
	if ( m_pTerrain && m_pTerrain->getData() ) {
		m_pTerrain->getData()->updateMaps( REGION_TYPE_MAX, true, false );
	}
	update( FULL_REBUILD );
}

void Terrain3DMaterial::setShaderOverride( const Ref<Shader> &rShader ) {
	if ( !t3dDiffers( m_authoringState.m_shaderOverride, rShader ) ) {
		return;
	}
	disconnectShaderOverrideSignals();
	m_authoringState.m_shaderOverride = rShader;
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting override shader" );
	update( FULL_REBUILD );
}

void Terrain3DMaterial::setBufferShaderOverrideEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bBufferShaderOverrideEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable shader override: ", bEnabled );
	if ( m_authoringState.m_bBufferShaderOverrideEnabled && m_authoringState.m_bufferShaderOverride.is_null() ) {
		LOG( DEBUG, "Instantiating new _shader_override" );
		m_authoringState.m_bufferShaderOverride.instantiate();
	}
	if ( m_pTerrain && m_pTerrain->getData() ) {
		m_pTerrain->getData()->updateMaps( REGION_TYPE_MAX, true, false );
	}
	updateShader();
}

void Terrain3DMaterial::setBufferShaderOverride( const Ref<Shader> &rShader ) {
	if ( !t3dDiffers( m_authoringState.m_bufferShaderOverride, rShader ) ) {
		return;
	}
	disconnectShaderOverrideSignals();
	m_authoringState.m_bufferShaderOverride = rShader;
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Setting override shader" );
	updateShader();
}

void Terrain3DMaterial::setShaderParam( const StringName &strName, const Variant &rValue ) {
	LOG( DEBUG, "Setting shader parameter: ", strName );
	if ( strName.begins_with( "_" ) ) {
		setRuntimeMaterialParam( strName, rValue );
	} else {
		_set( strName, rValue );
	}
	if ( m_pTerrain && m_pTerrain->getData()
	        && ( strName == StringName( "auto_slope" ) || strName == StringName( "auto_height_reduction" )
	                || strName == StringName( "auto_base_texture" ) || strName == StringName( "auto_overlay_texture" ) ) ) {
		m_pTerrain->getData()->invalidateGpuFarSurface();
	}
}

Variant Terrain3DMaterial::getShaderParam( const StringName &strName ) const {
	if ( strName.begins_with( "_" ) ) {
		return m_runtimeState.m_runtimeMaterialParams.get( strName, Variant() );
	}
	return m_authoringState.m_shaderParams.get( strName, m_authoringState.m_shaderParamDefaults.get( strName, Variant() ) );
}

void Terrain3DMaterial::setOutputAlbedoEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputAlbedoEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output albedo: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setOutputRoughnessEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputRoughnessEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output roughness: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setOutputSpecularEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputSpecularEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output specular: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setOutputNormalMapEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputNormalMapEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output normal map: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setOutputAmbientOcclusionEnabled( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bOutputAmbientOcclusionEnabled, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	m_authoringState.m_shaderPerformancePreset = SHADER_PRESET_CUSTOM;
	LOG( INFO, "Enable PBR output ambient occlusion: ", bEnabled );
	updateShader();
	emit_changed();
}

void Terrain3DMaterial::setShowRegionGrid( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowRegionGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_region_grid: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowInstancerGrid( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowInstancerGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_instancer_grid: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowVertexGrid( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowVertexGrid, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_vertex_grid: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowContours( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowContours, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_contours: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowNavigation( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bShowNavigation, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_navigation: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowCheckered( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewCheckered, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable set_show_checkered: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowGrey( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewGrey, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_grey: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowHeightmap( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewHeightmap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_heightmap: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowJaggedness( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewJaggedness, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_jaggedness: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowAutoshader( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewAutoshader, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_autoshader: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowControlTexture( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlTexture, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_texture: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowControlBlend( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlBlend, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_blend: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowControlAngle( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlAngle, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_angle: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowControlScale( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewControlScale, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_control_scale: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowColormap( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewColormap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_colormap: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowRoughmap( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bDebugViewRoughmap, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_roughmap: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowTextureAlbedo( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexAlbedo, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_albedo: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowTextureHeight( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexHeight, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_height: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowTextureNormal( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexNormal, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_normal: ", bEnabled );
	updateShader();
}

void Terrain3DMaterial::setShowTextureRough( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexRough, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_rough: ", bEnabled );
	updateShader();
}
void Terrain3DMaterial::setShowDisplacementBuffer( const bool bEnabled ) {
	LOG( INFO, "Enable show_texture_rough: ", bEnabled );
	m_authoringState.m_bDebugViewDisplacementBuffer = bEnabled;
	updateShader();
}

void Terrain3DMaterial::setShowTextureAo( const bool bEnabled ) {
	SET_IF_DIFF( m_authoringState.m_bPbrViewTexAo, bEnabled );
	m_authoringState.m_bPersistenceDirty = true;
	LOG( INFO, "Enable show_texture_ao: ", bEnabled );
	updateShader();
}

Error Terrain3DMaterial::save( const String &rPath ) {
	if ( rPath.is_empty() && get_path().is_empty() ) {
		return ERR_FILE_NOT_FOUND;
	}
	if ( !rPath.is_empty() ) {
		LOG( DEBUG, "Setting file path to ", rPath );
		take_over_path( rPath );
	}

	// Remove saved shader params that are not present in the property metadata
	// captured at the last shader rebuild. Saving must not trigger another shader
	// reflection pass.
	const Array keys = m_authoringState.m_shaderParams.keys();
	for ( size_t iKey = 0u; iKey < ( size_t )( ( keys ).size() ); iKey++ ) {
		const StringName name = ( keys )[iKey];
		if ( !m_runtimeState.m_activeParams.has( name ) ) {
			LOG( DEBUG, "'", name, "' not found in shader parameters. Removing from dictionary." );
			m_authoringState.m_shaderParams.erase( name );
			m_authoringState.m_bPersistenceDirty = true;
		}
	}

	// Save external resources only when authored state actually changed. Rewriting
	// a clean .tres/.res on every scene save changes its mtime and makes the editor
	// rediscover Terrain3D's own write on the next filesystem reconciliation.
	Error err = OK;
	String path = get_path();
	const bool explicit_path = !rPath.is_empty();
	if ( path.get_extension() == "tres" || path.get_extension() == "res" ) {
		if ( !explicit_path && !m_authoringState.m_bPersistenceDirty && FileAccess::file_exists( path ) ) {
			return OK;
		}
		LOG( DEBUG, String( "Attempting to save external file: " ) + path );
		err = ResourceSaver::get_singleton()->save( this, path, ResourceSaver::FLAG_COMPRESS );
		if ( err == OK ) {
			m_authoringState.m_bPersistenceDirty = false;
			LOG( INFO, "File saved successfully: ", path );
			if ( IS_EDITOR && path.begins_with( "res://" ) ) {
				EditorFileSystem *const pFilesystem = EditorInterface::get_singleton()->get_resource_filesystem();
				if ( pFilesystem && !pFilesystem->is_scanning() ) {
					pFilesystem->update_file( path );
				}
			}
		} else {
			LOG( ERROR, "Cannot save file: ", path, ". Error code: ", ERROR, ". Look up @GlobalScope Error enum in the Godot docs" );
		}
	}
	return err;
}

// -----------------------------------------------------------------------------
// Protected functions

void Terrain3DMaterial::refreshShaderPropertyCache( void ) {
	m_authoringState.m_shaderPropertyCache.clear();
	m_authoringState.m_shaderParamDefaults.clear();
	m_runtimeState.m_activeParams.clear();
	if ( !m_pTerrain || !m_shaderConfig.m_shader.is_valid() ) {
		return;
	}

	Array param_list;
	if ( m_authoringState.m_bShaderOverrideEnabled && m_authoringState.m_shaderOverride.is_valid() ) {
		param_list = m_authoringState.m_shaderOverride->get_shader_uniform_list( true );
	} else {
		param_list = m_shaderConfig.m_shader->get_shader_uniform_list( true );
		++m_runtimeState.m_nShaderParameterListQueryCount;
	}
	const int buffer_param = param_list.size();
	if ( m_authoringState.m_bBufferShaderOverrideEnabled && m_authoringState.m_bufferShaderOverride.is_valid() ) {
		param_list.append_array( m_authoringState.m_bufferShaderOverride->get_shader_uniform_list( true ) );
	} else {
		if ( m_pTerrain->getTessellationLevel() > 0 ) {
			param_list.append_array( m_shaderConfig.m_bufferShader->get_shader_uniform_list( true ) );
			++m_runtimeState.m_nShaderParameterListQueryCount;
		}
	}

	TypedArray<StringName> new_active_params;
	Dictionary grouped_params;
	StringName current_group = StringName( "shader_uniforms.general" );
	grouped_params[current_group] = Array();
	for ( int i = 0; i < param_list.size(); i++ ) {
		Dictionary dict = param_list[i];
		const StringName name = dict["name"];

		// An empty name indicates a group being closed, reset to the "general" group.
		if ( name.is_empty() && i < buffer_param ) {
			current_group = StringName( "shader_uniforms.general" );
		}

		// Filter out private uniforms that start with _ and nulls
		if ( !name.begins_with( "_" ) && !name.is_empty() ) {
			const uint64_t use = dict["usage"];
			if ( use == PROPERTY_USAGE_GROUP ) {
				PackedStringArray split_name = name.split( "::" );
				dict["name"] = split_name[MAX( split_name.size() - 1, 0 )].capitalize();
				// Ensure sub groups are batched with their parent group
				current_group = split_name[0].capitalize();
				dict["usage"] = name.contains( "::" ) ? PROPERTY_USAGE_SUBGROUP : PROPERTY_USAGE_GROUP;
			} else {
				// Filter out duplicate non-groups entries from displacement buffer shader
				if ( new_active_params.has( name ) ) {
					continue;
				}
				dict["usage"] = PROPERTY_USAGE_EDITOR;
			}
			// Filter out extraneous parameters from the inspector if they are not present in the terrain shader.
			if ( i >= buffer_param && ( !new_active_params.has( name ) && use != PROPERTY_USAGE_GROUP )
			        && !current_group.contains( "Displacement" ) ) {
				LOG( INFO, "Displacement buffer has active parameter: ", name, " not present in terrain shader." );
				continue;
			}
			// Write dict to grouped arrays to ensure property list groups are populated contigiously.
			Array group;
			if ( grouped_params.has( current_group ) ) {
				group = grouped_params[current_group];
			} else {
				grouped_params[current_group] = Array();
				group = grouped_params[current_group];
			}
			group.push_back( dict );
			grouped_params[current_group] = group;

			// Populate list of public parameters for current shader.
			new_active_params.push_back( name );
		}
	}
	m_runtimeState.m_activeParams = new_active_params;
	Ref<ShaderMaterial> near_defaults;
	if ( m_shaderConfig.m_shader.is_valid() ) {
		near_defaults.instantiate();
		near_defaults->set_shader( m_shaderConfig.m_shader );
	}
	Ref<ShaderMaterial> buffer_defaults;
	if ( m_shaderConfig.m_bufferShader.is_valid() ) {
		buffer_defaults.instantiate();
		buffer_defaults->set_shader( m_shaderConfig.m_bufferShader );
	}
	for ( size_t iParam = 0u; iParam < ( size_t )( ( m_runtimeState.m_activeParams ).size() ); iParam++ ) {
		const StringName name = ( m_runtimeState.m_activeParams )[iParam];
		Variant default_value;
		if ( near_defaults.is_valid() ) {
			default_value = near_defaults->get_shader_parameter( name );
			++m_runtimeState.m_nShaderParameterDefaultQueryCount;
		}
		if ( default_value.get_type() == Variant::NIL && buffer_defaults.is_valid() ) {
			default_value = buffer_defaults->get_shader_parameter( name );
			++m_runtimeState.m_nShaderParameterDefaultQueryCount;
		}
		m_authoringState.m_shaderParamDefaults[name] = default_value;
		if ( !m_authoringState.m_shaderParams.has( name ) ) {
			m_authoringState.m_shaderParams[name] = default_value;
		}
	}

	// Preserve the grouped inspector order in a stable local cache.
	Array keys = grouped_params.keys();
	for ( int i = 0; i < keys.size(); i++ ) {
		const StringName key = keys[i];
		Array group = grouped_params[key];
		for ( int k = 0; k < group.size(); k++ ) {
			Dictionary dict = group[k];
			PropertyInfo pi;
			pi.class_name = dict["class_name"];
			pi.name = dict["name"];
			pi.type = Variant::Type( ( int )( dict["type"] ) );
			pi.hint = dict["hint"];
			pi.hint_string = dict["hint_string"];
			pi.usage = dict["usage"];
			m_authoringState.m_shaderPropertyCache.push_back( pi );
		}
	}
}

// Add cached shader uniforms to properties. Shader reflection is event-driven
// from _update_shader(), never from idle inspector enumeration.
void Terrain3DMaterial::_get_property_list( List<PropertyInfo> *const pList ) const {
	Resource::_get_property_list( pList );
	IS_INIT( VOID );
	for ( size_t iProperty = 0u; iProperty < ( size_t )( ( m_authoringState.m_shaderPropertyCache ).size() ); iProperty++ ) {
		const PropertyInfo &property = ( m_authoringState.m_shaderPropertyCache )[iProperty];
		pList->push_back( property );
	}
}

// Flag uniforms with non-default values
// This is called 10x more than the others, so be efficient
bool Terrain3DMaterial::_property_can_revert( const StringName &strName ) const {
	IS_INIT_COND( !m_runtimeState.m_activeParams.has( strName ), Resource::_property_can_revert( strName ) );
	const Variant default_value = m_authoringState.m_shaderParamDefaults.get( strName, Variant() );
	const Variant current_value = m_authoringState.m_shaderParams.get( strName, default_value );
	return default_value != current_value;
}

// Provide uniform default values in r_property
bool Terrain3DMaterial::_property_get_revert( const StringName &strName, Variant &rProperty ) const {
	IS_INIT_COND( !m_runtimeState.m_activeParams.has( strName ), Resource::_property_get_revert( strName, rProperty ) );
	rProperty = m_authoringState.m_shaderParamDefaults.get( strName, Variant() );
	return true;
}

bool Terrain3DMaterial::_set( const StringName &strName, const Variant &rProperty ) {
	IS_INIT_COND( !m_runtimeState.m_activeParams.has( strName ), Resource::_set( strName, rProperty ) );
	m_authoringState.m_bPersistenceDirty = true;
	if ( rProperty.get_type() == Variant::NIL ) {
		setRuntimeMaterialParam( strName, Variant() );
		m_authoringState.m_shaderParams.erase( strName );
		return true;
	}

	// Preserve Resource objects in authoring state; the RD coordinator resolves
	// texture RIDs when publishing an immutable render snapshot.
	if ( rProperty.get_type() == Variant::OBJECT ) {
		const Ref<Texture> pTex = rProperty;
		if ( pTex.is_valid() ) {
			m_authoringState.m_shaderParams[strName] = pTex;
			setRuntimeMaterialParam( strName, pTex );
		} else {
			setRuntimeMaterialParam( strName, Variant() );
		}
	} else {
		m_authoringState.m_shaderParams[strName] = rProperty;
		setRuntimeMaterialParam( strName, rProperty );
		if ( strName == StringName( "noise1_angle" ) ) {
			const real_t fAngle = rProperty;
			const Vector2 rotation( Math::cos( fAngle ), Math::sin( fAngle ) );
			setRuntimeMaterialParam( "_noise1_rotation", rotation );
		}
	}
	return true;
}

// This is called continuously while the material is open in the inspector. Keep
// it local: shader reflection belongs to shader-change
// events, not idle editor frames.
bool Terrain3DMaterial::_get( const StringName &strName, Variant &rProperty ) const {
	IS_INIT_COND( !m_runtimeState.m_activeParams.has( strName ), Resource::_get( strName, rProperty ) );
	++m_runtimeState.m_nShaderInspectorRuntimeReadCount;
	rProperty = m_authoringState.m_shaderParams.get( strName, m_authoringState.m_shaderParamDefaults.get( strName, Variant() ) );
	return true;
}

void Terrain3DMaterial::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( NONE );
	BIND_ENUM_CONSTANT( FLAT );
	BIND_ENUM_CONSTANT( NOISE );
	BIND_ENUM_CONSTANT( LINEAR_ANISOTROPIC );
	BIND_ENUM_CONSTANT( LINEAR );
	BIND_ENUM_CONSTANT( NEAREST_ANISOTROPIC );
	BIND_ENUM_CONSTANT( NEAREST );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_DISABLED );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_ON_DEMAND );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_PREWARM_ACTIVE_BRANCHES );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_PREWARM_ACTIVE_AND_RECENT );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_PREWARM_FEATURE_MATRIX );
	BIND_ENUM_CONSTANT( SHADER_VARIANTS_PREWARM_COMMON );
	BIND_ENUM_CONSTANT( SHADER_PRESET_CUSTOM );
	BIND_ENUM_CONSTANT( SHADER_PRESET_CINEMATIC );
	BIND_ENUM_CONSTANT( SHADER_PRESET_HIGH );
	BIND_ENUM_CONSTANT( SHADER_PRESET_RUNTIME_360 );
	BIND_ENUM_CONSTANT( SHADER_PRESET_LOW );
	BIND_ENUM_CONSTANT( SHADER_PRESET_MINIMUM );
	BIND_ENUM_CONSTANT( SHADER_PRESET_XBOX_360_AUTHENTIC );
	BIND_ENUM_CONSTANT( SHADER_PRESET_XBOX_360_STYLED );
	BIND_ENUM_CONSTANT( SHADER_PRESET_XBOX_360 );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ALWAYS_SMOOTH );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_SINGLE_SAMPLE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE_WEIGHTED );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_DISTANCE_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_SAMPLING_ADAPTIVE_2_CORNER );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_ADAPTIVE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_HEIGHT_8_TAP );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_GEOMETRY_DERIVATIVE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_DERIVED_TEXTURE );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_HEIGHT_3_TAP );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_AUTO );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_HARDWARE_FILTERED );
	BIND_ENUM_CONSTANT( COLOR_SAMPLING_EXACT_4_TAP );
	BIND_ENUM_CONSTANT( PROJECTION_DISABLED );
	BIND_ENUM_CONSTANT( PROJECTION_SHARED_FRAGMENT );
	BIND_ENUM_CONSTANT( PROJECTION_EXACT_PER_CORNER );
	BIND_ENUM_CONSTANT( TESSELLATION_GEOMETRY_ONLY );
	BIND_ENUM_CONSTANT( TESSELLATION_GEOMETRY_AND_MATERIAL_HEIGHT );
	BIND_ENUM_CONSTANT( SURFACE_DATA_AUTO );
	BIND_ENUM_CONSTANT( SURFACE_DATA_FULL );
	BIND_ENUM_CONSTANT( SURFACE_DATA_ALBEDO_ONLY );
	BIND_ENUM_CONSTANT( TEXTURE_SURFACE_TEXTURED );
	BIND_ENUM_CONSTANT( TEXTURE_SURFACE_SILHOUETTE );
	BIND_ENUM_CONSTANT( SHADING_BURLEY_GGX );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_GGX );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_DIFFUSE_ONLY );
	BIND_ENUM_CONSTANT( SHADING_UNSHADED );
	BIND_ENUM_CONSTANT( SHADING_LAMBERT_VERTEX );
	BIND_ENUM_CONSTANT( SHADING_XBOX_360_BLINN_PHONG );
	BIND_ENUM_CONSTANT( MATERIAL_SAMPLING_FAST );
	BIND_ENUM_CONSTANT( MATERIAL_SAMPLING_FULL );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_QUALITY_GEOMETRY );
	BIND_ENUM_CONSTANT( TERRAIN_NORMAL_QUALITY_DETAILED );
	BIND_ENUM_CONSTANT( TERRAIN_COLOR_QUALITY_FAST );
	BIND_ENUM_CONSTANT( TERRAIN_COLOR_QUALITY_EXACT );
	BIND_ENUM_CONSTANT( SURFACE_RESPONSE_SIMPLE );
	BIND_ENUM_CONSTANT( SURFACE_RESPONSE_DETAILED );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_UNSHADED );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_LAMBERT );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_BLINN_PHONG );
	BIND_ENUM_CONSTANT( TIER_LIGHTING_PBR );
	BIND_ENUM_CONSTANT( TIER_DETILING_DISABLED );
	BIND_ENUM_CONSTANT( TIER_DETILING_ROTATION );
	BIND_ENUM_CONSTANT( DETILING_CONTINUOUS );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_8 );
	BIND_ENUM_CONSTANT( DETILING_DISABLED );
	BIND_ENUM_CONSTANT( DETILING_TRANSLATION_ONLY );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_4 );
	BIND_ENUM_CONSTANT( DETILING_DISCRETE_16 );
	BIND_ENUM_CONSTANT( WORLD_NOISE_DYNAMIC );
	BIND_ENUM_CONSTANT( WORLD_NOISE_CACHED_VERTEX_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_CACHED_FRAGMENT_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_PROCEDURAL_VERTEX_NORMALS );
	BIND_ENUM_CONSTANT( WORLD_NOISE_PROCEDURAL_FRAGMENT_NORMALS );
	BIND_ENUM_CONSTANT( UNIFORMS_ONLY );
	BIND_ENUM_CONSTANT( TEXTURE_ARRAYS );
	BIND_ENUM_CONSTANT( REGION_ARRAYS );
	BIND_ENUM_CONSTANT( UPDATE_ARRAYS );
	BIND_ENUM_CONSTANT( FULL_REBUILD );

	// Private
	ClassDB::bind_method( D_METHOD( "_set_shader_parameters", "dict" ), &Terrain3DMaterial::setShaderParameters );
	ClassDB::bind_method( D_METHOD( "_get_shader_parameters" ), &Terrain3DMaterial::getShaderParameters );
	ADD_PROPERTY( PropertyInfo( Variant::DICTIONARY, "_shader_parameters", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "_set_shader_parameters",
	        "_get_shader_parameters" );

	// Public
	ClassDB::bind_method( D_METHOD( "update", "flags" ), &Terrain3DMaterial::update, DEFVAL( UNIFORMS_ONLY ) );
	ClassDB::bind_method( D_METHOD( "update_editor_overlay" ), &Terrain3DMaterial::updateEditorOverlay );
	ClassDB::bind_method( D_METHOD( "is_render_shader_ready" ), &Terrain3DMaterial::isRenderShaderReady );
	ClassDB::bind_method( D_METHOD( "is_region_page_table_valid" ), &Terrain3DMaterial::isRegionPageTableValid );
	ClassDB::bind_method( D_METHOD( "get_shader_rid" ), &Terrain3DMaterial::getShaderRid );
	ClassDB::bind_method( D_METHOD( "get_buffer_shader_rid" ), &Terrain3DMaterial::getBufferShaderRid );

	ClassDB::bind_method( D_METHOD( "set_world_background", "background" ), &Terrain3DMaterial::setWorldBackground );
	ClassDB::bind_method( D_METHOD( "get_world_background" ), &Terrain3DMaterial::getWorldBackground );
	ClassDB::bind_method( D_METHOD( "set_texture_filtering", "filtering" ), &Terrain3DMaterial::setTextureFiltering );
	ClassDB::bind_method( D_METHOD( "get_texture_filtering" ), &Terrain3DMaterial::getTextureFiltering );
	ClassDB::bind_method( D_METHOD( "set_shader_variant_mode", "mode" ), &Terrain3DMaterial::setShaderVariantMode );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_mode" ), &Terrain3DMaterial::getShaderVariantMode );
	ClassDB::bind_method( D_METHOD( "set_shader_variant_cache_limit", "limit" ), &Terrain3DMaterial::setShaderVariantCacheLimit );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_cache_limit" ), &Terrain3DMaterial::getShaderVariantCacheLimit );
	ClassDB::bind_method( D_METHOD( "set_shader_variant_prewarm_limit", "limit" ), &Terrain3DMaterial::setShaderVariantPrewarmLimit );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_prewarm_limit" ), &Terrain3DMaterial::getShaderVariantPrewarmLimit );
	ClassDB::bind_method(
	        D_METHOD( "set_shader_variant_source_budget_mb", "budget_mb" ), &Terrain3DMaterial::setShaderVariantSourceBudgetMb );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_source_budget_mb" ), &Terrain3DMaterial::getShaderVariantSourceBudgetMb );
	ClassDB::bind_method( D_METHOD( "set_shader_performance_preset", "preset" ), &Terrain3DMaterial::setShaderPerformancePreset );
	ClassDB::bind_method( D_METHOD( "get_shader_performance_preset" ), &Terrain3DMaterial::getShaderPerformancePreset );
	ClassDB::bind_method( D_METHOD( "set_near_texture_filtering", "filtering" ), &Terrain3DMaterial::setNearTextureFiltering );
	ClassDB::bind_method( D_METHOD( "get_near_texture_filtering" ), &Terrain3DMaterial::getNearTextureFiltering );
	ClassDB::bind_method( D_METHOD( "set_near_material_sampling_quality", "quality" ), &Terrain3DMaterial::setNearMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "get_near_material_sampling_quality" ), &Terrain3DMaterial::getNearMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "set_near_terrain_normal_quality", "quality" ), &Terrain3DMaterial::setNearTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "get_near_terrain_normal_quality" ), &Terrain3DMaterial::getNearTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "set_near_terrain_color_quality", "quality" ), &Terrain3DMaterial::setNearTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "get_near_terrain_color_quality" ), &Terrain3DMaterial::getNearTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "set_near_surface_response_quality", "quality" ), &Terrain3DMaterial::setNearSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "get_near_surface_response_quality" ), &Terrain3DMaterial::getNearSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "set_near_texture_surface_mode", "mode" ), &Terrain3DMaterial::setNearTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "get_near_texture_surface_mode" ), &Terrain3DMaterial::getNearTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "set_near_lighting_model", "model" ), &Terrain3DMaterial::setNearLightingModel );
	ClassDB::bind_method( D_METHOD( "get_near_lighting_model" ), &Terrain3DMaterial::getNearLightingModel );
	ClassDB::bind_method( D_METHOD( "set_near_detiling_quality", "quality" ), &Terrain3DMaterial::setNearDetilingQuality );
	ClassDB::bind_method( D_METHOD( "get_near_detiling_quality" ), &Terrain3DMaterial::getNearDetilingQuality );
	ClassDB::bind_method(
	        D_METHOD( "set_near_authored_color_contribution", "contribution" ), &Terrain3DMaterial::setNearAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "get_near_authored_color_contribution" ), &Terrain3DMaterial::getNearAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "set_near_wetness_contribution", "contribution" ), &Terrain3DMaterial::setNearWetnessContribution );
	ClassDB::bind_method( D_METHOD( "get_near_wetness_contribution" ), &Terrain3DMaterial::getNearWetnessContribution );
	ClassDB::bind_method( D_METHOD( "set_near_material_lod_bias", "bias" ), &Terrain3DMaterial::setNearMaterialLodBias );
	ClassDB::bind_method( D_METHOD( "get_near_material_lod_bias" ), &Terrain3DMaterial::getNearMaterialLodBias );

	ClassDB::bind_method(
	        D_METHOD( "set_middle_material_sampling_quality", "quality" ), &Terrain3DMaterial::setMiddleMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "get_middle_material_sampling_quality" ), &Terrain3DMaterial::getMiddleMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "set_middle_surface_source", "source" ), &Terrain3DMaterial::setMiddleSurfaceSource );
	ClassDB::bind_method( D_METHOD( "get_middle_surface_source" ), &Terrain3DMaterial::getMiddleSurfaceSource );
	ClassDB::bind_method( D_METHOD( "set_middle_terrain_normal_quality", "quality" ), &Terrain3DMaterial::setMiddleTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "get_middle_terrain_normal_quality" ), &Terrain3DMaterial::getMiddleTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "set_middle_terrain_color_quality", "quality" ), &Terrain3DMaterial::setMiddleTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "get_middle_terrain_color_quality" ), &Terrain3DMaterial::getMiddleTerrainColorQuality );
	ClassDB::bind_method(
	        D_METHOD( "set_middle_surface_response_quality", "quality" ), &Terrain3DMaterial::setMiddleSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "get_middle_surface_response_quality" ), &Terrain3DMaterial::getMiddleSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "set_middle_lighting_model", "model" ), &Terrain3DMaterial::setMiddleLightingModel );
	ClassDB::bind_method( D_METHOD( "get_middle_lighting_model" ), &Terrain3DMaterial::getMiddleLightingModel );
	ClassDB::bind_method( D_METHOD( "set_middle_detiling_quality", "quality" ), &Terrain3DMaterial::setMiddleDetilingQuality );
	ClassDB::bind_method( D_METHOD( "get_middle_detiling_quality" ), &Terrain3DMaterial::getMiddleDetilingQuality );
	ClassDB::bind_method(
	        D_METHOD( "set_middle_authored_color_contribution", "contribution" ), &Terrain3DMaterial::setMiddleAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "get_middle_authored_color_contribution" ), &Terrain3DMaterial::getMiddleAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "set_middle_wetness_contribution", "contribution" ), &Terrain3DMaterial::setMiddleWetnessContribution );
	ClassDB::bind_method( D_METHOD( "get_middle_wetness_contribution" ), &Terrain3DMaterial::getMiddleWetnessContribution );

	ClassDB::bind_method( D_METHOD( "set_far_material_sampling_quality", "quality" ), &Terrain3DMaterial::setFarMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "get_far_material_sampling_quality" ), &Terrain3DMaterial::getFarMaterialSamplingQuality );
	ClassDB::bind_method( D_METHOD( "set_far_terrain_normal_quality", "quality" ), &Terrain3DMaterial::setFarTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "get_far_terrain_normal_quality" ), &Terrain3DMaterial::getFarTerrainNormalQuality );
	ClassDB::bind_method( D_METHOD( "set_far_terrain_color_quality", "quality" ), &Terrain3DMaterial::setFarTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "get_far_terrain_color_quality" ), &Terrain3DMaterial::getFarTerrainColorQuality );
	ClassDB::bind_method( D_METHOD( "set_far_surface_response_quality", "quality" ), &Terrain3DMaterial::setFarSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "get_far_surface_response_quality" ), &Terrain3DMaterial::getFarSurfaceResponseQuality );
	ClassDB::bind_method( D_METHOD( "set_far_lighting_model", "model" ), &Terrain3DMaterial::setFarLightingModel );
	ClassDB::bind_method( D_METHOD( "get_far_lighting_model" ), &Terrain3DMaterial::getFarLightingModel );
	ClassDB::bind_method( D_METHOD( "set_far_detiling_quality", "quality" ), &Terrain3DMaterial::setFarDetilingQuality );
	ClassDB::bind_method( D_METHOD( "get_far_detiling_quality" ), &Terrain3DMaterial::getFarDetilingQuality );
	ClassDB::bind_method(
	        D_METHOD( "set_far_authored_color_contribution", "contribution" ), &Terrain3DMaterial::setFarAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "get_far_authored_color_contribution" ), &Terrain3DMaterial::getFarAuthoredColorContribution );
	ClassDB::bind_method( D_METHOD( "set_far_wetness_contribution", "contribution" ), &Terrain3DMaterial::setFarWetnessContribution );
	ClassDB::bind_method( D_METHOD( "get_far_wetness_contribution" ), &Terrain3DMaterial::getFarWetnessContribution );
	ClassDB::bind_method( D_METHOD( "set_far_material_lod_bias", "bias" ), &Terrain3DMaterial::setFarMaterialLodBias );
	ClassDB::bind_method( D_METHOD( "get_far_material_lod_bias" ), &Terrain3DMaterial::getFarMaterialLodBias );

	ClassDB::bind_method( D_METHOD( "get_near_range_context" ), &Terrain3DMaterial::getNearRangeContext );
	ClassDB::bind_method( D_METHOD( "get_middle_range_context" ), &Terrain3DMaterial::getMiddleRangeContext );
	ClassDB::bind_method( D_METHOD( "get_far_range_context" ), &Terrain3DMaterial::getFarRangeContext );
	ClassDB::bind_method( D_METHOD( "get_near_shadow_context" ), &Terrain3DMaterial::getNearShadowContext );
	ClassDB::bind_method( D_METHOD( "get_middle_shadow_context" ), &Terrain3DMaterial::getMiddleShadowContext );
	ClassDB::bind_method( D_METHOD( "get_far_shadow_context" ), &Terrain3DMaterial::getFarShadowContext );
	ClassDB::bind_method( D_METHOD( "get_near_shader_status" ), &Terrain3DMaterial::getNearShaderStatus );
	ClassDB::bind_method( D_METHOD( "get_middle_shader_status" ), &Terrain3DMaterial::getMiddleShaderStatus );
	ClassDB::bind_method( D_METHOD( "get_far_shader_status" ), &Terrain3DMaterial::getFarShaderStatus );
	ClassDB::bind_method( D_METHOD( "set_middle_texture_filtering", "filtering" ), &Terrain3DMaterial::setMiddleTextureFiltering );
	ClassDB::bind_method( D_METHOD( "get_middle_texture_filtering" ), &Terrain3DMaterial::getMiddleTextureFiltering );
	ClassDB::bind_method( D_METHOD( "set_middle_terrain_sampling_mode", "mode" ), &Terrain3DMaterial::setMiddleTerrainSamplingMode );
	ClassDB::bind_method( D_METHOD( "get_middle_terrain_sampling_mode" ), &Terrain3DMaterial::getMiddleTerrainSamplingMode );
	ClassDB::bind_method( D_METHOD( "set_middle_terrain_normal_mode", "mode" ), &Terrain3DMaterial::setMiddleTerrainNormalMode );
	ClassDB::bind_method( D_METHOD( "get_middle_terrain_normal_mode" ), &Terrain3DMaterial::getMiddleTerrainNormalMode );
	ClassDB::bind_method( D_METHOD( "set_middle_color_sampling_mode", "mode" ), &Terrain3DMaterial::setMiddleColorSamplingMode );
	ClassDB::bind_method( D_METHOD( "get_middle_color_sampling_mode" ), &Terrain3DMaterial::getMiddleColorSamplingMode );
	ClassDB::bind_method( D_METHOD( "set_middle_surface_data_mode", "mode" ), &Terrain3DMaterial::setMiddleSurfaceDataMode );
	ClassDB::bind_method( D_METHOD( "get_middle_surface_data_mode" ), &Terrain3DMaterial::getMiddleSurfaceDataMode );
	ClassDB::bind_method( D_METHOD( "set_middle_texture_surface_mode", "mode" ), &Terrain3DMaterial::setMiddleTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "get_middle_texture_surface_mode" ), &Terrain3DMaterial::getMiddleTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "set_middle_terrain_shading_mode", "mode" ), &Terrain3DMaterial::setMiddleTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "get_middle_terrain_shading_mode" ), &Terrain3DMaterial::getMiddleTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "set_middle_detiling_mode", "mode" ), &Terrain3DMaterial::setMiddleDetilingMode );
	ClassDB::bind_method( D_METHOD( "get_middle_detiling_mode" ), &Terrain3DMaterial::getMiddleDetilingMode );
	ClassDB::bind_method( D_METHOD( "set_middle_auto_shader_enabled", "enabled" ), &Terrain3DMaterial::setMiddleAutoShaderEnabled );
	ClassDB::bind_method( D_METHOD( "get_middle_auto_shader_enabled" ), &Terrain3DMaterial::getMiddleAutoShaderEnabled );
	ClassDB::bind_method( D_METHOD( "set_middle_authored_color_enabled", "enabled" ), &Terrain3DMaterial::setMiddleAuthoredColorEnabled );
	ClassDB::bind_method( D_METHOD( "get_middle_authored_color_enabled" ), &Terrain3DMaterial::getMiddleAuthoredColorEnabled );
	ClassDB::bind_method( D_METHOD( "set_middle_wetness_enabled", "enabled" ), &Terrain3DMaterial::setMiddleWetnessEnabled );
	ClassDB::bind_method( D_METHOD( "get_middle_wetness_enabled" ), &Terrain3DMaterial::getMiddleWetnessEnabled );
	ClassDB::bind_method( D_METHOD( "set_middle_material_lod_bias", "bias" ), &Terrain3DMaterial::setMiddleMaterialLodBias );
	ClassDB::bind_method( D_METHOD( "get_middle_material_lod_bias" ), &Terrain3DMaterial::getMiddleMaterialLodBias );
	ClassDB::bind_method( D_METHOD( "set_far_texture_filtering", "filtering" ), &Terrain3DMaterial::setFarTextureFiltering );
	ClassDB::bind_method( D_METHOD( "get_far_texture_filtering" ), &Terrain3DMaterial::getFarTextureFiltering );
	ClassDB::bind_method( D_METHOD( "set_far_texture_surface_mode", "mode" ), &Terrain3DMaterial::setFarTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "get_far_texture_surface_mode" ), &Terrain3DMaterial::getFarTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "set_far_terrain_shading_mode", "mode" ), &Terrain3DMaterial::setFarTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "get_far_terrain_shading_mode" ), &Terrain3DMaterial::getFarTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "set_far_material_lod", "lod" ), &Terrain3DMaterial::setFarMaterialLod );
	ClassDB::bind_method( D_METHOD( "get_far_material_lod" ), &Terrain3DMaterial::getFarMaterialLod );
	ClassDB::bind_method( D_METHOD( "set_far_authored_color_enabled", "enabled" ), &Terrain3DMaterial::setFarAuthoredColorEnabled );
	ClassDB::bind_method( D_METHOD( "get_far_authored_color_enabled" ), &Terrain3DMaterial::getFarAuthoredColorEnabled );
	ClassDB::bind_method( D_METHOD( "set_far_wetness_enabled", "enabled" ), &Terrain3DMaterial::setFarWetnessEnabled );
	ClassDB::bind_method( D_METHOD( "get_far_wetness_enabled" ), &Terrain3DMaterial::getFarWetnessEnabled );
	ClassDB::bind_method( D_METHOD( "set_terrain_sampling_mode", "mode" ), &Terrain3DMaterial::setTerrainSamplingMode );
	ClassDB::bind_method( D_METHOD( "get_terrain_sampling_mode" ), &Terrain3DMaterial::getTerrainSamplingMode );
	ClassDB::bind_method( D_METHOD( "set_terrain_normal_mode", "mode" ), &Terrain3DMaterial::setTerrainNormalMode );
	ClassDB::bind_method( D_METHOD( "get_terrain_normal_mode" ), &Terrain3DMaterial::getTerrainNormalMode );
	ClassDB::bind_method( D_METHOD( "set_color_sampling_mode", "mode" ), &Terrain3DMaterial::setColorSamplingMode );
	ClassDB::bind_method( D_METHOD( "get_color_sampling_mode" ), &Terrain3DMaterial::getColorSamplingMode );
	ClassDB::bind_method( D_METHOD( "set_projection_mode", "mode" ), &Terrain3DMaterial::setProjectionMode );
	ClassDB::bind_method( D_METHOD( "get_projection_mode" ), &Terrain3DMaterial::getProjectionMode );
	ClassDB::bind_method( D_METHOD( "set_tessellation_material_mode", "mode" ), &Terrain3DMaterial::setTessellationMaterialMode );
	ClassDB::bind_method( D_METHOD( "get_tessellation_material_mode" ), &Terrain3DMaterial::getTessellationMaterialMode );
	ClassDB::bind_method( D_METHOD( "set_surface_data_mode", "mode" ), &Terrain3DMaterial::setSurfaceDataMode );
	ClassDB::bind_method( D_METHOD( "get_surface_data_mode" ), &Terrain3DMaterial::getSurfaceDataMode );
	ClassDB::bind_method( D_METHOD( "set_texture_surface_mode", "mode" ), &Terrain3DMaterial::setTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "get_texture_surface_mode" ), &Terrain3DMaterial::getTextureSurfaceMode );
	ClassDB::bind_method( D_METHOD( "set_terrain_shading_mode", "mode" ), &Terrain3DMaterial::setTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "get_terrain_shading_mode" ), &Terrain3DMaterial::getTerrainShadingMode );
	ClassDB::bind_method( D_METHOD( "set_detiling_mode", "mode" ), &Terrain3DMaterial::setDetilingMode );
	ClassDB::bind_method( D_METHOD( "get_detiling_mode" ), &Terrain3DMaterial::getDetilingMode );
	ClassDB::bind_method( D_METHOD( "set_world_noise_shader_mode", "mode" ), &Terrain3DMaterial::setWorldNoiseShaderMode );
	ClassDB::bind_method( D_METHOD( "get_world_noise_shader_mode" ), &Terrain3DMaterial::getWorldNoiseShaderMode );
	ClassDB::bind_method( D_METHOD( "prewarm_shader_variants" ), &Terrain3DMaterial::prewarmShaderVariants );
	ClassDB::bind_method( D_METHOD( "clear_shader_variant_cache" ), &Terrain3DMaterial::clearShaderVariantCache );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_count" ), &Terrain3DMaterial::getShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_runtime_shader_variant_count" ), &Terrain3DMaterial::getRuntimeShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_editor_shader_variant_count" ), &Terrain3DMaterial::getEditorShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_buffer_shader_variant_count" ), &Terrain3DMaterial::getBufferShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_speculative_shader_variant_count" ), &Terrain3DMaterial::getSpeculativeShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_source_bytes" ), &Terrain3DMaterial::getShaderVariantSourceBytes );
	ClassDB::bind_method( D_METHOD( "get_last_prewarm_variant_count" ), &Terrain3DMaterial::getLastPrewarmVariantCount );
	ClassDB::bind_method( D_METHOD( "get_last_prewarm_skipped_count" ), &Terrain3DMaterial::getLastPrewarmSkippedCount );
	ClassDB::bind_method( D_METHOD( "get_theoretical_shader_variant_count" ), &Terrain3DMaterial::getTheoreticalShaderVariantCount );
	ClassDB::bind_method( D_METHOD( "get_shader_variant_diagnostics" ), &Terrain3DMaterial::getShaderVariantDiagnostics );
	ClassDB::bind_method( D_METHOD( "get_shader_tier_diagnostics", "tier" ), &Terrain3DMaterial::getShaderTierDiagnostics );
	ClassDB::bind_method( D_METHOD( "reset_shader_variant_diagnostics" ), &Terrain3DMaterial::resetShaderVariantDiagnostics );
	ClassDB::bind_method(
	        D_METHOD( "get_shader_regression_source", "tier", "assembly_path", "control_maps_implicit", "color_maps_implicit" ),
	        &Terrain3DMaterial::getShaderRegressionSource );
#ifdef DEBUG_ENABLED
	ClassDB::bind_method( D_METHOD( "set_shader_regression_invalid_tier", "tier" ), &Terrain3DMaterial::setShaderRegressionInvalidTier );
	ClassDB::bind_method(
	        D_METHOD( "get_tier_regression_material_param", "tier", "name" ), &Terrain3DMaterial::getTierRegressionMaterialParam );
#endif
	ClassDB::bind_method( D_METHOD( "set_packed_pbr_single_fetch", "enabled" ), &Terrain3DMaterial::setPackedPbrSingleFetch );
	ClassDB::bind_method( D_METHOD( "get_packed_pbr_single_fetch" ), &Terrain3DMaterial::getPackedPbrSingleFetch );
	ClassDB::bind_method( D_METHOD( "set_auto_shader_enabled", "enabled" ), &Terrain3DMaterial::setAutoShaderEnabled );
	ClassDB::bind_method( D_METHOD( "set_auto_shader_enabled_transient", "enabled" ), &Terrain3DMaterial::setAutoShaderEnabledTransient );
	ClassDB::bind_method( D_METHOD( "get_auto_shader_enabled" ), &Terrain3DMaterial::getAutoShaderEnabled );
	ClassDB::bind_method( D_METHOD( "set_dual_scaling_enabled", "enabled" ), &Terrain3DMaterial::setDualScalingEnabled );
	ClassDB::bind_method( D_METHOD( "get_dual_scaling_enabled" ), &Terrain3DMaterial::getDualScalingEnabled );
	ClassDB::bind_method( D_METHOD( "set_macro_variation_enabled", "enabled" ), &Terrain3DMaterial::setMacroVariationEnabled );
	ClassDB::bind_method( D_METHOD( "get_macro_variation_enabled" ), &Terrain3DMaterial::getMacroVariationEnabled );
	ClassDB::bind_method( D_METHOD( "set_projection_enabled", "enabled" ), &Terrain3DMaterial::setProjectionEnabled );
	ClassDB::bind_method( D_METHOD( "get_projection_enabled" ), &Terrain3DMaterial::getProjectionEnabled );

	ClassDB::bind_method( D_METHOD( "set_shader_override_enabled", "enabled" ), &Terrain3DMaterial::setShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "is_shader_override_enabled" ), &Terrain3DMaterial::isShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "set_shader_override", "shader" ), &Terrain3DMaterial::setShaderOverride );
	ClassDB::bind_method( D_METHOD( "get_shader_override" ), &Terrain3DMaterial::getShaderOverride );

	ClassDB::bind_method( D_METHOD( "set_buffer_shader_override_enabled", "enabled" ), &Terrain3DMaterial::setBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "is_buffer_shader_override_enabled" ), &Terrain3DMaterial::isBufferShaderOverrideEnabled );
	ClassDB::bind_method( D_METHOD( "set_buffer_shader_override", "shader" ), &Terrain3DMaterial::setBufferShaderOverride );
	ClassDB::bind_method( D_METHOD( "get_buffer_shader_override" ), &Terrain3DMaterial::getBufferShaderOverride );
	ClassDB::bind_method( D_METHOD( "set_displacement_scale", "scale" ), &Terrain3DMaterial::setDisplacementScale );
	ClassDB::bind_method( D_METHOD( "get_displacement_scale" ), &Terrain3DMaterial::getDisplacementScale );
	ClassDB::bind_method( D_METHOD( "set_displacement_sharpness", "sharpness" ), &Terrain3DMaterial::setDisplacementSharpness );
	ClassDB::bind_method( D_METHOD( "get_displacement_sharpness" ), &Terrain3DMaterial::getDisplacementSharpness );

	ClassDB::bind_method( D_METHOD( "set_shader_param", "name", "value" ), &Terrain3DMaterial::setShaderParam );
	ClassDB::bind_method( D_METHOD( "get_shader_param", "name" ), &Terrain3DMaterial::getShaderParam );

	// PBR output
	ClassDB::bind_method( D_METHOD( "set_output_albedo_enabled", "enabled" ), &Terrain3DMaterial::setOutputAlbedoEnabled );
	ClassDB::bind_method( D_METHOD( "get_output_albedo_enabled" ), &Terrain3DMaterial::getOutputAlbedoEnabled );
	ClassDB::bind_method( D_METHOD( "set_output_roughness_enabled", "enabled" ), &Terrain3DMaterial::setOutputRoughnessEnabled );
	ClassDB::bind_method( D_METHOD( "get_output_roughness_enabled" ), &Terrain3DMaterial::getOutputRoughnessEnabled );
	ClassDB::bind_method( D_METHOD( "set_output_specular_enabled", "enabled" ), &Terrain3DMaterial::setOutputSpecularEnabled );
	ClassDB::bind_method( D_METHOD( "get_output_specular_enabled" ), &Terrain3DMaterial::getOutputSpecularEnabled );
	ClassDB::bind_method( D_METHOD( "set_output_normal_map_enabled", "enabled" ), &Terrain3DMaterial::setOutputNormalMapEnabled );
	ClassDB::bind_method( D_METHOD( "get_output_normal_map_enabled" ), &Terrain3DMaterial::getOutputNormalMapEnabled );
	ClassDB::bind_method(
	        D_METHOD( "set_output_ambient_occlusion_enabled", "enabled" ), &Terrain3DMaterial::setOutputAmbientOcclusionEnabled );
	ClassDB::bind_method( D_METHOD( "get_output_ambient_occlusion_enabled" ), &Terrain3DMaterial::getOutputAmbientOcclusionEnabled );

	// Overlays
	ClassDB::bind_method( D_METHOD( "set_show_region_grid", "enabled" ), &Terrain3DMaterial::setShowRegionGrid );
	ClassDB::bind_method( D_METHOD( "get_show_region_grid" ), &Terrain3DMaterial::getShowRegionGrid );
	ClassDB::bind_method( D_METHOD( "set_show_instancer_grid", "enabled" ), &Terrain3DMaterial::setShowInstancerGrid );
	ClassDB::bind_method( D_METHOD( "get_show_instancer_grid" ), &Terrain3DMaterial::getShowInstancerGrid );
	ClassDB::bind_method( D_METHOD( "set_show_vertex_grid", "enabled" ), &Terrain3DMaterial::setShowVertexGrid );
	ClassDB::bind_method( D_METHOD( "get_show_vertex_grid" ), &Terrain3DMaterial::getShowVertexGrid );
	ClassDB::bind_method( D_METHOD( "set_show_contours", "enabled" ), &Terrain3DMaterial::setShowContours );
	ClassDB::bind_method( D_METHOD( "get_show_contours" ), &Terrain3DMaterial::getShowContours );
	ClassDB::bind_method( D_METHOD( "set_show_navigation", "enabled" ), &Terrain3DMaterial::setShowNavigation );
	ClassDB::bind_method( D_METHOD( "get_show_navigation" ), &Terrain3DMaterial::getShowNavigation );

	// Debug Views
	ClassDB::bind_method( D_METHOD( "set_show_checkered", "enabled" ), &Terrain3DMaterial::setShowCheckered );
	ClassDB::bind_method( D_METHOD( "get_show_checkered" ), &Terrain3DMaterial::getShowCheckered );
	ClassDB::bind_method( D_METHOD( "set_show_grey", "enabled" ), &Terrain3DMaterial::setShowGrey );
	ClassDB::bind_method( D_METHOD( "get_show_grey" ), &Terrain3DMaterial::getShowGrey );
	ClassDB::bind_method( D_METHOD( "set_show_heightmap", "enabled" ), &Terrain3DMaterial::setShowHeightmap );
	ClassDB::bind_method( D_METHOD( "get_show_heightmap" ), &Terrain3DMaterial::getShowHeightmap );
	ClassDB::bind_method( D_METHOD( "set_show_jaggedness", "enabled" ), &Terrain3DMaterial::setShowJaggedness );
	ClassDB::bind_method( D_METHOD( "get_show_jaggedness" ), &Terrain3DMaterial::getShowJaggedness );
	ClassDB::bind_method( D_METHOD( "set_show_autoshader", "enabled" ), &Terrain3DMaterial::setShowAutoshader );
	ClassDB::bind_method( D_METHOD( "get_show_autoshader" ), &Terrain3DMaterial::getShowAutoshader );
	ClassDB::bind_method( D_METHOD( "set_show_control_texture", "enabled" ), &Terrain3DMaterial::setShowControlTexture );
	ClassDB::bind_method( D_METHOD( "get_show_control_texture" ), &Terrain3DMaterial::getShowControlTexture );
	ClassDB::bind_method( D_METHOD( "set_show_control_blend", "enabled" ), &Terrain3DMaterial::setShowControlBlend );
	ClassDB::bind_method( D_METHOD( "get_show_control_blend" ), &Terrain3DMaterial::getShowControlBlend );
	ClassDB::bind_method( D_METHOD( "set_show_control_angle", "enabled" ), &Terrain3DMaterial::setShowControlAngle );
	ClassDB::bind_method( D_METHOD( "get_show_control_angle" ), &Terrain3DMaterial::getShowControlAngle );
	ClassDB::bind_method( D_METHOD( "set_show_control_scale", "enabled" ), &Terrain3DMaterial::setShowControlScale );
	ClassDB::bind_method( D_METHOD( "get_show_control_scale" ), &Terrain3DMaterial::getShowControlScale );
	ClassDB::bind_method( D_METHOD( "set_show_colormap", "enabled" ), &Terrain3DMaterial::setShowColormap );
	ClassDB::bind_method( D_METHOD( "get_show_colormap" ), &Terrain3DMaterial::getShowColormap );
	ClassDB::bind_method( D_METHOD( "set_show_roughmap", "enabled" ), &Terrain3DMaterial::setShowRoughmap );
	ClassDB::bind_method( D_METHOD( "get_show_roughmap" ), &Terrain3DMaterial::getShowRoughmap );

	// PBR Views
	ClassDB::bind_method( D_METHOD( "set_show_texture_albedo", "enabled" ), &Terrain3DMaterial::setShowTextureAlbedo );
	ClassDB::bind_method( D_METHOD( "get_show_texture_albedo" ), &Terrain3DMaterial::getShowTextureAlbedo );
	ClassDB::bind_method( D_METHOD( "set_show_texture_height", "enabled" ), &Terrain3DMaterial::setShowTextureHeight );
	ClassDB::bind_method( D_METHOD( "get_show_texture_height" ), &Terrain3DMaterial::getShowTextureHeight );
	ClassDB::bind_method( D_METHOD( "set_show_texture_normal", "enabled" ), &Terrain3DMaterial::setShowTextureNormal );
	ClassDB::bind_method( D_METHOD( "get_show_texture_normal" ), &Terrain3DMaterial::getShowTextureNormal );
	ClassDB::bind_method( D_METHOD( "set_show_texture_ao", "enabled" ), &Terrain3DMaterial::setShowTextureAo );
	ClassDB::bind_method( D_METHOD( "get_show_texture_ao" ), &Terrain3DMaterial::getShowTextureAo );
	ClassDB::bind_method( D_METHOD( "set_show_texture_rough", "enabled" ), &Terrain3DMaterial::setShowTextureRough );
	ClassDB::bind_method( D_METHOD( "get_show_texture_rough" ), &Terrain3DMaterial::getShowTextureRough );
	ClassDB::bind_method( D_METHOD( "set_show_displacement_buffer", "enabled" ), &Terrain3DMaterial::setShowDisplacementBuffer );
	ClassDB::bind_method( D_METHOD( "get_show_displacement_buffer" ), &Terrain3DMaterial::getShowDisplacementBuffer );

	ClassDB::bind_method( D_METHOD( "save", "path" ), &Terrain3DMaterial::save, DEFVAL( "" ) );

	// Inspector groups are intentionally distinct from shader uniform-group names.

	ADD_GROUP( "Profile & Shader Cache", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
	                      "shader_performance_preset",
	                      PROPERTY_HINT_ENUM,
	                      "Custom,Cinematic,High,Xbox 360 Styled,Low,Minimum,Xbox 360 Authentic" ),
	        "set_shader_performance_preset",
	        "get_shader_performance_preset" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
	                      "shader_variant_mode",
	                      PROPERTY_HINT_ENUM,
	                      "Disabled,On Demand LRU,Prewarm Active Potential,Prewarm Active + Recent" ),
	        "set_shader_variant_mode",
	        "get_shader_variant_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shader_variant_cache_limit", PROPERTY_HINT_RANGE, "8,256,1" ),
	        "set_shader_variant_cache_limit",
	        "get_shader_variant_cache_limit" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shader_variant_prewarm_limit", PROPERTY_HINT_RANGE, "1,64,1" ),
	        "set_shader_variant_prewarm_limit",
	        "get_shader_variant_prewarm_limit" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "shader_variant_source_budget_mb", PROPERTY_HINT_RANGE, "1,256,1,suffix: MiB" ),
	        "set_shader_variant_source_budget_mb",
	        "get_shader_variant_source_budget_mb" );

	ADD_GROUP( "Common Material Policy", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "world_background", PROPERTY_HINT_ENUM, "None,Flat,Noise" ),
	        "set_world_background",
	        "get_world_background" );
	ADD_PROPERTY( PropertyInfo( Variant::INT,
	                      "world_noise_shader_mode",
	                      PROPERTY_HINT_ENUM,
	                      "Dynamic,Cached + Vertex Normals,Cached + Fragment Normals,Procedural + Vertex "
	                      "Normals,Procedural + Fragment Normals" ),
	        "set_world_noise_shader_mode",
	        "get_world_noise_shader_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "auto_shader_enabled" ), "set_auto_shader_enabled", "get_auto_shader_enabled" );

	ADD_GROUP( "Near Quality", "" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::INT, "near_texture_filtering", PROPERTY_HINT_ENUM, "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
	        "set_near_texture_filtering",
	        "get_near_texture_filtering" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
	        "set_near_material_sampling_quality",
	        "get_near_material_sampling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
	        "set_near_terrain_normal_quality",
	        "get_near_terrain_normal_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
	        "set_near_terrain_color_quality",
	        "get_near_terrain_color_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
	        "set_near_surface_response_quality",
	        "get_near_surface_response_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_texture_surface_mode", PROPERTY_HINT_ENUM, "Textured,Silhouette / No Textures" ),
	        "set_near_texture_surface_mode",
	        "get_near_texture_surface_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
	        "set_near_lighting_model",
	        "get_near_lighting_model" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "near_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
	        "set_near_detiling_quality",
	        "get_near_detiling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "near_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_near_authored_color_contribution",
	        "get_near_authored_color_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "near_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_near_wetness_contribution",
	        "get_near_wetness_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "near_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
	        "set_near_material_lod_bias",
	        "get_near_material_lod_bias" );

	ADD_GROUP( "Middle Quality", "" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::INT, "middle_texture_filtering", PROPERTY_HINT_ENUM, "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
	        "set_middle_texture_filtering",
	        "get_middle_texture_filtering" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
	        "set_middle_material_sampling_quality",
	        "get_middle_material_sampling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_surface_source", PROPERTY_HINT_ENUM, "Baked Composite,Runtime Material" ),
	        "set_middle_surface_source",
	        "get_middle_surface_source" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
	        "set_middle_terrain_normal_quality",
	        "get_middle_terrain_normal_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
	        "set_middle_terrain_color_quality",
	        "get_middle_terrain_color_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
	        "set_middle_surface_response_quality",
	        "get_middle_surface_response_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_texture_surface_mode", PROPERTY_HINT_ENUM, "Textured,Silhouette / No Textures" ),
	        "set_middle_texture_surface_mode",
	        "get_middle_texture_surface_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
	        "set_middle_lighting_model",
	        "get_middle_lighting_model" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
	        "set_middle_detiling_quality",
	        "get_middle_detiling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "middle_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_middle_authored_color_contribution",
	        "get_middle_authored_color_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "middle_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_middle_wetness_contribution",
	        "get_middle_wetness_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "middle_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
	        "set_middle_material_lod_bias",
	        "get_middle_material_lod_bias" );

	ADD_GROUP( "Far Quality", "" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::INT, "far_texture_filtering", PROPERTY_HINT_ENUM, "Linear Anisotropic,Linear,Nearest Anisotropic,Nearest" ),
	        "set_far_texture_filtering",
	        "get_far_texture_filtering" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_material_sampling_quality", PROPERTY_HINT_ENUM, "Fast,Full" ),
	        "set_far_material_sampling_quality",
	        "get_far_material_sampling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_terrain_normal_quality", PROPERTY_HINT_ENUM, "Geometry,Detailed" ),
	        "set_far_terrain_normal_quality",
	        "get_far_terrain_normal_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_terrain_color_quality", PROPERTY_HINT_ENUM, "Fast,Exact" ),
	        "set_far_terrain_color_quality",
	        "get_far_terrain_color_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_surface_response_quality", PROPERTY_HINT_ENUM, "Simple,Detailed" ),
	        "set_far_surface_response_quality",
	        "get_far_surface_response_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_texture_surface_mode", PROPERTY_HINT_ENUM, "Textured,Silhouette / No Textures" ),
	        "set_far_texture_surface_mode",
	        "get_far_texture_surface_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_lighting_model", PROPERTY_HINT_ENUM, "Unshaded,Lambert,Blinn-Phong,PBR" ),
	        "set_far_lighting_model",
	        "get_far_lighting_model" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_detiling_quality", PROPERTY_HINT_ENUM, "Disabled,Rotation" ),
	        "set_far_detiling_quality",
	        "get_far_detiling_quality" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "far_authored_color_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_far_authored_color_contribution",
	        "get_far_authored_color_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "far_wetness_contribution", PROPERTY_HINT_RANGE, "0,1,0.01" ),
	        "set_far_wetness_contribution",
	        "get_far_wetness_contribution" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "far_material_lod_bias", PROPERTY_HINT_RANGE, "-2,4,0.25" ),
	        "set_far_material_lod_bias",
	        "get_far_material_lod_bias" );

	ADD_GROUP( "Global Projection / Tessellation / Variation", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "projection_mode", PROPERTY_HINT_ENUM, "Disabled,Shared Fragment Basis,Exact Per-Corner" ),
	        "set_projection_mode",
	        "get_projection_mode" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::INT, "tessellation_material_mode", PROPERTY_HINT_ENUM, "Geometry Only,Geometry + Material Height" ),
	        "set_tessellation_material_mode",
	        "get_tessellation_material_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "dual_scaling_enabled" ), "set_dual_scaling_enabled", "get_dual_scaling_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "macro_variation_enabled" ), "set_macro_variation_enabled", "get_macro_variation_enabled" );

	ADD_GROUP( "PBR Output / Debug", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "output_albedo" ), "set_output_albedo_enabled", "get_output_albedo_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "output_roughness" ), "set_output_roughness_enabled", "get_output_roughness_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "output_specular" ), "set_output_specular_enabled", "get_output_specular_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "output_normal_map" ), "set_output_normal_map_enabled", "get_output_normal_map_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "output_ambient_occlusion" ),
	        "set_output_ambient_occlusion_enabled",
	        "get_output_ambient_occlusion_enabled" );

	ADD_GROUP( "Custom Shader", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "shader_override_enabled" ), "set_shader_override_enabled", "is_shader_override_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "shader_override", PROPERTY_HINT_RESOURCE_TYPE, "Shader" ),
	        "set_shader_override",
	        "get_shader_override" );

	ADD_GROUP( "Runtime Shader Status", "" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::STRING, "near_range_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_near_range_context" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::STRING, "near_shadow_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_near_shadow_context" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::STRING, "near_shader_status", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_near_shader_status" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::STRING, "middle_range_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_middle_range_context" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::STRING, "middle_shadow_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_middle_shadow_context" );
	ADD_PROPERTY(
	        PropertyInfo(
	                Variant::STRING, "middle_shader_status", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_middle_shader_status" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::STRING, "far_range_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_far_range_context" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::STRING, "far_shadow_context", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_far_shadow_context" );
	ADD_PROPERTY(
	        PropertyInfo( Variant::STRING, "far_shader_status", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY ),
	        "",
	        "get_far_shader_status" );

	ADD_GROUP( "Serialized Compatibility", "" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "texture_filtering", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_texture_filtering",
	        "get_texture_filtering" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "terrain_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_terrain_sampling_mode",
	        "get_terrain_sampling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "terrain_normal_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_terrain_normal_mode",
	        "get_terrain_normal_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "color_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_color_sampling_mode",
	        "get_color_sampling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "surface_data_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_surface_data_mode",
	        "get_surface_data_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "texture_surface_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_texture_surface_mode",
	        "get_texture_surface_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_terrain_shading_mode",
	        "get_terrain_shading_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "detiling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_detiling_mode",
	        "get_detiling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_terrain_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_terrain_sampling_mode",
	        "get_middle_terrain_sampling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_terrain_normal_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_terrain_normal_mode",
	        "get_middle_terrain_normal_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_color_sampling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_color_sampling_mode",
	        "get_middle_color_sampling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_surface_data_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_surface_data_mode",
	        "get_middle_surface_data_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_terrain_shading_mode",
	        "get_middle_terrain_shading_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "middle_detiling_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_detiling_mode",
	        "get_middle_detiling_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "middle_auto_shader_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_auto_shader_enabled",
	        "get_middle_auto_shader_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "middle_authored_color_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_authored_color_enabled",
	        "get_middle_authored_color_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "middle_wetness_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_middle_wetness_enabled",
	        "get_middle_wetness_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::INT, "far_terrain_shading_mode", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_far_terrain_shading_mode",
	        "get_far_terrain_shading_mode" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "far_material_lod", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_far_material_lod",
	        "get_far_material_lod" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "far_authored_color_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_far_authored_color_enabled",
	        "get_far_authored_color_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "far_wetness_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_far_wetness_enabled",
	        "get_far_wetness_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "packed_pbr_single_fetch", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_packed_pbr_single_fetch",
	        "get_packed_pbr_single_fetch" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "projection_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE ),
	        "set_projection_enabled",
	        "get_projection_enabled" );

	ADD_GROUP( "Hidden Terrain Aliases", "" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_region_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_region_grid",
	        "get_show_region_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_instancer_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_instancer_grid",
	        "get_show_instancer_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_vertex_grid", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_vertex_grid",
	        "get_show_vertex_grid" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_contours", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_contours",
	        "get_show_contours" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_navigation", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_navigation",
	        "get_show_navigation" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_scale", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_displacement_scale",
	        "get_displacement_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::FLOAT, "displacement_sharpness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_displacement_sharpness",
	        "get_displacement_sharpness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "buffer_shader_override_enabled", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_buffer_shader_override_enabled",
	        "is_buffer_shader_override_enabled" );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "buffer_shader_override", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_buffer_shader_override",
	        "get_buffer_shader_override" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_checkered", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_checkered",
	        "get_show_checkered" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_grey", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_grey",
	        "get_show_grey" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_heightmap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_heightmap",
	        "get_show_heightmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_jaggedness", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_jaggedness",
	        "get_show_jaggedness" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_autoshader", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_autoshader",
	        "get_show_autoshader" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_texture", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_control_texture",
	        "get_show_control_texture" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_blend", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_control_blend",
	        "get_show_control_blend" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_angle", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_control_angle",
	        "get_show_control_angle" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_control_scale", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_control_scale",
	        "get_show_control_scale" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_colormap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_colormap",
	        "get_show_colormap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_roughmap", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_roughmap",
	        "get_show_roughmap" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_albedo", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_texture_albedo",
	        "get_show_texture_albedo" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_height", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_texture_height",
	        "get_show_texture_height" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_normal", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_texture_normal",
	        "get_show_texture_normal" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_ao", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_texture_ao",
	        "get_show_texture_ao" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_texture_rough", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_texture_rough",
	        "get_show_texture_rough" );
	ADD_PROPERTY( PropertyInfo( Variant::BOOL, "show_displacement_buffer", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_NO_EDITOR ),
	        "set_show_displacement_buffer",
	        "get_show_displacement_buffer" );

	ADD_SIGNAL( MethodInfo( "shader_tier_state_changed",
	        PropertyInfo( Variant::INT, "tier" ),
	        PropertyInfo( Variant::STRING, "variant_signature" ),
	        PropertyInfo( Variant::BOOL, "compile_valid" ),
	        PropertyInfo( Variant::BOOL, "fallback_to_near" ) ) );
}
