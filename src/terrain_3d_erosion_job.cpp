#include <godot_cpp/classes/os.hpp>
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "terrain_3d_erosion_job.h"
#include "terrain_3d_cpp17_compat.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

#include "logger.h"
#include "terrain_3d.h"
#include "terrain_3d_assets.h"
#include "terrain_3d_collision.h"
#include "terrain_3d_data.h"
#include "terrain_3d_material.h"
#include "terrain_3d_instancer.h"
#include "terrain_3d_parallel.h"
#include "terrain_3d_simd.h"
#include "terrain_3d_region.h"
#include "terrain_3d_region_codec.h"
#include "terrain_3d_resident_pool.h"
#include "terrain_3d_texture_asset.h"
#include "terrain_3d_util.h"

namespace {
typedef struct alignas( 64 ) t3ddecodedHalfBlock_s {
	float values[TERRAIN3D_EROSION_HALF_DECODE_BLOCK] = {};

	void load(
		const T3DHalfBuffer &rSource, const size_t nBegin, const size_t nCount,
		const float fFallback = 0.0f
	) {
		if ( rSource.empty() ) {
			std::fill_n( values, nCount, fFallback );
			return;
		}
		terrain3d_simd::halfToFloat( rSource.data() + nBegin, values, nCount );
	}
} t3ddecodedHalfBlock_t;

inline float clamp01( float fValue ) {
	return CLAMP( fValue, 0.0f, 1.0f );
}

inline float safeDivide( float fNumerator, float fDenominator ) {
	return fDenominator > TERRAIN3D_EROSION_EPSILON ? fNumerator / fDenominator : 0.0f;
}

inline uint32_t hashU32( uint64_t nSeed, uint64_t iIndex, uint64_t nStep = 0 ) {
	uint64_t value = nSeed ^ ( iIndex + TERRAIN3D_HASH_GOLDEN_RATIO_64 + ( nSeed << 6 ) + ( nSeed >> 2 ) );
	value ^= nStep + TERRAIN3D_HASH_GOLDEN_RATIO_64 + ( value << 6 ) + ( value >> 2 );
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	value ^= value >> 31;
	return ( uint32_t ) ( value & 0xffffffffULL );
}

inline float hashUnit( uint64_t nSeed, uint64_t iIndex, uint64_t nStep = 0 ) {
	return ( float ) ( hashU32( nSeed, iIndex, nStep ) ) / ( float ) ( std::numeric_limits<uint32_t>::max() );
}

inline float readRf( const uint8_t *pBytes, size_t iIndex ) {
	float fValue = 0.0f;
	std::memcpy( &fValue, pBytes + iIndex * sizeof( float ), sizeof( float ) );
	return fValue;
}

inline void writeRf( uint8_t *pBytes, size_t iIndex, float fValue ) {
	std::memcpy( pBytes + iIndex * sizeof( float ), &fValue, sizeof( float ) );
}

inline int32_t resolveThreadCount( int32_t nRequested ) {
#if defined( __EMSCRIPTEN__ ) && !defined( __EMSCRIPTEN_PTHREADS__ )
	return 1;
#else
	int32_t thread_count = nRequested;
	if ( thread_count <= 0 ) {
		thread_count = ( int32_t ) ( ( uint32_t ) ( MAX( 1, OS::get_singleton()->get_processor_count() ) ) );
	}
	return CLAMP( thread_count, 1, 64 );
#endif
}

inline void addMapBytes(
	uint64_t &nBytesPerCell, const bool bEnabled, const uint64_t nCount,
	const uint64_t nElementSize
) {
	if ( bEnabled ) {
		nBytesPerCell += nCount * nElementSize;
	}
}

inline void keepValid( const bool bOperationSucceeded, bool &bValid ) {
	bValid = bOperationSucceeded && bValid;
}

inline bool checkedScaledCount( const size_t nCellCount, const size_t nMultiplier, size_t &nCount ) {
	if ( nMultiplier == 0u || nCellCount > std::numeric_limits<size_t>::max() / nMultiplier ) {
		return false;
	}
	nCount = nCellCount * nMultiplier;
	return true;
}

inline bool planFloatBuffer(
	T3DLinearArenaLayout &rLayout, const bool bEnabled,
	const size_t nCellCount, const size_t nMultiplier = 1u
) {
	if ( !bEnabled ) {
		return true;
	}
	size_t count = 0u;
	return checkedScaledCount( nCellCount, nMultiplier, count ) && rLayout.addArray<float>( count );
}

inline bool planHalfBuffer( T3DLinearArenaLayout &rLayout, const bool bEnabled, const size_t nCellCount ) {
	return !bEnabled || rLayout.addArray<uint16_t>( nCellCount );
}

inline bool planByteBuffer( T3DLinearArenaLayout &rLayout, const bool bEnabled, const size_t nCellCount ) {
	return !bEnabled || rLayout.addArray<uint8_t>( nCellCount );
}

inline bool planIndexBuffer( T3DLinearArenaLayout &rLayout, const bool bEnabled, const size_t nCellCount ) {
	return !bEnabled || rLayout.addArray<int64_t>( nCellCount );
}

inline bool assignFloatBuffer(
	T3DLinearArena &rArena, Terrain3DArenaBuffer<float> &fBuffer,
	const bool bEnabled, const size_t nCellCount, const float fInitial, const size_t nMultiplier = 1u
) {
	if ( !bEnabled ) {
		fBuffer.clear();
		return true;
	}
	size_t count = 0u;
	return checkedScaledCount( nCellCount, nMultiplier, count ) && fBuffer.assign( rArena, count, fInitial );
}

inline bool assignHalfBuffer(
	T3DLinearArena &rArena, T3DHalfBuffer &rBuffer,
	const bool bEnabled, const size_t nCellCount, const float fInitial
) {
	if ( !bEnabled ) {
		rBuffer.clear();
		return true;
	}
	return rBuffer.assign( rArena, nCellCount, fInitial );
}

inline bool assignByteBuffer(
	T3DLinearArena &rArena, Terrain3DArenaBuffer<uint8_t> &nBuffer,
	const bool bEnabled, const size_t nCellCount, const uint8_t nInitial
) {
	if ( !bEnabled ) {
		nBuffer.clear();
		return true;
	}
	return nBuffer.assign( rArena, nCellCount, nInitial );
}

inline bool assignIndexBuffer(
	T3DLinearArena &rArena, Terrain3DArenaBuffer<int64_t> &nBuffer,
	const bool bEnabled, const size_t nCellCount, const int64_t nInitial
) {
	if ( !bEnabled ) {
		nBuffer.clear();
		return true;
	}
	return nBuffer.assign( rArena, nCellCount, nInitial );
}

typedef struct t3derosionResultBuffers_s {
	Terrain3DArenaBuffer<uint8_t> bActive;
	Terrain3DArenaBuffer<float> initial_height;
	Terrain3DArenaBuffer<float> height;
	Terrain3DArenaBuffer<float> sediment_coarse;
	Terrain3DArenaBuffer<float> sediment_sand;
	Terrain3DArenaBuffer<float> sediment_fine;
	Terrain3DArenaBuffer<float> surface_coarse;
	Terrain3DArenaBuffer<float> surface_sand;
	Terrain3DArenaBuffer<float> surface_fine;
	Terrain3DArenaBuffer<float> oxidation;
	Terrain3DArenaBuffer<float> flow_accumulation;
	Terrain3DArenaBuffer<float> hydraulic_deposition;
	Terrain3DArenaBuffer<float> wind_deposition;
	Terrain3DArenaBuffer<float> wave_deposition;
	Terrain3DArenaBuffer<float> weathering;
	Terrain3DArenaBuffer<float> traffic;
	Terrain3DArenaBuffer<float> alluvial_deposition;
	Terrain3DArenaBuffer<float> debris_flow_deposition;
	Terrain3DArenaBuffer<float> dune_deposition;
	Terrain3DArenaBuffer<float> salt_weathering;
	Terrain3DArenaBuffer<float> wet_dry_cycles;
	Terrain3DArenaBuffer<float> snowpack;
	Terrain3DArenaBuffer<float> exposure_age;
	Terrain3DArenaBuffer<float> desert_pavement;
	Terrain3DArenaBuffer<float> cumulative_erosion;
	Terrain3DArenaBuffer<uint8_t> dominant_texture;
	Terrain3DArenaBuffer<uint8_t> material_transition_kind;
	Terrain3DArenaBuffer<float> material_transition_amount;
} t3derosionResultBuffers_t;

inline bool floatBufferIsZero( const Terrain3DArenaBuffer<float> &fBuffer ) {
	for ( const float fValue : fBuffer ) {
		if ( Math::abs( fValue ) > TERRAIN3D_EROSION_EPSILON ) {
			return false;
		}
	}
	return true;
}

inline void clearFloatBufferIfZero( Terrain3DArenaBuffer<float> &fBuffer ) {
	if ( !fBuffer.empty() && floatBufferIsZero( fBuffer ) ) {
		fBuffer.clear();
	}
}

inline bool planResultFloat( T3DLinearArenaLayout &rLayout, const Terrain3DArenaBuffer<float> &fSource ) {
	return fSource.empty() || rLayout.addArray<float>( fSource.size() );
}

inline bool planResultByte( T3DLinearArenaLayout &rLayout, const Terrain3DArenaBuffer<uint8_t> &nSource ) {
	return nSource.empty() || rLayout.addArray<uint8_t>( nSource.size() );
}

inline bool copyResultFloat(
	T3DLinearArena &rArena, const Terrain3DArenaBuffer<float> &fSource,
	Terrain3DArenaBuffer<float> &fDestination
) {
	if ( fSource.empty() ) {
		fDestination.clear();
		return true;
	}
	return fDestination.allocateUninitialized( rArena, fSource.size() ) && fDestination.copyFrom( fSource );
}

inline bool copyResultByte(
	T3DLinearArena &rArena, const Terrain3DArenaBuffer<uint8_t> &nSource,
	Terrain3DArenaBuffer<uint8_t> &nDestination
) {
	if ( nSource.empty() ) {
		nDestination.clear();
		return true;
	}
	return nDestination.allocateUninitialized( rArena, nSource.size() ) && nDestination.copyFrom( nSource );
}

inline void fnvHashBytes( uint64_t &nHash, const void *pData, const size_t nSize ) {
	const uint8_t *pBytes = ( const uint8_t * ) ( pData );
	for ( size_t index = 0u; index < nSize; ++index ) {
		nHash ^= ( uint64_t ) ( pBytes[index] );
		nHash *= TERRAIN3D_EROSION_FNV_PRIME;
	}
}

inline void fnvHashPackedBytes( uint64_t &nHash, const PackedByteArray &arrData ) {
	fnvHashBytes( nHash, arrData.ptr(), ( size_t ) ( arrData.size() ) );
}

inline void fnvHashU8( uint64_t &nHash, const uint8_t nValue ) {
	fnvHashBytes( nHash, &nValue, sizeof( nValue ) );
}

inline void fnvHashI32( uint64_t &nHash, const int32_t nValue ) {
	fnvHashBytes( nHash, &nValue, sizeof( nValue ) );
}

inline void fnvHashF64( uint64_t &nHash, const double fValue ) {
	fnvHashBytes( nHash, &fValue, sizeof( fValue ) );
}

inline bool addImageSnapshotBytes( uint64_t &nTotal, const Ref<Image> &rImage ) {
	if ( rImage.is_null() ) {
		return true;
	}
	const uint64_t width = ( uint64_t ) ( MAX( 0, rImage->get_width() ) );
	const uint64_t height = ( uint64_t ) ( MAX( 0, rImage->get_height() ) );
	if ( height > 0u && width > std::numeric_limits<uint64_t>::max() / height ) {
		return false;
	}
	const uint64_t pixels = width * height;
	if ( pixels > std::numeric_limits<uint64_t>::max() / sizeof( float ) ) {
		return false;
	}
	const uint64_t bytes = pixels * sizeof( float );
	if ( nTotal > std::numeric_limits<uint64_t>::max() - bytes ) {
		return false;
	}
	nTotal += bytes;
	return true;
}

using ParallelExecutor = T3DParallelExecutor;

} // namespace

typedef struct t3derosionFinalStatistics_s {
	double fEroded = 0.0;
	double fDeposited = 0.0;
	float fMaximumLowering = 0.0f;
	float fMaximumRaising = 0.0f;
} t3derosionFinalStatistics_t;

typedef enum t3derosionKernel_e : uint8_t {
	WEATHERING,
	THERMAL_FLUX,
	THERMAL_APPLY,
	RAINFALL,
	HYDRAULIC_FLUX,
	HYDRAULIC_WATER,
	HYDRAULIC_CAPACITY,
	DEBRIS_TRANSFER,
	SEDIMENT_ADVECTION,
	WIND_TRANSPORT,
	WIND_GATHER,
	DUNE_FLUX,
	DUNE_APPLY,
	PAVEMENT,
	WAVE_TRANSPORT,
	WAVE_GATHER,
	BEACH_SMOOTH,
	TRAFFIC,
	STRATIGRAPHY,
	EXPOSURE,
	FINAL_STATISTICS,
	MAXIMUM_FLOW,
	MATERIAL_TRANSITIONS
} t3derosionKernel_t;

typedef struct t3derosionKernelContext_s {
	T3DErosionJob *job = nullptr;
	t3derosionKernel_t kernel = t3derosionKernel_t::WEATHERING;
	float fYearsStep = 0.0f;
	float fTalusGradient = 0.0f;
	float fDuneGradient = 0.0f;
	int32_t step = 0;
	float fStepProgress = 0.0f;
	float fMaster = 0.0f;
	float fWeatherCurve = 0.0f;
	float fOxidationCurve = 0.0f;
	float fRainfallCurve = 0.0f;
	float fWindCurve = 0.0f;
	float fWaveCurve = 0.0f;
	float fTrafficCurve = 0.0f;
	float fSnowCurve = 0.0f;
	float fSaltCurve = 0.0f;
	float fDebrisCurve = 0.0f;
	float fTemperature = 0.0f;
	float fStepRainfall = 0.0f;
	float fFreezeFactor = 0.0f;
	int32_t wind_dx = 0;
	int32_t wind_dy = 0;
	float fCurrentSeaLevel = 0.0f;
	float fCoastalBand = 0.0f;
	int32_t wave_dx = 0;
	int32_t wave_dy = 0;
	int32_t fetch_cells = 1;
	float fMaximumFlow = TERRAIN3D_EROSION_EPSILON;
	eastl::vector<t3derosionFinalStatistics_t> *pFinalStatistics = nullptr;
	eastl::vector<float> *pWorkerMaximumFlow = nullptr;
} t3derosionKernelContext_t;


typedef struct t3derosionApplyContext_s {
	T3DErosionJob *job = nullptr;
	int32_t region_id = -1;
	const uint8_t *pHeightSourceBytes = nullptr;
	const uint8_t *pControlSourceBytes = nullptr;
	uint8_t *pHeightBytes = nullptr;
	uint8_t *pControlBytes = nullptr;
	uint8_t *pColorBytes = nullptr;
} t3derosionApplyContext_t;
typedef struct t3derosionKernels_s {
	static void dispatchStatic( void *pUserData, const size_t nBegin, const size_t nEnd, const int32_t iWorkerIndex ) {
		t3derosionKernelContext_t *pContext = ( t3derosionKernelContext_t * ) ( pUserData );
		switch ( pContext->kernel ) {
			case t3derosionKernel_t::WEATHERING:
				weathering( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::THERMAL_FLUX:
				thermalFlux( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::THERMAL_APPLY:
				thermalApply( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::RAINFALL:
				rainfall( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::HYDRAULIC_FLUX:
				hydraulicFlux( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::HYDRAULIC_WATER:
				hydraulicWater( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::HYDRAULIC_CAPACITY:
				hydraulicCapacity( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::DEBRIS_TRANSFER:
				debrisTransfer( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::SEDIMENT_ADVECTION:
				sedimentAdvection( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::WIND_TRANSPORT:
				windTransport( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::WIND_GATHER:
				windGather( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::DUNE_FLUX:
				duneFlux( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::DUNE_APPLY:
				duneApply( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::PAVEMENT:
				pavement( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::WAVE_TRANSPORT:
				waveTransport( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::WAVE_GATHER:
				waveGather( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::BEACH_SMOOTH:
				beachSmooth( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::TRAFFIC:
				traffic( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::STRATIGRAPHY:
				stratigraphy( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::EXPOSURE:
				exposure( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::FINAL_STATISTICS:
				finalStatistics( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::MAXIMUM_FLOW:
				maximumFlow( pContext, nBegin, nEnd, iWorkerIndex );
				break;
			case t3derosionKernel_t::MATERIAL_TRANSITIONS:
				materialTransitions( pContext, nBegin, nEnd, iWorkerIndex );
				break;
		}
	}

	static void dispatchDynamic( void *pContext, const size_t nBegin, const size_t nEnd ) {
		dispatchStatic( pContext, nBegin, nEnd, 0 );
	}

	static void prepareStep(
		t3derosionKernelContext_t &rContext, const int32_t nStep,
		float &fPreviousTemperature
	) {
		T3DErosionJob *job = rContext.job;
		rContext.step = nStep;
		rContext.fStepProgress = ( float ) ( nStep ) / ( float ) ( job->m_config.simulationSteps );
		rContext.fMaster = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_MASTER, nStep ) ) *
			job->m_config.fGlobalIntensity;
		rContext.fWeatherCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_WEATHERING, nStep ) ) *
			rContext.fMaster;
		rContext.fOxidationCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_OXIDATION, nStep ) ) *
			rContext.fMaster;
		rContext.fRainfallCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_RAINFALL, nStep ) ) *
			rContext.fMaster;
		rContext.fWindCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_WIND, nStep ) ) *
			rContext.fMaster;
		rContext.fWaveCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_WAVE, nStep ) ) *
			rContext.fMaster;
		rContext.fTrafficCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_TRAFFIC, nStep ) ) *
			rContext.fMaster;
		rContext.fSnowCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_SNOWMELT, nStep ) ) *
			rContext.fMaster;
		rContext.fSaltCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_SALT_WEATHERING, nStep ) ) *
			rContext.fMaster;
		rContext.fDebrisCurve = MAX( 0.0f, job->sampleCurve( T3DErosionJob::CURVE_DEBRIS_FLOW, nStep ) ) *
			rContext.fMaster;
		rContext.fTemperature = job->sampleCurve( T3DErosionJob::CURVE_TEMPERATURE, nStep );

		const bool storm_active = job->m_config.fStormFrequency >= 1.0f ||
			hashUnit( job->m_config.seed ^ 0x5a17ULL, ( size_t ) ( nStep ), ( uint64_t ) ( nStep ) ) <=
				job->m_config.fStormFrequency;
		rContext.fStepRainfall = job->m_config.fRainfallMPerYear * rContext.fYearsStep *
			rContext.fRainfallCurve * ( storm_active ? job->m_config.fStormIntensity : 0.0f );
		const bool crossed_freezing =
			( fPreviousTemperature < job->m_config.fFreezePoint && rContext.fTemperature >= job->m_config.fFreezePoint ) ||
			( fPreviousTemperature >= job->m_config.fFreezePoint && rContext.fTemperature < job->m_config.fFreezePoint );
		const float freeze_distance = Math::abs( rContext.fTemperature - job->m_config.fFreezePoint );
		rContext.fFreezeFactor = clamp01( ( 1.0f - freeze_distance ) * 0.5f +
			( crossed_freezing ? 0.75f : 0.0f ) );
		fPreviousTemperature = rContext.fTemperature;
	}

	static void applyRegion( void *pUserData, const size_t nBegin, const size_t nEnd ) {
		t3derosionApplyContext_t *pContext = ( t3derosionApplyContext_t * ) ( pUserData );
		T3DErosionJob *job = pContext->job;
		for ( size_t source_index = nBegin; source_index < nEnd; ++source_index ) {
			const float source_height = readRf( pContext->pHeightSourceBytes, source_index );
			if ( std::isnan( source_height ) ) {
				continue;
			}
			const int32_t x = ( int32_t ) ( source_index % ( size_t ) ( job->m_nSourceRegionSize ) );
			const int32_t y = ( int32_t ) ( source_index / ( size_t ) ( job->m_nSourceRegionSize ) );

			if ( pContext->pHeightBytes ) {
				const float final_height = job->sampleOutput( job->m_height, pContext->region_id, ( float ) ( x ), ( float ) ( y ) );
				const float initial_height = job->sampleOutput( job->m_initialHeight, pContext->region_id, ( float ) ( x ), ( float ) ( y ) );
				const float delta = ( final_height - initial_height ) * job->m_config.fHeightOutputStrength;
				writeRf( pContext->pHeightBytes, source_index, source_height + delta );
			}

			const int32_t sx = CLAMP( x / job->m_resolutionScale, 0, job->m_nSimulationRegionSize - 1 );
			const int32_t sy = CLAMP( y / job->m_resolutionScale, 0, job->m_nSimulationRegionSize - 1 );
			const size_t sim_index = ( size_t ) ( pContext->region_id ) * job->m_nCellsPerRegion +
				( size_t ) ( sy ) * ( size_t ) ( job->m_nSimulationRegionSize ) + ( size_t ) ( sx );
			if ( pContext->pControlBytes ) {
				const T3DErosionJob::t3dmaterialTransitionKind_t transition_kind =
					( T3DErosionJob::t3dmaterialTransitionKind_t ) ( job->m_materialTransitionKind[sim_index] );
				const float transition_amount = clamp01( job->m_materialTransitionAmount[sim_index] );
				if ( transition_kind != T3DErosionJob::TRANSITION_NONE && transition_amount > TERRAIN3D_EROSION_EPSILON ) {
					const uint32_t old_control = asUint( readRf( pContext->pControlSourceBytes, source_index ) );
					const uint8_t old_base = getBase( old_control );
					const uint8_t old_overlay = getOverlay( old_control );
					const uint8_t old_blend = getBlend( old_control );
					const bool auto_material = job->m_config.bResolveAutoMaterials &&
						job->m_config.bAutoShaderEnabled && isAuto( old_control );
					const int32_t source_texture = auto_material ? ( int32_t ) ( job->m_dominantTexture[sim_index] )
						: ( old_blend >= 128U ? ( int32_t ) ( old_overlay ) : ( int32_t ) ( old_base ) );
					const int32_t target_texture = job->getTransitionTarget( source_texture, transition_kind );
					if ( target_texture >= 0 && target_texture != source_texture ) {
						uint8_t output_base = ( uint8_t ) ( source_texture );
						uint8_t output_overlay = ( uint8_t ) ( target_texture );
						uint8_t output_blend = ( uint8_t ) ( CLAMP( Math::round( transition_amount * TERRAIN3D_BYTE_MAX_FLOAT ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
						if ( !auto_material && source_texture == ( int32_t ) ( old_overlay ) ) {
							output_base = ( uint8_t ) ( target_texture );
							output_overlay = ( uint8_t ) ( source_texture );
							output_blend = ( uint8_t ) ( 255U - output_blend );
							if ( target_texture == ( int32_t ) ( old_base ) ) {
								output_blend = MIN( output_blend, old_blend );
							}
						} else if ( !auto_material && target_texture == ( int32_t ) ( old_overlay ) ) {
							output_blend = MAX( output_blend, old_blend );
						}
						uint32_t preserved = job->m_config.bPreserveControlFlags ? old_control
							: ( job->m_config.bPreserveHoles ? encHole( isHole( old_control ) ) : 0U );
						preserved &= ~( ( 0x1FU << 27 ) | ( 0x1FU << 22 ) | ( 0xFFU << 14 ) | 0x1U );
						const uint32_t value = preserved | encBase( output_base ) | encOverlay( output_overlay ) |
							encBlend( output_blend ) | encAuto( false );
						writeRf( pContext->pControlBytes, source_index, asFloat( value ) );
					}
				}
			}

			if ( pContext->pColorBytes ) {
				const float oxidation = clamp01( job->sampleOutput( job->m_oxidation, pContext->region_id, ( float ) ( x ), ( float ) ( y ) ) *
					job->m_config.fOxidationTintStrength );
				const size_t offset = source_index * 4U;
				const float inverse = 1.0f - oxidation;
				pContext->pColorBytes[offset] = ( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->pColorBytes[offset] ) * inverse +
					job->m_config.oxidationTint.r * TERRAIN3D_BYTE_MAX_FLOAT * oxidation ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
				pContext->pColorBytes[offset + 1U] = ( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->pColorBytes[offset + 1U] ) * inverse +
					job->m_config.oxidationTint.g * TERRAIN3D_BYTE_MAX_FLOAT * oxidation ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
				pContext->pColorBytes[offset + 2U] = ( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->pColorBytes[offset + 2U] ) * inverse +
					job->m_config.oxidationTint.b * TERRAIN3D_BYTE_MAX_FLOAT * oxidation ), 0.0f, TERRAIN3D_BYTE_MAX_FLOAT ) );
			}
		}
	}

	static void weathering( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		t3ddecodedHalfBlock_t substrate_hardness;
		t3ddecodedHalfBlock_t fWetDrySusceptibility;
		t3ddecodedHalfBlock_t salinity;
		t3ddecodedHalfBlock_t fSaltSusceptibility;
		t3ddecodedHalfBlock_t fOxidationSusceptibility;
		t3ddecodedHalfBlock_t fWeatheringSusceptibility;
		t3ddecodedHalfBlock_t fGrainSize;
		t3ddecodedHalfBlock_t fBedrockHardness;
		for ( size_t block_begin = begin; block_begin < end; block_begin += TERRAIN3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( TERRAIN3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			substrate_hardness.load( job->m_substrateHardness, block_begin, block_count, job->m_config.fBaseHardness );
			if ( job->m_config.bWetDryEnabled ) {
				fWetDrySusceptibility.load( job->m_wetDrySusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.bSaltWeatheringEnabled ) {
				salinity.load( job->m_salinity, block_begin, block_count, job->m_config.fBackgroundSalinity );
				fSaltSusceptibility.load( job->m_saltSusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.bOxidationEnabled ) {
				fOxidationSusceptibility.load( job->m_oxidationSusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.bWeatheringEnabled ) {
				fWeatheringSusceptibility.load( job->m_weatheringSusceptibility, block_begin, block_count, 1.0f );
			}
			fGrainSize.load( job->m_grainSize, block_begin, block_count, 0.5f );
			if ( job->m_config.bStratigraphyEnabled ) {
				fBedrockHardness.load( job->m_bedrockHardness, block_begin, block_count, job->m_config.fBedrockHardness );
			}

			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] || job->m_simulationWeight[index] <= TERRAIN3D_EROSION_EPSILON ) {
					continue;
				}
				const float local_weight = job->m_simulationWeight[index];
				const float slope = job->getSlope( index );
				const float standing_water = job->m_water.empty() ? 0.0f : job->m_water[index];
				const float snowpack = job->m_snowpack.empty() ? 0.0f : job->m_snowpack[index];
				const float moisture = clamp01( safeDivide( standing_water + snowpack * 0.15f,
					MAX( job->m_fCellSize, TERRAIN3D_EROSION_EPSILON ) ) + pContext->fStepRainfall * 10.0f );
				const float loose_ratio =
					clamp01( safeDivide( job->getLooseMaterial( index ), MAX( job->m_fCellSize * 0.2f, TERRAIN3D_EROSION_EPSILON ) ) );
				const float exposure = 1.0f - loose_ratio;
				const float stability =
					clamp01( 1.0f - safeDivide( slope, MAX( job->m_config.fVarnishStabilitySlope, TERRAIN3D_EROSION_EPSILON ) ) );
				const float varnish = job->m_config.bExposureAgeEnabled
					? clamp01( safeDivide( job->m_exposureAge[index], MAX( job->m_config.fVarnishMaturationYears, 1.0f ) ) )
					: 0.0f;

				float generated_loose = 0.0f;
				if ( job->m_config.bWetDryEnabled ) {
					const bool was_wet = job->m_wetState[index] != 0;
					if ( !was_wet && moisture >= job->m_config.fWetThreshold ) {
						job->m_wetState[index] = 1;
					} else if ( was_wet && moisture <= job->m_config.fDryThreshold ) {
						job->m_wetState[index] = 0;
						job->m_wetDryCycles[index] += 1.0f;
						generated_loose += job->m_config.fWetDryWeatheringMPerCycle *
							fWetDrySusceptibility.values[local] * local_weight * pContext->fMaster;
					}
				}

				if ( job->m_config.bSaltWeatheringEnabled && pContext->fSaltCurve > 0.0f ) {
					const float drying = MAX( 0.0f, job->m_previousMoisture[index] - moisture );
					const float evaporative_cycle = clamp01(
						drying * 8.0f + job->m_config.fEvaporationFraction * job->m_config.fSaltEvaporationBias );
					const float salt_damage = MIN( job->m_config.fMaxHeightChangePerStep,
						job->m_config.fSaltWeatheringMPerYear * pContext->fYearsStep * pContext->fSaltCurve *
							salinity.values[local] * job->m_config.fSaltCrystallizationStrength *
							( 0.2f + 0.8f * evaporative_cycle ) * fSaltSusceptibility.values[local] *
							exposure * local_weight );
					generated_loose += salt_damage;
					job->m_saltWeathering[index] += salt_damage;
				}
				if ( !job->m_previousMoisture.empty() ) {
					job->m_previousMoisture[index] = moisture;
				}

				if ( job->m_config.bOxidationEnabled ) {
					const float varnish_multiplier =
						1.0f + varnish * job->m_config.fVarnishOxidationMultiplier * stability;
					const float amount = job->m_config.fOxidationRatePerYear * pContext->fYearsStep * pContext->fOxidationCurve *
						exposure * ( 0.25f + 0.75f * moisture ) *
						fOxidationSusceptibility.values[local] * varnish_multiplier * local_weight;
					job->m_oxidation[index] = clamp01( job->m_oxidation[index] + amount );
				}

				if ( job->m_config.bWeatheringEnabled ) {
					const float physical = job->m_config.fPhysicalWeatheringMPerYear * pContext->fYearsStep *
						( 0.35f + slope * job->m_config.fWeatheringSlopeBias );
					const float chemical = job->m_config.fChemicalWeatheringMPerYear * pContext->fYearsStep *
						Math::pow( MAX( moisture, TERRAIN3D_EROSION_EPSILON ), job->m_config.fChemicalMoistureExponent );
					const float frost = job->m_config.fPhysicalWeatheringMPerYear * pContext->fYearsStep *
						job->m_config.fFreezeThawStrength * pContext->fFreezeFactor;
					generated_loose += MIN( job->m_config.fMaxHeightChangePerStep,
						MAX( 0.0f, ( physical + chemical + frost ) * pContext->fWeatherCurve *
							fWeatheringSusceptibility.values[local] * local_weight ) );
				}

				if ( generated_loose > TERRAIN3D_EROSION_EPSILON ) {
					float coarse = 0.0f;
					float sand = 0.0f;
					float fine = 0.0f;
					job->partitionMaterial( fGrainSize.values[local], generated_loose, coarse, sand, fine );
					job->depositSurface( index, coarse, sand, fine );
					job->m_weathering[index] = clamp01(
						job->m_weathering[index] + safeDivide( generated_loose, MAX( job->m_fCellSize * 0.1f, TERRAIN3D_EROSION_EPSILON ) ) );
				}

				float substrate = substrate_hardness.values[local];
				if ( job->m_config.bStratigraphyEnabled ) {
					const float transition_width = MAX( job->m_config.fStratigraphyTransitionWidthM, TERRAIN3D_EROSION_EPSILON );
					const float bedrock_mix = clamp01(
						( job->m_cumulativeErosion[index] - job->m_surfaceLayerDepth[index] + transition_width ) /
						transition_width );
					substrate = Math::lerp( substrate, fBedrockHardness.values[local], bedrock_mix );
				}
				const float salt_weakening =
					clamp01( safeDivide( job->m_saltWeathering[index], MAX( job->m_fCellSize * 0.1f, TERRAIN3D_EROSION_EPSILON ) ) ) *
					job->m_config.fSaltCohesionLoss;
				job->m_hardness[index] = clamp01( substrate - job->m_oxidation[index] * job->m_config.fOxidationWeakening -
					job->m_weathering[index] * 0.25f - salt_weakening );
			}
		}

	}
	static void thermalFlux( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		t3ddecodedHalfBlock_t fCohesion;
		t3ddecodedHalfBlock_t susceptibility;
		for ( size_t block_begin = begin; block_begin < end; block_begin += TERRAIN3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( TERRAIN3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fCohesion.load( job->m_cohesion, block_begin, block_count, 0.0f );
			susceptibility.load( job->m_weatheringSusceptibility, block_begin, block_count, 1.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float total_excess = 0.0f;
				eastl::array<float, TERRAIN3D_EROSION_DIRECTION_COUNT> excess{};
				for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
					if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
						excess[( size_t ) ( dir )] = MAX( 0.0f,
							job->m_height[index] - job->m_height[( size_t ) ( neighbor )] - pContext->fTalusGradient * job->m_fCellSize );
						total_excess += excess[( size_t ) ( dir )];
					}
				}
				if ( total_excess <= TERRAIN3D_EROSION_EPSILON ) {
					continue;
				}
				const float available = MIN( job->m_config.fMaxHeightChangePerStep, job->getLooseMaterial( index ) );
				const float total_transfer = MIN( available,
					total_excess * job->m_config.fThermalCreepRate * pContext->fWeatherCurve *
						( 1.0f - fCohesion.values[local] ) * susceptibility.values[local] *
						job->m_simulationWeight[index] );
				for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
					const float target_weight =
						neighbor >= 0 ? job->m_simulationWeight[( size_t ) ( neighbor )] : 0.0f;
					job->m_szFlux[( size_t ) ( dir )][index] = total_transfer *
						safeDivide( excess[( size_t ) ( dir )], total_excess ) *
						MIN( job->m_simulationWeight[index], target_weight );
				}
			}
		}

	}
	static void thermalApply( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		t3ddecodedHalfBlock_t fGrainSize;
		for ( size_t block_begin = begin; block_begin < end; block_begin += TERRAIN3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( TERRAIN3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fGrainSize.load( job->m_grainSize, block_begin, block_count, 0.5f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float outgoing = 0.0f;
				float incoming = 0.0f;
				for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
					outgoing += job->m_szFlux[( size_t ) ( dir )][index];
					const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
					if ( neighbor >= 0 ) {
						incoming += job->m_szFlux[( size_t ) ( TERRAIN3D_EROSION_OPPOSITE_DIRECTION[dir] )][( size_t ) ( neighbor )];
					}
				}
				if ( outgoing > TERRAIN3D_EROSION_EPSILON ) {
					float c = 0.0f, s = 0.0f, f = 0.0f;
					job->entrainSurface( index, outgoing, c, s, f );
				}
				if ( incoming > TERRAIN3D_EROSION_EPSILON ) {
					float c = 0.0f, s = 0.0f, f = 0.0f;
					job->partitionMaterial( fGrainSize.values[local], incoming, c, s, f );
					job->depositSurface( index, c, s, f );
				}
				job->m_height[index] += incoming - outgoing;
			}
		}

	}
	static void rainfall( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		t3ddecodedHalfBlock_t fInfiltrationMultiplier;
		for ( size_t block_begin = begin; block_begin < end; block_begin += TERRAIN3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( TERRAIN3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fInfiltrationMultiplier.load( job->m_infiltrationMultiplier, block_begin, block_count, 1.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				const float variation =
					Math::lerp( 1.0f - job->m_config.fRainfallVariation, 1.0f + job->m_config.fRainfallVariation,
						hashUnit( job->m_config.seed ^ 0x1257ULL, job->stochasticIndex( index ), ( uint64_t ) ( pContext->step ) ) );
				float precipitation = pContext->fStepRainfall * variation * job->m_simulationWeight[index];
				if ( job->m_config.bSnowmeltEnabled ) {
					if ( pContext->fTemperature <= job->m_config.fSnowAccumulationTemperature ) {
						const float snowfall = precipitation * job->m_config.fSnowfallFraction;
						job->m_snowpack[index] += snowfall;
						precipitation -= snowfall;
					}
					if ( pContext->fTemperature >= job->m_config.fSnowMeltTemperature && job->m_snowpack[index] > TERRAIN3D_EROSION_EPSILON ) {
						const float melt = MIN( job->m_snowpack[index],
							job->m_snowpack[index] * job->m_config.fSnowMeltFractionPerStep * pContext->fSnowCurve );
						job->m_snowpack[index] -= melt;
						precipitation += melt;
					}
					job->m_snowpack[index] *= 1.0f - clamp01( job->m_config.fSnowSublimationFraction );
				}
				const float compacted = job->m_compaction.empty() ? 0.0f : job->m_compaction[index];
				const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
				const float runoff_multiplier = 1.0f + compacted * job->m_config.fTrafficRunoffMultiplier;
				const float infiltration =
					clamp01( job->m_config.fInfiltrationFraction * fInfiltrationMultiplier.values[local] *
						( 1.0f - compacted ) *
						( 1.0f - pavement * job->m_config.fPavementInfiltrationReduction ) );
				const float added_water = precipitation * runoff_multiplier * ( 1.0f - infiltration );
				job->m_water[index] = MIN( job->m_fCellSize * 8.0f, job->m_water[index] + MAX( 0.0f, added_water ) );
			}
		}

	}
	static void hydraulicFlux( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_water[index] <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			const float surface = job->m_height[index] + job->m_water[index];
			float total_potential = 0.0f;
			eastl::array<float, TERRAIN3D_EROSION_DIRECTION_COUNT> potential{};
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					potential[( size_t ) ( dir )] =
						MAX( 0.0f, surface - ( job->m_height[( size_t ) ( neighbor )] + job->m_water[( size_t ) ( neighbor )] ) );
				} else if ( job->m_config.boundaryMode == T3DErosionSettings::BOUNDARY_OPEN ) {
					potential[( size_t ) ( dir )] = job->m_water[index] * 0.5f;
				}
				total_potential += potential[( size_t ) ( dir )];
			}
			if ( total_potential <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			const float transferable = MIN( job->m_water[index], total_potential * job->m_config.fFlowRate * 0.25f );
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				job->m_szFlux[( size_t ) ( dir )][index] =
					transferable * safeDivide( potential[( size_t ) ( dir )], total_potential );
			}
		}

	}
	static void hydraulicWater( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				job->m_nextA[index] = 0.0f;
				continue;
			}
			float outgoing = 0.0f;
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 ) {
					incoming += job->m_szFlux[( size_t ) ( TERRAIN3D_EROSION_OPPOSITE_DIRECTION[dir] )][( size_t ) ( neighbor )];
				}
			}
			job->m_nextA[index] = MAX( 0.0f, job->m_water[index] + incoming - outgoing );
			job->m_flowAccumulation[index] += incoming + outgoing;
			job->m_nextB[index] = job->getSlope( index );
		}

	}
	static void hydraulicCapacity( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		t3ddecodedHalfBlock_t fHydraulicErodibility;
		t3ddecodedHalfBlock_t fCohesion;
		for ( size_t block_begin = begin; block_begin < end; block_begin += TERRAIN3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( TERRAIN3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fHydraulicErodibility.load( job->m_hydraulicErodibility, block_begin, block_count, 1.0f );
			fCohesion.load( job->m_cohesion, block_begin, block_count, 0.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float outgoing = 0.0f;
				for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
					outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				}
				const float velocity = safeDivide( outgoing, MAX( job->m_nextA[index], TERRAIN3D_EROSION_EPSILON ) );
				const float slope = MAX( job->m_config.fMinimumSlope, job->m_nextB[index] );
				const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
				const float armor = 1.0f - pavement * job->m_config.fPavementArmorStrength;
				const float material_mobility = fHydraulicErodibility.values[local] *
					( 1.0f - fCohesion.values[local] ) * MAX( 0.0f, armor );
				const float base_capacity = job->m_config.fSedimentCapacity * velocity * Math::sqrt( slope ) *
					MAX( job->m_nextA[index], 0.001f ) * MAX( 0.0f, material_mobility );
				const float coarse_capacity = base_capacity *
					( job->m_config.bSedimentSortingEnabled ? job->m_config.fCoarseTransportMultiplier : 1.0f );
				const float sand_capacity = base_capacity *
					( job->m_config.bSedimentSortingEnabled ? job->m_config.fSandTransportMultiplier : 1.0f );
				const float fine_capacity = base_capacity *
					( job->m_config.bSedimentSortingEnabled ? job->m_config.fFineTransportMultiplier : 1.0f );
				const float demand = MAX( 0.0f, coarse_capacity - job->m_sedimentCoarse[index] ) +
					MAX( 0.0f, sand_capacity - job->m_sedimentSand[index] ) +
					MAX( 0.0f, fine_capacity - job->m_sedimentFine[index] );
				if ( demand > TERRAIN3D_EROSION_EPSILON ) {
					const float erodibility = ( 1.0f - clamp01( job->m_hardness[index] ) ) *
						fHydraulicErodibility.values[local] * ( 1.0f - fCohesion.values[local] ) *
						MAX( 0.0f, armor );
					const float channel_signal = clamp01(
						Math::log( 1.0f + job->m_flowAccumulation[index] / MAX( job->m_fCellSize, TERRAIN3D_EROSION_EPSILON ) ) / 8.0f );
					const float channel_factor = 1.0f + job->m_config.fChannelIncisionStrength * channel_signal;
					const float rate_limit = job->m_config.fHydraulicErosionMPerYear * pContext->fYearsStep * pContext->fRainfallCurve *
						MAX( 0.0f, 0.15f * fHydraulicErodibility.values[local] + 0.85f * erodibility ) *
						channel_factor * job->m_simulationWeight[index];
					const float eroded =
						MIN( job->m_config.fMaxHeightChangePerStep, MIN( demand, MAX( 0.0f, rate_limit ) ) );
					if ( eroded > TERRAIN3D_EROSION_EPSILON ) {
						float c = 0.0f, s = 0.0f, f = 0.0f;
						job->entrainSurface( index, eroded, c, s, f );
						job->m_sedimentCoarse[index] += c;
						job->m_sedimentSand[index] += s;
						job->m_sedimentFine[index] += f;
						job->m_height[index] -= eroded;
					}
				}

				const float still_water = 1.0f - clamp01( velocity );
				const float base_deposition = clamp01(
					job->m_config.fHydraulicDepositionRate + job->m_config.fStandingWaterDepositionRate * still_water );
				const float coarse_excess = MAX( 0.0f, job->m_sedimentCoarse[index] - coarse_capacity );
				const float sand_excess = MAX( 0.0f, job->m_sedimentSand[index] - sand_capacity );
				const float fine_excess = MAX( 0.0f, job->m_sedimentFine[index] - fine_capacity );
				float coarse_deposit = coarse_excess * base_deposition * job->m_config.fCoarseSettlingRate;
				float sand_deposit = sand_excess * base_deposition * job->m_config.fSandSettlingRate;
				float fine_deposit = fine_excess * base_deposition * job->m_config.fFineSettlingRate;
				const float total_deposit = coarse_deposit + sand_deposit + fine_deposit;
				if ( total_deposit > job->m_config.fMaxHeightChangePerStep ) {
					const float scale = safeDivide( job->m_config.fMaxHeightChangePerStep, total_deposit );
					coarse_deposit *= scale;
					sand_deposit *= scale;
					fine_deposit *= scale;
				}
				const float deposited =
					( coarse_deposit + sand_deposit + fine_deposit ) * job->m_simulationWeight[index];
				if ( deposited > TERRAIN3D_EROSION_EPSILON ) {
					job->m_sedimentCoarse[index] -= coarse_deposit * job->m_simulationWeight[index];
					job->m_sedimentSand[index] -= sand_deposit * job->m_simulationWeight[index];
					job->m_sedimentFine[index] -= fine_deposit * job->m_simulationWeight[index];
					job->depositSurface( index, coarse_deposit * job->m_simulationWeight[index],
						sand_deposit * job->m_simulationWeight[index], fine_deposit * job->m_simulationWeight[index] );
					job->m_height[index] += deposited;
					job->m_hydraulicDeposition[index] += deposited;
				}
			}
		}

	}
	static void debrisTransfer( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_water[index] <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			float upstream_slope = 0.0f;
			float downstream_slope = 0.0f;
			int64_t spread_destination = -1;
			float flattest_downstream = std::numeric_limits<float>::max();
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor < 0 || !job->m_active[( size_t ) ( neighbor )] ) {
					continue;
				}
				const float delta = safeDivide( job->m_height[( size_t ) ( neighbor )] - job->m_height[index], job->m_fCellSize );
				if ( delta > 0.0f ) {
					upstream_slope = MAX( upstream_slope, delta );
				} else {
					const float downhill = -delta;
					downstream_slope = MAX( downstream_slope, downhill );
					if ( downhill < flattest_downstream && job->m_simulationWeight[( size_t ) ( neighbor )] > TERRAIN3D_EROSION_EPSILON ) {
						flattest_downstream = downhill;
						spread_destination = neighbor;
					}
				}
			}
			const float slope_drop =
				MAX( 0.0f, upstream_slope - downstream_slope - job->m_config.fAlluvialFanSlopeDrop );
			const float flow_signal =
				clamp01( Math::log( 1.0f + job->m_flowAccumulation[index] / MAX( job->m_fCellSize, TERRAIN3D_EROSION_EPSILON ) ) / 8.0f );
			const float fan_signal =
				clamp01( slope_drop * 8.0f ) * flow_signal * job->m_config.fAlluvialFanStrength;
			if ( fan_signal <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			float coarse = MIN( job->m_sedimentCoarse[index], job->m_sedimentCoarse[index] * fan_signal *
															( 0.5f + 0.5f * job->m_config.fAlluvialFanSpread ) );
			float sand = MIN( job->m_sedimentSand[index], job->m_sedimentSand[index] * fan_signal * 0.5f );
			const float limited = MIN( job->m_config.fMaxHeightChangePerStep, coarse + sand );
			if ( coarse + sand > limited && coarse + sand > TERRAIN3D_EROSION_EPSILON ) {
				const float scale = limited / ( coarse + sand );
				coarse *= scale;
				sand *= scale;
			}
			job->m_transferCoarse[index] = coarse;
			job->m_transferSand[index] = sand;
			job->m_transferAmount[index] = spread_destination >= 0 ? clamp01( job->m_config.fAlluvialFanSpread ) : 0.0f;
			job->m_transferDestination[index] = spread_destination;
		}

	}
	static void sedimentAdvection( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				job->m_nextCoarse[index] = job->m_nextSand[index] = job->m_nextFine[index] = 0.0f;
				continue;
			}
			const float source_water = MAX( job->m_water[index], TERRAIN3D_EROSION_EPSILON );
			const float concentrations[3] = {
				safeDivide( job->m_sedimentCoarse[index], source_water ),
				safeDivide( job->m_sedimentSand[index], source_water ),
				safeDivide( job->m_sedimentFine[index], source_water ),
			};
			float outgoing[3] = {0.0f, 0.0f, 0.0f};
			float incoming[3] = {0.0f, 0.0f, 0.0f};
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const float water_flux = job->m_szFlux[( size_t ) ( dir )][index];
				for ( int32_t cls = 0; cls < 3; ++cls ) {
					outgoing[cls] += water_flux * concentrations[cls];
				}
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					const float neighbor_water = MAX( job->m_water[( size_t ) ( neighbor )], TERRAIN3D_EROSION_EPSILON );
					const float incoming_flux = job->m_szFlux[( size_t ) ( TERRAIN3D_EROSION_OPPOSITE_DIRECTION[dir] )][( size_t ) ( neighbor )];
					incoming[0] +=
						incoming_flux * safeDivide( job->m_sedimentCoarse[( size_t ) ( neighbor )], neighbor_water );
					incoming[1] +=
						incoming_flux * safeDivide( job->m_sedimentSand[( size_t ) ( neighbor )], neighbor_water );
					incoming[2] +=
						incoming_flux * safeDivide( job->m_sedimentFine[( size_t ) ( neighbor )], neighbor_water );
				}
			}
			job->m_nextCoarse[index] =
				MAX( 0.0f, job->m_sedimentCoarse[index] - MIN( outgoing[0], job->m_sedimentCoarse[index] ) + incoming[0] );
			job->m_nextSand[index] =
				MAX( 0.0f, job->m_sedimentSand[index] - MIN( outgoing[1], job->m_sedimentSand[index] ) + incoming[1] );
			job->m_nextFine[index] =
				MAX( 0.0f, job->m_sedimentFine[index] - MIN( outgoing[2], job->m_sedimentFine[index] ) + incoming[2] );
			job->m_nextA[index] *= 1.0f - clamp01( job->m_config.fEvaporationFraction );
		}

	}
	static void windTransport( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			int64_t destination = ( int64_t ) ( index );
			for ( int32_t travel = 0; travel < job->m_config.windTransportDistanceCells; ++travel ) {
				const int64_t next = job->neighborIndex( ( size_t ) ( destination ), pContext->wind_dx, pContext->wind_dy );
				if ( next < 0 || !job->m_active[( size_t ) ( next )] ) {
					destination = -1;
					break;
				}
				destination = next;
			}
			if ( destination < 0 ) {
				continue;
			}
			const int64_t upwind = job->neighborIndex( index, -pContext->wind_dx, -pContext->wind_dy );
			float shelter = 0.0f;
			if ( upwind >= 0 && job->m_active[( size_t ) ( upwind )] ) {
				shelter = clamp01( safeDivide( job->m_height[( size_t ) ( upwind )] - job->m_height[index], job->m_fCellSize ) *
								  job->m_config.fWindShelterStrength );
			}
			const float variation = Math::lerp( 1.0f - job->m_config.fWindVariation, 1.0f + job->m_config.fWindVariation,
											   hashUnit( job->m_config.seed ^ 0x91f5ULL, job->stochasticIndex( index ), ( uint64_t ) ( pContext->step ) ) );
			const float fStrength = MAX( 0.0f, job->m_config.fWindStrength * pContext->fWindCurve * variation * ( 1.0f - shelter ) -
												 job->m_config.fWindThreshold );
			const float exposed =
				1.0f - clamp01( safeDivide( job->getLooseMaterial( index ), MAX( job->m_fCellSize * 0.2f, TERRAIN3D_EROSION_EPSILON ) ) );
			const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
			const float armor = 1.0f - pavement * job->m_config.fPavementArmorStrength;
			const float abrasion =
				MIN( job->m_config.fMaxHeightChangePerStep,
					job->m_config.fWindErosionMPerYear * pContext->fYearsStep * fStrength * exposed *
						( 0.25f + 0.75f * ( 1.0f - job->m_hardness[index] ) ) * job->m_windErodibility[index] *
						( 1.0f - job->m_cohesion[index] ) * MAX( 0.0f, armor ) * job->m_simulationWeight[index] );
			const float available = job->getLooseMaterial( index ) + abrasion;
			const float destination_flatness = 1.0f - clamp01( job->getSlope( ( size_t ) ( destination ) ) );
			const float moved =
				MIN( job->m_config.fMaxHeightChangePerStep,
					available * job->m_config.fWindTransportRate * job->m_config.fWindDepositionRate * clamp01( fStrength ) *
						( 1.0f - job->m_cohesion[index] ) *
						( 1.0f + job->m_config.fWindDepositionSlopeBias * destination_flatness ) *
						MIN( job->m_simulationWeight[index], job->m_simulationWeight[( size_t ) ( destination )] ) );
			if ( moved <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			float c = 0.0f, s = 0.0f, f = 0.0f;
			job->entrainSurface( index, moved, c, s, f );
			// Saltation strongly favors sand, suspension favors fines, coarse lag barely moves.
			job->m_transferCoarse[index] = c * 0.15f;
			job->m_transferSand[index] = s;
			job->m_transferFine[index] = f * 0.65f;
			const float exported = job->m_transferCoarse[index] + job->m_transferSand[index] + job->m_transferFine[index];
			const float retained = moved - exported;
			if ( retained > TERRAIN3D_EROSION_EPSILON ) {
				job->depositSurface( index, c - job->m_transferCoarse[index], s - job->m_transferSand[index],
								 f - job->m_transferFine[index] );
			}
			job->m_height[index] -= exported;
			job->m_transferDestination[index] = destination;
		}

	}
	static void windGather( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float c = 0.0f, s = 0.0f, f = 0.0f;
			int64_t source = ( int64_t ) ( index );
			for ( int32_t travel = 0; travel < job->m_config.windTransportDistanceCells; ++travel ) {
				source = job->neighborIndex( ( size_t ) ( source ), -pContext->wind_dx, -pContext->wind_dy );
				if ( source < 0 ) {
					break;
				}
				if ( job->m_transferDestination[( size_t ) ( source )] == ( int64_t ) ( index ) ) {
					c += job->m_transferCoarse[( size_t ) ( source )];
					s += job->m_transferSand[( size_t ) ( source )];
					f += job->m_transferFine[( size_t ) ( source )];
				}
			}
			if ( c + s + f > TERRAIN3D_EROSION_EPSILON ) {
				job->depositSurface( index, c, s, f );
				job->m_height[index] += c + s + f;
				job->m_windDeposition[index] += c + s + f;
			}
		}

	}
	static void duneFlux( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_surfaceSand[index] < job->m_config.fDuneMinSandDepthM ) {
				continue;
			}
			float total_excess = 0.0f;
			eastl::array<float, TERRAIN3D_EROSION_DIRECTION_COUNT> excess{};
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					excess[( size_t ) ( dir )] =
						MAX( 0.0f, job->m_height[index] - job->m_height[( size_t ) ( neighbor )] - pContext->fDuneGradient * job->m_fCellSize );
					total_excess += excess[( size_t ) ( dir )];
				}
			}
			if ( total_excess <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			const float moved = MIN( job->m_surfaceSand[index], MIN( job->m_config.fMaxHeightChangePerStep,
															  total_excess * job->m_config.fDuneAvalancheRate ) );
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				job->m_szFlux[( size_t ) ( dir )][index] =
					moved * safeDivide( excess[( size_t ) ( dir )], total_excess );
			}
		}

	}
	static void duneApply( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float outgoing = 0.0f;
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 ) {
					incoming += job->m_szFlux[( size_t ) ( TERRAIN3D_EROSION_OPPOSITE_DIRECTION[dir] )][( size_t ) ( neighbor )];
				}
			}
			job->m_surfaceSand[index] = MAX( 0.0f, job->m_surfaceSand[index] + incoming - outgoing );
			job->m_height[index] += incoming - outgoing;
			job->m_duneDeposition[index] += incoming;
		}

	}
	static void pavement( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const float total = MAX( job->getLooseMaterial( index ), TERRAIN3D_EROSION_EPSILON );
			const float coarse_fraction = job->m_surfaceCoarse[index] / total;
			const float fine_fraction = job->m_surfaceFine[index] / total;
			const float lag_signal =
				coarse_fraction * clamp01( ( job->m_config.fPavementFineRemovalThreshold - fine_fraction ) /
										  MAX( job->m_config.fPavementFineRemovalThreshold, TERRAIN3D_EROSION_EPSILON ) );
			const float process_signal = MAX( job->m_config.bWindEnabled ? pContext->fWindCurve : 0.0f,
											 job->m_config.bHydraulicEnabled ? pContext->fRainfallCurve : 0.0f );
			job->m_desertPavement[index] =
				clamp01( job->m_desertPavement[index] + lag_signal * job->m_config.fPavementFormationRate *
													  process_signal / ( float ) ( job->m_config.simulationSteps ) );
		}

	}
	static void waveTransport( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_height[index] < pContext->fCurrentSeaLevel - pContext->fCoastalBand ||
				job->m_height[index] > pContext->fCurrentSeaLevel + pContext->fCoastalBand ) {
				continue;
			}
			bool touches_water = false;
			int64_t lowest_neighbor = -1;
			float lowest_height = job->m_height[index];
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					touches_water = touches_water || job->m_height[( size_t ) ( neighbor )] <= pContext->fCurrentSeaLevel;
					if ( job->m_height[( size_t ) ( neighbor )] < lowest_height ) {
						lowest_height = job->m_height[( size_t ) ( neighbor )];
						lowest_neighbor = neighbor;
					}
				}
			}
			if ( !touches_water ) {
				continue;
			}
			int32_t fetch = 0;
			int64_t cursor = ( int64_t ) ( index );
			for ( ; fetch < pContext->fetch_cells; ++fetch ) {
				cursor = job->neighborIndex( ( size_t ) ( cursor ), -pContext->wave_dx, -pContext->wave_dy );
				if ( cursor < 0 || !job->m_active[( size_t ) ( cursor )] || job->m_height[( size_t ) ( cursor )] > pContext->fCurrentSeaLevel ) {
					break;
				}
			}
			const float exposure = safeDivide( ( float ) ( fetch ), ( float ) ( pContext->fetch_cells ) );
			const float slope = job->getSlope( index );
			const float erosion =
				MIN( job->m_config.fMaxHeightChangePerStep,
					MAX( 0.0f, job->m_config.fWaveErosionMPerYear * pContext->fYearsStep * job->m_config.fWaveStrength *
								  pContext->fWaveCurve * exposure * ( 1.0f + job->m_config.fWaveUndercutBias * clamp01( slope ) ) *
								  ( 0.2f + 0.8f * ( 1.0f - job->m_hardness[index] ) ) * job->m_waveErodibility[index] *
								  ( 1.0f - job->m_cohesion[index] ) * job->m_simulationWeight[index] ) );
			if ( erosion <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			int64_t destination = job->neighborIndex( index, -pContext->wave_dx, -pContext->wave_dy );
			if ( destination < 0 || !job->m_active[( size_t ) ( destination )] ||
				job->m_height[( size_t ) ( destination )] > pContext->fCurrentSeaLevel ) {
				destination = lowest_neighbor;
			}
			job->m_transferAmount[index] = erosion;
			job->m_transferDestination[index] = destination;
		}

	}
	static void waveGather( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_transferDestination[( size_t ) ( neighbor )] == ( int64_t ) ( index ) ) {
					incoming += job->m_transferAmount[( size_t ) ( neighbor )] * job->m_config.fWaveDepositionRate *
								MIN( job->m_simulationWeight[( size_t ) ( neighbor )], job->m_simulationWeight[index] );
				}
			}
			const float outgoing = job->m_transferAmount[index];
			if ( outgoing > TERRAIN3D_EROSION_EPSILON ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->entrainSurface( index, outgoing, c, s, f );
			}
			if ( incoming > TERRAIN3D_EROSION_EPSILON ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->partitionMaterial( job->m_grainSize[index], incoming, c, s, f );
				job->depositSurface( index, c, s, f );
				job->m_waveDeposition[index] += incoming;
			}
			job->m_height[index] += incoming - outgoing;
		}

	}
	static void beachSmooth( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			job->m_nextA[index] = job->m_height[index];
			if ( !job->m_active[index] || Math::abs( job->m_height[index] - pContext->fCurrentSeaLevel ) > pContext->fCoastalBand ) {
				continue;
			}
			float sum = 0.0f;
			int32_t nCount = 0;
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					sum += job->m_height[( size_t ) ( neighbor )];
					nCount += 1;
				}
			}
			if ( nCount > 0 ) {
				job->m_nextA[index] = Math::lerp(
					job->m_height[index], sum / ( float ) ( nCount ),
					clamp01( job->m_config.fBeachSmoothing * pContext->fWaveCurve * job->m_simulationWeight[index] * 0.25f ) );
			}
		}

	}
	static void traffic( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_traffic[index] <= TERRAIN3D_EROSION_EPSILON ) {
				job->m_nextA[index] = job->m_height[index];
				continue;
			}
			const float weight = job->m_traffic[index] * pContext->fTrafficCurve * job->m_simulationWeight[index];
			const float erosion =
				MIN( job->m_config.fMaxHeightChangePerStep,
					job->m_config.fTrafficErosionMPerYear * pContext->fYearsStep * weight * ( 1.0f + job->getSlope( index ) ) *
						job->m_trafficErodibility[index] * ( 1.0f - job->m_cohesion[index] * 0.75f ) );
			float neighbor_sum = 0.0f;
			int32_t neighbor_count = 0;
			for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor = job->neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					neighbor_sum += job->m_height[( size_t ) ( neighbor )];
					neighbor_count += 1;
				}
			}
			float smoothed = job->m_height[index];
			if ( neighbor_count > 0 ) {
				smoothed = Math::lerp( job->m_height[index], neighbor_sum / ( float ) ( neighbor_count ),
									  job->m_config.fTrafficSmoothing * weight * 0.25f );
			}
			job->m_nextA[index] = smoothed - erosion;
			if ( erosion > TERRAIN3D_EROSION_EPSILON ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->entrainSurface( index, erosion, c, s, f );
			}
			job->m_compaction[index] = clamp01( job->m_compaction[index] +
										 weight * job->m_config.fTrafficCompaction / ( float ) ( job->m_config.simulationSteps ) );
		}

	}
	static void stratigraphy( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		terrain3d_simd::updateCumulativeErosion( job->m_active.data() + begin, job->m_initialHeight.data() + begin,
			job->m_height.data() + begin, job->m_cumulativeErosion.data() + begin, end - begin );

	}
	static void exposure( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const float change = Math::abs( job->m_height[index] - job->m_stepHeightStart[index] );
			const float loose_cover =
				clamp01( safeDivide( job->getLooseMaterial( index ), MAX( job->m_config.fExposureResetDepthM, TERRAIN3D_EROSION_EPSILON ) ) );
			const float stability =
				clamp01( 1.0f - safeDivide( job->getSlope( index ), MAX( job->m_config.fVarnishStabilitySlope, TERRAIN3D_EROSION_EPSILON ) ) );
			if ( change >= job->m_config.fExposureResetDepthM || loose_cover >= 1.0f ) {
				job->m_exposureAge[index] =
					MAX( 0.0f, job->m_exposureAge[index] -
								  pContext->fYearsStep * clamp01( change / MAX( job->m_config.fExposureResetDepthM, TERRAIN3D_EROSION_EPSILON ) ) );
			} else {
				job->m_exposureAge[index] +=
					pContext->fYearsStep * stability * ( 1.0f - loose_cover ) * job->m_simulationWeight[index];
			}
		}

	}
	static void finalStatistics( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		const int32_t worker_index = iWorkerIndex;

		t3derosionFinalStatistics_t local;
		const double cell_area = ( double ) ( job->m_fCellSize ) * ( double ) ( job->m_fCellSize );
		for ( size_t index = begin; index < end; ++index ) {
			if ( job->m_active[index] == 0u ) {
				continue;
			}
			const float delta = job->m_height[index] - job->m_initialHeight[index];
			if ( delta < 0.0f ) {
				local.fEroded += ( double ) ( -delta ) * cell_area;
				local.fMaximumLowering = MAX( local.fMaximumLowering, -delta );
			} else {
				local.fDeposited += ( double ) ( delta ) * cell_area;
				local.fMaximumRaising = MAX( local.fMaximumRaising, delta );
			}
		}
		( *pContext->pFinalStatistics )[( size_t ) ( worker_index )] = local;

	}
	static void maximumFlow( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		const int32_t worker_index = iWorkerIndex;

		float local_maximum = TERRAIN3D_EROSION_EPSILON;
		for ( size_t index = begin; index < end; ++index ) {
			if ( job->m_active[index] != 0u ) {
				local_maximum = MAX( local_maximum, job->m_flowAccumulation[index] );
			}
		}
		( *pContext->pWorkerMaximumFlow )[( size_t ) ( worker_index )] = local_maximum;

	}
	static void materialTransitions( t3derosionKernelContext_t *pContext, const size_t begin, const size_t end, const int32_t iWorkerIndex ) {
		T3DErosionJob *job = pContext->job;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const int32_t source_texture = ( int32_t ) ( job->m_dominantTexture[index] );
			const float slope = clamp01( job->getSlope( index ) );
			const float transition_depth = MAX( job->m_config.fMaterialTransitionDepthM, TERRAIN3D_EROSION_EPSILON );
			const float loose = clamp01( safeDivide( job->getLooseMaterial( index ), transition_depth ) );
			const float lowering =
				clamp01( safeDivide( MAX( 0.0f, job->m_initialHeight[index] - job->m_height[index] ), transition_depth ) );
			const float flow = clamp01( safeDivide( job->m_flowAccumulation[index], pContext->fMaximumFlow ) );
			const float hydraulic = clamp01( safeDivide( job->m_hydraulicDeposition[index], transition_depth ) );
			const float wind = clamp01( safeDivide( job->m_windDeposition[index], transition_depth ) );
			const float wave = clamp01( safeDivide( job->m_waveDeposition[index], transition_depth ) );
			const float alluvial = clamp01( safeDivide( job->m_alluvialDeposition[index], transition_depth ) );
			const float debris = clamp01( safeDivide( job->m_debrisFlowDeposition[index], transition_depth ) );
			const float dune = clamp01( safeDivide( job->m_duneDeposition[index], transition_depth ) );
			const float loose_total = MAX( job->getLooseMaterial( index ), TERRAIN3D_EROSION_EPSILON );
			const float coarse_fraction = safeDivide( job->m_surfaceCoarse[index], loose_total );
			const float fine_fraction = safeDivide( job->m_surfaceFine[index], loose_total );
			const float coarse_deposition = MAX( MAX( alluvial, debris ), MAX( wind * coarse_fraction, wave * coarse_fraction ) );
			const float fine_deposition = MAX( hydraulic * ( 0.5f + 0.5f * flow ), MAX( wind * fine_fraction, dune * 0.25f ) );
			const float varnish =
				job->m_config.bExposureAgeEnabled
					? clamp01( safeDivide( job->m_exposureAge[index], MAX( job->m_config.fVarnishMaturationYears, 1.0f ) ) )
					: 0.0f;

			typedef struct t3dtransitionScore_s {
				T3DErosionJob::t3dmaterialTransitionKind_t kind;
				float fScore;
			} t3dtransitionScore_t;
			const eastl::array<t3dtransitionScore_t, 8> scores = {
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_EXPOSED, lowering * ( 0.35f + 0.65f * slope ) * ( 1.0f - loose * 0.75f )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_WEATHERED, job->m_weathering[index] * ( 0.5f + 0.5f * ( 1.0f - slope ) )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_OXIDIZED, job->m_oxidation[index] * ( 1.0f - loose * 0.5f )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_COARSE_DEPOSIT, coarse_deposition * ( 0.35f + 0.65f * loose )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_FINE_DEPOSIT, fine_deposition * ( 1.0f - slope * 0.75f )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_SALT_WEATHERED, clamp01( safeDivide( job->m_saltWeathering[index], transition_depth ) )},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_DESERT_PAVEMENT,
					job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index]},
				t3dtransitionScore_t{T3DErosionJob::TRANSITION_VARNISHED, varnish * job->m_oxidation[index] * ( 1.0f - loose )},
			};

			T3DErosionJob::t3dmaterialTransitionKind_t best_kind = T3DErosionJob::TRANSITION_NONE;
			float best_score = 0.0f;
			for ( const t3dtransitionScore_t &candidate : scores ) {
				if ( job->getTransitionTarget( source_texture, candidate.kind ) < 0 ) {
					continue;
				}
				if ( candidate.fScore > best_score ) {
					best_kind = candidate.kind;
					best_score = candidate.fScore;
				}
			}
			if ( best_kind == T3DErosionJob::TRANSITION_NONE || best_score <= job->m_config.fMaterialTransitionThreshold ) {
				continue;
			}
			const float normalized = safeDivide( best_score - job->m_config.fMaterialTransitionThreshold,
				MAX( 1.0f - job->m_config.fMaterialTransitionThreshold, TERRAIN3D_EROSION_EPSILON ) );
			const float amount = clamp01( normalized * job->m_config.fMaterialTransitionStrength );
			if ( amount <= TERRAIN3D_EROSION_EPSILON ) {
				continue;
			}
			job->m_materialTransitionKind[index] = ( uint8_t ) ( best_kind );
			job->m_materialTransitionAmount[index] = amount;
		}

	}
} t3derosionKernels_t;

static void runStaticErosionKernel(
	const ParallelExecutor &rExecutor, t3derosionKernelContext_t &rContext,
	const t3derosionKernel_t kernel, const size_t nCount, const size_t nAlignment
) {
	rContext.kernel = kernel;
	rExecutor.runStatic( nCount, nAlignment, t3derosionKernels_t::dispatchStatic, &rContext );
}

static void runDynamicErosionKernel(
	const ParallelExecutor &rExecutor, t3derosionKernelContext_t &rContext,
	const t3derosionKernel_t kernel, const size_t nCount, const size_t nGrain
) {
	rContext.kernel = kernel;
	rExecutor.run( nCount, nGrain, t3derosionKernels_t::dispatchDynamic, &rContext );
}

T3DErosionJob::T3DErosionJob( void ) {
	std::fill_n( m_szRegionLookup, 1024, ( int32_t ) ( -1 ) );
}

void T3DErosionJob::setErrorMessage( const String &rMessage ) {
	std::lock_guard<std::mutex> lock( m_statusMutex );
	m_errorMessage = rMessage;
}

void T3DErosionJob::setFailed( const String &rMessage ) {
	setErrorMessage( rMessage );
	m_stage.store( STAGE_FAILED );
	m_state.store( STATE_FAILED );
	m_fProgress.store( 1.0f );
}

void T3DErosionJob::setStage( const t3dstage_t stage, const float fProgress ) {
	m_stage.store( stage );
	m_fProgress.store( clamp01( fProgress ) );
}

void T3DErosionJob::copyConfig( const Ref<T3DErosionSettings> &rSettings ) {
	m_config.fSimulationYears = rSettings->getSimulationYears();
	m_config.simulationSteps = rSettings->getSimulationSteps();
	m_config.requestedResolutionScale = rSettings->getResolutionScale();
	m_config.bAutoAdjustResolution = rSettings->getAutoAdjustResolution();
	m_config.memoryBudgetMb = rSettings->getMemoryBudgetMb();
	m_config.bLivePreviewEnabled = rSettings->getLivePreviewEnabled();
	m_config.livePreviewIntervalSteps = rSettings->getLivePreviewIntervalSteps();
	m_config.bLivePreviewStartPaused = rSettings->getLivePreviewStartPaused();
	m_config.nThreadCount = rSettings->getThreadCount();
	m_config.seed = ( uint64_t ) ( rSettings->getSeed() );
	m_config.boundaryMode = rSettings->getBoundaryMode();
	m_config.fMaxHeightChangePerStep = rSettings->getMaxHeightChangePerStep();
	m_config.bPreserveHoles = rSettings->getPreserveHoles();
	m_config.bApplyHeight = rSettings->getApplyHeight();
	m_config.fHeightOutputStrength = rSettings->getHeightOutputStrength();
	m_config.fGlobalIntensity = rSettings->getGlobalIntensity();
	m_config.simulationMaskWorldRect = rSettings->getSimulationMaskWorldRect();
	m_config.bInvertSimulationMask = rSettings->getInvertSimulationMask();
	m_config.fEdgeFadeDistanceM = rSettings->getEdgeFadeDistanceM();
	m_config.bHeightRangeEnabled = rSettings->getHeightRangeEnabled();
	m_config.fMinimumHeightM = rSettings->getMinimumHeightM();
	m_config.fMaximumHeightM = rSettings->getMaximumHeightM();
	m_config.fHeightRangeFalloffM = rSettings->getHeightRangeFalloffM();

	m_config.bHydraulicEnabled = rSettings->getHydraulicEnabled();
	m_config.fRainfallMPerYear = rSettings->getRainfallMPerYear();
	m_config.fRainfallVariation = rSettings->getRainfallVariation();
	m_config.fEvaporationFraction = rSettings->getEvaporationFraction();
	m_config.fInfiltrationFraction = rSettings->getInfiltrationFraction();
	m_config.fFlowRate = rSettings->getFlowRate();
	m_config.fSedimentCapacity = rSettings->getSedimentCapacity();
	m_config.fHydraulicErosionMPerYear = rSettings->getHydraulicErosionMPerYear();
	m_config.fHydraulicDepositionRate = rSettings->getHydraulicDepositionRate();
	m_config.fMinimumSlope = rSettings->getMinimumSlope();
	m_config.fStormFrequency = rSettings->getStormFrequency();
	m_config.fStormIntensity = rSettings->getStormIntensity();
	m_config.fChannelIncisionStrength = rSettings->getChannelIncisionStrength();
	m_config.fStandingWaterDepositionRate = rSettings->getStandingWaterDepositionRate();
	m_config.bSedimentSortingEnabled = rSettings->getSedimentSortingEnabled();
	m_config.fCoarseTransportMultiplier = rSettings->getCoarseTransportMultiplier();
	m_config.fSandTransportMultiplier = rSettings->getSandTransportMultiplier();
	m_config.fFineTransportMultiplier = rSettings->getFineTransportMultiplier();
	m_config.fCoarseSettlingRate = rSettings->getCoarseSettlingRate();
	m_config.fSandSettlingRate = rSettings->getSandSettlingRate();
	m_config.fFineSettlingRate = rSettings->getFineSettlingRate();
	m_config.bAlluvialFanEnabled = rSettings->getAlluvialFanEnabled();
	m_config.fAlluvialFanStrength = rSettings->getAlluvialFanStrength();
	m_config.fAlluvialFanSlopeDrop = rSettings->getAlluvialFanSlopeDrop();
	m_config.fAlluvialFanSpread = rSettings->getAlluvialFanSpread();
	m_config.bDebrisFlowEnabled = rSettings->getDebrisFlowEnabled();
	m_config.fDebrisFlowConcentrationThreshold = rSettings->getDebrisFlowConcentrationThreshold();
	m_config.fDebrisFlowSlopeThreshold = rSettings->getDebrisFlowSlopeThreshold();
	m_config.fDebrisFlowRate = rSettings->getDebrisFlowRate();
	m_config.fDebrisFlowDepositionRate = rSettings->getDebrisFlowDepositionRate();
	m_config.debrisFlowRunoutCells = rSettings->getDebrisFlowRunoutCells();

	m_config.bWeatheringEnabled = rSettings->getWeatheringEnabled();
	m_config.fPhysicalWeatheringMPerYear = rSettings->getPhysicalWeatheringMPerYear();
	m_config.fChemicalWeatheringMPerYear = rSettings->getChemicalWeatheringMPerYear();
	m_config.fFreezeThawStrength = rSettings->getFreezeThawStrength();
	m_config.fTalusAngleDegrees = rSettings->getTalusAngleDegrees();
	m_config.fThermalCreepRate = rSettings->getThermalCreepRate();
	m_config.fBaseHardness = rSettings->getBaseHardness();
	m_config.fHardnessVariation = rSettings->getHardnessVariation();
	m_config.bOxidationEnabled = rSettings->getOxidationEnabled();
	m_config.fOxidationRatePerYear = rSettings->getOxidationRatePerYear();
	m_config.fOxidationWeakening = rSettings->getOxidationWeakening();
	m_config.fWeatheringSlopeBias = rSettings->getWeatheringSlopeBias();
	m_config.fChemicalMoistureExponent = rSettings->getChemicalMoistureExponent();
	m_config.fFreezePoint = rSettings->getFreezePoint();
	m_config.bWetDryEnabled = rSettings->getWetDryEnabled();
	m_config.fWetDryWeatheringMPerCycle = rSettings->getWetDryWeatheringMPerCycle();
	m_config.fWetThreshold = rSettings->getWetThreshold();
	m_config.fDryThreshold = rSettings->getDryThreshold();
	m_config.bSaltWeatheringEnabled = rSettings->getSaltWeatheringEnabled();
	m_config.salinityWorldRect = rSettings->getSalinityWorldRect();
	m_config.fBackgroundSalinity = rSettings->getBackgroundSalinity();
	m_config.fSaltWeatheringMPerYear = rSettings->getSaltWeatheringMPerYear();
	m_config.fSaltCrystallizationStrength = rSettings->getSaltCrystallizationStrength();
	m_config.fSaltEvaporationBias = rSettings->getSaltEvaporationBias();
	m_config.fSaltCohesionLoss = rSettings->getSaltCohesionLoss();
	m_config.bExposureAgeEnabled = rSettings->getExposureAgeEnabled();
	m_config.fVarnishMaturationYears = rSettings->getVarnishMaturationYears();
	m_config.fExposureResetDepthM = rSettings->getExposureResetDepthM();
	m_config.fVarnishStabilitySlope = rSettings->getVarnishStabilitySlope();
	m_config.fVarnishOxidationMultiplier = rSettings->getVarnishOxidationMultiplier();
	m_config.bSnowmeltEnabled = rSettings->getSnowmeltEnabled();
	m_config.fSnowfallFraction = rSettings->getSnowfallFraction();
	m_config.fSnowAccumulationTemperature = rSettings->getSnowAccumulationTemperature();
	m_config.fSnowMeltTemperature = rSettings->getSnowMeltTemperature();
	m_config.fSnowMeltFractionPerStep = rSettings->getSnowMeltFractionPerStep();
	m_config.fSnowSublimationFraction = rSettings->getSnowSublimationFraction();

	m_config.bWindEnabled = rSettings->getWindEnabled();
	m_config.windDirection = rSettings->getWindDirection();
	m_config.fWindStrength = rSettings->getWindStrength();
	m_config.fWindVariation = rSettings->getWindVariation();
	m_config.fWindErosionMPerYear = rSettings->getWindErosionMPerYear();
	m_config.fWindTransportRate = rSettings->getWindTransportRate();
	m_config.fWindDepositionRate = rSettings->getWindDepositionRate();
	m_config.fWindShelterStrength = rSettings->getWindShelterStrength();
	m_config.fWindThreshold = rSettings->getWindThreshold();
	m_config.windTransportDistanceCells = rSettings->getWindTransportDistanceCells();
	m_config.fWindDepositionSlopeBias = rSettings->getWindDepositionSlopeBias();
	m_config.bDuneEnabled = rSettings->getDuneEnabled();
	m_config.fSandReposeAngleDegrees = rSettings->getSandReposeAngleDegrees();
	m_config.fDuneAvalancheRate = rSettings->getDuneAvalancheRate();
	m_config.duneRelaxationIterations = rSettings->getDuneRelaxationIterations();
	m_config.fDuneMinSandDepthM = rSettings->getDuneMinSandDepthM();
	m_config.bDesertPavementEnabled = rSettings->getDesertPavementEnabled();
	m_config.fPavementFormationRate = rSettings->getPavementFormationRate();
	m_config.fPavementFineRemovalThreshold = rSettings->getPavementFineRemovalThreshold();
	m_config.fPavementArmorStrength = rSettings->getPavementArmorStrength();
	m_config.fPavementInfiltrationReduction = rSettings->getPavementInfiltrationReduction();

	m_config.bWaveEnabled = rSettings->getWaveEnabled();
	m_config.fSeaLevel = rSettings->getSeaLevel();
	m_config.fTidalRange = rSettings->getTidalRange();
	m_config.waveDirection = rSettings->getWaveDirection();
	m_config.fWaveStrength = rSettings->getWaveStrength();
	m_config.fWaveErosionMPerYear = rSettings->getWaveErosionMPerYear();
	m_config.fWaveDepositionRate = rSettings->getWaveDepositionRate();
	m_config.fCoastlineBandM = rSettings->getCoastlineBandM();
	m_config.fWaveFetchDistanceM = rSettings->getWaveFetchDistanceM();
	m_config.fWaveUndercutBias = rSettings->getWaveUndercutBias();
	m_config.fBeachSmoothing = rSettings->getBeachSmoothing();

	m_config.bTrafficEnabled = rSettings->getTrafficEnabled();
	m_config.trafficWorldRect = rSettings->getTrafficWorldRect();
	m_config.fTrafficErosionMPerYear = rSettings->getTrafficErosionMPerYear();
	m_config.fTrafficCompaction = rSettings->getTrafficCompaction();
	m_config.fTrafficSmoothing = rSettings->getTrafficSmoothing();
	m_config.fTrafficRunoffMultiplier = rSettings->getTrafficRunoffMultiplier();
	m_config.fTrafficThreshold = rSettings->getTrafficThreshold();
	m_config.trafficWideningCells = rSettings->getTrafficWideningCells();

	m_config.bStratigraphyEnabled = rSettings->getStratigraphyEnabled();
	m_config.fSurfaceLayerDepthM = rSettings->getSurfaceLayerDepthM();
	m_config.surfaceLayerDepthWorldRect = rSettings->getSurfaceLayerDepthWorldRect();
	m_config.fBedrockHardness = rSettings->getBedrockHardness();
	m_config.bedrockHardnessWorldRect = rSettings->getBedrockHardnessWorldRect();
	m_config.fStratigraphyTransitionWidthM = rSettings->getStratigraphyTransitionWidthM();

	m_config.bMaterialOutputEnabled = rSettings->getMaterialOutputEnabled();
	m_config.bPreserveControlFlags = rSettings->getPreserveControlFlags();
	m_config.bResolveAutoMaterials = rSettings->getResolveAutoMaterials();
	m_config.fMaterialTransitionStrength = rSettings->getMaterialTransitionStrength();
	m_config.fMaterialTransitionThreshold = rSettings->getMaterialTransitionThreshold();
	m_config.fMaterialTransitionDepthM = rSettings->getMaterialTransitionDepthM();
	m_config.bOxidationColorEnabled = rSettings->getOxidationColorEnabled();
	m_config.oxidationTint = rSettings->getOxidationTint();
	m_config.fOxidationTintStrength = rSettings->getOxidationTintStrength();
}

void T3DErosionJob::sampleCurves( const Ref<T3DErosionSettings> &rSettings ) {
	const int32_t steps = m_config.simulationSteps;
	m_config.curveStride = steps;
	m_config.fCurveSamples.resize( ( size_t ) ( CURVE_COUNT ) * ( size_t ) ( steps ) );
	const eastl::array<Ref<Curve>, CURVE_COUNT> pCurves = {
		rSettings->getMasterIntensityCurve(), rSettings->getRainfallCurve(),
		rSettings->getWeatheringCurve(), rSettings->getOxidationCurve(), rSettings->getWindCurve(),
		rSettings->getWaveCurve(), rSettings->getTrafficCurve(), rSettings->getTemperatureCurve(),
		rSettings->getSeaLevelCurve(), rSettings->getSnowmeltCurve(),
		rSettings->getSaltWeatheringCurve(), rSettings->getDebrisFlowCurve(),
	};
	const eastl::array<float, CURVE_COUNT> defaults = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f,
		1.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f};
	for ( int32_t curve = 0; curve < CURVE_COUNT; ++curve ) {
		float *pDestination = m_config.fCurveSamples.data() + ( size_t ) ( curve ) * ( size_t ) ( steps );
		for ( int32_t step = 0; step < steps; ++step ) {
			const float fT = ( ( float ) ( step ) + 0.5f ) / ( float ) ( steps );
			pDestination[step] = pCurves[( size_t ) ( curve )].is_valid() ? pCurves[( size_t ) ( curve )]->sample_baked( fT )
														 : defaults[( size_t ) ( curve )];
		}
	}
}

float T3DErosionJob::sampleCurve( const t3dcurveId_t curve, const int32_t nStep ) const {
	if ( m_config.curveStride <= 0 || m_config.fCurveSamples.empty() ) {
		if ( curve == CURVE_TEMPERATURE ) {
			return 0.5f;
		}
		if ( curve == CURVE_SEA_LEVEL ) {
			return 0.0f;
		}
		return 1.0f;
	}
	const int32_t step = CLAMP( nStep, 0, m_config.curveStride - 1 );
	return m_config.fCurveSamples[( size_t ) ( curve ) * ( size_t ) ( m_config.curveStride ) + ( size_t ) ( step )];
}

uint64_t T3DErosionJob::estimateMemoryBytes( const int32_t nRegionCount, const int32_t nScale ) const {
	if ( m_nSourceRegionSize <= 0 || nScale <= 0 || nRegionCount <= 0 ) {
		return std::numeric_limits<uint64_t>::max();
	}

	const bool bWeatherStage = m_config.bWeatheringEnabled || m_config.bOxidationEnabled || m_config.bWetDryEnabled ||
		m_config.bSaltWeatheringEnabled || m_config.bExposureAgeEnabled || m_config.bStratigraphyEnabled;
	const bool bHydraulicFamily = m_config.bHydraulicEnabled || m_config.bDebrisFlowEnabled;
	const bool bSurfaceFamily = bWeatherStage || bHydraulicFamily || m_config.bWindEnabled || m_config.bDuneEnabled ||
		m_config.bDesertPavementEnabled || m_config.bWaveEnabled || m_config.bTrafficEnabled ||
		m_config.bMaterialOutputEnabled;
	const bool bHardnessFamily = bWeatherStage || m_config.bHydraulicEnabled || m_config.bWindEnabled ||
		m_config.bWaveEnabled || m_config.bTrafficEnabled;

	uint64_t bytes_per_cell = sizeof( uint8_t ) + 3ULL * sizeof( float );
	addMapBytes( bytes_per_cell, m_config.bLivePreviewEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, bSurfaceFamily, 3u, sizeof( float ) );
	addMapBytes( bytes_per_cell, bHardnessFamily, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, bHardnessFamily, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, bSurfaceFamily, 2u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, bSurfaceFamily, 1u, sizeof( uint8_t ) );
	addMapBytes( bytes_per_cell, bHydraulicFamily, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bHydraulicEnabled && m_config.bSnowmeltEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, bHydraulicFamily, 3u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bHydraulicEnabled || m_config.bMaterialOutputEnabled, 2u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bHydraulicEnabled, 2u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bWindEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bWindEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bWaveEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bWaveEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bTrafficEnabled, 2u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bTrafficEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, bWeatherStage || m_config.bMaterialOutputEnabled, 2u, sizeof( float ) );
	addMapBytes( bytes_per_cell,
		bWeatherStage || m_config.bMaterialOutputEnabled || m_config.bOxidationColorEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bSaltWeatheringEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bWeatheringEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bOxidationEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bSaltWeatheringEnabled, 2u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bWetDryEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bWetDryEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bWetDryEnabled, 1u, sizeof( uint8_t ) );
	addMapBytes( bytes_per_cell, m_config.bExposureAgeEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bAlluvialFanEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bDebrisFlowEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bDuneEnabled || m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bDesertPavementEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bStratigraphyEnabled, 2u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bStratigraphyEnabled, 1u, sizeof( uint16_t ) );
	addMapBytes( bytes_per_cell, m_config.bMaterialOutputEnabled, 1u, sizeof( uint8_t ) );
	addMapBytes( bytes_per_cell, m_config.bMaterialOutputEnabled, 1u, sizeof( float ) );

	addMapBytes( bytes_per_cell,
		( m_config.bWeatheringEnabled && m_config.fThermalCreepRate > 0.0f ) || m_config.bHydraulicEnabled ||
			m_config.bDuneEnabled,
		TERRAIN3D_EROSION_DIRECTION_COUNT, sizeof( float ) );
	addMapBytes( bytes_per_cell,
		m_config.bHydraulicEnabled || ( m_config.bWaveEnabled && m_config.fBeachSmoothing > 0.0f ) ||
			m_config.bTrafficEnabled,
		1u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bHydraulicEnabled, 4u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bAlluvialFanEnabled || m_config.bWaveEnabled, 1u, sizeof( float ) );
	addMapBytes( bytes_per_cell,
		m_config.bAlluvialFanEnabled || m_config.bDebrisFlowEnabled || m_config.bWindEnabled, 2u, sizeof( float ) );
	addMapBytes( bytes_per_cell, m_config.bDebrisFlowEnabled || m_config.bWindEnabled, 1u, sizeof( float ) );
	if ( m_config.bAlluvialFanEnabled || m_config.bDebrisFlowEnabled || m_config.bWindEnabled || m_config.bWaveEnabled ) {
		bytes_per_cell += sizeof( int64_t );
	}
	addMapBytes( bytes_per_cell, m_config.bExposureAgeEnabled, 1u, sizeof( float ) );

	const uint64_t simulation_size = ( uint64_t ) ( m_nSourceRegionSize / nScale );
	const uint64_t simulation_cells = ( uint64_t ) ( nRegionCount ) * simulation_size * simulation_size;
	const uint64_t source_cells =
		( uint64_t ) ( nRegionCount ) * ( uint64_t ) ( m_nSourceRegionSize ) * ( uint64_t ) ( m_nSourceRegionSize );
	const uint64_t source_snapshot_bytes = source_cells * sizeof( float ) + m_nSourceControlSnapshotBytes;
	const uint64_t working_bytes = simulation_cells * bytes_per_cell;
	return source_snapshot_bytes + m_nInputSnapshotBytes + working_bytes + working_bytes / 5ULL + 1024ULL * 1024ULL;
}

int32_t T3DErosionJob::selectResolutionScale( const int32_t nRegionCount ) const {
	int32_t scale = m_config.requestedResolutionScale;
	const uint64_t budget = ( uint64_t ) ( m_config.memoryBudgetMb ) * 1024ULL * 1024ULL;
	if ( !m_config.bAutoAdjustResolution ) {
		return scale;
	}
	while ( scale < 16 && estimateMemoryBytes( nRegionCount, scale ) > budget ) {
		scale *= 2;
	}
	return scale;
}

bool T3DErosionJob::allocateBuffers( void ) {
	const bool bWeatherStage = m_config.bWeatheringEnabled || m_config.bOxidationEnabled || m_config.bWetDryEnabled ||
		m_config.bSaltWeatheringEnabled || m_config.bExposureAgeEnabled || m_config.bStratigraphyEnabled;
	const bool bHydraulicFamily = m_config.bHydraulicEnabled || m_config.bDebrisFlowEnabled;
	const bool bSurfaceFamily = bWeatherStage || bHydraulicFamily || m_config.bWindEnabled || m_config.bDuneEnabled ||
		m_config.bDesertPavementEnabled || m_config.bWaveEnabled || m_config.bTrafficEnabled ||
		m_config.bMaterialOutputEnabled;
	const bool bHardnessFamily = bWeatherStage || m_config.bHydraulicEnabled || m_config.bWindEnabled ||
		m_config.bWaveEnabled || m_config.bTrafficEnabled;
	const bool bNeedsFlux = ( m_config.bWeatheringEnabled && m_config.fThermalCreepRate > 0.0f ) ||
		m_config.bHydraulicEnabled || m_config.bDuneEnabled;
	const bool bNeedsNextA = m_config.bHydraulicEnabled ||
		( m_config.bWaveEnabled && m_config.fBeachSmoothing > 0.0f ) || m_config.bTrafficEnabled;
	const bool bNeedsTransferAmount = m_config.bAlluvialFanEnabled || m_config.bWaveEnabled;
	const bool bNeedsTransferCoarseSand =
		m_config.bAlluvialFanEnabled || m_config.bDebrisFlowEnabled || m_config.bWindEnabled;
	const bool bNeedsTransferFine = m_config.bDebrisFlowEnabled || m_config.bWindEnabled;
	const bool bNeedsTransferDestination =
		m_config.bAlluvialFanEnabled || m_config.bDebrisFlowEnabled || m_config.bWindEnabled || m_config.bWaveEnabled;

	m_active.clear();
	m_simulationWeight.clear();
	m_initialHeight.clear();
	m_height.clear();
	m_surfaceCoarse.clear();
	m_surfaceSand.clear();
	m_surfaceFine.clear();
	m_substrateHardness.clear();
	m_hardness.clear();
	m_cohesion.clear();
	m_grainSize.clear();
	m_dominantTexture.clear();
	m_water.clear();
	m_snowpack.clear();
	m_sedimentCoarse.clear();
	m_sedimentSand.clear();
	m_sedimentFine.clear();
	m_flowAccumulation.clear();
	m_hydraulicDeposition.clear();
	m_hydraulicErodibility.clear();
	m_infiltrationMultiplier.clear();
	m_windDeposition.clear();
	m_windErodibility.clear();
	m_waveDeposition.clear();
	m_waveErodibility.clear();
	m_traffic.clear();
	m_compaction.clear();
	m_trafficErodibility.clear();
	m_weathering.clear();
	m_oxidation.clear();
	m_saltWeathering.clear();
	m_previousMoisture.clear();
	m_weatheringSusceptibility.clear();
	m_oxidationSusceptibility.clear();
	m_salinity.clear();
	m_saltSusceptibility.clear();
	m_wetDryCycles.clear();
	m_wetDrySusceptibility.clear();
	m_wetState.clear();
	m_exposureAge.clear();
	m_alluvialDeposition.clear();
	m_debrisFlowDeposition.clear();
	m_duneDeposition.clear();
	m_desertPavement.clear();
	m_cumulativeErosion.clear();
	m_surfaceLayerDepth.clear();
	m_bedrockHardness.clear();
	m_materialTransitionKind.clear();
	m_materialTransitionAmount.clear();
	m_nextA.clear();
	m_nextB.clear();
	m_nextCoarse.clear();
	m_nextSand.clear();
	m_nextFine.clear();
	for ( FloatBuffer &plane : m_szFlux ) {
		plane.clear();
	}
	m_transferAmount.clear();
	m_transferCoarse.clear();
	m_transferSand.clear();
	m_transferFine.clear();
	m_transferDestination.clear();
	m_stepHeightStart.clear();
	m_persistentMemory.release();
	m_phaseMemory.release();

	T3DLinearArenaLayout persistent_layout;
	T3DLinearArenaLayout phase_layout;
	bool bLayoutValid = true;
	keepValid( planByteBuffer( persistent_layout, true, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, true, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, true, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, true, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bSurfaceFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bSurfaceFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bSurfaceFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, bHardnessFamily, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bHardnessFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, bSurfaceFamily, m_nCellCount ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, bSurfaceFamily, m_nCellCount ), bLayoutValid );
	keepValid( planByteBuffer( persistent_layout, bSurfaceFamily, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bHydraulicFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled && m_config.bSnowmeltEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bHydraulicFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bHydraulicFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bHydraulicFamily, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bWindEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bWindEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bWaveEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bWaveEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bTrafficEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bTrafficEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bTrafficEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bWeatherStage || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bWeatherStage || m_config.bMaterialOutputEnabled || m_config.bOxidationColorEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bWeatherStage || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bSaltWeatheringEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bWeatheringEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bOxidationEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bSaltWeatheringEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bSaltWeatheringEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bWetDryEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bWetDryEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planByteBuffer( persistent_layout, m_config.bWetDryEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bExposureAgeEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bAlluvialFanEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bDebrisFlowEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bDuneEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bDesertPavementEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bStratigraphyEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bStratigraphyEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planHalfBuffer( persistent_layout, m_config.bStratigraphyEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planByteBuffer( persistent_layout, m_config.bMaterialOutputEnabled, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bMaterialOutputEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, bNeedsNextA, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( persistent_layout, m_config.bHydraulicEnabled, m_nCellCount, 1u ), bLayoutValid );
	for ( int32_t plane = 0; plane < TERRAIN3D_EROSION_DIRECTION_COUNT; ++plane ) {
		keepValid( planFloatBuffer( phase_layout, bNeedsFlux, m_nCellCount, 1u ), bLayoutValid );
	}
	keepValid( planFloatBuffer( phase_layout, bNeedsTransferAmount, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( phase_layout, bNeedsTransferCoarseSand, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( phase_layout, bNeedsTransferCoarseSand, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planFloatBuffer( phase_layout, bNeedsTransferFine, m_nCellCount, 1u ), bLayoutValid );
	keepValid( planIndexBuffer( phase_layout, bNeedsTransferDestination, m_nCellCount ), bLayoutValid );
	keepValid( planFloatBuffer( phase_layout, m_config.bExposureAgeEnabled, m_nCellCount, 1u ), bLayoutValid );

	if ( !bLayoutValid || !persistent_layout.valid() || !phase_layout.valid() ||
		!m_persistentMemory.configure( persistent_layout.size() ) || !m_phaseMemory.configure( phase_layout.size() ) ) {
		m_persistentMemory.release();
		m_phaseMemory.release();
		setFailed( "Erosion buffer layout exceeds the configured address space or could not be allocated." );
		return false;
	}

	bool bAllocationValid = true;
	keepValid( assignByteBuffer( m_persistentMemory, m_active, true, m_nCellCount, ( uint8_t ) ( 1 ) ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_simulationWeight, true, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_initialHeight, true, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_height, true, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_surfaceCoarse, bSurfaceFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_surfaceSand, bSurfaceFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_surfaceFine, bSurfaceFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_substrateHardness, bHardnessFamily, m_nCellCount, m_config.fBaseHardness ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_hardness, bHardnessFamily, m_nCellCount, m_config.fBaseHardness ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_cohesion, bSurfaceFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_grainSize, bSurfaceFamily, m_nCellCount, 0.5f ), bAllocationValid );
	keepValid( assignByteBuffer( m_persistentMemory, m_dominantTexture, bSurfaceFamily, m_nCellCount, ( uint8_t ) ( 0 ) ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_water, bHydraulicFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_snowpack, m_config.bHydraulicEnabled && m_config.bSnowmeltEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_sedimentCoarse, bHydraulicFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_sedimentSand, bHydraulicFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_sedimentFine, bHydraulicFamily, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_flowAccumulation, m_config.bHydraulicEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_hydraulicDeposition, m_config.bHydraulicEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_hydraulicErodibility, m_config.bHydraulicEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_infiltrationMultiplier, m_config.bHydraulicEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_windDeposition, m_config.bWindEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_windErodibility, m_config.bWindEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_waveDeposition, m_config.bWaveEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_waveErodibility, m_config.bWaveEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_traffic, m_config.bTrafficEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_compaction, m_config.bTrafficEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_trafficErodibility, m_config.bTrafficEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_weathering, bWeatherStage || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_oxidation, bWeatherStage || m_config.bMaterialOutputEnabled || m_config.bOxidationColorEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_saltWeathering, bWeatherStage || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_previousMoisture, m_config.bSaltWeatheringEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_weatheringSusceptibility, m_config.bWeatheringEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_oxidationSusceptibility, m_config.bOxidationEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_salinity, m_config.bSaltWeatheringEnabled, m_nCellCount, m_config.fBackgroundSalinity ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_saltSusceptibility, m_config.bSaltWeatheringEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_wetDryCycles, m_config.bWetDryEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_wetDrySusceptibility, m_config.bWetDryEnabled, m_nCellCount, 1.0f ), bAllocationValid );
	keepValid( assignByteBuffer( m_persistentMemory, m_wetState, m_config.bWetDryEnabled, m_nCellCount, ( uint8_t ) ( 0 ) ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_exposureAge, m_config.bExposureAgeEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_alluvialDeposition, m_config.bAlluvialFanEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_debrisFlowDeposition, m_config.bDebrisFlowEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_duneDeposition, m_config.bDuneEnabled || m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_desertPavement, m_config.bDesertPavementEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_cumulativeErosion, m_config.bStratigraphyEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_surfaceLayerDepth, m_config.bStratigraphyEnabled, m_nCellCount, m_config.fSurfaceLayerDepthM ), bAllocationValid );
	keepValid( assignHalfBuffer( m_persistentMemory, m_bedrockHardness, m_config.bStratigraphyEnabled, m_nCellCount, m_config.fBedrockHardness ), bAllocationValid );
	keepValid( assignByteBuffer( m_persistentMemory, m_materialTransitionKind, m_config.bMaterialOutputEnabled, m_nCellCount, ( uint8_t ) ( TRANSITION_NONE ) ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_materialTransitionAmount, m_config.bMaterialOutputEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_nextA, bNeedsNextA, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_nextB, m_config.bHydraulicEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_nextCoarse, m_config.bHydraulicEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_nextSand, m_config.bHydraulicEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_persistentMemory, m_nextFine, m_config.bHydraulicEnabled, m_nCellCount, 0.0f ), bAllocationValid );
	for ( FloatBuffer &plane : m_szFlux ) {
		keepValid( assignFloatBuffer( m_phaseMemory, plane, bNeedsFlux, m_nCellCount, 0.0f ), bAllocationValid );
	}
	keepValid( assignFloatBuffer( m_phaseMemory, m_transferAmount, bNeedsTransferAmount, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_phaseMemory, m_transferCoarse, bNeedsTransferCoarseSand, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_phaseMemory, m_transferSand, bNeedsTransferCoarseSand, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignFloatBuffer( m_phaseMemory, m_transferFine, bNeedsTransferFine, m_nCellCount, 0.0f ), bAllocationValid );
	keepValid( assignIndexBuffer( m_phaseMemory, m_transferDestination, bNeedsTransferDestination, m_nCellCount, ( int64_t ) ( -1 ) ), bAllocationValid );
	keepValid( assignFloatBuffer( m_phaseMemory, m_stepHeightStart, m_config.bExposureAgeEnabled, m_nCellCount, 0.0f ), bAllocationValid );

	if ( !bAllocationValid || m_persistentMemory.used() != persistent_layout.size() ||
		m_phaseMemory.used() != phase_layout.size() ) {
		m_persistentMemory.release();
		m_phaseMemory.release();
		setFailed( "Erosion buffer allocation did not match its exact arena layout." );
		return false;
	}
	if ( !allocatePreviewBuffer() ) {
		m_persistentMemory.release();
		m_phaseMemory.release();
		return false;
	}
	return true;
}

bool T3DErosionJob::allocatePreviewBuffer( void ) {
	std::lock_guard<std::mutex> lock( m_previewMutex );
	m_previewHeight.clear();
	m_previewMemory.release();
	m_previewRevision.store( 0 );
	m_previewStep.store( 0 );
	m_bPreviewWaiting.store( false );
	m_previewStepRequests.store( 0 );
	m_bPreviewPaused.store( m_config.bLivePreviewEnabled && m_config.bLivePreviewStartPaused );
	if ( !m_config.bLivePreviewEnabled ) {
		return true;
	}
	T3DLinearArenaLayout layout;
	if ( !layout.addArray<float>( m_nCellCount ) || !layout.valid() || !m_previewMemory.configure( layout.size() ) ||
		!m_previewHeight.allocateUninitialized( m_previewMemory, m_nCellCount ) ||
		m_previewMemory.used() != layout.size() ) {
		m_previewHeight.clear();
		m_previewMemory.release();
		setFailed( "Live erosion preview could not allocate its exact height snapshot." );
		return false;
	}
	return true;
}

void T3DErosionJob::publishPreviewSnapshot( const int32_t nStep, const bool bForce ) {
	if ( !m_config.bLivePreviewEnabled || m_previewHeight.empty() || m_height.empty() ) {
		return;
	}
	const int32_t interval = MAX( 1, m_config.livePreviewIntervalSteps );
	if ( !bForce && !m_bPreviewPaused.load() && nStep > 0 && nStep < m_config.simulationSteps &&
		nStep % interval != 0 ) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock( m_previewMutex );
		if ( !m_previewHeight.copyFrom( m_height ) ) {
			return;
		}
		m_previewStep.store( CLAMP( nStep, 0, m_config.simulationSteps ) );
		m_previewRevision.fetch_add( 1 );
	}
}

bool T3DErosionJob::waitForPreviewPermission( void ) {
	if ( !m_config.bLivePreviewEnabled || !m_bPreviewPaused.load() ) {
		return !m_bCancelRequested.load();
	}
	std::unique_lock<std::mutex> lock( m_previewMutex );
	m_bPreviewWaiting.store( true );
	while ( !m_bCancelRequested.load() && m_bPreviewPaused.load() && m_previewStepRequests.load() <= 0 ) {
		m_previewCondition.wait( lock );
	}
	m_bPreviewWaiting.store( false );
	if ( m_bCancelRequested.load() ) {
		return false;
	}
	if ( m_bPreviewPaused.load() ) {
		const int32_t requests = m_previewStepRequests.load();
		if ( requests > 0 ) {
			m_previewStepRequests.fetch_sub( 1 );
		}
	}
	return true;
}

Ref<Image> T3DErosionJob::makeHeightImage(
	const FloatBuffer &rSource, const int32_t nRegionId,
	const bool bFullResolution
) const {
	if ( rSource.empty() || nRegionId < 0 || nRegionId >= ( int32_t ) ( m_regions.size() ) ) {
		return Ref<Image>();
	}
	const int32_t size = bFullResolution ? m_nSourceRegionSize : m_nSimulationRegionSize;
	const size_t pixel_count = ( size_t ) ( size ) * ( size_t ) ( size );
	PackedByteArray image_data;
	image_data.resize( ( int64_t ) ( pixel_count * sizeof( float ) ) );
	uint8_t *const pDestination = image_data.ptrw();
	const size_t source_offset = ( size_t ) ( nRegionId ) * m_nCellsPerRegion;
	if ( !bFullResolution ) {
		std::memcpy( pDestination, rSource.data() + source_offset, pixel_count * sizeof( float ) );
		return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
	}
	for ( int32_t y = 0; y < size; ++y ) {
		for ( int32_t x = 0; x < size; ++x ) {
			const float fValue = sampleOutput( rSource, nRegionId, ( float ) ( x ), ( float ) ( y ) );
			const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
			std::memcpy( pDestination + index * sizeof( float ), &fValue, sizeof( float ) );
		}
	}
	return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
}

bool T3DErosionJob::compactCompletedResults( void ) {
	{
		std::lock_guard<std::mutex> lock( m_statusMutex );
		m_bResultsCompacted = false;
		m_nRetainedResultBytes = 0u;
	}
	clearFloatBufferIfZero( m_sedimentCoarse );
	clearFloatBufferIfZero( m_sedimentSand );
	clearFloatBufferIfZero( m_sedimentFine );
	clearFloatBufferIfZero( m_oxidation );
	clearFloatBufferIfZero( m_flowAccumulation );
	clearFloatBufferIfZero( m_hydraulicDeposition );
	clearFloatBufferIfZero( m_windDeposition );
	clearFloatBufferIfZero( m_waveDeposition );
	clearFloatBufferIfZero( m_weathering );
	clearFloatBufferIfZero( m_traffic );
	clearFloatBufferIfZero( m_alluvialDeposition );
	clearFloatBufferIfZero( m_debrisFlowDeposition );
	clearFloatBufferIfZero( m_duneDeposition );
	clearFloatBufferIfZero( m_saltWeathering );
	clearFloatBufferIfZero( m_wetDryCycles );
	clearFloatBufferIfZero( m_snowpack );
	clearFloatBufferIfZero( m_exposureAge );
	clearFloatBufferIfZero( m_desertPavement );
	clearFloatBufferIfZero( m_cumulativeErosion );

	if ( !m_materialTransitionKind.empty() ) {
		bool bHasTransition = false;
		for ( const uint8_t kind : m_materialTransitionKind ) {
			if ( kind != ( uint8_t ) ( TRANSITION_NONE ) ) {
				bHasTransition = true;
				break;
			}
		}
		if ( !bHasTransition ) {
			m_materialTransitionKind.clear();
			m_materialTransitionAmount.clear();
			m_dominantTexture.clear();
		}
	} else {
		m_dominantTexture.clear();
	}

	t3derosionResultBuffers_t result;
	T3DLinearArenaLayout layout;
	bool valid = true;
	keepValid( planResultByte( layout, m_active ), valid );
	keepValid( planResultFloat( layout, m_initialHeight ), valid );
	keepValid( planResultFloat( layout, m_height ), valid );
	keepValid( planResultFloat( layout, m_sedimentCoarse ), valid );
	keepValid( planResultFloat( layout, m_sedimentSand ), valid );
	keepValid( planResultFloat( layout, m_sedimentFine ), valid );
	keepValid( planResultFloat( layout, m_surfaceCoarse ), valid );
	keepValid( planResultFloat( layout, m_surfaceSand ), valid );
	keepValid( planResultFloat( layout, m_surfaceFine ), valid );
	keepValid( planResultFloat( layout, m_oxidation ), valid );
	keepValid( planResultFloat( layout, m_flowAccumulation ), valid );
	keepValid( planResultFloat( layout, m_hydraulicDeposition ), valid );
	keepValid( planResultFloat( layout, m_windDeposition ), valid );
	keepValid( planResultFloat( layout, m_waveDeposition ), valid );
	keepValid( planResultFloat( layout, m_weathering ), valid );
	keepValid( planResultFloat( layout, m_traffic ), valid );
	keepValid( planResultFloat( layout, m_alluvialDeposition ), valid );
	keepValid( planResultFloat( layout, m_debrisFlowDeposition ), valid );
	keepValid( planResultFloat( layout, m_duneDeposition ), valid );
	keepValid( planResultFloat( layout, m_saltWeathering ), valid );
	keepValid( planResultFloat( layout, m_wetDryCycles ), valid );
	keepValid( planResultFloat( layout, m_snowpack ), valid );
	keepValid( planResultFloat( layout, m_exposureAge ), valid );
	keepValid( planResultFloat( layout, m_desertPavement ), valid );
	keepValid( planResultFloat( layout, m_cumulativeErosion ), valid );
	keepValid( planResultByte( layout, m_dominantTexture ), valid );
	keepValid( planResultByte( layout, m_materialTransitionKind ), valid );
	keepValid( planResultFloat( layout, m_materialTransitionAmount ), valid );
	if ( !valid || !layout.valid() ) {
		return false;
	}

	m_resultMemory.release();
	if ( !m_resultMemory.configure( layout.size() ) ) {
		return false;
	}
	keepValid( copyResultByte( m_resultMemory, m_active, result.bActive ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_initialHeight, result.initial_height ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_height, result.height ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_sedimentCoarse, result.sediment_coarse ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_sedimentSand, result.sediment_sand ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_sedimentFine, result.sediment_fine ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_surfaceCoarse, result.surface_coarse ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_surfaceSand, result.surface_sand ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_surfaceFine, result.surface_fine ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_oxidation, result.oxidation ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_flowAccumulation, result.flow_accumulation ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_hydraulicDeposition, result.hydraulic_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_windDeposition, result.wind_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_waveDeposition, result.wave_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_weathering, result.weathering ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_traffic, result.traffic ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_alluvialDeposition, result.alluvial_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_debrisFlowDeposition, result.debris_flow_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_duneDeposition, result.dune_deposition ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_saltWeathering, result.salt_weathering ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_wetDryCycles, result.wet_dry_cycles ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_snowpack, result.snowpack ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_exposureAge, result.exposure_age ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_desertPavement, result.desert_pavement ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_cumulativeErosion, result.cumulative_erosion ), valid );
	keepValid( copyResultByte( m_resultMemory, m_dominantTexture, result.dominant_texture ), valid );
	keepValid( copyResultByte( m_resultMemory, m_materialTransitionKind, result.material_transition_kind ), valid );
	keepValid( copyResultFloat( m_resultMemory, m_materialTransitionAmount, result.material_transition_amount ), valid );
	if ( !valid || m_resultMemory.used() != layout.size() ) {
		m_resultMemory.release();
		return false;
	}

	m_active.swap( result.bActive );
	m_initialHeight.swap( result.initial_height );
	m_height.swap( result.height );
	m_sedimentCoarse.swap( result.sediment_coarse );
	m_sedimentSand.swap( result.sediment_sand );
	m_sedimentFine.swap( result.sediment_fine );
	m_surfaceCoarse.swap( result.surface_coarse );
	m_surfaceSand.swap( result.surface_sand );
	m_surfaceFine.swap( result.surface_fine );
	m_oxidation.swap( result.oxidation );
	m_flowAccumulation.swap( result.flow_accumulation );
	m_hydraulicDeposition.swap( result.hydraulic_deposition );
	m_windDeposition.swap( result.wind_deposition );
	m_waveDeposition.swap( result.wave_deposition );
	m_weathering.swap( result.weathering );
	m_traffic.swap( result.traffic );
	m_alluvialDeposition.swap( result.alluvial_deposition );
	m_debrisFlowDeposition.swap( result.debris_flow_deposition );
	m_duneDeposition.swap( result.dune_deposition );
	m_saltWeathering.swap( result.salt_weathering );
	m_wetDryCycles.swap( result.wet_dry_cycles );
	m_snowpack.swap( result.snowpack );
	m_exposureAge.swap( result.exposure_age );
	m_desertPavement.swap( result.desert_pavement );
	m_cumulativeErosion.swap( result.cumulative_erosion );
	m_dominantTexture.swap( result.dominant_texture );
	m_materialTransitionKind.swap( result.material_transition_kind );
	m_materialTransitionAmount.swap( result.material_transition_amount );

	m_simulationWeight.clear();
	m_water.clear();
	m_substrateHardness.clear();
	m_hardness.clear();
	m_compaction.clear();
	m_previousMoisture.clear();
	m_salinity.clear();
	m_surfaceLayerDepth.clear();
	m_bedrockHardness.clear();
	m_hydraulicErodibility.clear();
	m_windErodibility.clear();
	m_waveErodibility.clear();
	m_weatheringSusceptibility.clear();
	m_oxidationSusceptibility.clear();
	m_saltSusceptibility.clear();
	m_wetDrySusceptibility.clear();
	m_infiltrationMultiplier.clear();
	m_trafficErodibility.clear();
	m_cohesion.clear();
	m_grainSize.clear();
	for ( FloatBuffer &plane : m_szFlux ) {
		plane.clear();
	}
	m_nextA.clear();
	m_nextB.clear();
	m_nextCoarse.clear();
	m_nextSand.clear();
	m_nextFine.clear();
	m_transferAmount.clear();
	m_transferCoarse.clear();
	m_transferSand.clear();
	m_transferFine.clear();
	m_transferDestination.clear();
	m_stepHeightStart.clear();
	m_phaseMemory.release();
	m_persistentMemory.release();
	{
		std::lock_guard<std::mutex> lock( m_statusMutex );
		m_bResultsCompacted = true;
		m_nRetainedResultBytes = ( uint64_t ) ( m_resultMemory.capacity() );
	}
	return true;
}

uint64_t T3DErosionJob::hashRegionImages( const Ref<Image> &rHeight, const Ref<Image> &rControl ) const {
	uint64_t hash = TERRAIN3D_EROSION_FNV_OFFSET;
	if ( rHeight.is_valid() ) {
		fnvHashPackedBytes( hash, rHeight->get_data() );
	}
	if ( rControl.is_valid() ) {
		fnvHashPackedBytes( hash, rControl->get_data() );
	} else {
		hash ^= ( uint64_t ) ( TERRAIN3D_REGION_DEFAULT_CONTROL );
		hash *= TERRAIN3D_EROSION_FNV_PRIME;
	}
	return hash;
}

uint64_t T3DErosionJob::hashTextureProfiles( T3DData *const pData ) const {
	if ( !pData || !pData->m_pTerrain ) {
		return 0;
	}
	const Ref<T3DAssets> pAssets = pData->m_pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		return 0;
	}

	uint64_t hash = TERRAIN3D_EROSION_FNV_OFFSET;

	for ( int32_t texture_id = 0; texture_id < TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
		const Ref<T3DTextureAsset> pAsset = pAssets->getTextureAsset( texture_id );
		fnvHashU8( hash, pAsset.is_valid() ? 1u : 0u );
		if ( pAsset.is_null() ) {
			continue;
		}
		fnvHashI32( hash, ( int32_t ) ( pAsset->getId() ) );
		fnvHashU8( hash, pAsset->getErosionEnabled() ? 1u : 0u );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionHardness() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionHydraulicErodibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionWindErodibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionWaveErodibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionWeatheringSusceptibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionOxidationSusceptibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionSaltSusceptibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionWetDrySusceptibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionInfiltrationMultiplier() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionTrafficErodibility() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionCohesion() ) );
		fnvHashF64( hash, ( double ) ( pAsset->getErosionGrainSize() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionExposedTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionWeatheredTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionOxidizedTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionCoarseDepositTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionFineDepositTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionSaltWeatheredTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionPavementTextureId() ) );
		fnvHashI32( hash, ( int32_t ) ( pAsset->getErosionVarnishedTextureId() ) );
	}

	if ( m_config.bResolveAutoMaterials ) {
		const Ref<T3DMaterial> pMaterial = pData->m_pTerrain->getMaterial();
		fnvHashU8( hash, ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) ? 1u : 0u );
		if ( pMaterial.is_valid() ) {
			fnvHashI32( hash, ( int32_t ) ( ( int32_t ) ( pMaterial->getShaderParam( "auto_base_texture" ) ) ) );
			fnvHashI32( hash, ( int32_t ) ( ( int32_t ) ( pMaterial->getShaderParam( "auto_overlay_texture" ) ) ) );
			fnvHashF64( hash, ( double ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_slope" ) ) ) );
			fnvHashF64( hash, ( double ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_height_reduction" ) ) ) );
		}
	}
	return hash;
}

bool T3DErosionJob::snapshotTextureProfiles( T3DData *const pData ) {
	if ( !pData || !pData->m_pTerrain ) {
		setFailed(
			"Erosion requires Terrain3DData initialized by a Terrain3D node so texture assets can be resolved." );
		return false;
	}
	const Ref<T3DAssets> pAssets = pData->m_pTerrain->getAssets();
	if ( pAssets.is_null() ) {
		setFailed( "Erosion could not read the Terrain3DAssets resource." );
		return false;
	}

	bool bHasTransition = false;
	for ( int32_t texture_id = 0; texture_id < TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
		t3dtextureErosionProfile_t &profile = m_config.szTextureProfiles[( size_t ) ( texture_id )];
		const Ref<T3DTextureAsset> pAsset = pAssets->getTextureAsset( texture_id );
		if ( pAsset.is_null() ) {
			continue;
		}
		profile.bValid = true;
		profile.bEnabled = pAsset->getErosionEnabled();
		profile.fHardness = ( float ) ( pAsset->getErosionHardness() );
		profile.fHydraulicErodibility = ( float ) ( pAsset->getErosionHydraulicErodibility() );
		profile.fWindErodibility = ( float ) ( pAsset->getErosionWindErodibility() );
		profile.fWaveErodibility = ( float ) ( pAsset->getErosionWaveErodibility() );
		profile.fWeatheringSusceptibility = ( float ) ( pAsset->getErosionWeatheringSusceptibility() );
		profile.fOxidationSusceptibility = ( float ) ( pAsset->getErosionOxidationSusceptibility() );
		profile.fSaltSusceptibility = ( float ) ( pAsset->getErosionSaltSusceptibility() );
		profile.fWetDrySusceptibility = ( float ) ( pAsset->getErosionWetDrySusceptibility() );
		profile.fInfiltrationMultiplier = ( float ) ( pAsset->getErosionInfiltrationMultiplier() );
		profile.fTrafficErodibility = ( float ) ( pAsset->getErosionTrafficErodibility() );
		profile.fCohesion = ( float ) ( pAsset->getErosionCohesion() );
		profile.fGrainSize = ( float ) ( pAsset->getErosionGrainSize() );
		profile.exposedTextureId = pAsset->getErosionExposedTextureId();
		profile.weatheredTextureId = pAsset->getErosionWeatheredTextureId();
		profile.oxidizedTextureId = pAsset->getErosionOxidizedTextureId();
		profile.coarseDepositTextureId = pAsset->getErosionCoarseDepositTextureId();
		profile.fineDepositTextureId = pAsset->getErosionFineDepositTextureId();
		profile.saltWeatheredTextureId = pAsset->getErosionSaltWeatheredTextureId();
		profile.pavementTextureId = pAsset->getErosionPavementTextureId();
		profile.varnishedTextureId = pAsset->getErosionVarnishedTextureId();

		const eastl::array<int32_t, 8> targets = {
			profile.exposedTextureId,		   profile.weatheredTextureId,	profile.oxidizedTextureId,
			profile.coarseDepositTextureId, profile.fineDepositTextureId, profile.saltWeatheredTextureId,
			profile.pavementTextureId,	   profile.varnishedTextureId,
		};
		for ( const int32_t target : targets ) {
			if ( target < 0 ) {
				continue;
			}
			if ( target == texture_id ) {
				setFailed( String( "Texture asset " ) + itos( texture_id ) + " contains an erosion transition to itself." );
				return false;
			}
			bHasTransition = true;
			if ( pAssets->getTextureAsset( target ).is_null() ) {
				setFailed( String( "Texture asset " ) + itos( texture_id ) +
							" references missing erosion transition texture ID " + itos( target ) + "." );
				return false;
			}
		}
	}

	if ( m_config.bResolveAutoMaterials ) {
		const Ref<T3DMaterial> pMaterial = pData->m_pTerrain->getMaterial();
		if ( pMaterial.is_valid() ) {
			m_config.bAutoShaderEnabled = pMaterial->getAutoShaderEnabled();
			m_config.autoBaseTexture =
				CLAMP( ( int32_t ) ( pMaterial->getShaderParam( "auto_base_texture" ) ), 0, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
			m_config.autoOverlayTexture =
				CLAMP( ( int32_t ) ( pMaterial->getShaderParam( "auto_overlay_texture" ) ), 0, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
			m_config.fAutoSlope = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_slope" ) ) );
			m_config.fAutoHeightReduction = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_height_reduction" ) ) );
		}
	}

	if ( m_config.bMaterialOutputEnabled && !bHasTransition ) {
		setFailed(
			"Material output is enabled, but no Terrain3DTextureAsset has an erosion transition texture configured." );
		return false;
	}
	m_assetProfileHash = hashTextureProfiles( pData );
	return true;
}

bool T3DErosionJob::regionSnapshotLess( const t3dregionSnapshot_t &rA, const t3dregionSnapshot_t &rB ) {
	return rA.location.y == rB.location.y ? rA.location.x < rB.location.x : rA.location.y < rB.location.y;
}

bool T3DErosionJob::captureSourceRegions( T3DData *const pData ) {
	if ( !pData ) {
		setFailed( "Erosion source capture requires valid Terrain3DData." );
		return false;
	}
	const TypedArray<Vector2i> locations = pData->getRegionLocations();
	m_regions.clear();
	m_regions.resize( ( size_t ) ( locations.size() ) );
	std::fill_n( m_szRegionLookup, 1024, ( int32_t ) ( -1 ) );

	const size_t texels = ( size_t ) ( m_nSourceRegionSize ) * ( size_t ) ( m_nSourceRegionSize );
	const size_t expected_height_bytes = texels * sizeof( uint16_t );
	const size_t expected_rf_bytes = texels * sizeof( float );
	for ( int32_t region_id = 0; region_id < locations.size(); ++region_id ) {
		const Vector2i location = locations[region_id];
		if ( !pData->ensureCpuRegionResident( location ) ) {
			setFailed( "A terrain region could not be paged into the CPU map cache for erosion capture." );
			return false;
		}
		const Ref<T3DRegion> pRegion = pData->getRegion( location );
		if ( pRegion.is_null() ) {
			setFailed( "An active terrain region disappeared while creating the erosion source snapshot." );
			return false;
		}
		const Ref<Image> pHeightMap = pRegion->getHeightMap();
		const Ref<Image> pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			pHeightMap->get_width() != m_nSourceRegionSize || pHeightMap->get_height() != m_nSourceRegionSize ||
			( pControlMap.is_valid() && ( pControlMap->get_format() != Image::FORMAT_RF ||
				pControlMap->get_width() != m_nSourceRegionSize || pControlMap->get_height() != m_nSourceRegionSize ) ) ) {
			setFailed( "Erosion requires compact R16 height maps and optional RF control maps." );
			return false;
		}

		const PackedByteArray encoded_height = pHeightMap->get_data();
		if ( ( size_t ) ( encoded_height.size() ) < expected_height_bytes ) {
			setFailed( "A Terrain3D region contains truncated R16 height data." );
			return false;
		}
		t3dregionSnapshot_t &snapshot = m_regions[( size_t ) ( region_id )];
		snapshot.location = location;
		snapshot.sourceOrder = region_id;
		snapshot.heightData.resize( ( int64_t ) ( expected_rf_bytes ) );
		uint8_t *const pHeightDst = snapshot.heightData.ptrw();
		const uint8_t *pHeightSrc = encoded_height.ptr();
		PackedByteArray control_source;
		const uint8_t *pControlSrc = nullptr;
		if ( pControlMap.is_valid() ) {
			snapshot.controlData.resize( ( int64_t ) ( expected_rf_bytes ) );
			control_source = pControlMap->get_data();
			if ( ( size_t ) ( control_source.size() ) < expected_rf_bytes ) {
				setFailed( "A Terrain3D region contains truncated control data." );
				return false;
			}
			pControlSrc = control_source.ptr();
			std::memcpy( snapshot.controlData.ptrw(), pControlSrc, expected_rf_bytes );
		} else {
			snapshot.controlData.clear();
		}
		for ( size_t i = 0; i < texels; ++i ) {
			writeRf( pHeightDst, i, Terrain3DRegionCodec::readHeightBytes( pHeightSrc, ( int ) ( i ), Image::FORMAT_R16 ) );
		}
		snapshot.sourceHash = hashRegionImages( pHeightMap, pControlMap );
		const int32_t map_index = T3DData::getRegionMapIndex( location );
		if ( map_index >= 0 ) {
			m_szRegionLookup[map_index] = region_id;
		}
	}
	// Keep neighboring regions adjacent in every solver plane. Active region
	// order is editor/history dependent and otherwise turns cross-boundary reads
	// into arbitrary jumps through the working set.
	std::sort( m_regions.begin(), m_regions.end(), regionSnapshotLess );
	std::fill_n( m_szRegionLookup, 1024, ( int32_t ) ( -1 ) );
	for ( int32_t region_id = 0; region_id < ( int32_t ) ( m_regions.size() ); ++region_id ) {
		const int32_t map_index = T3DData::getRegionMapIndex( m_regions[( size_t ) ( region_id )].location );
		if ( map_index >= 0 ) {
			m_szRegionLookup[map_index] = region_id;
		}
	}
	m_szRegionNeighbors.resize( m_regions.size() );
	for ( int32_t region_id = 0; region_id < ( int32_t ) ( m_regions.size() ); ++region_id ) {
		const Vector2i location = m_regions[( size_t ) ( region_id )].location;
		eastl::array<int32_t, TERRAIN3D_EROSION_DIRECTION_COUNT> &neighbors = m_szRegionNeighbors[( size_t ) ( region_id )];
		for ( int32_t direction = 0; direction < TERRAIN3D_EROSION_DIRECTION_COUNT; ++direction ) {
			const int32_t map_index = T3DData::getRegionMapIndex(
				location + Vector2i( TERRAIN3D_EROSION_DIRECTION_X[direction], TERRAIN3D_EROSION_DIRECTION_Y[direction] ) );
			neighbors[( size_t ) ( direction )] = map_index >= 0 ? m_szRegionLookup[map_index] : -1;
		}
	}
	return true;
}

void T3DErosionJob::releaseSourceSnapshots( void ) {
	for ( t3dregionSnapshot_t &snapshot : m_regions ) {
		snapshot.heightData.clear();
		snapshot.controlData.clear();
	}
	m_simulationMaskSnapshot.clear();
	m_trafficMaskSnapshot.clear();
	m_salinityMaskSnapshot.clear();
	m_surfaceLayerDepthSnapshot.clear();
	m_bedrockHardnessSnapshot.clear();
}

float T3DErosionJob::sampleImageSnapshot(
	const t3dimageSnapshot_t &rImage, const Rect2 &rWorldRect,
	const Vector2 &rWorldPosition
) {
	if ( !rImage.isValid() || rWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON || rWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		return 1.0f;
	}
	const Vector2 uv = ( rWorldPosition - rWorldRect.position ) / rWorldRect.size;
	if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ) {
		return 0.0f;
	}
	const float fPx = uv.x * ( float ) ( MAX( 1, rImage.nSize.x - 1 ) );
	const float fPy = uv.y * ( float ) ( MAX( 1, rImage.nSize.y - 1 ) );
	const int32_t x0 = CLAMP( ( int32_t ) ( Math::floor( fPx ) ), 0, rImage.nSize.x - 1 );
	const int32_t y0 = CLAMP( ( int32_t ) ( Math::floor( fPy ) ), 0, rImage.nSize.y - 1 );
	const int32_t x1 = MIN( x0 + 1, rImage.nSize.x - 1 );
	const int32_t y1 = MIN( y0 + 1, rImage.nSize.y - 1 );
	const float fFx = fPx - ( float ) ( x0 );
	const float fFy = fPy - ( float ) ( y0 );
	const uint8_t *pBytes = rImage.rfData.ptr();
	const size_t stride = ( size_t ) ( rImage.nSize.x );
	const float fV00 = readRf( pBytes, ( size_t ) ( y0 ) * stride + ( size_t ) ( x0 ) );
	const float fV10 = readRf( pBytes, ( size_t ) ( y0 ) * stride + ( size_t ) ( x1 ) );
	const float fV01 = readRf( pBytes, ( size_t ) ( y1 ) * stride + ( size_t ) ( x0 ) );
	const float fV11 = readRf( pBytes, ( size_t ) ( y1 ) * stride + ( size_t ) ( x1 ) );
	return clamp01( Math::lerp( Math::lerp( fV00, fV10, fFx ), Math::lerp( fV01, fV11, fFx ), fFy ) );
}

bool T3DErosionJob::buildSimulationSnapshot( void ) {
	const int32_t region_count = ( int32_t ) ( m_regions.size() );
	if ( region_count <= 0 ) {
		setFailed( "Erosion source snapshot contains no active regions." );
		return false;
	}

	Vector2i minimumLocation( INT32_MAX, INT32_MAX );
	Vector2i maximumLocation( INT32_MIN, INT32_MIN );
	for ( int32_t region_id = 0; region_id < region_count; ++region_id ) {
		const Vector2i location = m_regions[( size_t ) ( region_id )].location;
		minimumLocation.x = MIN( minimumLocation.x, location.x );
		minimumLocation.y = MIN( minimumLocation.y, location.y );
		maximumLocation.x = MAX( maximumLocation.x, location.x );
		maximumLocation.y = MAX( maximumLocation.y, location.y );
	}

	const Vector2 world_min = Vector2( minimumLocation.x, minimumLocation.y ) * ( float ) ( m_nSourceRegionSize ) * m_fVertexSpacing;
	const Vector2 world_max =
		Vector2( maximumLocation.x + 1, maximumLocation.y + 1 ) * ( float ) ( m_nSourceRegionSize ) * m_fVertexSpacing;
	m_terrainWorldRect = Rect2( world_min, world_max - world_min );
	if ( m_config.simulationMaskWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON || m_config.simulationMaskWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		m_config.simulationMaskWorldRect = m_terrainWorldRect;
	}
	if ( m_config.trafficWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON || m_config.trafficWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		m_config.trafficWorldRect = m_terrainWorldRect;
	}
	if ( m_config.salinityWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON || m_config.salinityWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		m_config.salinityWorldRect = m_terrainWorldRect;
	}
	if ( m_config.surfaceLayerDepthWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON ||
		m_config.surfaceLayerDepthWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		m_config.surfaceLayerDepthWorldRect = m_terrainWorldRect;
	}
	if ( m_config.bedrockHardnessWorldRect.size.x <= TERRAIN3D_EROSION_EPSILON ||
		m_config.bedrockHardnessWorldRect.size.y <= TERRAIN3D_EROSION_EPSILON ) {
		m_config.bedrockHardnessWorldRect = m_terrainWorldRect;
	}

	const t3dimageSnapshot_t &simulation_mask_image = m_simulationMaskSnapshot;
	const t3dimageSnapshot_t &traffic_image = m_trafficMaskSnapshot;
	const t3dimageSnapshot_t &salinity_image = m_salinityMaskSnapshot;
	const t3dimageSnapshot_t &surface_depth_image = m_surfaceLayerDepthSnapshot;
	const t3dimageSnapshot_t &bedrock_hardness_image = m_bedrockHardnessSnapshot;



	for ( int32_t region_id = 0; region_id < region_count; ++region_id ) {
		if ( m_bCancelRequested.load() ) {
			return false;
		}
		const t3dregionSnapshot_t &snapshot = m_regions[( size_t ) ( region_id )];
		const uint8_t *pHeightBytes = snapshot.heightData.ptr();
		const uint8_t *pControlBytes = snapshot.controlData.is_empty() ? nullptr : snapshot.controlData.ptr();

		for ( int32_t sy = 0; sy < m_nSimulationRegionSize; ++sy ) {
			for ( int32_t sx = 0; sx < m_nSimulationRegionSize; ++sx ) {
				double fHeightSum = 0.0;
				int32_t valid_count = 0;
				int32_t hole_count = 0;
				for ( int32_t oy = 0; oy < m_resolutionScale; ++oy ) {
					const int32_t source_y = sy * m_resolutionScale + oy;
					for ( int32_t ox = 0; ox < m_resolutionScale; ++ox ) {
						const int32_t source_x = sx * m_resolutionScale + ox;
						const size_t source_index = ( size_t ) ( source_y ) * ( size_t ) ( m_nSourceRegionSize ) + ( size_t ) ( source_x );
						const float fSourceHeight = readRf( pHeightBytes, source_index );
						const uint32_t control = pControlBytes ? asUint( readRf( pControlBytes, source_index ) )
													 : TERRAIN3D_REGION_DEFAULT_CONTROL;
						if ( isHole( control ) ) {
							hole_count += 1;
						}
						if ( !std::isnan( fSourceHeight ) ) {
							fHeightSum += fSourceHeight;
							valid_count += 1;
						}
					}
				}

				size_t index =
					( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
				bool bActive = valid_count > 0;
				if ( m_config.bPreserveHoles && hole_count * 2 >= m_resolutionScale * m_resolutionScale ) {
					bActive = false;
				}
				m_active[index] = bActive ? ( uint8_t ) ( 1 ) : ( uint8_t ) ( 0 );
				const float fValue = valid_count > 0 ? ( float ) ( fHeightSum / ( double ) ( valid_count ) ) : 0.0f;
				m_initialHeight[index] = fValue;
				m_height[index] = fValue;

				const Vector2i region_location = m_regions[( size_t ) ( region_id )].location;
				const Vector2 world_position =
					( Vector2(
						region_location.x * m_nSourceRegionSize + sx * m_resolutionScale + m_resolutionScale * 0.5f,
						region_location.y * m_nSourceRegionSize + sy * m_resolutionScale + m_resolutionScale * 0.5f ) ) *
					m_fVertexSpacing;

				float fSimulationWeight =
					sampleImageSnapshot( simulation_mask_image, m_config.simulationMaskWorldRect, world_position );
				if ( m_config.bInvertSimulationMask && simulation_mask_image.isValid() ) {
					fSimulationWeight = 1.0f - fSimulationWeight;
				}
				if ( m_config.bHeightRangeEnabled ) {
					const float fMinimumHeight = MIN( m_config.fMinimumHeightM, m_config.fMaximumHeightM );
					const float fMaximumHeight = MAX( m_config.fMinimumHeightM, m_config.fMaximumHeightM );
					float fRangeWeight = 1.0f;
					if ( fValue < fMinimumHeight ) {
						fRangeWeight = m_config.fHeightRangeFalloffM > TERRAIN3D_EROSION_EPSILON
										   ? clamp01( ( fValue - ( fMinimumHeight - m_config.fHeightRangeFalloffM ) ) /
													 m_config.fHeightRangeFalloffM )
										   : 0.0f;
					} else if ( fValue > fMaximumHeight ) {
						fRangeWeight = m_config.fHeightRangeFalloffM > TERRAIN3D_EROSION_EPSILON
										   ? clamp01( ( ( fMaximumHeight + m_config.fHeightRangeFalloffM ) - fValue ) /
													 m_config.fHeightRangeFalloffM )
										   : 0.0f;
					}
					fSimulationWeight *= fRangeWeight;
				}
				if ( m_config.fEdgeFadeDistanceM > TERRAIN3D_EROSION_EPSILON ) {
					const float fLeft = world_position.x - m_terrainWorldRect.position.x;
					const float fRight = m_terrainWorldRect.get_end().x - world_position.x;
					const float fTop = world_position.y - m_terrainWorldRect.position.y;
					const float fBottom = m_terrainWorldRect.get_end().y - world_position.y;
					const float fEdgeDistance = MIN( MIN( fLeft, fRight ), MIN( fTop, fBottom ) );
					fSimulationWeight *= clamp01( fEdgeDistance / m_config.fEdgeFadeDistanceM );
				}
				m_simulationWeight[index] = clamp01( fSimulationWeight );

				if ( m_config.bTrafficEnabled && traffic_image.isValid() ) {
					const float fSampled = sampleImageSnapshot( traffic_image, m_config.trafficWorldRect, world_position );
					m_traffic[index] = fSampled > m_config.fTrafficThreshold
										  ? clamp01( ( fSampled - m_config.fTrafficThreshold ) /
													MAX( 1.0f - m_config.fTrafficThreshold, TERRAIN3D_EROSION_EPSILON ) )
										  : 0.0f;
				}
				if ( m_config.bSaltWeatheringEnabled ) {
					const float fMappedSalinity =
						salinity_image.isValid()
							? sampleImageSnapshot( salinity_image, m_config.salinityWorldRect, world_position )
							: 0.0f;
					m_salinity[index] =
						clamp01( m_config.fBackgroundSalinity + fMappedSalinity * ( 1.0f - m_config.fBackgroundSalinity ) );
				}
				if ( m_config.bStratigraphyEnabled ) {
					const float fDepthFactor =
						surface_depth_image.isValid()
							? sampleImageSnapshot( surface_depth_image, m_config.surfaceLayerDepthWorldRect, world_position )
							: 1.0f;
					m_surfaceLayerDepth[index] = MAX( 0.0f, m_config.fSurfaceLayerDepthM * fDepthFactor );
					m_bedrockHardness[index] =
						bedrock_hardness_image.isValid()
							? clamp01( sampleImageSnapshot( bedrock_hardness_image, m_config.bedrockHardnessWorldRect,
												  world_position ) )
							: m_config.fBedrockHardness;
				}
			}
		}
		m_fProgress.store( 0.02f + 0.08f * ( float ) ( region_id + 1 ) / ( float ) ( MAX( 1, region_count ) ) );
	}

	if ( m_config.bTrafficEnabled && m_config.trafficWideningCells > 0 ) {
		for ( int32_t pass = 0; pass < m_config.trafficWideningCells; ++pass ) {
			for ( size_t index = 0; index < m_nCellCount; ++index ) {
				float fValue = m_traffic[index];
				for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor = neighborIndex( index, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
					if ( neighbor >= 0 ) {
						fValue = MAX( fValue, m_traffic[( size_t ) ( neighbor )] * 0.82f );
					}
				}
				m_nextA[index] = fValue;
			}
			m_traffic.swap( m_nextA );
		}
	}

	const bool bNeedsMaterialProfiles = !m_dominantTexture.empty() || !m_substrateHardness.empty() ||
										 !m_hydraulicErodibility.empty() || !m_windErodibility.empty() ||
										 !m_waveErodibility.empty() || !m_weatheringSusceptibility.empty() ||
										 !m_oxidationSusceptibility.empty() || !m_saltSusceptibility.empty() ||
										 !m_wetDrySusceptibility.empty() || !m_infiltrationMultiplier.empty() ||
										 !m_trafficErodibility.empty() || !m_cohesion.empty() || !m_grainSize.empty();
	if ( !bNeedsMaterialProfiles ) {
		return true;
	}

	// Resolve control-map materials only after every height cell exists. The worker
	// reads immutable RF snapshots only; no Terrain3D or Image resource is touched.
	for ( int32_t region_id = 0; region_id < region_count; ++region_id ) {
		const PackedByteArray &control_data = m_regions[( size_t ) ( region_id )].controlData;
		const uint8_t *pControlBytes = control_data.is_empty() ? nullptr : control_data.ptr();

		for ( int32_t sy = 0; sy < m_nSimulationRegionSize; ++sy ) {
			for ( int32_t sx = 0; sx < m_nSimulationRegionSize; ++sx ) {
				const size_t index =
					( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
				eastl::array<double, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT> weights{};
				const float fSlope = getSlope( index );
				const float fNormalY = 1.0f / Math::sqrt( 1.0f + fSlope * fSlope );
				const float fAutoBlend = clamp01( ( m_config.fAutoSlope * 2.0f * ( fNormalY - 1.0f ) + 1.0f ) -
												 m_config.fAutoHeightReduction * 0.01f * m_height[index] );

				for ( int32_t oy = 0; oy < m_resolutionScale; ++oy ) {
					const int32_t source_y = sy * m_resolutionScale + oy;
					for ( int32_t ox = 0; ox < m_resolutionScale; ++ox ) {
						const int32_t source_x = sx * m_resolutionScale + ox;
						const size_t source_index = ( size_t ) ( source_y ) * ( size_t ) ( m_nSourceRegionSize ) + ( size_t ) ( source_x );
						const uint32_t control = pControlBytes ? asUint( readRf( pControlBytes, source_index ) )
													 : TERRAIN3D_REGION_DEFAULT_CONTROL;
						if ( isHole( control ) ) {
							continue;
						}
						int32_t base = ( int32_t ) ( getBase( control ) );
						int32_t overlay = ( int32_t ) ( getOverlay( control ) );
						float fBlend = ( float ) ( getBlend( control ) ) / TERRAIN3D_BYTE_MAX_FLOAT;
						if ( m_config.bResolveAutoMaterials && m_config.bAutoShaderEnabled && isAuto( control ) ) {
							base = m_config.autoBaseTexture;
							overlay = m_config.autoOverlayTexture;
							fBlend = fAutoBlend;
						}
						base = CLAMP( base, 0, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
						overlay = CLAMP( overlay, 0, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
						weights[( size_t ) ( base )] += ( double ) ( 1.0f - fBlend );
						weights[( size_t ) ( overlay )] += ( double ) ( fBlend );
					}
				}

				double fTotalWeight = std::accumulate( weights.begin(), weights.end(), 0.0 );
				int32_t dominant = 0;
				for ( int32_t texture_id = 1; texture_id < TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
					if ( weights[( size_t ) ( texture_id )] > weights[( size_t ) ( dominant )] ) {
						dominant = texture_id;
					}
				}
				if ( !m_dominantTexture.empty() ) {
					m_dominantTexture[index] = ( uint8_t ) ( dominant );
				}
				if ( fTotalWeight <= TERRAIN3D_EROSION_EPSILON ) {
					fTotalWeight = 1.0;
					weights[( size_t ) ( dominant )] = 1.0;
				}

				double hardness = 0.0;
				double fHydraulic = 0.0;
				double fWind = 0.0;
				double fWave = 0.0;
				double fWeathering = 0.0;
				double fOxidation = 0.0;
				double fSalt = 0.0;
				double fWetDry = 0.0;
				double fInfiltration = 0.0;
				double fTraffic = 0.0;
				double cohesion = 0.0;
				double grain_size = 0.0;
				for ( int32_t texture_id = 0; texture_id < TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
					const double fWeight = weights[( size_t ) ( texture_id )] / fTotalWeight;
					if ( fWeight <= 0.0 ) {
						continue;
					}
					const t3dtextureErosionProfile_t &profile = m_config.szTextureProfiles[( size_t ) ( texture_id )];
					const float fProfileHardness = profile.fHardness >= 0.0f ? profile.fHardness : m_config.fBaseHardness;
					const bool enabled = profile.bValid ? profile.bEnabled : true;
					hardness += fWeight * ( double ) ( enabled ? fProfileHardness : 1.0f );
					fHydraulic += fWeight * ( double ) ( enabled ? profile.fHydraulicErodibility : 0.0f );
					fWind += fWeight * ( double ) ( enabled ? profile.fWindErodibility : 0.0f );
					fWave += fWeight * ( double ) ( enabled ? profile.fWaveErodibility : 0.0f );
					fWeathering += fWeight * ( double ) ( enabled ? profile.fWeatheringSusceptibility : 0.0f );
					fOxidation += fWeight * ( double ) ( enabled ? profile.fOxidationSusceptibility : 0.0f );
					fSalt += fWeight * ( double ) ( enabled ? profile.fSaltSusceptibility : 0.0f );
					fWetDry += fWeight * ( double ) ( enabled ? profile.fWetDrySusceptibility : 0.0f );
					fInfiltration += fWeight * ( double ) ( profile.fInfiltrationMultiplier );
					fTraffic += fWeight * ( double ) ( enabled ? profile.fTrafficErodibility : 0.0f );
					cohesion += fWeight * ( double ) ( enabled ? profile.fCohesion : 1.0f );
					grain_size += fWeight * ( double ) ( profile.fGrainSize );
				}

				const float fHardnessNoise =
					( hashUnit( m_config.seed, stochasticIndex( index ) ) * 2.0f - 1.0f ) * m_config.fHardnessVariation;
				if ( !m_substrateHardness.empty() ) {
					m_substrateHardness[index] = clamp01( ( float ) ( hardness ) + fHardnessNoise );
				}
				if ( !m_hardness.empty() ) {
					m_hardness[index] = !m_substrateHardness.empty() ? m_substrateHardness[index]
																	: clamp01( ( float ) ( hardness ) + fHardnessNoise );
				}
				if ( !m_hydraulicErodibility.empty() ) {
					m_hydraulicErodibility[index] = MAX( 0.0f, ( float ) ( fHydraulic ) );
				}
				if ( !m_windErodibility.empty() ) {
					m_windErodibility[index] = MAX( 0.0f, ( float ) ( fWind ) );
				}
				if ( !m_waveErodibility.empty() ) {
					m_waveErodibility[index] = MAX( 0.0f, ( float ) ( fWave ) );
				}
				if ( !m_weatheringSusceptibility.empty() ) {
					m_weatheringSusceptibility[index] = MAX( 0.0f, ( float ) ( fWeathering ) );
				}
				if ( !m_oxidationSusceptibility.empty() ) {
					m_oxidationSusceptibility[index] = MAX( 0.0f, ( float ) ( fOxidation ) );
				}
				if ( !m_saltSusceptibility.empty() ) {
					m_saltSusceptibility[index] = MAX( 0.0f, ( float ) ( fSalt ) );
				}
				if ( !m_wetDrySusceptibility.empty() ) {
					m_wetDrySusceptibility[index] = MAX( 0.0f, ( float ) ( fWetDry ) );
				}
				if ( !m_infiltrationMultiplier.empty() ) {
					m_infiltrationMultiplier[index] = MAX( 0.0f, ( float ) ( fInfiltration ) );
				}
				if ( !m_trafficErodibility.empty() ) {
					m_trafficErodibility[index] = MAX( 0.0f, ( float ) ( fTraffic ) );
				}
				if ( !m_cohesion.empty() ) {
					m_cohesion[index] = clamp01( ( float ) ( cohesion ) );
				}
				if ( !m_grainSize.empty() ) {
					m_grainSize[index] = clamp01( ( float ) ( grain_size ) );
				}
			}
		}
	}
	return true;
}

bool T3DErosionJob::captureImageSnapshot(
	const Ref<Image> &rImage, t3dimageSnapshot_t &rSnapshot,
	const char *pszLabel
) {
	rSnapshot.clear();
	if ( rImage.is_null() ) {
		return true;
	}
	Ref<Image> pCopy = Image::create_from_data( rImage->get_width(), rImage->get_height(), rImage->has_mipmaps(),
		rImage->get_format(), rImage->get_data() );
	if ( pCopy.is_null() || pCopy->is_empty() ) {
		setFailed( String( "Failed to snapshot erosion input image: " ) + pszLabel + "." );
		return false;
	}
	pCopy->clear_mipmaps();
	if ( pCopy->get_format() != Image::FORMAT_RF ) {
		pCopy->convert( Image::FORMAT_RF );
	}
	rSnapshot.nSize = pCopy->get_size();
	rSnapshot.rfData = pCopy->get_data();
	if ( !rSnapshot.isValid() ) {
		setFailed( String( "Erosion input image produced invalid RF data: " ) + pszLabel + "." );
		return false;
	}
	return true;
}

Error T3DErosionJob::setup( T3DData *const pData, const Ref<T3DErosionSettings> &rSettings ) {
	if ( m_state.load() != STATE_NEW ) {
		return ERR_ALREADY_IN_USE;
	}
	if ( !pData || rSettings.is_null() ) {
		setFailed( "Erosion job setup requires valid Terrain3DData and Terrain3DErosionSettings objects." );
		return ERR_INVALID_PARAMETER;
	}
	if ( pData->getRegionCount() <= 0 ) {
		setFailed( "Terrain3D has no active regions to erode." );
		return ERR_DOES_NOT_EXIST;
	}

	setStage( STAGE_SNAPSHOT, 0.0f );
	copyConfig( rSettings );
	sampleCurves( rSettings );
	if ( !snapshotTextureProfiles( pData ) ) {
		return ERR_INVALID_DATA;
	}

	const Ref<Image> pSimulationMask = rSettings->getSimulationMask();
	const Ref<Image> pTrafficMask = rSettings->getTrafficMask();
	const Ref<Image> pSalinityMask = rSettings->getSalinityMask();
	const Ref<Image> pSurfaceLayerDepthMap = rSettings->getSurfaceLayerDepthMap();
	const Ref<Image> pBedrockHardnessMap = rSettings->getBedrockHardnessMap();
	if ( pSimulationMask.is_valid() && pSimulationMask->is_empty() ) {
		setFailed( "simulation_mask is assigned but empty." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.bHeightRangeEnabled && Math::is_equal_approx( m_config.fMinimumHeightM, m_config.fMaximumHeightM ) &&
		m_config.fHeightRangeFalloffM <= TERRAIN3D_EROSION_EPSILON ) {
		setFailed( "Height-range limiting has zero width and no falloff." );
		return ERR_INVALID_PARAMETER;
	}

	if ( m_config.bTrafficEnabled ) {
		if ( pTrafficMask.is_null() || pTrafficMask->is_empty() ) {
			setFailed( "Foot traffic simulation is enabled, but traffic_mask is empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.bSaltWeatheringEnabled ) {
		if ( pSalinityMask.is_valid() && pSalinityMask->is_empty() ) {
			setFailed( "salinity_mask is assigned but empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.bStratigraphyEnabled ) {
		if ( ( pSurfaceLayerDepthMap.is_valid() && pSurfaceLayerDepthMap->is_empty() ) ||
			( pBedrockHardnessMap.is_valid() && pBedrockHardnessMap->is_empty() ) ) {
			setFailed( "A stratigraphy input map is assigned but empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.bWetDryEnabled && m_config.fDryThreshold >= m_config.fWetThreshold ) {
		setFailed( "dry_threshold must be lower than wet_threshold for wet-dry hysteresis." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.bSnowmeltEnabled && m_config.fSnowMeltTemperature <= m_config.fSnowAccumulationTemperature ) {
		setFailed( "snow_melt_temperature must be greater than snow_accumulation_temperature." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.bWindEnabled && m_config.windDirection.length_squared() <= TERRAIN3D_EROSION_EPSILON ) {
		setFailed( "wind_direction must be non-zero when wind erosion is enabled." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.bWaveEnabled && m_config.waveDirection.length_squared() <= TERRAIN3D_EROSION_EPSILON ) {
		setFailed( "wave_direction must be non-zero when wave erosion is enabled." );
		return ERR_INVALID_PARAMETER;
	}
	m_nInputSnapshotBytes = 0;

	if ( !addImageSnapshotBytes( m_nInputSnapshotBytes, pSimulationMask ) || !addImageSnapshotBytes( m_nInputSnapshotBytes, pTrafficMask ) ||
		!addImageSnapshotBytes( m_nInputSnapshotBytes, pSalinityMask ) || !addImageSnapshotBytes( m_nInputSnapshotBytes, pSurfaceLayerDepthMap ) ||
		!addImageSnapshotBytes( m_nInputSnapshotBytes, pBedrockHardnessMap ) ) {
		setFailed( "Erosion input image dimensions overflow the memory preflight." );
		return ERR_OUT_OF_MEMORY;
	}

	TypedArray<Vector2i> locations = pData->getRegionLocations();
	const Ref<T3DRegion> pFirstRegion = pData->getRegion( Vector2i( locations[0] ) );
	if ( pFirstRegion.is_null() ) {
		setFailed( "Terrain3D's first active region could not be read." );
		return ERR_DOES_NOT_EXIST;
	}
	m_nSourceRegionSize = pFirstRegion->getRegionSize();
	m_fVertexSpacing = pFirstRegion->getVertexSpacing();
	m_nSourceControlSnapshotBytes = 0;
	const uint64_t source_texel_count = ( uint64_t ) ( m_nSourceRegionSize ) * ( uint64_t ) ( m_nSourceRegionSize );
	const uint64_t control_bytes_per_region = source_texel_count * sizeof( float );
	for ( int32_t i = 0; i < locations.size(); ++i ) {
		const Vector2i location = Vector2i( locations[i] );
		if ( !pData->ensureCpuRegionResident( location ) ) {
			setFailed( "A terrain region could not be paged into the CPU map cache during erosion preflight." );
			return ERR_CANT_OPEN;
		}
		const Ref<T3DRegion> pRegion = pData->getRegion( location );
		if ( pRegion.is_valid() && pRegion->getControlMap().is_valid() ) {
			if ( m_nSourceControlSnapshotBytes > std::numeric_limits<uint64_t>::max() - control_bytes_per_region ) {
				setFailed( "Erosion control snapshot dimensions overflow the memory preflight." );
				return ERR_OUT_OF_MEMORY;
			}
			m_nSourceControlSnapshotBytes += control_bytes_per_region;
		}
	}
	m_resolutionScale = selectResolutionScale( locations.size() );
	if ( m_nSourceRegionSize <= 0 || m_nSourceRegionSize % m_resolutionScale != 0 ) {
		setFailed( "The selected erosion resolution scale does not divide the Terrain3D region size." );
		return ERR_INVALID_DATA;
	}
	const uint64_t budget = ( uint64_t ) ( m_config.memoryBudgetMb ) * 1024ULL * 1024ULL;
	if ( estimateMemoryBytes( locations.size(), m_resolutionScale ) > budget ) {
		setFailed( "The erosion working set exceeds memory_budget_mb even at the maximum supported resolution scale." );
		return ERR_OUT_OF_MEMORY;
	}

	m_nSimulationRegionSize = m_nSourceRegionSize / m_resolutionScale;
	if ( m_nSimulationRegionSize <= 0 || ( m_nSimulationRegionSize & ( m_nSimulationRegionSize - 1 ) ) != 0 ) {
		setFailed( "Erosion simulation regions must remain power-of-two for cache-local indexing." );
		return ERR_INVALID_DATA;
	}
	m_nSimulationRegionShift = ( int32_t ) ( terrain3d::countrZeroU32( ( uint32_t ) ( m_nSimulationRegionSize ) ) );
	m_nSimulationRegionMask = m_nSimulationRegionSize - 1;
	m_fCellSize = m_fVertexSpacing * ( float ) ( m_resolutionScale );
	m_nCellsPerRegion = ( size_t ) ( m_nSimulationRegionSize ) * ( size_t ) ( m_nSimulationRegionSize );
	m_nCellsPerRegionShift = m_nSimulationRegionShift * 2;
	m_nCellsPerRegionMask = m_nCellsPerRegion - 1u;
	m_nCellCount = ( size_t ) ( locations.size() ) * m_nCellsPerRegion;



	if ( !captureImageSnapshot( pSimulationMask, m_simulationMaskSnapshot, "simulation_mask" ) ||
		!captureImageSnapshot( pTrafficMask, m_trafficMaskSnapshot, "traffic_mask" ) ||
		!captureImageSnapshot( pSalinityMask, m_salinityMaskSnapshot, "salinity_mask" ) ||
		!captureImageSnapshot( pSurfaceLayerDepthMap, m_surfaceLayerDepthSnapshot, "surface_layer_depth_map" ) ||
		!captureImageSnapshot( pBedrockHardnessMap, m_bedrockHardnessSnapshot, "bedrock_hardness_map" ) ) {
		return ERR_INVALID_DATA;
	}
	if ( !captureSourceRegions( pData ) ) {
		if ( m_state.load() != STATE_FAILED ) {
			m_state.store( STATE_CANCELLED );
			m_stage.store( STAGE_CANCELLED );
		}
		return ERR_SKIP;
	}

	m_state.store( STATE_READY );
	setStage( STAGE_IDLE, 0.02f );
	return OK;
}

int64_t T3DErosionJob::neighborIndex( const size_t iIndex, const int32_t nDx, const int32_t nDy ) const {
	const size_t local_index = iIndex & m_nCellsPerRegionMask;
	int32_t x = 0;
	int32_t y = 0;
	int32_t direction = -1;
	if ( nDx != 0 ) {
		x = ( int32_t ) ( local_index & ( size_t ) ( m_nSimulationRegionMask ) );
		if ( nDx < 0 ) {
			if ( x > 0 ) {
				return ( int64_t ) ( iIndex ) - 1;
			}
			x = m_nSimulationRegionMask;
			direction = 0;
		} else {
			if ( x < m_nSimulationRegionMask ) {
				return ( int64_t ) ( iIndex ) + 1;
			}
			x = 0;
			direction = 1;
		}
		y = ( int32_t ) ( local_index >> m_nSimulationRegionShift );
	} else if ( nDy != 0 ) {
		y = ( int32_t ) ( local_index >> m_nSimulationRegionShift );
		if ( nDy < 0 ) {
			if ( y > 0 ) {
				return ( int64_t ) ( iIndex ) - ( int64_t ) ( m_nSimulationRegionSize );
			}
			y = m_nSimulationRegionMask;
			direction = 2;
		} else {
			if ( y < m_nSimulationRegionMask ) {
				return ( int64_t ) ( iIndex ) + ( int64_t ) ( m_nSimulationRegionSize );
			}
			y = 0;
			direction = 3;
		}
		x = ( int32_t ) ( local_index & ( size_t ) ( m_nSimulationRegionMask ) );
	}

	// Only boundary cells need region identity; their adjacent region IDs were
	// precomputed once after spatial region sorting.
	const int32_t region_id = ( int32_t ) ( iIndex >> m_nCellsPerRegionShift );
	if ( direction < 0 || ( size_t ) ( region_id ) >= m_szRegionNeighbors.size() ) {
		return -1;
	}
	const int32_t neighbor_region_id = m_szRegionNeighbors[( size_t ) ( region_id )][( size_t ) ( direction )];
	if ( neighbor_region_id < 0 ) {
		return -1;
	}
	return ( int64_t ) ( ( size_t ) ( neighbor_region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( m_nSimulationRegionSize ) +
				   ( size_t ) ( x ) );
}

uint64_t T3DErosionJob::stochasticIndex( const size_t iIndex ) const {
	const size_t region_id = iIndex >> m_nCellsPerRegionShift;
	const size_t local_index = iIndex & m_nCellsPerRegionMask;
	return ( uint64_t ) ( m_regions[region_id].sourceOrder ) * ( uint64_t ) ( m_nCellsPerRegion ) + ( uint64_t ) ( local_index );
}

void T3DErosionJob::partitionMaterial(
	const float fGrainSize, const float fAmount, float &fCoarse, float &fSand,
	float &fFine
) const {
	const float fGrain = clamp01( fGrainSize );
	const float fFineWeight = clamp01( 1.0f - fGrain * 2.0f );
	const float fCoarseWeight = clamp01( fGrain * 2.0f - 1.0f );
	const float fSandWeight = MAX( 0.0f, 1.0f - fFineWeight - fCoarseWeight );
	fCoarse = fAmount * fCoarseWeight;
	fSand = fAmount * fSandWeight;
	fFine = fAmount * fFineWeight;
}

float T3DErosionJob::getLooseMaterial( const size_t iIndex ) const {
	if ( m_surfaceCoarse.empty() ) {
		return 0.0f;
	}
	return MAX( 0.0f, m_surfaceCoarse[iIndex] + m_surfaceSand[iIndex] + m_surfaceFine[iIndex] );
}

float T3DErosionJob::getSediment( const size_t iIndex ) const {
	if ( m_sedimentCoarse.empty() ) {
		return 0.0f;
	}
	return MAX( 0.0f, m_sedimentCoarse[iIndex] + m_sedimentSand[iIndex] + m_sedimentFine[iIndex] );
}

float T3DErosionJob::entrainSurface(
	size_t iIndex, const float fAmount, float &fCoarse, float &fSand,
	float &fFine
) {
	fCoarse = 0.0f;
	fSand = 0.0f;
	fFine = 0.0f;
	float fRemaining = MAX( 0.0f, fAmount );
	if ( fRemaining <= TERRAIN3D_EROSION_EPSILON ) {
		return 0.0f;
	}

	// Preferentially entrain fines, then sand, then coarse lag. This is the
	// mechanism that allows wind and sheetwash to create desert pavement.
	const float fFineTake = MIN( m_surfaceFine[iIndex], fRemaining );
	m_surfaceFine[iIndex] -= fFineTake;
	fFine += fFineTake;
	fRemaining -= fFineTake;
	const float fSandTake = MIN( m_surfaceSand[iIndex], fRemaining );
	m_surfaceSand[iIndex] -= fSandTake;
	fSand += fSandTake;
	fRemaining -= fSandTake;
	const float fCoarseTake = MIN( m_surfaceCoarse[iIndex], fRemaining );
	m_surfaceCoarse[iIndex] -= fCoarseTake;
	fCoarse += fCoarseTake;
	fRemaining -= fCoarseTake;

	if ( fRemaining > TERRAIN3D_EROSION_EPSILON ) {
		float fSubstrateCoarse = 0.0f;
		float fSubstrateSand = 0.0f;
		float fSubstrateFine = 0.0f;
		partitionMaterial( m_grainSize[iIndex], fRemaining, fSubstrateCoarse, fSubstrateSand, fSubstrateFine );
		fCoarse += fSubstrateCoarse;
		fSand += fSubstrateSand;
		fFine += fSubstrateFine;
		fRemaining = 0.0f;
	}
	return fAmount - fRemaining;
}

void T3DErosionJob::depositSurface( size_t iIndex, const float fCoarse, const float fSand, const float fFine ) {
	m_surfaceCoarse[iIndex] += MAX( 0.0f, fCoarse );
	m_surfaceSand[iIndex] += MAX( 0.0f, fSand );
	m_surfaceFine[iIndex] += MAX( 0.0f, fFine );
}

float T3DErosionJob::getSlope( const size_t iIndex ) const {
	if ( !m_active[iIndex] ) {
		return 0.0f;
	}
	float fMaximumDifference = 0.0f;
	for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
		const int64_t neighbor = neighborIndex( iIndex, TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
		if ( neighbor >= 0 && m_active[( size_t ) ( neighbor )] ) {
			fMaximumDifference = MAX( fMaximumDifference, Math::abs( m_height[iIndex] - m_height[( size_t ) ( neighbor )] ) );
		}
	}
	return safeDivide( fMaximumDifference, m_fCellSize );
}

void T3DErosionJob::run( void ) {
	int32_t expected_state = STATE_READY;
	if ( !m_state.compare_exchange_strong( expected_state, STATE_RUNNING ) ) {
		if ( expected_state == STATE_NEW ) {
			setFailed( "Erosion job run() was called before setup completed." );
		}
		return;
	}
	m_bCancelRequested.store( false );
	setStage( STAGE_SNAPSHOT, 0.02f );
	if ( !allocateBuffers() ) {
		releaseSourceSnapshots();
		return;
	}
	if ( !buildSimulationSnapshot() ) {
		releaseSourceSnapshots();
		if ( m_state.load() != STATE_FAILED ) {
			m_state.store( STATE_CANCELLED );
			setStage( STAGE_CANCELLED, 1.0f );
		}
		return;
	}
	releaseSourceSnapshots();
	publishPreviewSnapshot( 0, true );

	const int32_t thread_count = resolveThreadCount( m_config.nThreadCount );
	const ParallelExecutor executor( thread_count );
	const size_t raw_grain = std::max<size_t>( 256, m_nCellCount / ( size_t ) ( MAX( 1, thread_count * 32 ) ) );
	// Every task starts on a complete simulation row and on a cacheline boundary
	// for float, half and byte planes. Adjacent workers therefore never write the
	// same boundary line across the solver's parallel SoA channels.
	const size_t grain_alignment = std::max<size_t>( 64u, ( size_t ) ( m_nSimulationRegionSize ) );
	const size_t grain = ( ( raw_grain + grain_alignment - 1u ) / grain_alignment ) * grain_alignment;
	t3derosionKernelContext_t kernel_context;
	kernel_context.job = this;


	kernel_context.fYearsStep = ( float ) ( m_config.fSimulationYears / ( double ) ( m_config.simulationSteps ) );
	kernel_context.fTalusGradient = Math::tan( Math::deg_to_rad( m_config.fTalusAngleDegrees ) );
	kernel_context.fDuneGradient = Math::tan( Math::deg_to_rad( m_config.fSandReposeAngleDegrees ) );
	float fPreviousTemperature = sampleCurve( CURVE_TEMPERATURE, 0 );

	for ( int32_t step = 0; step < m_config.simulationSteps; ++step ) {
		if ( !waitForPreviewPermission() || m_bCancelRequested.load() ) {
			m_state.store( STATE_CANCELLED );
			setStage( STAGE_CANCELLED, 1.0f );
			m_previewCondition.notify_all();
			return;
		}
		if ( m_config.bExposureAgeEnabled && !m_stepHeightStart.copyFrom( m_height ) ) {
			setFailed( "Erosion exposure-age scratch buffer does not match the height layout." );
			return;
		}

		t3derosionKernels_t::prepareStep( kernel_context, step, fPreviousTemperature );
		const float step_progress = kernel_context.fStepProgress;

		// Weathering creates classed regolith. Oxidation, salt crystallization,
		// wet-dry cycling, exposure age, and stratigraphy feed back into hardness.
		if ( m_config.bWeatheringEnabled || m_config.bOxidationEnabled || m_config.bWetDryEnabled ||
			m_config.bSaltWeatheringEnabled || m_config.bExposureAgeEnabled || m_config.bStratigraphyEnabled ) {
			setStage( STAGE_WEATHERING, 0.1f + 0.8f * step_progress );
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::WEATHERING, m_nCellCount, grain_alignment );

			if ( m_config.bWeatheringEnabled && m_config.fThermalCreepRate > 0.0f ) {
				for ( FloatBuffer &plane : m_szFlux ) {
					std::fill( plane.begin(), plane.end(), 0.0f );
				}
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::THERMAL_FLUX, m_nCellCount, grain_alignment );
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::THERMAL_APPLY, m_nCellCount, grain_alignment );
			}
		}

		if ( m_config.bHydraulicEnabled ) {
			setStage( STAGE_HYDRAULIC, 0.1f + 0.8f * step_progress );
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::RAINFALL, m_nCellCount, grain_alignment );

			for ( FloatBuffer &plane : m_szFlux ) {
				std::fill( plane.begin(), plane.end(), 0.0f );
			}
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::HYDRAULIC_FLUX, m_nCellCount, grain_alignment );

			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::HYDRAULIC_WATER, m_nCellCount, grain_alignment );

			// Capacity is solved independently for coarse bedload, sand, and fines.
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::HYDRAULIC_CAPACITY, m_nCellCount, grain_alignment );

			// Alluvial fans sort and spread bedload where confined, steep flow emerges onto a flatter surface.
			if ( m_config.bAlluvialFanEnabled ) {
				std::fill( m_transferAmount.begin(), m_transferAmount.end(), 0.0f );
				std::fill( m_transferCoarse.begin(), m_transferCoarse.end(), 0.0f );
				std::fill( m_transferSand.begin(), m_transferSand.end(), 0.0f );
				std::fill( m_transferDestination.begin(), m_transferDestination.end(), -1 );
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::DEBRIS_TRANSFER, m_nCellCount, grain_alignment );
				// Deterministic apply pass permits multiple source channels to build the same fan lobe.
				for ( size_t source = 0; source < m_nCellCount; ++source ) {
					const float fCoarse = m_transferCoarse[source];
					const float fSand = m_transferSand[source];
					if ( fCoarse + fSand <= TERRAIN3D_EROSION_EPSILON ) {
						continue;
					}
					m_sedimentCoarse[source] = MAX( 0.0f, m_sedimentCoarse[source] - fCoarse );
					m_sedimentSand[source] = MAX( 0.0f, m_sedimentSand[source] - fSand );
					const float fSpread = m_transferAmount[source];
					const float fLocalCoarse = fCoarse * ( 1.0f - fSpread );
					const float fLocalSand = fSand * ( 1.0f - fSpread );
					depositSurface( source, fLocalCoarse, fLocalSand, 0.0f );
					m_height[source] += fLocalCoarse + fLocalSand;
					m_alluvialDeposition[source] += fLocalCoarse + fLocalSand;
					const int64_t destination = m_transferDestination[source];
					if ( destination >= 0 && fSpread > TERRAIN3D_EROSION_EPSILON ) {
						const float fSpreadCoarse = fCoarse * fSpread;
						const float fSpreadSand = fSand * fSpread;
						depositSurface( ( size_t ) ( destination ), fSpreadCoarse, fSpreadSand, 0.0f );
						m_height[( size_t ) ( destination )] += fSpreadCoarse + fSpreadSand;
						m_alluvialDeposition[( size_t ) ( destination )] += fSpreadCoarse + fSpreadSand;
					}
				}
			}

			// Advect all three suspended classes with the water _flux.
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::SEDIMENT_ADVECTION, m_nCellCount, grain_alignment );
			m_water.swap( m_nextA );
			m_sedimentCoarse.swap( m_nextCoarse );
			m_sedimentSand.swap( m_nextSand );
			m_sedimentFine.swap( m_nextFine );
		}

		if ( m_config.bDebrisFlowEnabled && kernel_context.fDebrisCurve > 0.0f ) {
			setStage( STAGE_DEBRIS_FLOW, 0.1f + 0.8f * step_progress );
			std::fill( m_transferCoarse.begin(), m_transferCoarse.end(), 0.0f );
			std::fill( m_transferSand.begin(), m_transferSand.end(), 0.0f );
			std::fill( m_transferFine.begin(), m_transferFine.end(), 0.0f );
			std::fill( m_transferDestination.begin(), m_transferDestination.end(), -1 );
			// Deterministic serial source pass: runout paths may converge on one cell.
			for ( size_t index = 0; index < m_nCellCount; ++index ) {
				if ( !m_active[index] || m_water[index] <= TERRAIN3D_EROSION_EPSILON ) {
					continue;
				}
				const float fSuspended = m_sedimentCoarse[index] + m_sedimentSand[index] + m_sedimentFine[index];
				const float fMobile = fSuspended + getLooseMaterial( index );
				const float fConcentration = safeDivide( fMobile, fMobile + m_water[index] );
				if ( fConcentration < m_config.fDebrisFlowConcentrationThreshold ||
					getSlope( index ) < m_config.fDebrisFlowSlopeThreshold ) {
					continue;
				}
				int64_t destination = ( int64_t ) ( index );
				for ( int32_t travel = 0; travel < m_config.debrisFlowRunoutCells; ++travel ) {
					int64_t lowest = destination;
					float fLowestHeight = m_height[( size_t ) ( destination )];
					for ( int32_t dir = 0; dir < TERRAIN3D_EROSION_DIRECTION_COUNT; ++dir ) {
						const int64_t neighbor = neighborIndex( ( size_t ) ( destination ), TERRAIN3D_EROSION_DIRECTION_X[dir], TERRAIN3D_EROSION_DIRECTION_Y[dir] );
						if ( neighbor >= 0 && m_active[( size_t ) ( neighbor )] && m_height[( size_t ) ( neighbor )] < fLowestHeight ) {
							lowest = neighbor;
							fLowestHeight = m_height[( size_t ) ( neighbor )];
						}
					}
					if ( lowest == destination ) {
						break;
					}
					destination = lowest;
				}
				if ( destination == ( int64_t ) ( index ) ) {
					continue;
				}
				const float fAmount = MIN( m_config.fMaxHeightChangePerStep,
										 fMobile * m_config.fDebrisFlowRate * kernel_context.fDebrisCurve * m_simulationWeight[index] );
				if ( fAmount <= TERRAIN3D_EROSION_EPSILON ) {
					continue;
				}
				float fC = 0.0f, fS = 0.0f, fF = 0.0f;
				const float fSurfaceAmount = MIN( getLooseMaterial( index ), fAmount );
				entrainSurface( index, fSurfaceAmount, fC, fS, fF );
				m_height[index] -= fSurfaceAmount;
				const float fRemainder = fAmount - fSurfaceAmount;
				const float fSuspendedTotal = MAX( fSuspended, TERRAIN3D_EROSION_EPSILON );
				const float fSc = MIN( m_sedimentCoarse[index], fRemainder * m_sedimentCoarse[index] / fSuspendedTotal );
				const float fSs = MIN( m_sedimentSand[index], fRemainder * m_sedimentSand[index] / fSuspendedTotal );
				const float fSf = MIN( m_sedimentFine[index], fRemainder * m_sedimentFine[index] / fSuspendedTotal );
				m_sedimentCoarse[index] -= fSc;
				m_sedimentSand[index] -= fSs;
				m_sedimentFine[index] -= fSf;
				m_transferCoarse[index] = fC + fSc;
				m_transferSand[index] = fS + fSs;
				m_transferFine[index] = fF + fSf;
				m_transferDestination[index] = destination;
			}
			for ( size_t source = 0; source < m_nCellCount; ++source ) {
				const int64_t destination = m_transferDestination[source];
				if ( destination < 0 ) {
					continue;
				}
				const float fDepositFraction =
					clamp01( m_config.fDebrisFlowDepositionRate * ( 1.0f - clamp01( getSlope( ( size_t ) ( destination ) ) ) ) );
				const float fC = m_transferCoarse[source] * fDepositFraction;
				const float fS = m_transferSand[source] * fDepositFraction;
				const float fF = m_transferFine[source] * fDepositFraction;
				depositSurface( ( size_t ) ( destination ), fC, fS, fF );
				m_height[( size_t ) ( destination )] += fC + fS + fF;
				m_debrisFlowDeposition[( size_t ) ( destination )] += fC + fS + fF;
				m_sedimentCoarse[( size_t ) ( destination )] += m_transferCoarse[source] - fC;
				m_sedimentSand[( size_t ) ( destination )] += m_transferSand[source] - fS;
				m_sedimentFine[( size_t ) ( destination )] += m_transferFine[source] - fF;
			}
		}

		if ( m_config.bWindEnabled && kernel_context.fWindCurve > 0.0f ) {
			setStage( STAGE_WIND, 0.1f + 0.8f * step_progress );
			const int32_t wind_dx =
				Math::abs( m_config.windDirection.x ) >= 0.25f ? ( m_config.windDirection.x >= 0.0f ? 1 : -1 ) : 0;
			const int32_t wind_dy =
				Math::abs( m_config.windDirection.y ) >= 0.25f ? ( m_config.windDirection.y >= 0.0f ? 1 : -1 ) : 0;
			kernel_context.wind_dx = wind_dx;
			kernel_context.wind_dy = wind_dy;
			std::fill( m_transferCoarse.begin(), m_transferCoarse.end(), 0.0f );
			std::fill( m_transferSand.begin(), m_transferSand.end(), 0.0f );
			std::fill( m_transferFine.begin(), m_transferFine.end(), 0.0f );
			std::fill( m_transferDestination.begin(), m_transferDestination.end(), -1 );
			// Wind travel has variable traversal length, so retain dynamic work stealing.
			runDynamicErosionKernel( executor, kernel_context, t3derosionKernel_t::WIND_TRANSPORT, m_nCellCount, grain );
			// Gather incoming wind deposits by following the finite upwind transport corridor.
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::WIND_GATHER, m_nCellCount, grain_alignment );
		}

		if ( m_config.bDuneEnabled ) {
			setStage( STAGE_DUNES, 0.1f + 0.8f * step_progress );
			for ( int32_t iteration = 0; iteration < m_config.duneRelaxationIterations; ++iteration ) {
				for ( FloatBuffer &plane : m_szFlux ) {
					std::fill( plane.begin(), plane.end(), 0.0f );
				}
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::DUNE_FLUX, m_nCellCount, grain_alignment );
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::DUNE_APPLY, m_nCellCount, grain_alignment );
			}
		}

		// Desert pavement is a lag deposit: hydraulic or aeolian removal of fines leaves an armored coarse surface.
		if ( m_config.bDesertPavementEnabled ) {
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::PAVEMENT, m_nCellCount, grain_alignment );
		}

		if ( m_config.bWaveEnabled && kernel_context.fWaveCurve > 0.0f ) {
			setStage( STAGE_WAVES, 0.1f + 0.8f * step_progress );
			const float current_sea_level = m_config.fSeaLevel + sampleCurve( CURVE_SEA_LEVEL, step );
			const int32_t wave_dx =
				Math::abs( m_config.waveDirection.x ) >= 0.25f ? ( m_config.waveDirection.x >= 0.0f ? 1 : -1 ) : 0;
			const int32_t wave_dy =
				Math::abs( m_config.waveDirection.y ) >= 0.25f ? ( m_config.waveDirection.y >= 0.0f ? 1 : -1 ) : 0;
			const int32_t fetch_cells =
				CLAMP( ( int32_t ) ( Math::ceil( m_config.fWaveFetchDistanceM / MAX( m_fCellSize, TERRAIN3D_EROSION_EPSILON ) ) ), 1, 256 );
			const float coastal_band = MAX( m_config.fCoastlineBandM, m_config.fTidalRange );
			kernel_context.fCurrentSeaLevel = current_sea_level;
			kernel_context.fCoastalBand = coastal_band;
			kernel_context.wave_dx = wave_dx;
			kernel_context.wave_dy = wave_dy;
			kernel_context.fetch_cells = fetch_cells;
			std::fill( m_transferAmount.begin(), m_transferAmount.end(), 0.0f );
			std::fill( m_transferDestination.begin(), m_transferDestination.end(), -1 );
			// Fetch length varies strongly with shoreline topology.
			runDynamicErosionKernel( executor, kernel_context, t3derosionKernel_t::WAVE_TRANSPORT, m_nCellCount, grain );
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::WAVE_GATHER, m_nCellCount, grain_alignment );
			if ( m_config.fBeachSmoothing > 0.0f ) {
				runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::BEACH_SMOOTH, m_nCellCount, grain_alignment );
				m_height.swap( m_nextA );
			}
		}

		if ( m_config.bTrafficEnabled && kernel_context.fTrafficCurve > 0.0f ) {
			setStage( STAGE_TRAFFIC, 0.1f + 0.8f * step_progress );
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::TRAFFIC, m_nCellCount, grain_alignment );
			m_height.swap( m_nextA );
		}

		if ( m_config.bStratigraphyEnabled ) {
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::STRATIGRAPHY, m_nCellCount, grain_alignment );
		}

		if ( m_config.bExposureAgeEnabled ) {
			runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::EXPOSURE, m_nCellCount, grain_alignment );
		}

		m_nCompletedSteps.store( step + 1 );
		m_fProgress.store( 0.1f + 0.8f * ( float ) ( step + 1 ) / ( float ) ( m_config.simulationSteps ) );
		publishPreviewSnapshot( step + 1 );
	}

	if ( m_bCancelRequested.load() ) {
		m_state.store( STATE_CANCELLED );
		setStage( STAGE_CANCELLED, 1.0f );
		return;
	}

	if ( m_config.bMaterialOutputEnabled ) {
		setStage( STAGE_MATERIALS, 0.92f );
		buildMaterialTransitions();
	}

	eastl::vector<t3derosionFinalStatistics_t> finalStatistics( ( size_t ) ( MAX( 1, thread_count ) ) );
	kernel_context.pFinalStatistics = &finalStatistics;
	runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::FINAL_STATISTICS, m_nCellCount, grain_alignment );
	double fTotalEroded = 0.0;
	double fTotalDeposited = 0.0;
	float maximum_lowering = 0.0f;
	float maximum_raising = 0.0f;
	for ( const t3derosionFinalStatistics_t &local : finalStatistics ) {
		fTotalEroded += local.fEroded;
		fTotalDeposited += local.fDeposited;
		maximum_lowering = MAX( maximum_lowering, local.fMaximumLowering );
		maximum_raising = MAX( maximum_raising, local.fMaximumRaising );
	}
	{
		std::lock_guard<std::mutex> lock( m_statusMutex );
		m_fTotalEroded = fTotalEroded;
		m_fTotalDeposited = fTotalDeposited;
		m_fMaximumLowering = maximum_lowering;
		m_fMaximumRaising = maximum_raising;
	}

	publishPreviewSnapshot( m_config.simulationSteps, true );
	if ( !compactCompletedResults() ) {
		setFailed( "Erosion completed, but its retained result could not be compacted." );
		m_previewCondition.notify_all();
		return;
	}

	m_state.store( STATE_COMPLETED );
	setStage( STAGE_COMPLETE, 1.0f );
	m_bPreviewWaiting.store( false );
	m_previewCondition.notify_all();
	{
		std::lock_guard<std::mutex> lock( m_previewMutex );
		m_previewHeight.clear();
		m_previewMemory.release();
	}
}

int32_t T3DErosionJob::getTransitionTarget( const int32_t nTextureId, const t3dmaterialTransitionKind_t kind ) const {
	if ( nTextureId < 0 || nTextureId >= TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT ) {
		return -1;
	}
	const t3dtextureErosionProfile_t &profile = m_config.szTextureProfiles[( size_t ) ( nTextureId )];
	if ( !profile.bValid ) {
		return -1;
	}
	switch ( kind ) {
		case TRANSITION_EXPOSED:
			return profile.exposedTextureId;
		case TRANSITION_WEATHERED:
			return profile.weatheredTextureId;
		case TRANSITION_OXIDIZED:
			return profile.oxidizedTextureId;
		case TRANSITION_COARSE_DEPOSIT:
			return profile.coarseDepositTextureId;
		case TRANSITION_FINE_DEPOSIT:
			return profile.fineDepositTextureId;
		case TRANSITION_SALT_WEATHERED:
			return profile.saltWeatheredTextureId;
		case TRANSITION_DESERT_PAVEMENT:
			return profile.pavementTextureId;
		case TRANSITION_VARNISHED:
			return profile.varnishedTextureId;
		default:
			return -1;
	}
}

void T3DErosionJob::buildMaterialTransitions( void ) {
	std::fill( m_materialTransitionKind.begin(), m_materialTransitionKind.end(), ( uint8_t ) ( TRANSITION_NONE ) );
	std::fill( m_materialTransitionAmount.begin(), m_materialTransitionAmount.end(), 0.0f );

	const int32_t thread_count = resolveThreadCount( m_config.nThreadCount );
	const ParallelExecutor executor( thread_count );
	const size_t alignment = std::max<size_t>( 64u, ( size_t ) ( m_nSimulationRegionSize ) );
	eastl::vector<float> workerMaximumFlow( ( size_t ) ( MAX( 1, thread_count ) ), TERRAIN3D_EROSION_EPSILON );
	t3derosionKernelContext_t kernel_context;
	kernel_context.job = this;
	kernel_context.pWorkerMaximumFlow = &workerMaximumFlow;
	runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::MAXIMUM_FLOW, m_nCellCount, alignment );
	float maximumFlow = TERRAIN3D_EROSION_EPSILON;
	for ( const float fLocalMaximum : workerMaximumFlow ) {
		maximumFlow = MAX( maximumFlow, fLocalMaximum );
	}
	kernel_context.fMaximumFlow = maximumFlow;

	runStaticErosionKernel( executor, kernel_context, t3derosionKernel_t::MATERIAL_TRANSITIONS, m_nCellCount, alignment );
}

void T3DErosionJob::cancel( void ) {
	m_bCancelRequested.store( true );
	m_previewCondition.notify_all();
}

void T3DErosionJob::setPreviewPaused( const bool bPaused ) {
	if ( !m_config.bLivePreviewEnabled || isComplete() ) {
		return;
	}
	m_bPreviewPaused.store( bPaused );
	if ( !bPaused ) {
		m_previewStepRequests.store( 0 );
	}
	m_previewCondition.notify_all();
}

void T3DErosionJob::requestPreviewStep( void ) {
	if ( !m_config.bLivePreviewEnabled || isComplete() ) {
		return;
	}
	m_bPreviewPaused.store( true );
	m_previewStepRequests.fetch_add( 1 );
	m_previewCondition.notify_all();
}

bool T3DErosionJob::verifySourceUnchanged( T3DData *const pData ) const {
	if ( !pData || pData->getRegionCount() != ( int32_t ) ( m_regions.size() ) ) {
		return false;
	}
	if ( hashTextureProfiles( pData ) != m_assetProfileHash ) {
		return false;
	}
	for ( const t3dregionSnapshot_t &snapshot : m_regions ) {
		if ( !pData->ensureCpuRegionResident( snapshot.location ) ) {
			return false;
		}
		const Ref<T3DRegion> pRegion = pData->getRegion( snapshot.location );
		if ( pRegion.is_null() || pRegion->getRegionSize() != m_nSourceRegionSize ||
			!Math::is_equal_approx( pRegion->getVertexSpacing(), m_fVertexSpacing ) ) {
			return false;
		}
		const Ref<Image> pHeightMap = pRegion->getHeightMap();
		const Ref<Image> pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			pHeightMap->get_width() != m_nSourceRegionSize || pHeightMap->get_height() != m_nSourceRegionSize ||
			( pControlMap.is_valid() && ( pControlMap->get_format() != Image::FORMAT_RF ||
				pControlMap->get_width() != m_nSourceRegionSize || pControlMap->get_height() != m_nSourceRegionSize ) ) ) {
			return false;
		}
		if ( hashRegionImages( pHeightMap, pControlMap ) != snapshot.sourceHash ) {
			return false;
		}
	}
	return true;
}

float T3DErosionJob::sampleOutputCell(
	const FloatBuffer &rMap, const int32_t nRegionId,
	const int32_t iBaseX, const int32_t iBaseY, const int32_t iX, const int32_t iY
) const {
	const size_t base_index = ( size_t ) ( nRegionId ) * m_nCellsPerRegion +
		( size_t ) ( iBaseY ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( iBaseX );
	const int64_t resolved = neighborIndex( base_index, iX - iBaseX, iY - iBaseY );
	if ( resolved < 0 || !m_active[( size_t ) ( resolved )] ) {
		return rMap[base_index];
	}
	return rMap[( size_t ) ( resolved )];
}

float T3DErosionJob::sampleOutput(
	const FloatBuffer &rMap, const int32_t nRegionId,
	const float fSourceX, const float fSourceY
) const {
	if ( rMap.size() < m_nCellCount || nRegionId < 0 || nRegionId >= ( int32_t ) ( m_regions.size() ) ) {
		return 0.0f;
	}
	const float fSimX = ( fSourceX + 0.5f ) / ( float ) ( m_resolutionScale ) - 0.5f;
	const float fSimY = ( fSourceY + 0.5f ) / ( float ) ( m_resolutionScale ) - 0.5f;
	const int32_t x0 = ( int32_t ) ( Math::floor( fSimX ) );
	const int32_t y0 = ( int32_t ) ( Math::floor( fSimY ) );
	const float fFx = fSimX - ( float ) ( x0 );
	const float fFy = fSimY - ( float ) ( y0 );
	const int32_t base_x = CLAMP( x0, 0, m_nSimulationRegionSize - 1 );
	const int32_t base_y = CLAMP( y0, 0, m_nSimulationRegionSize - 1 );
	const float fV00 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0, y0 );
	const float fV10 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0 + 1, y0 );
	const float fV01 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0, y0 + 1 );
	const float fV11 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0 + 1, y0 + 1 );
	return Math::lerp( Math::lerp( fV00, fV10, fFx ), Math::lerp( fV01, fV11, fFx ), fFy );
}

Error T3DErosionJob::apply( T3DData *const pData, const bool bForce ) {
	std::lock_guard<std::mutex> applyLock( m_applyMutex );
	setErrorMessage( String() );
	if ( m_state.load() != STATE_COMPLETED ) {
		setErrorMessage( "Erosion results can be applied only from the completed state." );
		return ERR_UNCONFIGURED;
	}
	if ( !pData ) {
		setErrorMessage( "Erosion apply requires a valid Terrain3DData object." );
		return ERR_INVALID_PARAMETER;
	}
	if ( !m_config.bApplyHeight && !m_config.bMaterialOutputEnabled && !m_config.bOxidationColorEnabled ) {
		setErrorMessage( "This erosion job has no terrain outputs enabled; it is diagnostic-only." );
		return ERR_UNCONFIGURED;
	}
	if ( !bForce && !verifySourceUnchanged( pData ) ) {
		setErrorMessage( "Terrain data changed after the erosion snapshot was created. Refusing to apply stale "
			"results; create a new job or pass force=true." );
		return ERR_INVALID_DATA;
	}

	eastl::vector<Ref<T3DRegion>> pApplyRegions;
	eastl::vector<Vector2i> applied_locations;
	pApplyRegions.reserve( m_regions.size() );
	applied_locations.reserve( m_regions.size() );
	for ( const t3dregionSnapshot_t &snapshot : m_regions ) {
		if ( !pData->ensureCpuRegionResident( snapshot.location ) ) {
			setErrorMessage( "A terrain region could not be paged into the CPU map cache before erosion apply." );
			return ERR_CANT_OPEN;
		}
		const Ref<T3DRegion> pRegion = pData->getRegion( snapshot.location );
		if ( pRegion.is_null() ) {
			setErrorMessage( "A terrain region captured by the erosion snapshot no longer exists." );
			return ERR_DOES_NOT_EXIST;
		}
		const Ref<Image> pHeightMap = pRegion->getHeightMap();
		const Ref<Image> pControlMap = pRegion->getControlMap();
		const Ref<Image> pColorMap = pRegion->getColorMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			pHeightMap->get_width() != m_nSourceRegionSize || pHeightMap->get_height() != m_nSourceRegionSize ||
			( pControlMap.is_valid() && ( pControlMap->get_format() != Image::FORMAT_RF ||
				pControlMap->get_width() != m_nSourceRegionSize || pControlMap->get_height() != m_nSourceRegionSize ) ) ) {
			setErrorMessage( "A terrain region no longer has compatible R16 height or optional RF control data." );
			return ERR_INVALID_DATA;
		}
		if ( m_config.bOxidationColorEnabled && pColorMap.is_valid() &&
			( pColorMap->get_width() != m_nSourceRegionSize || pColorMap->get_height() != m_nSourceRegionSize ) ) {
			setErrorMessage( "A terrain color map no longer matches the snapshotted region resolution." );
			return ERR_INVALID_DATA;
		}
		pApplyRegions.push_back( pRegion );
	}

	const int32_t thread_count = resolveThreadCount( m_config.nThreadCount );
	const ParallelExecutor executor( thread_count );
	const size_t source_pixel_count = ( size_t ) ( m_nSourceRegionSize ) * ( size_t ) ( m_nSourceRegionSize );
	const size_t apply_grain = std::max<size_t>( 4096, source_pixel_count / ( size_t ) ( MAX( 1, thread_count * 16 ) ) );

	// Application is region-serial, so reuse three raw staging buffers instead of
	// returning them to the general heap after every region. Image/PackedByteArray
	// publication remains per-region because those buffers become resource state.
	eastl::vector<uint8_t> height_buffer;
	eastl::vector<uint8_t> control_buffer;
	eastl::vector<uint8_t> color_buffer;
	height_buffer.reserve( source_pixel_count * sizeof( float ) );
	control_buffer.reserve( source_pixel_count * sizeof( float ) );
	color_buffer.reserve( source_pixel_count * 4U );
	bool bAnyControlChanged = false;
	bool bAnyColorChanged = false;

	for ( int32_t region_id = 0; region_id < ( int32_t ) ( pApplyRegions.size() ); ++region_id ) {
		Ref<T3DRegion> pRegion = pApplyRegions[( size_t ) ( region_id )];
		if ( !pData->ensureCpuRegionResident( pRegion->getLocation() ) ) {
			setErrorMessage( "A terrain region could not be paged into the CPU map cache during erosion apply." );
			return ERR_CANT_OPEN;
		}
		Ref<Image> pHeightMap = pRegion->getHeightMap();
		const size_t simulation_begin = ( size_t ) ( region_id ) * m_nCellsPerRegion;
		const size_t simulation_end = simulation_begin + m_nCellsPerRegion;
		bool bRegionNeedsControl = false;
		if ( m_config.bMaterialOutputEnabled && !m_materialTransitionKind.empty() ) {
			for ( size_t index = simulation_begin; index < simulation_end; ++index ) {
				if ( m_materialTransitionKind[index] != ( uint8_t ) ( TRANSITION_NONE ) &&
					m_materialTransitionAmount[index] > TERRAIN3D_EROSION_EPSILON ) {
					bRegionNeedsControl = true;
					break;
				}
			}
		}
		bool bRegionNeedsColor = false;
		if ( m_config.bOxidationColorEnabled && !m_oxidation.empty() && m_config.fOxidationTintStrength > TERRAIN3D_EROSION_EPSILON ) {
			for ( size_t index = simulation_begin; index < simulation_end; ++index ) {
				if ( m_oxidation[index] * m_config.fOxidationTintStrength > ( 0.5f / TERRAIN3D_BYTE_MAX_FLOAT ) ) {
					bRegionNeedsColor = true;
					break;
				}
			}
		}
		Ref<Image> pControlMap = bRegionNeedsControl ? pRegion->materializeMap( T3DRegion::TYPE_CONTROL ) : pRegion->getControlMap();
		Ref<Image> pColorMap = bRegionNeedsColor ? pRegion->materializeMap( T3DRegion::TYPE_COLOR ) : pRegion->getColorMap();

		const PackedByteArray encoded_height_data = pHeightMap->get_data();
		const size_t encoded_height_byte_count = source_pixel_count * sizeof( uint16_t );
		if ( ( size_t ) ( encoded_height_data.size() ) < encoded_height_byte_count ) {
			setErrorMessage( "A terrain height map returned an incomplete R16 data buffer." );
			return ERR_INVALID_DATA;
		}
		PackedByteArray source_height_data;
		source_height_data.resize( ( int64_t ) ( source_pixel_count * sizeof( float ) ) );
		uint8_t *const pSourceHeightWrite = source_height_data.ptrw();
		for ( size_t i = 0; i < source_pixel_count; ++i ) {
			writeRf( pSourceHeightWrite, i,
				Terrain3DRegionCodec::readHeightBytes( encoded_height_data.ptr(), ( int ) ( i ), Image::FORMAT_R16 ) );
		}
		PackedByteArray source_control_data;
		if ( bRegionNeedsControl ) {
			if ( pControlMap.is_valid() ) {
				source_control_data = pControlMap->get_data();
			} else {
				source_control_data.resize( ( int64_t ) ( source_pixel_count * sizeof( float ) ) );
				uint8_t *const pControlWrite = source_control_data.ptrw();
				for ( size_t i = 0; i < source_pixel_count; ++i ) {
					writeRf( pControlWrite, i, asFloat( TERRAIN3D_REGION_DEFAULT_CONTROL ) );
				}
			}
		}
		const size_t height_byte_count = ( size_t ) ( source_height_data.size() );
		const size_t control_byte_count = ( size_t ) ( source_control_data.size() );
		if ( height_byte_count < source_pixel_count * sizeof( float ) ||
			( bRegionNeedsControl && control_byte_count < source_pixel_count * sizeof( float ) ) ) {
			setErrorMessage( "A terrain height or control staging buffer is incomplete." );
			return ERR_INVALID_DATA;
		}

		height_buffer.clear();
		control_buffer.clear();
		color_buffer.clear();
		if ( m_config.bApplyHeight ) {
			height_buffer.resize( height_byte_count );
			std::memcpy( height_buffer.data(), source_height_data.ptr(), height_byte_count );
		}
		if ( bRegionNeedsControl ) {
			control_buffer.resize( control_byte_count );
			std::memcpy( control_buffer.data(), source_control_data.ptr(), control_byte_count );
		}

		const bool bTintRgba8 =
			bRegionNeedsColor && pColorMap.is_valid() && pColorMap->get_format() == Image::FORMAT_RGBA8;
		PackedByteArray source_color_data;
		if ( bTintRgba8 ) {
			source_color_data = pColorMap->get_data();
			if ( ( size_t ) ( source_color_data.size() ) < source_pixel_count * 4U ) {
				setErrorMessage( "A terrain color map returned an incomplete RGBA8 data buffer." );
				return ERR_INVALID_DATA;
			}
			color_buffer.resize( ( size_t ) ( source_color_data.size() ) );
			std::memcpy( color_buffer.data(), source_color_data.ptr(), color_buffer.size() );
		}

		const uint8_t *height_source_bytes = source_height_data.ptr();
		const uint8_t *control_source_bytes = source_control_data.is_empty() ? nullptr : source_control_data.ptr();
		uint8_t *const height_bytes = height_buffer.empty() ? nullptr : height_buffer.data();
		uint8_t *control_bytes = control_buffer.empty() ? nullptr : control_buffer.data();
		uint8_t *color_bytes = color_buffer.empty() ? nullptr : color_buffer.data();

		t3derosionApplyContext_t apply_context;
		apply_context.job = this;
		apply_context.region_id = region_id;
		apply_context.pHeightSourceBytes = height_source_bytes;
		apply_context.pControlSourceBytes = control_source_bytes;
		apply_context.pHeightBytes = height_bytes;
		apply_context.pControlBytes = control_bytes;
		apply_context.pColorBytes = color_bytes;
		executor.run( source_pixel_count, apply_grain, t3derosionKernels_t::applyRegion, &apply_context );

		if ( bRegionNeedsColor && pColorMap.is_valid() && !bTintRgba8 ) {
			// Non-RGBA8 formats use Image accessors on the main thread. This is slower,
			// but avoids changing the user's color-map format or touching engine objects
			// from erosion worker threads.
			for ( int32_t y = 0; y < m_nSourceRegionSize; ++y ) {
				for ( int32_t x = 0; x < m_nSourceRegionSize; ++x ) {
					const size_t source_index = ( size_t ) ( y ) * ( size_t ) ( m_nSourceRegionSize ) + ( size_t ) ( x );
					if ( std::isnan( readRf( height_source_bytes, source_index ) ) ) {
						continue;
					}
					const float fOxidation = clamp01( sampleOutput( m_oxidation, region_id, ( float ) ( x ), ( float ) ( y ) ) *
													m_config.fOxidationTintStrength );
					Color source_color = pColorMap->get_pixel( x, y );
					const float fRoughness = source_color.a;
					source_color = source_color.lerp( m_config.oxidationTint, fOxidation );
					source_color.a = fRoughness;
					pColorMap->set_pixel( x, y, source_color );
				}
			}
		}

		if ( control_bytes && std::memcmp( control_bytes, source_control_data.ptr(), control_buffer.size() ) == 0 ) {
			control_buffer.clear();
			control_bytes = nullptr;
			pRegion->compactMapIfDefault( T3DRegion::TYPE_CONTROL );
		}
		if ( color_bytes && std::memcmp( color_bytes, source_color_data.ptr(), color_buffer.size() ) == 0 ) {
			color_buffer.clear();
			color_bytes = nullptr;
			pRegion->compactMapIfDefault( T3DRegion::TYPE_COLOR );
		}

		if ( height_bytes ) {
			PackedByteArray output_height_data;
			output_height_data.resize( ( int64_t ) ( source_pixel_count * sizeof( uint16_t ) ) );
			uint8_t *const pEncoded = output_height_data.ptrw();
			for ( size_t i = 0; i < source_pixel_count; ++i ) {
				const uint16_t value = Terrain3DRegionCodec::encodeHeightU16( readRf( height_bytes, i ) );
				std::memcpy( pEncoded + i * sizeof( uint16_t ), &value, sizeof( uint16_t ) );
			}
			pHeightMap->set_data( pHeightMap->get_width(), pHeightMap->get_height(), false, Image::FORMAT_R16,
				output_height_data );
		}
		if ( control_bytes ) {
			PackedByteArray output_control_data;
			output_control_data.resize( ( int64_t ) ( control_buffer.size() ) );
			std::memcpy( output_control_data.ptrw(), control_buffer.data(), control_buffer.size() );
			pControlMap->set_data( pControlMap->get_width(), pControlMap->get_height(), pControlMap->has_mipmaps(),
								  Image::FORMAT_RF, output_control_data );
			pRegion->compactMapIfDefault( T3DRegion::TYPE_CONTROL );
			bAnyControlChanged = true;
		}
		if ( color_bytes ) {
			PackedByteArray output_color_data;
			output_color_data.resize( ( int64_t ) ( color_buffer.size() ) );
			std::memcpy( output_color_data.ptrw(), color_buffer.data(), color_buffer.size() );
			pColorMap->set_data( pColorMap->get_width(), pColorMap->get_height(), pColorMap->has_mipmaps(),
								Image::FORMAT_RGBA8, output_color_data );
		}
		const bool bRegionColorChanged = color_bytes || ( bRegionNeedsColor && pColorMap.is_valid() && !bTintRgba8 );
		if ( bRegionColorChanged ) {
			pColorMap->clear_mipmaps();
			pRegion->compactMapIfDefault( T3DRegion::TYPE_COLOR );
			bAnyColorChanged = true;
		}
		const bool bRegionChanged = height_bytes || control_bytes || bRegionColorChanged;
		if ( bRegionChanged ) {
			pRegion->setModified( true );
			pRegion->setEdited( true );
		}
		if ( m_config.bApplyHeight ) {
			pRegion->calcHeightRange();
		}
		applied_locations.push_back( pRegion->getLocation() );
	}

	const bool bHeightChanged = m_config.bApplyHeight;
	const bool bControlChanged = bAnyControlChanged;
	const bool bColorChanged = bAnyColorChanged;
	const bool bGeometryOrControlChanged = bHeightChanged || bControlChanged;
	if ( !bHeightChanged && !bControlChanged && !bColorChanged ) {
		setErrorMessage( "The enabled erosion outputs did not produce any terrain-map changes." );
		return ERR_DOES_NOT_EXIST;
	}

	t3dmapType_t changed_map_type = T3DRegion::TYPE_MAX;
	const int32_t changed_map_count = ( int32_t ) ( bHeightChanged ) + ( int32_t ) ( bControlChanged ) + ( int32_t ) ( bColorChanged );
	if ( changed_map_count == 1 ) {
		changed_map_type = bHeightChanged ? T3DRegion::TYPE_HEIGHT : ( bControlChanged ? T3DRegion::TYPE_CONTROL : T3DRegion::TYPE_COLOR );
	}

	// Publish all authoritative image replacements through one mutation boundary.
	// The bounded CPU mirror and sampler-local views are invalidated exactly once;
	// derived data and dependent systems are refreshed only for outputs they use.
	if ( pData->m_pResidentPool ) {
		pData->m_pResidentPool->resetResidency();
	}
	pData->invalidateSamplingCache();
	if ( bHeightChanged ) {
		pData->calcHeightRange( false );
	}

	const Vector2 height_range = pData->getHeightRange();
	AABB edited_area;
	edited_area.position = Vector3( m_terrainWorldRect.position.x, height_range.x, m_terrainWorldRect.position.y );
	edited_area.size = Vector3( m_terrainWorldRect.size.x, MAX( height_range.y - height_range.x, m_fVertexSpacing ),
							   m_terrainWorldRect.size.y );
	if ( bHeightChanged ) {
		pData->regenerateDerivedMaps( applied_locations, edited_area );
	}
	if ( pData->canUpdateRegionSlotsIncrementally() ) {
		pData->updateMapRegions( changed_map_type, applied_locations );
	} else {
		pData->updateMaps( changed_map_type, true, false );
	}
	pData->addEditedArea( edited_area );

	if ( bGeometryOrControlChanged && pData->m_pTerrain ) {
		if ( T3DInstancer *const instancer = pData->m_pTerrain->getInstancer() ) {
			instancer->updateTransforms( edited_area );
		}
		if ( T3DCollision *const collision = pData->m_pTerrain->getCollision(); collision && collision->isEnabled() ) {
			if ( collision->isDynamicMode() ) {
				collision->update( V2I_MAX, true );
			} else {
				for ( const Vector2i &location : applied_locations ) {
					collision->update( location, true );
				}
			}
		}
		pData->m_pTerrain->snap();
	}
	m_state.store( STATE_APPLIED );
	return OK;
}

bool T3DErosionJob::isSourceUnchanged( T3DData *pData ) const {
	return pData && verifySourceUnchanged( pData );
}

bool T3DErosionJob::isComplete( void ) const {
	const int32_t state = m_state.load();
	return state == STATE_COMPLETED || state == STATE_APPLIED || state == STATE_CANCELLED || state == STATE_FAILED;
}

String T3DErosionJob::getStageName( void ) const {
	switch ( ( t3dstage_t ) ( m_stage.load() ) ) {
		case STAGE_SNAPSHOT:
			return "Snapshot";
		case STAGE_WEATHERING:
			return "Weathering and oxidation";
		case STAGE_HYDRAULIC:
			return "Hydraulic erosion and sediment sorting";
		case STAGE_DEBRIS_FLOW:
			return "Debris flows";
		case STAGE_WIND:
			return "Wind erosion";
		case STAGE_DUNES:
			return "Dune relaxation";
		case STAGE_WAVES:
			return "Wave erosion";
		case STAGE_TRAFFIC:
			return "Foot traffic";
		case STAGE_MATERIALS:
			return "Material classification";
		case STAGE_COMPLETE:
			return "Complete";
		case STAGE_CANCELLED:
			return "Cancelled";
		case STAGE_FAILED:
			return "Failed";
		default:
			return "Idle";
	}
}

Dictionary T3DErosionJob::getStatistics( void ) const {
	double fTotalEroded = 0.0;
	double fTotalDeposited = 0.0;
	float fMaximumLowering = 0.0f;
	float fMaximumRaising = 0.0f;
	bool bResultsCompacted = false;
	uint64_t retained_result_bytes = 0u;
	String error_message;
	{
		std::lock_guard<std::mutex> lock( m_statusMutex );
		fTotalEroded = m_fTotalEroded;
		fTotalDeposited = m_fTotalDeposited;
		fMaximumLowering = m_fMaximumLowering;
		fMaximumRaising = m_fMaximumRaising;
		bResultsCompacted = m_bResultsCompacted;
		retained_result_bytes = m_nRetainedResultBytes;
		error_message = m_errorMessage;
	}
	Dictionary statistics;
	statistics["state"] = getState();
	statistics["stage"] = getStageName();
	statistics["progress"] = getProgress();
	statistics["region_count"] = ( int32_t ) ( m_regions.size() );
	statistics["source_region_size"] = m_nSourceRegionSize;
	statistics["simulation_region_size"] = m_nSimulationRegionSize;
	statistics["resolution_scale"] = m_resolutionScale;
	statistics["cell_count"] = ( int64_t ) ( m_nCellCount );
	statistics["completed_steps"] = m_nCompletedSteps.load();
	statistics["simulation_years"] = m_config.fSimulationYears;
	statistics["estimated_eroded_volume_m3"] = fTotalEroded;
	statistics["estimated_deposited_volume_m3"] = fTotalDeposited;
	statistics["maximum_lowering_m"] = fMaximumLowering;
	statistics["maximum_raising_m"] = fMaximumRaising;
	statistics["sediment_sorting"] = m_config.bSedimentSortingEnabled;
	statistics["alluvial_fans"] = m_config.bAlluvialFanEnabled;
	statistics["debris_flows"] = m_config.bDebrisFlowEnabled;
	statistics["dune_relaxation"] = m_config.bDuneEnabled;
	statistics["salt_weathering"] = m_config.bSaltWeatheringEnabled;
	statistics["wet_dry_cycles"] = m_config.bWetDryEnabled;
	statistics["snowmelt"] = m_config.bSnowmeltEnabled;
	statistics["stratigraphy"] = m_config.bStratigraphyEnabled;
	statistics["results_compacted"] = bResultsCompacted;
	statistics["retained_result_bytes"] = ( int64_t ) ( retained_result_bytes );
	statistics["error"] = error_message;
	return statistics;
}

String T3DErosionJob::getErrorMessage( void ) const {
	std::lock_guard<std::mutex> lock( m_statusMutex );
	return m_errorMessage;
}

TypedArray<Vector2i> T3DErosionJob::getRegionLocations( void ) const {
	TypedArray<Vector2i> locations;
	for ( const t3dregionSnapshot_t &region : m_regions ) {
		locations.push_back( region.location );
	}
	return locations;
}

Ref<Image> T3DErosionJob::getInitialRegionHeightMap(
	const Vector2i &rRegionLocation,
	const bool bFullResolution
) const {
	const int32_t state = m_state.load();
	if ( ( state == STATE_RUNNING && m_previewRevision.load() <= 0 ) ||
		( state != STATE_RUNNING && state != STATE_COMPLETED && state != STATE_APPLIED ) ) {
		return Ref<Image>();
	}
	const int32_t map_index = T3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref<Image>();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref<Image>();
	}
	std::lock_guard<std::mutex> lock( m_previewMutex );
	return makeHeightImage( m_initialHeight, region_id, bFullResolution );
}

Ref<Image> T3DErosionJob::getPreviewRegionHeightMap(
	const Vector2i &rRegionLocation,
	const bool bFullResolution
) const {
	const int32_t map_index = T3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref<Image>();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref<Image>();
	}

	const int32_t state = m_state.load();
	if ( state == STATE_COMPLETED || state == STATE_APPLIED ) {
		return makeHeightImage( m_height, region_id, bFullResolution );
	}
	if ( state != STATE_RUNNING || m_previewRevision.load() <= 0 ) {
		return Ref<Image>();
	}
	std::lock_guard<std::mutex> lock( m_previewMutex );
	const int32_t locked_state = m_state.load();
	if ( locked_state == STATE_COMPLETED || locked_state == STATE_APPLIED ) {
		return makeHeightImage( m_height, region_id, bFullResolution );
	}
	return makeHeightImage( m_previewHeight, region_id, bFullResolution );
}

Ref<Image> T3DErosionJob::getRegionOutputMap(
	const Vector2i &rRegionLocation, const t3doutputMap_t output,
	const bool bFullResolution
) const {
	if ( m_state.load() != STATE_COMPLETED && m_state.load() != STATE_APPLIED ) {
		return Ref<Image>();
	}
	if ( output < 0 || output >= OUTPUT_MAX ) {
		return Ref<Image>();
	}
	const int32_t map_index = T3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref<Image>();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref<Image>();
	}

	const FloatBuffer *pSource = nullptr;
	bool bDeriveSediment = false;
	bool bDeriveLooseMaterial = false;
	switch ( output ) {
		case OUTPUT_FLOW_ACCUMULATION:
			pSource = &m_flowAccumulation;
			break;
		case OUTPUT_SEDIMENT:
			bDeriveSediment = true;
			break;
		case OUTPUT_COARSE_SEDIMENT:
			pSource = &m_sedimentCoarse;
			break;
		case OUTPUT_SAND_SEDIMENT:
			pSource = &m_sedimentSand;
			break;
		case OUTPUT_FINE_SEDIMENT:
			pSource = &m_sedimentFine;
			break;
		case OUTPUT_LOOSE_MATERIAL:
			bDeriveLooseMaterial = true;
			break;
		case OUTPUT_OXIDATION:
			pSource = &m_oxidation;
			break;
		case OUTPUT_WEATHERING:
			pSource = &m_weathering;
			break;
		case OUTPUT_HYDRAULIC_DEPOSITION:
			pSource = &m_hydraulicDeposition;
			break;
		case OUTPUT_WIND_DEPOSITION:
			pSource = &m_windDeposition;
			break;
		case OUTPUT_WAVE_DEPOSITION:
			pSource = &m_waveDeposition;
			break;
		case OUTPUT_TRAFFIC:
			pSource = &m_traffic;
			break;
		case OUTPUT_ALLUVIAL_DEPOSITION:
			pSource = &m_alluvialDeposition;
			break;
		case OUTPUT_DEBRIS_FLOW_DEPOSITION:
			pSource = &m_debrisFlowDeposition;
			break;
		case OUTPUT_DUNE_DEPOSITION:
			pSource = &m_duneDeposition;
			break;
		case OUTPUT_SALT_WEATHERING:
			pSource = &m_saltWeathering;
			break;
		case OUTPUT_WET_DRY_CYCLES:
			pSource = &m_wetDryCycles;
			break;
		case OUTPUT_EXPOSURE_AGE:
			pSource = &m_exposureAge;
			break;
		case OUTPUT_SNOWPACK:
			pSource = &m_snowpack;
			break;
		case OUTPUT_DESERT_PAVEMENT:
			pSource = &m_desertPavement;
			break;
		case OUTPUT_STRATIGRAPHY:
			pSource = &m_cumulativeErosion;
			break;
		default:
			break;
	}

	const int32_t size = bFullResolution ? m_nSourceRegionSize : m_nSimulationRegionSize;
	const size_t pixel_count = ( size_t ) ( size ) * ( size_t ) ( size );
	PackedByteArray image_data;
	image_data.resize( ( int64_t ) ( pixel_count * sizeof( float ) ) );
	uint8_t *const pDestination = image_data.ptrw();
	for ( int32_t y = 0; y < size; ++y ) {
		for ( int32_t x = 0; x < size; ++x ) {
			float fValue = 0.0f;
			if ( output == OUTPUT_HEIGHT_DELTA ) {
				const float fFinalHeight =
					bFullResolution
						? sampleOutput( m_height, region_id, ( float ) ( x ), ( float ) ( y ) )
						: m_height[( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
				const float fInitialHeight =
					bFullResolution
						? sampleOutput( m_initialHeight, region_id, ( float ) ( x ), ( float ) ( y ) )
						: m_initialHeight[( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
				fValue = fFinalHeight - fInitialHeight;
			} else if ( output == OUTPUT_MATERIAL_ID ) {
				if ( !m_materialTransitionKind.empty() ) {
					const int32_t sx = bFullResolution ? CLAMP( x / m_resolutionScale, 0, m_nSimulationRegionSize - 1 ) : x;
					const int32_t sy = bFullResolution ? CLAMP( y / m_resolutionScale, 0, m_nSimulationRegionSize - 1 ) : y;
					const size_t index = ( size_t ) ( region_id ) * m_nCellsPerRegion +
								   ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
					const t3dmaterialTransitionKind_t kind = ( t3dmaterialTransitionKind_t ) ( m_materialTransitionKind[index] );
					const int32_t target = getTransitionTarget( ( int32_t ) ( m_dominantTexture[index] ), kind );
					fValue = target >= 0 ? ( float ) ( target ) / 31.0f : 0.0f;
				}
			} else if ( bDeriveSediment ) {
				if ( bFullResolution ) {
					fValue = sampleOutput( m_sedimentCoarse, region_id, ( float ) ( x ), ( float ) ( y ) ) +
						sampleOutput( m_sedimentSand, region_id, ( float ) ( x ), ( float ) ( y ) ) +
						sampleOutput( m_sedimentFine, region_id, ( float ) ( x ), ( float ) ( y ) );
				} else {
					const size_t index = ( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
					fValue = getSediment( index );
				}
			} else if ( bDeriveLooseMaterial ) {
				if ( bFullResolution ) {
					fValue = sampleOutput( m_surfaceCoarse, region_id, ( float ) ( x ), ( float ) ( y ) ) +
						sampleOutput( m_surfaceSand, region_id, ( float ) ( x ), ( float ) ( y ) ) +
						sampleOutput( m_surfaceFine, region_id, ( float ) ( x ), ( float ) ( y ) );
				} else {
					const size_t index = ( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
					fValue = getLooseMaterial( index );
				}
			} else if ( pSource ) {
				fValue = bFullResolution
							? sampleOutput( *pSource, region_id, ( float ) ( x ), ( float ) ( y ) )
							: ( *pSource )[( size_t ) ( region_id ) * m_nCellsPerRegion + ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
			}
			const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
			std::memcpy( pDestination + index * sizeof( float ), &fValue, sizeof( float ) );
		}
	}
	return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
}

void T3DErosionJob::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( STATE_NEW );
	BIND_ENUM_CONSTANT( STATE_READY );
	BIND_ENUM_CONSTANT( STATE_RUNNING );
	BIND_ENUM_CONSTANT( STATE_COMPLETED );
	BIND_ENUM_CONSTANT( STATE_CANCELLED );
	BIND_ENUM_CONSTANT( STATE_FAILED );
	BIND_ENUM_CONSTANT( STATE_APPLIED );

	BIND_ENUM_CONSTANT( OUTPUT_HEIGHT_DELTA );
	BIND_ENUM_CONSTANT( OUTPUT_FLOW_ACCUMULATION );
	BIND_ENUM_CONSTANT( OUTPUT_SEDIMENT );
	BIND_ENUM_CONSTANT( OUTPUT_COARSE_SEDIMENT );
	BIND_ENUM_CONSTANT( OUTPUT_SAND_SEDIMENT );
	BIND_ENUM_CONSTANT( OUTPUT_FINE_SEDIMENT );
	BIND_ENUM_CONSTANT( OUTPUT_LOOSE_MATERIAL );
	BIND_ENUM_CONSTANT( OUTPUT_OXIDATION );
	BIND_ENUM_CONSTANT( OUTPUT_WEATHERING );
	BIND_ENUM_CONSTANT( OUTPUT_HYDRAULIC_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_WIND_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_WAVE_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_TRAFFIC );
	BIND_ENUM_CONSTANT( OUTPUT_ALLUVIAL_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_DEBRIS_FLOW_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_DUNE_DEPOSITION );
	BIND_ENUM_CONSTANT( OUTPUT_SALT_WEATHERING );
	BIND_ENUM_CONSTANT( OUTPUT_WET_DRY_CYCLES );
	BIND_ENUM_CONSTANT( OUTPUT_EXPOSURE_AGE );
	BIND_ENUM_CONSTANT( OUTPUT_SNOWPACK );
	BIND_ENUM_CONSTANT( OUTPUT_DESERT_PAVEMENT );
	BIND_ENUM_CONSTANT( OUTPUT_STRATIGRAPHY );
	BIND_ENUM_CONSTANT( OUTPUT_MATERIAL_ID );
	BIND_ENUM_CONSTANT( OUTPUT_MAX );

	ClassDB::bind_method( D_METHOD( "run" ), &T3DErosionJob::run );
	ClassDB::bind_method( D_METHOD( "cancel" ), &T3DErosionJob::cancel );
	ClassDB::bind_method( D_METHOD( "set_preview_paused", "paused" ), &T3DErosionJob::setPreviewPaused );
	ClassDB::bind_method( D_METHOD( "request_preview_step" ), &T3DErosionJob::requestPreviewStep );
	ClassDB::bind_method( D_METHOD( "apply", "data", "force" ), &T3DErosionJob::apply, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_state" ), &T3DErosionJob::getState );
	ClassDB::bind_method( D_METHOD( "get_progress" ), &T3DErosionJob::getProgress );
	ClassDB::bind_method( D_METHOD( "get_stage_name" ), &T3DErosionJob::getStageName );
	ClassDB::bind_method( D_METHOD( "get_error_message" ), &T3DErosionJob::getErrorMessage );
	ClassDB::bind_method( D_METHOD( "is_complete" ), &T3DErosionJob::isComplete );
	ClassDB::bind_method( D_METHOD( "is_source_unchanged", "data" ), &T3DErosionJob::isSourceUnchanged );
	ClassDB::bind_method( D_METHOD( "was_cancelled" ), &T3DErosionJob::wasCancelled );
	ClassDB::bind_method( D_METHOD( "get_effective_resolution_scale" ),
						 &T3DErosionJob::getEffectiveResolutionScale );
	ClassDB::bind_method( D_METHOD( "get_completed_steps" ), &T3DErosionJob::getCompletedSteps );
	ClassDB::bind_method( D_METHOD( "is_live_preview_enabled" ), &T3DErosionJob::isLivePreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_preview_paused" ), &T3DErosionJob::isPreviewPaused );
	ClassDB::bind_method( D_METHOD( "is_preview_waiting" ), &T3DErosionJob::isPreviewWaiting );
	ClassDB::bind_method( D_METHOD( "get_preview_revision" ), &T3DErosionJob::getPreviewRevision );
	ClassDB::bind_method( D_METHOD( "get_preview_step" ), &T3DErosionJob::getPreviewStep );
	ClassDB::bind_method( D_METHOD( "get_simulation_steps" ), &T3DErosionJob::getSimulationSteps );
	ClassDB::bind_method( D_METHOD( "get_simulation_region_size" ), &T3DErosionJob::getSimulationRegionSize );
	ClassDB::bind_method( D_METHOD( "get_preview_cell_size" ), &T3DErosionJob::getPreviewCellSize );
	ClassDB::bind_method( D_METHOD( "get_statistics" ), &T3DErosionJob::getStatistics );
	ClassDB::bind_method( D_METHOD( "get_region_locations" ), &T3DErosionJob::getRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_initial_region_height_map", "region_location", "full_resolution" ),
						 &T3DErosionJob::getInitialRegionHeightMap, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_preview_region_height_map", "region_location", "full_resolution" ),
						 &T3DErosionJob::getPreviewRegionHeightMap, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_region_output_map", "region_location", "output", "full_resolution" ),
						 &T3DErosionJob::getRegionOutputMap, DEFVAL( false ) );
}
