#include "core/utility/terrain_3d_hash_bytes.h"
#include <EASTL/numeric_limits.h>

#include "Terrain3DTypes.h"
#include "core/utility/terrain_3d_sync.h"
#include <godot_cpp/classes/os.hpp>
// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "assets/catalog/terrain_3d_assets.h"
#include "assets/resources/terrain_3d_texture_asset.h"
#include "authoring/erosion/terrain_3d_erosion_job.h"
#include "core/utility/logger.h"
#include "core/config/terrain_3d_compiler.h"
#include "core/config/terrain_3d_cpp17_compat.h"
#include "core/utility/terrain_3d_util.h"
#include "data/model/terrain_3d_data.h"
#include "data/model/terrain_3d_region.h"
#include "data/persistence/terrain_3d_region_codec.h"
#include "instances/runtime/terrain_3d_instancer.h"
#include "memory/storage/terrain_3d_resident_pool.h"
#include "physics/runtime/terrain_3d_collision.h"
#include "rendering/material/terrain_3d_material.h"
#include "simd/core/terrain_3d_parallel.h"
#include "simd/core/terrain_3d_simd.h"

#include <math.h>
#include <string.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <EASTL/algorithm.h>
#include <EASTL/utility.h>

typedef struct t3dErosionApplyContext_s
{
	Terrain3DErosionJob *m_pJob = NULL;
	int32_t				 m_nRegionId = -1;
	const uint8_t		*m_pHeightSourceBytes = NULL;
	const uint8_t		*m_pControlSourceBytes = NULL;
	uint8_t				*m_pHeightBytes = NULL;
	uint8_t				*m_pControlBytes = NULL;
	uint8_t				*m_pColorBytes = NULL;
} t3dErosionApplyContext_t;

typedef struct alignas(
	64 ) t3dDecodedHalfBlock_s
{
	float m_szValues[T3D_EROSION_HALF_DECODE_BLOCK] = {};

	void load(
		const Terrain3DHalfBuffer &rSource,
		const size_t			   nBegin,
		const size_t			   nCount,
		const float				   fFallback = 0.0f ) {
		if ( rSource.empty() ) {
			memset( m_szValues, fFallback, sizeof( *m_szValues ) * nCount );
			return;
		}
		t3dSimdHalfToFloat( rSource.data() + nBegin, m_szValues, nCount );
	}
} t3dDecodedHalfBlock_t;

EA_FORCE_INLINE static float t3dErosionJobClamp01(
	float fValue ) {
	return CLAMP( fValue, 0.0f, 1.0f );
}

EA_FORCE_INLINE static float t3dErosionJobSafeDivide(
	float fNumerator,
	float fDenominator ) {
	return fDenominator > s_fErosionEpsilon ? fNumerator / fDenominator : 0.0f;
}

static inline uint32_t t3dErosionJobHashU32(
	uint64_t nSeed,
	uint64_t iIndex,
	uint64_t nStep = 0 ) {
	uint64_t value = nSeed ^ ( iIndex + T3D_HASH_GOLDEN_RATIO64 + ( nSeed << 6 ) + ( nSeed >> 2 ) );
	value ^= nStep + T3D_HASH_GOLDEN_RATIO64 + ( value << 6 ) + ( value >> 2 );
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ULL;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebULL;
	value ^= value >> 31;
	return ( uint32_t ) ( value & 0xffffffffULL );
}

EA_FORCE_INLINE static float t3dErosionJobHashUnit(
	uint64_t nSeed,
	uint64_t iIndex,
	uint64_t nStep = 0 ) {
	return ( float ) ( t3dErosionJobHashU32( nSeed, iIndex, nStep ) ) /
		   ( float ) ( eastl::numeric_limits< uint32_t >::max() );
}

EA_FORCE_INLINE static float t3dErosionJobReadRf(
	const uint8_t *pBytes,
	size_t		   iIndex ) {
	float fValue = 0.0f;
	memcpy( &fValue, pBytes + iIndex * sizeof( float ), sizeof( float ) );
	return fValue;
}

EA_FORCE_INLINE static void t3dErosionJobWriteRf(
	uint8_t *pBytes,
	size_t	 iIndex,
	float	 fValue ) {
	memcpy( pBytes + iIndex * sizeof( float ), &fValue, sizeof( float ) );
}

static inline int32_t t3dErosionJobResolveThreadCount(
	int32_t nRequested ) {
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

static inline void t3dErosionJobAddMapBytes(
	uint64_t	  &nBytesPerCell,
	const bool	   bEnabled,
	const uint64_t nCount,
	const uint64_t nElementSize ) {
	if ( bEnabled ) {
		nBytesPerCell += nCount * nElementSize;
	}
}

EA_FORCE_INLINE static void t3dErosionJobKeepValid(
	const bool bOperationSucceeded,
	bool	  &bValid ) {
	bValid = bOperationSucceeded && bValid;
}

static inline bool t3dErosionJobCheckedScaledCount(
	const size_t nCellCount,
	const size_t nMultiplier,
	size_t		&nCount ) {
	if ( nMultiplier == 0u || nCellCount > eastl::numeric_limits< size_t >::max() / nMultiplier ) {
		return false;
	}
	nCount = nCellCount * nMultiplier;
	return true;
}

static inline bool t3dErosionJobPlanFloatBuffer(
	Terrain3DLinearArenaLayout &rLayout,
	const bool					bEnabled,
	const size_t				nCellCount,
	const size_t				nMultiplier = 1u ) {
	if ( !bEnabled ) {
		return true;
	}
	size_t count = 0u;
	return t3dErosionJobCheckedScaledCount( nCellCount, nMultiplier, count ) && rLayout.addArray< float >( count );
}

EA_FORCE_INLINE static bool t3dErosionJobPlanHalfBuffer(
	Terrain3DLinearArenaLayout &rLayout,
	const bool					bEnabled,
	const size_t				nCellCount ) {
	return !bEnabled || rLayout.addArray< uint16_t >( nCellCount );
}

EA_FORCE_INLINE static bool t3dErosionJobPlanByteBuffer(
	Terrain3DLinearArenaLayout &rLayout,
	const bool					bEnabled,
	const size_t				nCellCount ) {
	return !bEnabled || rLayout.addArray< uint8_t >( nCellCount );
}

EA_FORCE_INLINE static bool t3dErosionJobPlanIndexBuffer(
	Terrain3DLinearArenaLayout &rLayout,
	const bool					bEnabled,
	const size_t				nCellCount ) {
	return !bEnabled || rLayout.addArray< int64_t >( nCellCount );
}

static inline bool t3dErosionJobAssignFloatBuffer(
	Terrain3DLinearArena		  &rArena,
	Terrain3DArenaBuffer< float > &fBuffer,
	const bool					   bEnabled,
	const size_t				   nCellCount,
	const float					   fInitial,
	const size_t				   nMultiplier = 1u ) {
	if ( !bEnabled ) {
		fBuffer.clear();
		return true;
	}
	size_t count = 0u;
	return t3dErosionJobCheckedScaledCount( nCellCount, nMultiplier, count ) &&
		   fBuffer.assign( rArena, count, fInitial );
}

static inline bool t3dErosionJobAssignHalfBuffer(
	Terrain3DLinearArena &rArena,
	Terrain3DHalfBuffer	 &rBuffer,
	const bool			  bEnabled,
	const size_t		  nCellCount,
	const float			  fInitial ) {
	if ( !bEnabled ) {
		rBuffer.clear();
		return true;
	}
	return rBuffer.assign( rArena, nCellCount, fInitial );
}

static inline bool t3dErosionJobAssignByteBuffer(
	Terrain3DLinearArena			&rArena,
	Terrain3DArenaBuffer< uint8_t > &nBuffer,
	const bool						 bEnabled,
	const size_t					 nCellCount,
	const uint8_t					 nInitial ) {
	if ( !bEnabled ) {
		nBuffer.clear();
		return true;
	}
	return nBuffer.assign( rArena, nCellCount, nInitial );
}

static inline bool t3dErosionJobAssignIndexBuffer(
	Terrain3DLinearArena			&rArena,
	Terrain3DArenaBuffer< int64_t > &nBuffer,
	const bool						 bEnabled,
	const size_t					 nCellCount,
	const int64_t					 nInitial ) {
	if ( !bEnabled ) {
		nBuffer.clear();
		return true;
	}
	return nBuffer.assign( rArena, nCellCount, nInitial );
}

typedef struct t3dErosionResultBuffers_s
{
	Terrain3DArenaBuffer< uint8_t > m_bActive;
	Terrain3DArenaBuffer< float >	m_initialHeight;
	Terrain3DArenaBuffer< float >	m_height;
	Terrain3DArenaBuffer< float >	m_sedimentCoarse;
	Terrain3DArenaBuffer< float >	m_sedimentSand;
	Terrain3DArenaBuffer< float >	m_sedimentFine;
	Terrain3DArenaBuffer< float >	m_surfaceCoarse;
	Terrain3DArenaBuffer< float >	m_surfaceSand;
	Terrain3DArenaBuffer< float >	m_surfaceFine;
	Terrain3DArenaBuffer< float >	m_oxidation;
	Terrain3DArenaBuffer< float >	m_flowAccumulation;
	Terrain3DArenaBuffer< float >	m_hydraulicDeposition;
	Terrain3DArenaBuffer< float >	m_windDeposition;
	Terrain3DArenaBuffer< float >	m_waveDeposition;
	Terrain3DArenaBuffer< float >	m_weathering;
	Terrain3DArenaBuffer< float >	m_traffic;
	Terrain3DArenaBuffer< float >	m_alluvialDeposition;
	Terrain3DArenaBuffer< float >	m_debrisFlowDeposition;
	Terrain3DArenaBuffer< float >	m_duneDeposition;
	Terrain3DArenaBuffer< float >	m_saltWeathering;
	Terrain3DArenaBuffer< float >	m_wetDryCycles;
	Terrain3DArenaBuffer< float >	m_snowpack;
	Terrain3DArenaBuffer< float >	m_exposureAge;
	Terrain3DArenaBuffer< float >	m_desertPavement;
	Terrain3DArenaBuffer< float >	m_cumulativeErosion;
	Terrain3DArenaBuffer< uint8_t > m_dominantTexture;
	Terrain3DArenaBuffer< uint8_t > m_materialTransitionKind;
	Terrain3DArenaBuffer< float >	m_materialTransitionAmount;
} t3dErosionResultBuffers_t;

static inline bool t3dErosionJobFloatBufferIsZero(
	const Terrain3DArenaBuffer< float > &fBuffer ) {
	for ( size_t iSample = 0u; iSample < ( size_t ) ( ( fBuffer ).size() ); iSample++ ) {
		const float fValue = ( fBuffer )[iSample];
		if ( Math::abs( fValue ) > s_fErosionEpsilon ) {
			return false;
		}
	}
	return true;
}

static inline void t3dErosionJobClearFloatBufferIfZero(
	Terrain3DArenaBuffer< float > &fBuffer ) {
	if ( !fBuffer.empty() && t3dErosionJobFloatBufferIsZero( fBuffer ) ) {
		fBuffer.clear();
	}
}

EA_FORCE_INLINE static bool t3dErosionJobPlanResultFloat(
	Terrain3DLinearArenaLayout			&rLayout,
	const Terrain3DArenaBuffer< float > &fSource ) {
	return fSource.empty() || rLayout.addArray< float >( fSource.size() );
}

EA_FORCE_INLINE static bool t3dErosionJobPlanResultByte(
	Terrain3DLinearArenaLayout			  &rLayout,
	const Terrain3DArenaBuffer< uint8_t > &nSource ) {
	return nSource.empty() || rLayout.addArray< uint8_t >( nSource.size() );
}

static inline bool t3dErosionJobCopyResultFloat(
	Terrain3DLinearArena				&rArena,
	const Terrain3DArenaBuffer< float > &fSource,
	Terrain3DArenaBuffer< float >		&fDestination ) {
	if ( fSource.empty() ) {
		fDestination.clear();
		return true;
	}
	return fDestination.allocateUninitialized( rArena, fSource.size() ) && fDestination.copyFrom( fSource );
}

static inline bool t3dErosionJobCopyResultByte(
	Terrain3DLinearArena				  &rArena,
	const Terrain3DArenaBuffer< uint8_t > &nSource,
	Terrain3DArenaBuffer< uint8_t >		  &nDestination ) {
	if ( nSource.empty() ) {
		nDestination.clear();
		return true;
	}
	return nDestination.allocateUninitialized( rArena, nSource.size() ) && nDestination.copyFrom( nSource );
}

EA_FORCE_INLINE static void t3dErosionJobFnvHashPackedBytes(
	uint64_t			  &nHash,
	const PackedByteArray &arrData ) {
	t3dHashFnv1a64Append( nHash, arrData.ptr(), ( size_t ) ( arrData.size() ) );
}

EA_FORCE_INLINE static void t3dErosionJobFnvHashU8(
	uint64_t	 &nHash,
	const uint8_t nValue ) {
	t3dHashFnv1a64Append( nHash, &nValue, sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dErosionJobFnvHashI32(
	uint64_t	 &nHash,
	const int32_t nValue ) {
	t3dHashFnv1a64Append( nHash, &nValue, sizeof( nValue ) );
}

EA_FORCE_INLINE static void t3dErosionJobFnvHashF64(
	uint64_t	&nHash,
	const double fValue ) {
	t3dHashFnv1a64Append( nHash, &fValue, sizeof( fValue ) );
}

static inline bool t3dErosionJobAddImageSnapshotBytes(
	uint64_t		   &nTotal,
	const Ref< Image > &rImage ) {
	if ( rImage.is_null() ) {
		return true;
	}
	const uint64_t width = ( uint64_t ) ( MAX( 0, rImage->get_width() ) );
	const uint64_t height = ( uint64_t ) ( MAX( 0, rImage->get_height() ) );
	if ( height > 0u && width > eastl::numeric_limits< uint64_t >::max() / height ) {
		return false;
	}
	const uint64_t pixels = width * height;
	if ( pixels > eastl::numeric_limits< uint64_t >::max() / sizeof( float ) ) {
		return false;
	}
	const uint64_t bytes = pixels * sizeof( float );
	if ( nTotal > eastl::numeric_limits< uint64_t >::max() - bytes ) {
		return false;
	}
	nTotal += bytes;
	return true;
}

typedef Terrain3DParallelExecutor ParallelExecutor;

// t3dErosionFinalStatistics_s centralized in Terrain3DTypes.h.

// t3dErosionKernel_e centralized in Terrain3DTypes.h.

typedef struct t3dErosionKernelContext_s
{
	Terrain3DErosionJob							 *m_pJob = NULL;
	t3dErosionKernel_t							  m_kernel = t3dErosionKernel_t::WEATHERING;
	float										  m_fYearsStep = 0.0f;
	float										  m_fTalusGradient = 0.0f;
	float										  m_fDuneGradient = 0.0f;
	int32_t										  m_nStep = 0;
	float										  m_fStepProgress = 0.0f;
	float										  m_fMaster = 0.0f;
	float										  m_fWeatherCurve = 0.0f;
	float										  m_fOxidationCurve = 0.0f;
	float										  m_fRainfallCurve = 0.0f;
	float										  m_fWindCurve = 0.0f;
	float										  m_fWaveCurve = 0.0f;
	float										  m_fTrafficCurve = 0.0f;
	float										  m_fSnowCurve = 0.0f;
	float										  m_fSaltCurve = 0.0f;
	float										  m_fDebrisCurve = 0.0f;
	float										  m_fTemperature = 0.0f;
	float										  m_fStepRainfall = 0.0f;
	float										  m_fFreezeFactor = 0.0f;
	int32_t										  m_nWindDx = 0;
	int32_t										  m_nWindDy = 0;
	float										  m_fCurrentSeaLevel = 0.0f;
	float										  m_fCoastalBand = 0.0f;
	int32_t										  m_nWaveDx = 0;
	int32_t										  m_nWaveDy = 0;
	int32_t										  m_nFetchCells = 1;
	float										  m_fMaximumFlow = s_fErosionEpsilon;
	eastl::vector< t3dErosionFinalStatistics_t > *m_pFinalStatistics = NULL;
	eastl::vector< float >						 *m_pWorkerMaximumFlow = NULL;
} t3dErosionKernelContext_t;

// t3dErosionApplyContext_s centralized in Terrain3DTypes.h.

struct t3dErosionStepExecution_t
{
	const ParallelExecutor	  &m_executor;
	t3dErosionKernelContext_t &m_context;
	size_t					   m_nGrainAlignment;
	size_t					   m_nGrain;
	int32_t					   m_nStep;
};

struct t3dErosionKernels_s
{
	static void runWeatheringPhase( const t3dErosionStepExecution_t &rExecution );
	static void runHydraulicPhase( const t3dErosionStepExecution_t &rExecution );
	static void runDebrisFlowPhase( const t3dErosionStepExecution_t &rExecution );
	static void runWindPhase( const t3dErosionStepExecution_t &rExecution );
	static void runDunePhase( const t3dErosionStepExecution_t &rExecution );
	static void runSurfaceAgingPhase( const t3dErosionStepExecution_t &rExecution );
	static void runWavePhase( const t3dErosionStepExecution_t &rExecution );
	static void runTrafficPhase( const t3dErosionStepExecution_t &rExecution );
	static void runStratigraphyPhase( const t3dErosionStepExecution_t &rExecution );

	static void dispatchStatic(
		void		 *pUserData,
		const size_t  nBegin,
		const size_t  nEnd,
		const int32_t iWorkerIndex ) {
		t3dErosionKernelContext_t *pContext = ( t3dErosionKernelContext_t * ) ( pUserData );
		switch ( pContext->m_kernel ) {
		case t3dErosionKernel_t::WEATHERING:
			weathering( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::THERMAL_FLUX:
			thermalFlux( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::THERMAL_APPLY:
			thermalApply( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::RAINFALL:
			rainfall( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::HYDRAULIC_FLUX:
			hydraulicFlux( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::HYDRAULIC_WATER:
			hydraulicWater( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::HYDRAULIC_CAPACITY:
			hydraulicCapacity( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::DEBRIS_TRANSFER:
			debrisTransfer( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::SEDIMENT_ADVECTION:
			sedimentAdvection( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::WIND_TRANSPORT:
			windTransport( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::WIND_GATHER:
			windGather( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::DUNE_FLUX:
			duneFlux( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::DUNE_APPLY:
			duneApply( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::PAVEMENT:
			pavement( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::WAVE_TRANSPORT:
			waveTransport( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::WAVE_GATHER:
			waveGather( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::BEACH_SMOOTH:
			beachSmooth( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::TRAFFIC:
			traffic( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::STRATIGRAPHY:
			stratigraphy( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::EXPOSURE:
			exposure( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::FINAL_STATISTICS:
			finalStatistics( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::MAXIMUM_FLOW:
			maximumFlow( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		case t3dErosionKernel_t::MATERIAL_TRANSITIONS:
			materialTransitions( pContext, nBegin, nEnd, iWorkerIndex );
			break;
		}
	}

	EA_FORCE_INLINE static void dispatchDynamic(
		void		*pContext,
		const size_t nBegin,
		const size_t nEnd ) {
		dispatchStatic( pContext, nBegin, nEnd, 0 );
	}

	static void prepareStep(
		t3dErosionKernelContext_t &rContext,
		const int32_t			   nStep,
		float					  &fPreviousTemperature ) {
		Terrain3DErosionJob *job = rContext.m_pJob;
		rContext.m_nStep = nStep;
		rContext.m_fStepProgress = ( float ) ( nStep ) / ( float ) ( job->m_config.m_nSimulationSteps );
		rContext.m_fMaster = MAX( 0.0f, job->sampleCurve( CURVE_MASTER, nStep ) ) * job->m_config.m_fGlobalIntensity;
		rContext.m_fWeatherCurve = MAX( 0.0f, job->sampleCurve( CURVE_WEATHERING, nStep ) ) * rContext.m_fMaster;
		rContext.m_fOxidationCurve = MAX( 0.0f, job->sampleCurve( CURVE_OXIDATION, nStep ) ) * rContext.m_fMaster;
		rContext.m_fRainfallCurve = MAX( 0.0f, job->sampleCurve( CURVE_RAINFALL, nStep ) ) * rContext.m_fMaster;
		rContext.m_fWindCurve = MAX( 0.0f, job->sampleCurve( CURVE_WIND, nStep ) ) * rContext.m_fMaster;
		rContext.m_fWaveCurve = MAX( 0.0f, job->sampleCurve( CURVE_WAVE, nStep ) ) * rContext.m_fMaster;
		rContext.m_fTrafficCurve = MAX( 0.0f, job->sampleCurve( CURVE_TRAFFIC, nStep ) ) * rContext.m_fMaster;
		rContext.m_fSnowCurve = MAX( 0.0f, job->sampleCurve( CURVE_SNOWMELT, nStep ) ) * rContext.m_fMaster;
		rContext.m_fSaltCurve = MAX( 0.0f, job->sampleCurve( CURVE_SALT_WEATHERING, nStep ) ) * rContext.m_fMaster;
		rContext.m_fDebrisCurve = MAX( 0.0f, job->sampleCurve( CURVE_DEBRIS_FLOW, nStep ) ) * rContext.m_fMaster;
		rContext.m_fTemperature = job->sampleCurve( CURVE_TEMPERATURE, nStep );

		const bool storm_active =
			job->m_config.m_fStormFrequency >= 1.0f ||
			t3dErosionJobHashUnit( job->m_config.m_nSeed ^ 0x5a17ULL, ( size_t ) ( nStep ), ( uint64_t ) ( nStep ) ) <=
				job->m_config.m_fStormFrequency;
		rContext.m_fStepRainfall = job->m_config.m_fRainfallMPerYear * rContext.m_fYearsStep *
								   rContext.m_fRainfallCurve *
								   ( storm_active ? job->m_config.m_fStormIntensity : 0.0f );
		const bool crossed_freezing = ( fPreviousTemperature < job->m_config.m_fFreezePoint &&
										rContext.m_fTemperature >= job->m_config.m_fFreezePoint ) ||
									  ( fPreviousTemperature >= job->m_config.m_fFreezePoint &&
										rContext.m_fTemperature < job->m_config.m_fFreezePoint );
		const float freeze_distance = Math::abs( rContext.m_fTemperature - job->m_config.m_fFreezePoint );
		rContext.m_fFreezeFactor =
			t3dErosionJobClamp01( ( 1.0f - freeze_distance ) * 0.5f + ( crossed_freezing ? 0.75f : 0.0f ) );
		fPreviousTemperature = rContext.m_fTemperature;
	}

	static void applyRegion(
		void		*pUserData,
		const size_t nBegin,
		const size_t nEnd ) {
		t3dErosionApplyContext_t *pContext = ( t3dErosionApplyContext_t * ) ( pUserData );
		Terrain3DErosionJob		 *job = pContext->m_pJob;
		for ( size_t source_index = nBegin; source_index < nEnd; ++source_index ) {
			const float source_height = t3dErosionJobReadRf( pContext->m_pHeightSourceBytes, source_index );
			if ( isnan( source_height ) ) {
				continue;
			}
			const int32_t x = ( int32_t ) ( source_index % ( size_t ) ( T3D_REGION_SIZE ) );
			const int32_t y = ( int32_t ) ( source_index / ( size_t ) ( T3D_REGION_SIZE ) );

			if ( pContext->m_pHeightBytes ) {
				const float final_height =
					job->sampleOutput( job->m_height, pContext->m_nRegionId, ( float ) ( x ), ( float ) ( y ) );
				const float initial_height =
					job->sampleOutput( job->m_initialHeight, pContext->m_nRegionId, ( float ) ( x ), ( float ) ( y ) );
				const float delta = ( final_height - initial_height ) * job->m_config.m_fHeightOutputStrength;
				t3dErosionJobWriteRf( pContext->m_pHeightBytes, source_index, source_height + delta );
			}

			const int32_t sx = CLAMP( x / job->m_nResolutionScale, 0, job->m_nSimulationRegionSize - 1 );
			const int32_t sy = CLAMP( y / job->m_nResolutionScale, 0, job->m_nSimulationRegionSize - 1 );
			const size_t  sim_index = ( size_t ) ( pContext->m_nRegionId ) * job->m_nCellsPerRegion +
									 ( size_t ) ( sy ) * ( size_t ) ( job->m_nSimulationRegionSize ) +
									 ( size_t ) ( sx );
			if ( pContext->m_pControlBytes ) {
				const t3dMaterialTransitionKind_t transition_kind =
					( t3dMaterialTransitionKind_t ) ( job->m_materialTransitionKind[sim_index] );
				const float transition_amount = t3dErosionJobClamp01( job->m_materialTransitionAmount[sim_index] );
				if ( transition_kind != TRANSITION_NONE && transition_amount > s_fErosionEpsilon ) {
					const uint32_t old_control =
						t3dAsUint( t3dErosionJobReadRf( pContext->m_pControlSourceBytes, source_index ) );
					const uint8_t old_base = t3dGetBase( old_control );
					const uint8_t old_overlay = t3dGetOverlay( old_control );
					const uint8_t old_blend = t3dGetBlend( old_control );
					const bool	  auto_material = job->m_config.m_bResolveAutoMaterials &&
											   job->m_config.m_bAutoShaderEnabled && t3dIsAuto( old_control );
					const int32_t source_texture =
						auto_material ? ( int32_t ) ( job->m_dominantTexture[sim_index] )
									  : ( old_blend >= 128U ? ( int32_t ) ( old_overlay ) : ( int32_t ) ( old_base ) );
					const int32_t target_texture = job->getTransitionTarget( source_texture, transition_kind );
					if ( target_texture >= 0 && target_texture != source_texture ) {
						uint8_t output_base = ( uint8_t ) ( source_texture );
						uint8_t output_overlay = ( uint8_t ) ( target_texture );
						uint8_t output_blend = ( uint8_t ) ( CLAMP( Math::round( transition_amount * s_fByteMaxFloat ),
																	0.0f,
																	s_fByteMaxFloat ) );
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
						uint32_t preserved =
							job->m_config.m_bPreserveControlFlags
								? old_control
								: ( job->m_config.m_bPreserveHoles ? t3dEncHole( t3dIsHole( old_control ) ) : 0U );
						preserved &= ~( ( 0x1FU << 27 ) | ( 0x1FU << 22 ) | ( 0xFFU << 14 ) | 0x1U );
						const uint32_t value = preserved | t3dEncBase( output_base ) | t3dEncOverlay( output_overlay ) |
											   t3dEncBlend( output_blend ) | t3dEncAuto( false );
						t3dErosionJobWriteRf( pContext->m_pControlBytes, source_index, t3dAsFloat( value ) );
					}
				}
			}

			if ( pContext->m_pColorBytes ) {
				const float oxidation = t3dErosionJobClamp01(
					job->sampleOutput( job->m_oxidation, pContext->m_nRegionId, ( float ) ( x ), ( float ) ( y ) ) *
					job->m_config.m_fOxidationTintStrength );
				const size_t offset = source_index * 4U;
				const float	 inverse = 1.0f - oxidation;
				pContext->m_pColorBytes[offset] =
					( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->m_pColorBytes[offset] ) * inverse +
													  job->m_config.m_oxidationTint.r * s_fByteMaxFloat * oxidation ),
										 0.0f,
										 s_fByteMaxFloat ) );
				pContext->m_pColorBytes[offset + 1U] =
					( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->m_pColorBytes[offset + 1U] ) * inverse +
													  job->m_config.m_oxidationTint.g * s_fByteMaxFloat * oxidation ),
										 0.0f,
										 s_fByteMaxFloat ) );
				pContext->m_pColorBytes[offset + 2U] =
					( uint8_t ) ( CLAMP( Math::round( ( float ) ( pContext->m_pColorBytes[offset + 2U] ) * inverse +
													  job->m_config.m_oxidationTint.b * s_fByteMaxFloat * oxidation ),
										 0.0f,
										 s_fByteMaxFloat ) );
			}
		}
	}

	static void weathering(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		t3dDecodedHalfBlock_t substrate_hardness;
		t3dDecodedHalfBlock_t fWetDrySusceptibility;
		t3dDecodedHalfBlock_t salinity;
		t3dDecodedHalfBlock_t fSaltSusceptibility;
		t3dDecodedHalfBlock_t fOxidationSusceptibility;
		t3dDecodedHalfBlock_t fWeatheringSusceptibility;
		t3dDecodedHalfBlock_t fGrainSize;
		t3dDecodedHalfBlock_t fBedrockHardness;
		for ( size_t block_begin = begin; block_begin < end; block_begin += T3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( T3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			substrate_hardness.load( job->m_substrateHardness,
									 block_begin,
									 block_count,
									 job->m_config.m_fBaseHardness );
			if ( job->m_config.m_bWetDryEnabled ) {
				fWetDrySusceptibility.load( job->m_wetDrySusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.m_bSaltWeatheringEnabled ) {
				salinity.load( job->m_salinity, block_begin, block_count, job->m_config.m_fBackgroundSalinity );
				fSaltSusceptibility.load( job->m_saltSusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.m_bOxidationEnabled ) {
				fOxidationSusceptibility.load( job->m_oxidationSusceptibility, block_begin, block_count, 1.0f );
			}
			if ( job->m_config.m_bWeatheringEnabled ) {
				fWeatheringSusceptibility.load( job->m_weatheringSusceptibility, block_begin, block_count, 1.0f );
			}
			fGrainSize.load( job->m_grainSize, block_begin, block_count, 0.5f );
			if ( job->m_config.m_bStratigraphyEnabled ) {
				fBedrockHardness.load( job->m_bedrockHardness,
									   block_begin,
									   block_count,
									   job->m_config.m_fBedrockHardness );
			}

			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] || job->m_simulationWeight[index] <= s_fErosionEpsilon ) {
					continue;
				}
				const float local_weight = job->m_simulationWeight[index];
				const float slope = job->getSlope( index );
				const float standing_water = job->m_water.empty() ? 0.0f : job->m_water[index];
				const float snowpack = job->m_snowpack.empty() ? 0.0f : job->m_snowpack[index];
				const float moisture =
					t3dErosionJobClamp01( t3dErosionJobSafeDivide( standing_water + snowpack * 0.15f,
																   MAX( job->m_fCellSize, s_fErosionEpsilon ) ) +
										  pContext->m_fStepRainfall * 10.0f );
				const float loose_ratio = t3dErosionJobClamp01(
					t3dErosionJobSafeDivide( job->getLooseMaterial( index ),
											 MAX( job->m_fCellSize * 0.2f, s_fErosionEpsilon ) ) );
				const float exposure = 1.0f - loose_ratio;
				const float stability =
					t3dErosionJobClamp01( 1.0f - t3dErosionJobSafeDivide( slope,
																		  MAX( job->m_config.m_fVarnishStabilitySlope,
																			   s_fErosionEpsilon ) ) );
				const float varnish = job->m_config.m_bExposureAgeEnabled
										  ? t3dErosionJobClamp01( t3dErosionJobSafeDivide(
												job->m_exposureAge[index],
												MAX( job->m_config.m_fVarnishMaturationYears, 1.0f ) ) )
										  : 0.0f;

				float generated_loose = 0.0f;
				if ( job->m_config.m_bWetDryEnabled ) {
					const bool was_wet = job->m_wetState[index] != 0;
					if ( !was_wet && moisture >= job->m_config.m_fWetThreshold ) {
						job->m_wetState[index] = 1;
					} else if ( was_wet && moisture <= job->m_config.m_fDryThreshold ) {
						job->m_wetState[index] = 0;
						job->m_wetDryCycles[index] += 1.0f;
						generated_loose += job->m_config.m_fWetDryWeatheringMPerCycle *
										   fWetDrySusceptibility.m_szValues[local] * local_weight * pContext->m_fMaster;
					}
				}

				if ( job->m_config.m_bSaltWeatheringEnabled && pContext->m_fSaltCurve > 0.0f ) {
					const float drying = MAX( 0.0f, job->m_previousMoisture[index] - moisture );
					const float evaporative_cycle = t3dErosionJobClamp01(
						drying * 8.0f + job->m_config.m_fEvaporationFraction * job->m_config.m_fSaltEvaporationBias );
					const float salt_damage =
						MIN( job->m_config.m_fMaxHeightChangePerStep,
							 job->m_config.m_fSaltWeatheringMPerYear * pContext->m_fYearsStep * pContext->m_fSaltCurve *
								 salinity.m_szValues[local] * job->m_config.m_fSaltCrystallizationStrength *
								 ( 0.2f + 0.8f * evaporative_cycle ) * fSaltSusceptibility.m_szValues[local] *
								 exposure * local_weight );
					generated_loose += salt_damage;
					job->m_saltWeathering[index] += salt_damage;
				}
				if ( !job->m_previousMoisture.empty() ) {
					job->m_previousMoisture[index] = moisture;
				}

				if ( job->m_config.m_bOxidationEnabled ) {
					const float varnish_multiplier =
						1.0f + varnish * job->m_config.m_fVarnishOxidationMultiplier * stability;
					const float amount = job->m_config.m_fOxidationRatePerYear * pContext->m_fYearsStep *
										 pContext->m_fOxidationCurve * exposure * ( 0.25f + 0.75f * moisture ) *
										 fOxidationSusceptibility.m_szValues[local] * varnish_multiplier * local_weight;
					job->m_oxidation[index] = t3dErosionJobClamp01( job->m_oxidation[index] + amount );
				}

				if ( job->m_config.m_bWeatheringEnabled ) {
					const float physical = job->m_config.m_fPhysicalWeatheringMPerYear * pContext->m_fYearsStep *
										   ( 0.35f + slope * job->m_config.m_fWeatheringSlopeBias );
					const float chemical =
						job->m_config.m_fChemicalWeatheringMPerYear * pContext->m_fYearsStep *
						Math::pow( MAX( moisture, s_fErosionEpsilon ), job->m_config.m_fChemicalMoistureExponent );
					const float frost = job->m_config.m_fPhysicalWeatheringMPerYear * pContext->m_fYearsStep *
										job->m_config.m_fFreezeThawStrength * pContext->m_fFreezeFactor;
					generated_loose += MIN( job->m_config.m_fMaxHeightChangePerStep,
											MAX( 0.0f,
												 ( physical + chemical + frost ) * pContext->m_fWeatherCurve *
													 fWeatheringSusceptibility.m_szValues[local] * local_weight ) );
				}

				if ( generated_loose > s_fErosionEpsilon ) {
					float coarse = 0.0f;
					float sand = 0.0f;
					float fine = 0.0f;
					job->partitionMaterial( fGrainSize.m_szValues[local], generated_loose, coarse, sand, fine );
					job->depositSurface( index, coarse, sand, fine );
					job->m_weathering[index] = t3dErosionJobClamp01(
						job->m_weathering[index] +
						t3dErosionJobSafeDivide( generated_loose, MAX( job->m_fCellSize * 0.1f, s_fErosionEpsilon ) ) );
				}

				float substrate = substrate_hardness.m_szValues[local];
				if ( job->m_config.m_bStratigraphyEnabled ) {
					const float transition_width =
						MAX( job->m_config.m_fStratigraphyTransitionWidthM, s_fErosionEpsilon );
					const float bedrock_mix = t3dErosionJobClamp01(
						( job->m_cumulativeErosion[index] - job->m_surfaceLayerDepth[index] + transition_width ) /
						transition_width );
					substrate = Math::lerp( substrate, fBedrockHardness.m_szValues[local], bedrock_mix );
				}
				const float salt_weakening = t3dErosionJobClamp01( t3dErosionJobSafeDivide(
												 job->m_saltWeathering[index],
												 MAX( job->m_fCellSize * 0.1f, s_fErosionEpsilon ) ) ) *
											 job->m_config.m_fSaltCohesionLoss;
				job->m_hardness[index] =
					t3dErosionJobClamp01( substrate - job->m_oxidation[index] * job->m_config.m_fOxidationWeakening -
										  job->m_weathering[index] * 0.25f - salt_weakening );
			}
		}
	}
	static void thermalFlux(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		t3dDecodedHalfBlock_t fCohesion;
		t3dDecodedHalfBlock_t susceptibility;
		for ( size_t block_begin = begin; block_begin < end; block_begin += T3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( T3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fCohesion.load( job->m_cohesion, block_begin, block_count, 0.0f );
			susceptibility.load( job->m_weatheringSusceptibility, block_begin, block_count, 1.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float total_excess = 0.0f;
				float excess[T3D_EROSION_DIRECTION_COUNT];
				memset( excess, 0, sizeof( excess ) );
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor =
						job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
					if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
						excess[( size_t ) ( dir )] =
							MAX( 0.0f,
								 job->m_height[index] - job->m_height[( size_t ) ( neighbor )] -
									 pContext->m_fTalusGradient * job->m_fCellSize );
						total_excess += excess[( size_t ) ( dir )];
					}
				}
				if ( total_excess <= s_fErosionEpsilon ) {
					continue;
				}
				const float available = MIN( job->m_config.m_fMaxHeightChangePerStep, job->getLooseMaterial( index ) );
				const float total_transfer =
					MIN( available,
						 total_excess * job->m_config.m_fThermalCreepRate * pContext->m_fWeatherCurve *
							 ( 1.0f - fCohesion.m_szValues[local] ) * susceptibility.m_szValues[local] *
							 job->m_simulationWeight[index] );
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor =
						job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
					const float target_weight = neighbor >= 0 ? job->m_simulationWeight[( size_t ) ( neighbor )] : 0.0f;
					job->m_szFlux[( size_t ) ( dir )][index] =
						total_transfer * t3dErosionJobSafeDivide( excess[( size_t ) ( dir )], total_excess ) *
						MIN( job->m_simulationWeight[index], target_weight );
				}
			}
		}
	}
	static void thermalApply(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		t3dDecodedHalfBlock_t fGrainSize;
		for ( size_t block_begin = begin; block_begin < end; block_begin += T3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( T3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fGrainSize.load( job->m_grainSize, block_begin, block_count, 0.5f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float outgoing = 0.0f;
				float incoming = 0.0f;
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					outgoing += job->m_szFlux[( size_t ) ( dir )][index];
					const int64_t neighbor =
						job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
					if ( neighbor >= 0 ) {
						incoming +=
							job->m_szFlux[( size_t ) ( s_szErosionOppositeDirection[dir] )][( size_t ) ( neighbor )];
					}
				}
				if ( outgoing > s_fErosionEpsilon ) {
					float c = 0.0f, s = 0.0f, f = 0.0f;
					job->entrainSurface( index, outgoing, c, s, f );
				}
				if ( incoming > s_fErosionEpsilon ) {
					float c = 0.0f, s = 0.0f, f = 0.0f;
					job->partitionMaterial( fGrainSize.m_szValues[local], incoming, c, s, f );
					job->depositSurface( index, c, s, f );
				}
				job->m_height[index] += incoming - outgoing;
			}
		}
	}
	static void rainfall(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		t3dDecodedHalfBlock_t fInfiltrationMultiplier;
		for ( size_t block_begin = begin; block_begin < end; block_begin += T3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( T3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fInfiltrationMultiplier.load( job->m_infiltrationMultiplier, block_begin, block_count, 1.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				const float variation = Math::lerp( 1.0f - job->m_config.m_fRainfallVariation,
													1.0f + job->m_config.m_fRainfallVariation,
													t3dErosionJobHashUnit( job->m_config.m_nSeed ^ 0x1257ULL,
																		   job->stochasticIndex( index ),
																		   ( uint64_t ) ( pContext->m_nStep ) ) );
				float		precipitation = pContext->m_fStepRainfall * variation * job->m_simulationWeight[index];
				if ( job->m_config.m_bSnowmeltEnabled ) {
					if ( pContext->m_fTemperature <= job->m_config.m_fSnowAccumulationTemperature ) {
						const float snowfall = precipitation * job->m_config.m_fSnowfallFraction;
						job->m_snowpack[index] += snowfall;
						precipitation -= snowfall;
					}
					if ( pContext->m_fTemperature >= job->m_config.m_fSnowMeltTemperature &&
						 job->m_snowpack[index] > s_fErosionEpsilon ) {
						const float melt = MIN( job->m_snowpack[index],
												job->m_snowpack[index] * job->m_config.m_fSnowMeltFractionPerStep *
													pContext->m_fSnowCurve );
						job->m_snowpack[index] -= melt;
						precipitation += melt;
					}
					job->m_snowpack[index] *= 1.0f - t3dErosionJobClamp01( job->m_config.m_fSnowSublimationFraction );
				}
				const float compacted = job->m_compaction.empty() ? 0.0f : job->m_compaction[index];
				const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
				const float runoff_multiplier = 1.0f + compacted * job->m_config.m_fTrafficRunoffMultiplier;
				const float infiltration = t3dErosionJobClamp01(
					job->m_config.m_fInfiltrationFraction * fInfiltrationMultiplier.m_szValues[local] *
					( 1.0f - compacted ) * ( 1.0f - pavement * job->m_config.m_fPavementInfiltrationReduction ) );
				const float added_water = precipitation * runoff_multiplier * ( 1.0f - infiltration );
				job->m_water[index] = MIN( job->m_fCellSize * 8.0f, job->m_water[index] + MAX( 0.0f, added_water ) );
			}
		}
	}
	static void hydraulicFlux(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_water[index] <= s_fErosionEpsilon ) {
				continue;
			}
			const float surface = job->m_height[index] + job->m_water[index];
			float		total_potential = 0.0f;
			float		potential[T3D_EROSION_DIRECTION_COUNT];
			memset( potential, 0, sizeof( potential ) );
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					potential[( size_t ) ( dir )] = MAX(
						0.0f,
						surface - ( job->m_height[( size_t ) ( neighbor )] + job->m_water[( size_t ) ( neighbor )] ) );
				} else if ( job->m_config.m_boundaryMode == BOUNDARY_OPEN ) {
					potential[( size_t ) ( dir )] = job->m_water[index] * 0.5f;
				}
				total_potential += potential[( size_t ) ( dir )];
			}
			if ( total_potential <= s_fErosionEpsilon ) {
				continue;
			}
			const float transferable = MIN( job->m_water[index], total_potential * job->m_config.m_fFlowRate * 0.25f );
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				job->m_szFlux[( size_t ) ( dir )][index] =
					transferable * t3dErosionJobSafeDivide( potential[( size_t ) ( dir )], total_potential );
			}
		}
	}
	static void hydraulicWater(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				job->m_nextA[index] = 0.0f;
				continue;
			}
			float outgoing = 0.0f;
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 ) {
					incoming +=
						job->m_szFlux[( size_t ) ( s_szErosionOppositeDirection[dir] )][( size_t ) ( neighbor )];
				}
			}
			job->m_nextA[index] = MAX( 0.0f, job->m_water[index] + incoming - outgoing );
			job->m_flowAccumulation[index] += incoming + outgoing;
			job->m_nextB[index] = job->getSlope( index );
		}
	}
	static void hydraulicCapacity(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		t3dDecodedHalfBlock_t fHydraulicErodibility;
		t3dDecodedHalfBlock_t fCohesion;
		for ( size_t block_begin = begin; block_begin < end; block_begin += T3D_EROSION_HALF_DECODE_BLOCK ) {
			const size_t block_count = MIN( T3D_EROSION_HALF_DECODE_BLOCK, end - block_begin );
			fHydraulicErodibility.load( job->m_hydraulicErodibility, block_begin, block_count, 1.0f );
			fCohesion.load( job->m_cohesion, block_begin, block_count, 0.0f );
			for ( size_t local = 0u; local < block_count; ++local ) {
				const size_t index = block_begin + local;
				if ( !job->m_active[index] ) {
					continue;
				}
				float outgoing = 0.0f;
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				}
				const float velocity =
					t3dErosionJobSafeDivide( outgoing, MAX( job->m_nextA[index], s_fErosionEpsilon ) );
				const float slope = MAX( job->m_config.m_fMinimumSlope, job->m_nextB[index] );
				const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
				const float armor = 1.0f - pavement * job->m_config.m_fPavementArmorStrength;
				const float material_mobility = fHydraulicErodibility.m_szValues[local] *
												( 1.0f - fCohesion.m_szValues[local] ) * MAX( 0.0f, armor );
				const float base_capacity = job->m_config.m_fSedimentCapacity * velocity * Math::sqrt( slope ) *
											MAX( job->m_nextA[index], 0.001f ) * MAX( 0.0f, material_mobility );
				const float coarse_capacity =
					base_capacity *
					( job->m_config.m_bSedimentSortingEnabled ? job->m_config.m_fCoarseTransportMultiplier : 1.0f );
				const float sand_capacity =
					base_capacity *
					( job->m_config.m_bSedimentSortingEnabled ? job->m_config.m_fSandTransportMultiplier : 1.0f );
				const float fine_capacity =
					base_capacity *
					( job->m_config.m_bSedimentSortingEnabled ? job->m_config.m_fFineTransportMultiplier : 1.0f );
				const float demand = MAX( 0.0f, coarse_capacity - job->m_sedimentCoarse[index] ) +
									 MAX( 0.0f, sand_capacity - job->m_sedimentSand[index] ) +
									 MAX( 0.0f, fine_capacity - job->m_sedimentFine[index] );
				if ( demand > s_fErosionEpsilon ) {
					const float erodibility = ( 1.0f - t3dErosionJobClamp01( job->m_hardness[index] ) ) *
											  fHydraulicErodibility.m_szValues[local] *
											  ( 1.0f - fCohesion.m_szValues[local] ) * MAX( 0.0f, armor );
					const float channel_signal =
						t3dErosionJobClamp01( Math::log( 1.0f + job->m_flowAccumulation[index] /
																	MAX( job->m_fCellSize, s_fErosionEpsilon ) ) /
											  8.0f );
					const float channel_factor = 1.0f + job->m_config.m_fChannelIncisionStrength * channel_signal;
					const float rate_limit =
						job->m_config.m_fHydraulicErosionMPerYear * pContext->m_fYearsStep *
						pContext->m_fRainfallCurve *
						MAX( 0.0f, 0.15f * fHydraulicErodibility.m_szValues[local] + 0.85f * erodibility ) *
						channel_factor * job->m_simulationWeight[index];
					const float eroded =
						MIN( job->m_config.m_fMaxHeightChangePerStep, MIN( demand, MAX( 0.0f, rate_limit ) ) );
					if ( eroded > s_fErosionEpsilon ) {
						float c = 0.0f, s = 0.0f, f = 0.0f;
						job->entrainSurface( index, eroded, c, s, f );
						job->m_sedimentCoarse[index] += c;
						job->m_sedimentSand[index] += s;
						job->m_sedimentFine[index] += f;
						job->m_height[index] -= eroded;
					}
				}

				const float still_water = 1.0f - t3dErosionJobClamp01( velocity );
				const float base_deposition =
					t3dErosionJobClamp01( job->m_config.m_fHydraulicDepositionRate +
										  job->m_config.m_fStandingWaterDepositionRate * still_water );
				const float coarse_excess = MAX( 0.0f, job->m_sedimentCoarse[index] - coarse_capacity );
				const float sand_excess = MAX( 0.0f, job->m_sedimentSand[index] - sand_capacity );
				const float fine_excess = MAX( 0.0f, job->m_sedimentFine[index] - fine_capacity );
				float		coarse_deposit = coarse_excess * base_deposition * job->m_config.m_fCoarseSettlingRate;
				float		sand_deposit = sand_excess * base_deposition * job->m_config.m_fSandSettlingRate;
				float		fine_deposit = fine_excess * base_deposition * job->m_config.m_fFineSettlingRate;
				const float total_deposit = coarse_deposit + sand_deposit + fine_deposit;
				if ( total_deposit > job->m_config.m_fMaxHeightChangePerStep ) {
					const float scale =
						t3dErosionJobSafeDivide( job->m_config.m_fMaxHeightChangePerStep, total_deposit );
					coarse_deposit *= scale;
					sand_deposit *= scale;
					fine_deposit *= scale;
				}
				const float deposited =
					( coarse_deposit + sand_deposit + fine_deposit ) * job->m_simulationWeight[index];
				if ( deposited > s_fErosionEpsilon ) {
					job->m_sedimentCoarse[index] -= coarse_deposit * job->m_simulationWeight[index];
					job->m_sedimentSand[index] -= sand_deposit * job->m_simulationWeight[index];
					job->m_sedimentFine[index] -= fine_deposit * job->m_simulationWeight[index];
					job->depositSurface( index,
										 coarse_deposit * job->m_simulationWeight[index],
										 sand_deposit * job->m_simulationWeight[index],
										 fine_deposit * job->m_simulationWeight[index] );
					job->m_height[index] += deposited;
					job->m_hydraulicDeposition[index] += deposited;
				}
			}
		}
	}
	static void debrisTransfer(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_water[index] <= s_fErosionEpsilon ) {
				continue;
			}
			float	upstream_slope = 0.0f;
			float	downstream_slope = 0.0f;
			int64_t spread_destination = -1;
			float	flattest_downstream = eastl::numeric_limits< float >::max();
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor < 0 || !job->m_active[( size_t ) ( neighbor )] ) {
					continue;
				}
				const float delta =
					t3dErosionJobSafeDivide( job->m_height[( size_t ) ( neighbor )] - job->m_height[index],
											 job->m_fCellSize );
				if ( delta > 0.0f ) {
					upstream_slope = MAX( upstream_slope, delta );
				} else {
					const float downhill = -delta;
					downstream_slope = MAX( downstream_slope, downhill );
					if ( downhill < flattest_downstream &&
						 job->m_simulationWeight[( size_t ) ( neighbor )] > s_fErosionEpsilon ) {
						flattest_downstream = downhill;
						spread_destination = neighbor;
					}
				}
			}
			const float slope_drop =
				MAX( 0.0f, upstream_slope - downstream_slope - job->m_config.m_fAlluvialFanSlopeDrop );
			const float flow_signal = t3dErosionJobClamp01(
				Math::log( 1.0f + job->m_flowAccumulation[index] / MAX( job->m_fCellSize, s_fErosionEpsilon ) ) /
				8.0f );
			const float fan_signal =
				t3dErosionJobClamp01( slope_drop * 8.0f ) * flow_signal * job->m_config.m_fAlluvialFanStrength;
			if ( fan_signal <= s_fErosionEpsilon ) {
				continue;
			}
			float coarse =
				MIN( job->m_sedimentCoarse[index],
					 job->m_sedimentCoarse[index] * fan_signal * ( 0.5f + 0.5f * job->m_config.m_fAlluvialFanSpread ) );
			float		sand = MIN( job->m_sedimentSand[index], job->m_sedimentSand[index] * fan_signal * 0.5f );
			const float limited = MIN( job->m_config.m_fMaxHeightChangePerStep, coarse + sand );
			if ( coarse + sand > limited && coarse + sand > s_fErosionEpsilon ) {
				const float scale = limited / ( coarse + sand );
				coarse *= scale;
				sand *= scale;
			}
			job->m_transferCoarse[index] = coarse;
			job->m_transferSand[index] = sand;
			job->m_transferAmount[index] =
				spread_destination >= 0 ? t3dErosionJobClamp01( job->m_config.m_fAlluvialFanSpread ) : 0.0f;
			job->m_transferDestination[index] = spread_destination;
		}
	}
	static void sedimentAdvection(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				job->m_nextCoarse[index] = job->m_nextSand[index] = job->m_nextFine[index] = 0.0f;
				continue;
			}
			const float source_water = MAX( job->m_water[index], s_fErosionEpsilon );
			const float concentrations[3] = {
				t3dErosionJobSafeDivide( job->m_sedimentCoarse[index], source_water ),
				t3dErosionJobSafeDivide( job->m_sedimentSand[index], source_water ),
				t3dErosionJobSafeDivide( job->m_sedimentFine[index], source_water ),
			};
			float outgoing[3] = { 0.0f, 0.0f, 0.0f };
			float incoming[3] = { 0.0f, 0.0f, 0.0f };
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const float water_flux = job->m_szFlux[( size_t ) ( dir )][index];
				for ( int32_t cls = 0; cls < 3; ++cls ) {
					outgoing[cls] += water_flux * concentrations[cls];
				}
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					const float neighbor_water = MAX( job->m_water[( size_t ) ( neighbor )], s_fErosionEpsilon );
					const float incoming_flux =
						job->m_szFlux[( size_t ) ( s_szErosionOppositeDirection[dir] )][( size_t ) ( neighbor )];
					incoming[0] +=
						incoming_flux *
						t3dErosionJobSafeDivide( job->m_sedimentCoarse[( size_t ) ( neighbor )], neighbor_water );
					incoming[1] +=
						incoming_flux *
						t3dErosionJobSafeDivide( job->m_sedimentSand[( size_t ) ( neighbor )], neighbor_water );
					incoming[2] +=
						incoming_flux *
						t3dErosionJobSafeDivide( job->m_sedimentFine[( size_t ) ( neighbor )], neighbor_water );
				}
			}
			job->m_nextCoarse[index] =
				MAX( 0.0f,
					 job->m_sedimentCoarse[index] - MIN( outgoing[0], job->m_sedimentCoarse[index] ) + incoming[0] );
			job->m_nextSand[index] =
				MAX( 0.0f, job->m_sedimentSand[index] - MIN( outgoing[1], job->m_sedimentSand[index] ) + incoming[1] );
			job->m_nextFine[index] =
				MAX( 0.0f, job->m_sedimentFine[index] - MIN( outgoing[2], job->m_sedimentFine[index] ) + incoming[2] );
			job->m_nextA[index] *= 1.0f - t3dErosionJobClamp01( job->m_config.m_fEvaporationFraction );
		}
	}
	static void windTransport(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			int64_t destination = ( int64_t ) ( index );
			for ( int32_t travel = 0; travel < job->m_config.m_nWindTransportDistanceCells; ++travel ) {
				const int64_t next =
					job->neighborIndex( ( size_t ) ( destination ), pContext->m_nWindDx, pContext->m_nWindDy );
				if ( next < 0 || !job->m_active[( size_t ) ( next )] ) {
					destination = -1;
					break;
				}
				destination = next;
			}
			if ( destination < 0 ) {
				continue;
			}
			const int64_t upwind = job->neighborIndex( index, -pContext->m_nWindDx, -pContext->m_nWindDy );
			float		  shelter = 0.0f;
			if ( upwind >= 0 && job->m_active[( size_t ) ( upwind )] ) {
				shelter = t3dErosionJobClamp01(
					t3dErosionJobSafeDivide( job->m_height[( size_t ) ( upwind )] - job->m_height[index],
											 job->m_fCellSize ) *
					job->m_config.m_fWindShelterStrength );
			}
			const float variation = Math::lerp( 1.0f - job->m_config.m_fWindVariation,
												1.0f + job->m_config.m_fWindVariation,
												t3dErosionJobHashUnit( job->m_config.m_nSeed ^ 0x91f5ULL,
																	   job->stochasticIndex( index ),
																	   ( uint64_t ) ( pContext->m_nStep ) ) );
			const float fStrength =
				MAX( 0.0f,
					 job->m_config.m_fWindStrength * pContext->m_fWindCurve * variation * ( 1.0f - shelter ) -
						 job->m_config.m_fWindThreshold );
			const float exposed =
				1.0f -
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->getLooseMaterial( index ),
															   MAX( job->m_fCellSize * 0.2f, s_fErosionEpsilon ) ) );
			const float pavement = job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index];
			const float armor = 1.0f - pavement * job->m_config.m_fPavementArmorStrength;
			const float abrasion =
				MIN( job->m_config.m_fMaxHeightChangePerStep,
					 job->m_config.m_fWindErosionMPerYear * pContext->m_fYearsStep * fStrength * exposed *
						 ( 0.25f + 0.75f * ( 1.0f - job->m_hardness[index] ) ) * job->m_windErodibility[index] *
						 ( 1.0f - job->m_cohesion[index] ) * MAX( 0.0f, armor ) * job->m_simulationWeight[index] );
			const float available = job->getLooseMaterial( index ) + abrasion;
			const float destination_flatness =
				1.0f - t3dErosionJobClamp01( job->getSlope( ( size_t ) ( destination ) ) );
			const float moved =
				MIN( job->m_config.m_fMaxHeightChangePerStep,
					 available * job->m_config.m_fWindTransportRate * job->m_config.m_fWindDepositionRate *
						 t3dErosionJobClamp01( fStrength ) * ( 1.0f - job->m_cohesion[index] ) *
						 ( 1.0f + job->m_config.m_fWindDepositionSlopeBias * destination_flatness ) *
						 MIN( job->m_simulationWeight[index], job->m_simulationWeight[( size_t ) ( destination )] ) );
			if ( moved <= s_fErosionEpsilon ) {
				continue;
			}
			float c = 0.0f, s = 0.0f, f = 0.0f;
			job->entrainSurface( index, moved, c, s, f );
			// Saltation strongly favors sand, suspension favors fines, coarse lag barely moves.
			job->m_transferCoarse[index] = c * 0.15f;
			job->m_transferSand[index] = s;
			job->m_transferFine[index] = f * 0.65f;
			const float exported =
				job->m_transferCoarse[index] + job->m_transferSand[index] + job->m_transferFine[index];
			const float retained = moved - exported;
			if ( retained > s_fErosionEpsilon ) {
				job->depositSurface( index,
									 c - job->m_transferCoarse[index],
									 s - job->m_transferSand[index],
									 f - job->m_transferFine[index] );
			}
			job->m_height[index] -= exported;
			job->m_transferDestination[index] = destination;
		}
	}
	static void windGather(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float	c = 0.0f, s = 0.0f, f = 0.0f;
			int64_t source = ( int64_t ) ( index );
			for ( int32_t travel = 0; travel < job->m_config.m_nWindTransportDistanceCells; ++travel ) {
				source = job->neighborIndex( ( size_t ) ( source ), -pContext->m_nWindDx, -pContext->m_nWindDy );
				if ( source < 0 ) {
					break;
				}
				if ( job->m_transferDestination[( size_t ) ( source )] == ( int64_t ) ( index ) ) {
					c += job->m_transferCoarse[( size_t ) ( source )];
					s += job->m_transferSand[( size_t ) ( source )];
					f += job->m_transferFine[( size_t ) ( source )];
				}
			}
			if ( c + s + f > s_fErosionEpsilon ) {
				job->depositSurface( index, c, s, f );
				job->m_height[index] += c + s + f;
				job->m_windDeposition[index] += c + s + f;
			}
		}
	}
	static void duneFlux(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_surfaceSand[index] < job->m_config.m_fDuneMinSandDepthM ) {
				continue;
			}
			float total_excess = 0.0f;
			float excess[T3D_EROSION_DIRECTION_COUNT];
			memset( excess, 0, sizeof( excess ) );
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					excess[( size_t ) ( dir )] = MAX( 0.0f,
													  job->m_height[index] - job->m_height[( size_t ) ( neighbor )] -
														  pContext->m_fDuneGradient * job->m_fCellSize );
					total_excess += excess[( size_t ) ( dir )];
				}
			}
			if ( total_excess <= s_fErosionEpsilon ) {
				continue;
			}
			const float moved = MIN(
				job->m_surfaceSand[index],
				MIN( job->m_config.m_fMaxHeightChangePerStep, total_excess * job->m_config.m_fDuneAvalancheRate ) );
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				job->m_szFlux[( size_t ) ( dir )][index] =
					moved * t3dErosionJobSafeDivide( excess[( size_t ) ( dir )], total_excess );
			}
		}
	}
	static void duneApply(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float outgoing = 0.0f;
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				outgoing += job->m_szFlux[( size_t ) ( dir )][index];
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 ) {
					incoming +=
						job->m_szFlux[( size_t ) ( s_szErosionOppositeDirection[dir] )][( size_t ) ( neighbor )];
				}
			}
			job->m_surfaceSand[index] = MAX( 0.0f, job->m_surfaceSand[index] + incoming - outgoing );
			job->m_height[index] += incoming - outgoing;
			job->m_duneDeposition[index] += incoming;
		}
	}
	static void pavement(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const float total = MAX( job->getLooseMaterial( index ), s_fErosionEpsilon );
			const float coarse_fraction = job->m_surfaceCoarse[index] / total;
			const float fine_fraction = job->m_surfaceFine[index] / total;
			const float lag_signal =
				coarse_fraction *
				t3dErosionJobClamp01( ( job->m_config.m_fPavementFineRemovalThreshold - fine_fraction ) /
									  MAX( job->m_config.m_fPavementFineRemovalThreshold, s_fErosionEpsilon ) );
			const float process_signal = MAX( job->m_config.m_bWindEnabled ? pContext->m_fWindCurve : 0.0f,
											  job->m_config.m_bHydraulicEnabled ? pContext->m_fRainfallCurve : 0.0f );
			job->m_desertPavement[index] = t3dErosionJobClamp01(
				job->m_desertPavement[index] + lag_signal * job->m_config.m_fPavementFormationRate * process_signal /
												   ( float ) ( job->m_config.m_nSimulationSteps ) );
		}
	}
	static void waveTransport(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ||
				 job->m_height[index] < pContext->m_fCurrentSeaLevel - pContext->m_fCoastalBand ||
				 job->m_height[index] > pContext->m_fCurrentSeaLevel + pContext->m_fCoastalBand ) {
				continue;
			}
			bool	touches_water = false;
			int64_t lowest_neighbor = -1;
			float	lowest_height = job->m_height[index];
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					touches_water =
						touches_water || job->m_height[( size_t ) ( neighbor )] <= pContext->m_fCurrentSeaLevel;
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
			for ( ; fetch < pContext->m_nFetchCells; ++fetch ) {
				cursor = job->neighborIndex( ( size_t ) ( cursor ), -pContext->m_nWaveDx, -pContext->m_nWaveDy );
				if ( cursor < 0 || !job->m_active[( size_t ) ( cursor )] ||
					 job->m_height[( size_t ) ( cursor )] > pContext->m_fCurrentSeaLevel ) {
					break;
				}
			}
			const float exposure =
				t3dErosionJobSafeDivide( ( float ) ( fetch ), ( float ) ( pContext->m_nFetchCells ) );
			const float slope = job->getSlope( index );
			const float erosion =
				MIN( job->m_config.m_fMaxHeightChangePerStep,
					 MAX( 0.0f,
						  job->m_config.m_fWaveErosionMPerYear * pContext->m_fYearsStep *
							  job->m_config.m_fWaveStrength * pContext->m_fWaveCurve * exposure *
							  ( 1.0f + job->m_config.m_fWaveUndercutBias * t3dErosionJobClamp01( slope ) ) *
							  ( 0.2f + 0.8f * ( 1.0f - job->m_hardness[index] ) ) * job->m_waveErodibility[index] *
							  ( 1.0f - job->m_cohesion[index] ) * job->m_simulationWeight[index] ) );
			if ( erosion <= s_fErosionEpsilon ) {
				continue;
			}
			int64_t destination = job->neighborIndex( index, -pContext->m_nWaveDx, -pContext->m_nWaveDy );
			if ( destination < 0 || !job->m_active[( size_t ) ( destination )] ||
				 job->m_height[( size_t ) ( destination )] > pContext->m_fCurrentSeaLevel ) {
				destination = lowest_neighbor;
			}
			job->m_transferAmount[index] = erosion;
			job->m_transferDestination[index] = destination;
		}
	}
	static void waveGather(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			float incoming = 0.0f;
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_transferDestination[( size_t ) ( neighbor )] == ( int64_t ) ( index ) ) {
					incoming += job->m_transferAmount[( size_t ) ( neighbor )] * job->m_config.m_fWaveDepositionRate *
								MIN( job->m_simulationWeight[( size_t ) ( neighbor )], job->m_simulationWeight[index] );
				}
			}
			const float outgoing = job->m_transferAmount[index];
			if ( outgoing > s_fErosionEpsilon ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->entrainSurface( index, outgoing, c, s, f );
			}
			if ( incoming > s_fErosionEpsilon ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->partitionMaterial( job->m_grainSize[index], incoming, c, s, f );
				job->depositSurface( index, c, s, f );
				job->m_waveDeposition[index] += incoming;
			}
			job->m_height[index] += incoming - outgoing;
		}
	}
	static void beachSmooth(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			job->m_nextA[index] = job->m_height[index];
			if ( !job->m_active[index] ||
				 Math::abs( job->m_height[index] - pContext->m_fCurrentSeaLevel ) > pContext->m_fCoastalBand ) {
				continue;
			}
			float	sum = 0.0f;
			int32_t nCount = 0;
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					sum += job->m_height[( size_t ) ( neighbor )];
					nCount += 1;
				}
			}
			if ( nCount > 0 ) {
				job->m_nextA[index] =
					Math::lerp( job->m_height[index],
								sum / ( float ) ( nCount ),
								t3dErosionJobClamp01( job->m_config.m_fBeachSmoothing * pContext->m_fWaveCurve *
													  job->m_simulationWeight[index] * 0.25f ) );
			}
		}
	}
	static void traffic(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] || job->m_traffic[index] <= s_fErosionEpsilon ) {
				job->m_nextA[index] = job->m_height[index];
				continue;
			}
			const float weight = job->m_traffic[index] * pContext->m_fTrafficCurve * job->m_simulationWeight[index];
			const float erosion = MIN( job->m_config.m_fMaxHeightChangePerStep,
									   job->m_config.m_fTrafficErosionMPerYear * pContext->m_fYearsStep * weight *
										   ( 1.0f + job->getSlope( index ) ) * job->m_trafficErodibility[index] *
										   ( 1.0f - job->m_cohesion[index] * 0.75f ) );
			float		neighbor_sum = 0.0f;
			int32_t		neighbor_count = 0;
			for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
				const int64_t neighbor =
					job->neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
				if ( neighbor >= 0 && job->m_active[( size_t ) ( neighbor )] ) {
					neighbor_sum += job->m_height[( size_t ) ( neighbor )];
					neighbor_count += 1;
				}
			}
			float smoothed = job->m_height[index];
			if ( neighbor_count > 0 ) {
				smoothed = Math::lerp( job->m_height[index],
									   neighbor_sum / ( float ) ( neighbor_count ),
									   job->m_config.m_fTrafficSmoothing * weight * 0.25f );
			}
			job->m_nextA[index] = smoothed - erosion;
			if ( erosion > s_fErosionEpsilon ) {
				float c = 0.0f, s = 0.0f, f = 0.0f;
				job->entrainSurface( index, erosion, c, s, f );
			}
			job->m_compaction[index] =
				t3dErosionJobClamp01( job->m_compaction[index] + weight * job->m_config.m_fTrafficCompaction /
																	 ( float ) ( job->m_config.m_nSimulationSteps ) );
		}
	}
	EA_FORCE_INLINE static void stratigraphy(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;
		t3dSimdUpdateCumulativeErosion( job->m_active.data() + begin,
										job->m_initialHeight.data() + begin,
										job->m_height.data() + begin,
										job->m_cumulativeErosion.data() + begin,
										end - begin );
	}
	static void exposure(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const float change = Math::abs( job->m_height[index] - job->m_stepHeightStart[index] );
			const float loose_cover = t3dErosionJobClamp01(
				t3dErosionJobSafeDivide( job->getLooseMaterial( index ),
										 MAX( job->m_config.m_fExposureResetDepthM, s_fErosionEpsilon ) ) );
			const float stability = t3dErosionJobClamp01(
				1.0f - t3dErosionJobSafeDivide( job->getSlope( index ),
												MAX( job->m_config.m_fVarnishStabilitySlope, s_fErosionEpsilon ) ) );
			if ( change >= job->m_config.m_fExposureResetDepthM || loose_cover >= 1.0f ) {
				job->m_exposureAge[index] =
					MAX( 0.0f,
						 job->m_exposureAge[index] -
							 pContext->m_fYearsStep *
								 t3dErosionJobClamp01(
									 change / MAX( job->m_config.m_fExposureResetDepthM, s_fErosionEpsilon ) ) );
			} else {
				job->m_exposureAge[index] +=
					pContext->m_fYearsStep * stability * ( 1.0f - loose_cover ) * job->m_simulationWeight[index];
			}
		}
	}
	static void finalStatistics(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		const int32_t		 worker_index = iWorkerIndex;

		t3dErosionFinalStatistics_t local;
		const double				cell_area = ( double ) ( job->m_fCellSize ) * ( double ) ( job->m_fCellSize );
		for ( size_t index = begin; index < end; ++index ) {
			if ( job->m_active[index] == 0u ) {
				continue;
			}
			const float delta = job->m_height[index] - job->m_initialHeight[index];
			if ( delta < 0.0f ) {
				local.m_fEroded += ( double ) ( -delta ) * cell_area;
				local.m_fMaximumLowering = MAX( local.m_fMaximumLowering, -delta );
			} else {
				local.m_fDeposited += ( double ) ( delta ) *cell_area;
				local.m_fMaximumRaising = MAX( local.m_fMaximumRaising, delta );
			}
		}
		( *pContext->m_pFinalStatistics )[( size_t ) ( worker_index )] = local;
	}
	static void maximumFlow(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		const int32_t		 worker_index = iWorkerIndex;

		float local_maximum = s_fErosionEpsilon;
		for ( size_t index = begin; index < end; ++index ) {
			if ( job->m_active[index] != 0u ) {
				local_maximum = MAX( local_maximum, job->m_flowAccumulation[index] );
			}
		}
		( *pContext->m_pWorkerMaximumFlow )[( size_t ) ( worker_index )] = local_maximum;
	}
	static void materialTransitions(
		t3dErosionKernelContext_t *pContext,
		const size_t			   begin,
		const size_t			   end,
		const int32_t			   iWorkerIndex ) {
		Terrain3DErosionJob *job = pContext->m_pJob;
		( void ) iWorkerIndex;

		for ( size_t index = begin; index < end; ++index ) {
			if ( !job->m_active[index] ) {
				continue;
			}
			const int32_t source_texture = ( int32_t ) ( job->m_dominantTexture[index] );
			const float	  slope = t3dErosionJobClamp01( job->getSlope( index ) );
			const float	  transition_depth = MAX( job->m_config.m_fMaterialTransitionDepthM, s_fErosionEpsilon );
			const float	  loose =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->getLooseMaterial( index ), transition_depth ) );
			const float lowering = t3dErosionJobClamp01(
				t3dErosionJobSafeDivide( MAX( 0.0f, job->m_initialHeight[index] - job->m_height[index] ),
										 transition_depth ) );
			const float flow = t3dErosionJobClamp01(
				t3dErosionJobSafeDivide( job->m_flowAccumulation[index], pContext->m_fMaximumFlow ) );
			const float hydraulic =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_hydraulicDeposition[index], transition_depth ) );
			const float wind =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_windDeposition[index], transition_depth ) );
			const float wave =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_waveDeposition[index], transition_depth ) );
			const float alluvial =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_alluvialDeposition[index], transition_depth ) );
			const float debris =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_debrisFlowDeposition[index], transition_depth ) );
			const float dune =
				t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_duneDeposition[index], transition_depth ) );
			const float loose_total = MAX( job->getLooseMaterial( index ), s_fErosionEpsilon );
			const float coarse_fraction = t3dErosionJobSafeDivide( job->m_surfaceCoarse[index], loose_total );
			const float fine_fraction = t3dErosionJobSafeDivide( job->m_surfaceFine[index], loose_total );
			const float coarse_deposition =
				MAX( MAX( alluvial, debris ), MAX( wind * coarse_fraction, wave * coarse_fraction ) );
			const float fine_deposition =
				MAX( hydraulic * ( 0.5f + 0.5f * flow ), MAX( wind * fine_fraction, dune * 0.25f ) );
			const float				   varnish = job->m_config.m_bExposureAgeEnabled
													 ? t3dErosionJobClamp01( t3dErosionJobSafeDivide(
											   job->m_exposureAge[index],
											   MAX( job->m_config.m_fVarnishMaturationYears, 1.0f ) ) )
													 : 0.0f;
			const t3dTransitionScore_t scores[8] = {
				t3dTransitionScore_t{ TRANSITION_EXPOSED,
									  lowering * ( 0.35f + 0.65f * slope ) * ( 1.0f - loose * 0.75f ) },
				t3dTransitionScore_t{ TRANSITION_WEATHERED,
									  job->m_weathering[index] * ( 0.5f + 0.5f * ( 1.0f - slope ) ) },
				t3dTransitionScore_t{ TRANSITION_OXIDIZED, job->m_oxidation[index] * ( 1.0f - loose * 0.5f ) },
				t3dTransitionScore_t{ TRANSITION_COARSE_DEPOSIT, coarse_deposition * ( 0.35f + 0.65f * loose ) },
				t3dTransitionScore_t{ TRANSITION_FINE_DEPOSIT, fine_deposition * ( 1.0f - slope * 0.75f ) },
				t3dTransitionScore_t{
					TRANSITION_SALT_WEATHERED,
					t3dErosionJobClamp01( t3dErosionJobSafeDivide( job->m_saltWeathering[index], transition_depth ) ) },
				t3dTransitionScore_t{ TRANSITION_DESERT_PAVEMENT,
									  job->m_desertPavement.empty() ? 0.0f : job->m_desertPavement[index] },
				t3dTransitionScore_t{ TRANSITION_VARNISHED, varnish * job->m_oxidation[index] * ( 1.0f - loose ) },
			};

			t3dMaterialTransitionKind_t best_kind = TRANSITION_NONE;
			float						best_score = 0.0f;
			for ( size_t iScore = 0u; iScore < T3D_ARRAY_COUNT( scores ); iScore++ ) {
				const t3dTransitionScore_t &candidate = ( scores )[iScore];
				if ( job->getTransitionTarget( source_texture, candidate.m_kind ) < 0 ) {
					continue;
				}
				if ( candidate.m_fScore > best_score ) {
					best_kind = candidate.m_kind;
					best_score = candidate.m_fScore;
				}
			}
			if ( best_kind == TRANSITION_NONE || best_score <= job->m_config.m_fMaterialTransitionThreshold ) {
				continue;
			}
			const float normalized = t3dErosionJobSafeDivide(
				best_score - job->m_config.m_fMaterialTransitionThreshold,
				MAX( 1.0f - job->m_config.m_fMaterialTransitionThreshold, s_fErosionEpsilon ) );
			const float amount = t3dErosionJobClamp01( normalized * job->m_config.m_fMaterialTransitionStrength );
			if ( amount <= s_fErosionEpsilon ) {
				continue;
			}
			job->m_materialTransitionKind[index] = ( uint8_t ) ( best_kind );
			job->m_materialTransitionAmount[index] = amount;
		}
	}
} t3dErosionKernels_t;

EA_FORCE_INLINE static void t3dErosionRunStaticKernel(
	const ParallelExecutor	  &rExecutor,
	t3dErosionKernelContext_t &rContext,
	const t3dErosionKernel_t   kernel,
	const size_t			   nCount,
	const size_t			   nAlignment ) {
	rContext.m_kernel = kernel;
	rExecutor.runStatic( nCount, nAlignment, t3dErosionKernels_s::dispatchStatic, &rContext );
}

EA_FORCE_INLINE static void t3dErosionRunDynamicKernel(
	const ParallelExecutor	  &rExecutor,
	t3dErosionKernelContext_t &rContext,
	const t3dErosionKernel_t   kernel,
	const size_t			   nCount,
	const size_t			   nGrain ) {
	rContext.m_kernel = kernel;
	rExecutor.run( nCount, nGrain, t3dErosionKernels_s::dispatchDynamic, &rContext );
}

Terrain3DErosionJob::Terrain3DErosionJob(
	void ) {
	const Terrain3DEastlAllocator bootstrap_allocator = Terrain3DEastlAllocator::bootstrapAllocator();
	m_config.m_curveSamples.set_allocator( bootstrap_allocator );
	m_regions.set_allocator( bootstrap_allocator );
	m_szRegionNeighbors.set_allocator( bootstrap_allocator );
	memset( m_szRegionLookup, 0xff, sizeof( m_szRegionLookup ) );
}

void Terrain3DErosionJob::setErrorMessage(
	const String &rMessage ) {
	Terrain3DScopedLock lock( m_statusMutex );
	m_errorMessage = rMessage;
}

void Terrain3DErosionJob::setFailed(
	const String &rMessage ) {
	setErrorMessage( rMessage );
	m_stage.store( STAGE_FAILED );
	m_state.store( STATE_FAILED );
	m_fProgress.store( 1.0f );
}

void Terrain3DErosionJob::setStage(
	const t3dStage_t stage,
	const float		 fProgress ) {
	m_stage.store( stage );
	m_fProgress.store( t3dErosionJobClamp01( fProgress ) );
}

void Terrain3DErosionJob::copyConfig(
	const Ref< Terrain3DErosionSettings > &rSettings ) {
	( t3dErosionConfigSnapshot_t & ) ( m_config ) = rSettings->getConfigSnapshot();
}

void Terrain3DErosionJob::sampleCurves(
	const Ref< Terrain3DErosionSettings > &rSettings ) {
	const int32_t steps = m_config.m_nSimulationSteps;
	m_config.m_nCurveStride = steps;
	m_config.m_curveSamples.resize( ( size_t ) ( CURVE_COUNT ) * ( size_t ) ( steps ) );
	const Ref< Curve > pCurves[CURVE_COUNT] = {
		rSettings->getMasterIntensityCurve(),
		rSettings->getRainfallCurve(),
		rSettings->getWeatheringCurve(),
		rSettings->getOxidationCurve(),
		rSettings->getWindCurve(),
		rSettings->getWaveCurve(),
		rSettings->getTrafficCurve(),
		rSettings->getTemperatureCurve(),
		rSettings->getSeaLevelCurve(),
		rSettings->getSnowmeltCurve(),
		rSettings->getSaltWeatheringCurve(),
		rSettings->getDebrisFlowCurve(),
	};
	const float defaults[CURVE_COUNT] = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f };
	for ( int32_t curve = 0; curve < CURVE_COUNT; ++curve ) {
		float *pDestination = m_config.m_curveSamples.data() + ( size_t ) ( curve ) * ( size_t ) ( steps );
		for ( int32_t step = 0; step < steps; ++step ) {
			const float fT = ( ( float ) ( step ) + 0.5f ) / ( float ) ( steps );
			pDestination[step] = pCurves[( size_t ) ( curve )].is_valid()
									 ? pCurves[( size_t ) ( curve )]->sample_baked( fT )
									 : defaults[( size_t ) ( curve )];
		}
	}
}

float Terrain3DErosionJob::sampleCurve(
	const t3dCurveId_t curve,
	const int32_t	   nStep ) const {
	if ( m_config.m_nCurveStride <= 0 || m_config.m_curveSamples.empty() ) {
		if ( curve == CURVE_TEMPERATURE ) {
			return 0.5f;
		}
		if ( curve == CURVE_SEA_LEVEL ) {
			return 0.0f;
		}
		return 1.0f;
	}
	const int32_t step = CLAMP( nStep, 0, m_config.m_nCurveStride - 1 );
	return m_config.m_curveSamples[( size_t ) ( curve ) * ( size_t ) ( m_config.m_nCurveStride ) + ( size_t ) ( step )];
}

uint64_t Terrain3DErosionJob::estimateMemoryBytes(
	const int32_t nRegionCount,
	const int32_t nScale ) const {
	if ( nScale <= 0 || nRegionCount <= 0 ) {
		return eastl::numeric_limits< uint64_t >::max();
	}

	const bool bWeatherStage = m_config.m_bWeatheringEnabled || m_config.m_bOxidationEnabled ||
							   m_config.m_bWetDryEnabled || m_config.m_bSaltWeatheringEnabled ||
							   m_config.m_bExposureAgeEnabled || m_config.m_bStratigraphyEnabled;
	const bool bHydraulicFamily = m_config.m_bHydraulicEnabled || m_config.m_bDebrisFlowEnabled;
	const bool bSurfaceFamily = bWeatherStage || bHydraulicFamily || m_config.m_bWindEnabled ||
								m_config.m_bDuneEnabled || m_config.m_bDesertPavementEnabled ||
								m_config.m_bWaveEnabled || m_config.m_bTrafficEnabled ||
								m_config.m_bMaterialOutputEnabled;
	const bool bHardnessFamily = bWeatherStage || m_config.m_bHydraulicEnabled || m_config.m_bWindEnabled ||
								 m_config.m_bWaveEnabled || m_config.m_bTrafficEnabled;

	uint64_t bytes_per_cell = sizeof( uint8_t ) + 3ULL * sizeof( float );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bLivePreviewEnabled, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bSurfaceFamily, 3u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bHardnessFamily, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bHardnessFamily, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bSurfaceFamily, 2u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bSurfaceFamily, 1u, sizeof( uint8_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bHydraulicFamily, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bHydraulicEnabled && m_config.m_bSnowmeltEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bHydraulicFamily, 3u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bHydraulicEnabled || m_config.m_bMaterialOutputEnabled,
							  2u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bHydraulicEnabled, 2u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bWindEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWindEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bWaveEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWaveEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bTrafficEnabled, 2u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bTrafficEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, bWeatherStage || m_config.m_bMaterialOutputEnabled, 2u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  bWeatherStage || m_config.m_bMaterialOutputEnabled || m_config.m_bOxidationColorEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bSaltWeatheringEnabled, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWeatheringEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bOxidationEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bSaltWeatheringEnabled, 2u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWetDryEnabled, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWetDryEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bWetDryEnabled, 1u, sizeof( uint8_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bExposureAgeEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bAlluvialFanEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bDebrisFlowEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bDuneEnabled || m_config.m_bMaterialOutputEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bDesertPavementEnabled, 1u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bStratigraphyEnabled, 2u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bStratigraphyEnabled, 1u, sizeof( uint16_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bMaterialOutputEnabled, 1u, sizeof( uint8_t ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bMaterialOutputEnabled, 1u, sizeof( float ) );

	t3dErosionJobAddMapBytes( bytes_per_cell,
							  ( m_config.m_bWeatheringEnabled && m_config.m_fThermalCreepRate > 0.0f ) ||
								  m_config.m_bHydraulicEnabled || m_config.m_bDuneEnabled,
							  T3D_EROSION_DIRECTION_COUNT,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bHydraulicEnabled ||
								  ( m_config.m_bWaveEnabled && m_config.m_fBeachSmoothing > 0.0f ) ||
								  m_config.m_bTrafficEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bHydraulicEnabled, 4u, sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bAlluvialFanEnabled || m_config.m_bWaveEnabled,
							  1u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bAlluvialFanEnabled || m_config.m_bDebrisFlowEnabled ||
								  m_config.m_bWindEnabled,
							  2u,
							  sizeof( float ) );
	t3dErosionJobAddMapBytes( bytes_per_cell,
							  m_config.m_bDebrisFlowEnabled || m_config.m_bWindEnabled,
							  1u,
							  sizeof( float ) );
	if ( m_config.m_bAlluvialFanEnabled || m_config.m_bDebrisFlowEnabled || m_config.m_bWindEnabled ||
		 m_config.m_bWaveEnabled ) {
		bytes_per_cell += sizeof( int64_t );
	}
	t3dErosionJobAddMapBytes( bytes_per_cell, m_config.m_bExposureAgeEnabled, 1u, sizeof( float ) );

	const uint64_t simulation_size = ( uint64_t ) ( T3D_REGION_SIZE / nScale );
	const uint64_t simulation_cells = ( uint64_t ) ( nRegionCount ) *simulation_size * simulation_size;
	const uint64_t source_cells =
		( uint64_t ) ( nRegionCount ) * ( uint64_t ) ( T3D_REGION_SIZE ) * ( uint64_t ) ( T3D_REGION_SIZE );
	const uint64_t source_snapshot_bytes = source_cells * sizeof( float ) + m_nSourceControlSnapshotBytes;
	const uint64_t working_bytes = simulation_cells * bytes_per_cell;
	return source_snapshot_bytes + m_nInputSnapshotBytes + working_bytes + working_bytes / 5ULL + 1024ULL * 1024ULL;
}

int32_t Terrain3DErosionJob::selectResolutionScale(
	const int32_t nRegionCount ) const {
	int32_t		   scale = m_config.m_nResolutionScale;
	const uint64_t budget = ( uint64_t ) ( m_config.m_nMemoryBudgetMb ) * 1024ULL * 1024ULL;
	if ( !m_config.m_bAutoAdjustResolution ) {
		return scale;
	}
	while ( scale < 16 && estimateMemoryBytes( nRegionCount, scale ) > budget ) {
		scale *= 2;
	}
	return scale;
}

void Terrain3DErosionJob::buildBufferBindings(
	eastl::vector< PlaneBinding > &rBindings ) {
	const bool bWeatherStage = m_config.m_bWeatheringEnabled || m_config.m_bOxidationEnabled ||
							   m_config.m_bWetDryEnabled || m_config.m_bSaltWeatheringEnabled ||
							   m_config.m_bExposureAgeEnabled || m_config.m_bStratigraphyEnabled;
	const bool bHydraulicFamily = m_config.m_bHydraulicEnabled || m_config.m_bDebrisFlowEnabled;
	const bool bSurfaceFamily = bWeatherStage || bHydraulicFamily || m_config.m_bWindEnabled ||
								m_config.m_bDuneEnabled || m_config.m_bDesertPavementEnabled ||
								m_config.m_bWaveEnabled || m_config.m_bTrafficEnabled ||
								m_config.m_bMaterialOutputEnabled;
	const bool bHardnessFamily = bWeatherStage || m_config.m_bHydraulicEnabled || m_config.m_bWindEnabled ||
								 m_config.m_bWaveEnabled || m_config.m_bTrafficEnabled;
	const bool bNeedsFlux = ( m_config.m_bWeatheringEnabled && m_config.m_fThermalCreepRate > 0.0f ) ||
							m_config.m_bHydraulicEnabled || m_config.m_bDuneEnabled;
	const bool bNeedsNextA = m_config.m_bHydraulicEnabled ||
							 ( m_config.m_bWaveEnabled && m_config.m_fBeachSmoothing > 0.0f ) ||
							 m_config.m_bTrafficEnabled;
	const bool bNeedsTransferAmount = m_config.m_bAlluvialFanEnabled || m_config.m_bWaveEnabled;
	const bool bNeedsTransferCoarseSand =
		m_config.m_bAlluvialFanEnabled || m_config.m_bDebrisFlowEnabled || m_config.m_bWindEnabled;
	const bool bNeedsTransferFine = m_config.m_bDebrisFlowEnabled || m_config.m_bWindEnabled;
	const bool bNeedsTransferDestination = m_config.m_bAlluvialFanEnabled || m_config.m_bDebrisFlowEnabled ||
										   m_config.m_bWindEnabled || m_config.m_bWaveEnabled;

	rBindings.emplace_back( m_active, true, ( uint8_t ) ( 1 ), false );
	rBindings.emplace_back( m_simulationWeight, true, 1.0f, false );
	rBindings.emplace_back( m_initialHeight, true, 0.0f, false );
	rBindings.emplace_back( m_height, true, 0.0f, false );
	rBindings.emplace_back( m_surfaceCoarse, bSurfaceFamily, 0.0f, false );
	rBindings.emplace_back( m_surfaceSand, bSurfaceFamily, 0.0f, false );
	rBindings.emplace_back( m_surfaceFine, bSurfaceFamily, 0.0f, false );
	rBindings.emplace_back( m_substrateHardness, bHardnessFamily, m_config.m_fBaseHardness, false );
	rBindings.emplace_back( m_hardness, bHardnessFamily, m_config.m_fBaseHardness, false );
	rBindings.emplace_back( m_cohesion, bSurfaceFamily, 0.0f, false );
	rBindings.emplace_back( m_grainSize, bSurfaceFamily, 0.5f, false );
	rBindings.emplace_back( m_dominantTexture, bSurfaceFamily, ( uint8_t ) ( 0 ), false );
	rBindings.emplace_back( m_water, bHydraulicFamily, 0.0f, false );
	rBindings.emplace_back( m_snowpack, m_config.m_bHydraulicEnabled && m_config.m_bSnowmeltEnabled, 0.0f, false );
	rBindings.emplace_back( m_sedimentCoarse, bHydraulicFamily, 0.0f, false );
	rBindings.emplace_back( m_sedimentSand, bHydraulicFamily, 0.0f, false );
	rBindings.emplace_back( m_sedimentFine, bHydraulicFamily, 0.0f, false );
	rBindings.emplace_back( m_flowAccumulation,
							m_config.m_bHydraulicEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_hydraulicDeposition,
							m_config.m_bHydraulicEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_hydraulicErodibility, m_config.m_bHydraulicEnabled, 1.0f, false );
	rBindings.emplace_back( m_infiltrationMultiplier, m_config.m_bHydraulicEnabled, 1.0f, false );
	rBindings.emplace_back( m_windDeposition,
							m_config.m_bWindEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_windErodibility, m_config.m_bWindEnabled, 1.0f, false );
	rBindings.emplace_back( m_waveDeposition,
							m_config.m_bWaveEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_waveErodibility, m_config.m_bWaveEnabled, 1.0f, false );
	rBindings.emplace_back( m_traffic, m_config.m_bTrafficEnabled, 0.0f, false );
	rBindings.emplace_back( m_compaction, m_config.m_bTrafficEnabled, 0.0f, false );
	rBindings.emplace_back( m_trafficErodibility, m_config.m_bTrafficEnabled, 1.0f, false );
	rBindings.emplace_back( m_weathering, bWeatherStage || m_config.m_bMaterialOutputEnabled, 0.0f, false );
	rBindings.emplace_back( m_oxidation,
							bWeatherStage || m_config.m_bMaterialOutputEnabled || m_config.m_bOxidationColorEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_saltWeathering, bWeatherStage || m_config.m_bMaterialOutputEnabled, 0.0f, false );
	rBindings.emplace_back( m_previousMoisture, m_config.m_bSaltWeatheringEnabled, 0.0f, false );
	rBindings.emplace_back( m_weatheringSusceptibility, m_config.m_bWeatheringEnabled, 1.0f, false );
	rBindings.emplace_back( m_oxidationSusceptibility, m_config.m_bOxidationEnabled, 1.0f, false );
	rBindings.emplace_back( m_salinity, m_config.m_bSaltWeatheringEnabled, m_config.m_fBackgroundSalinity, false );
	rBindings.emplace_back( m_saltSusceptibility, m_config.m_bSaltWeatheringEnabled, 1.0f, false );
	rBindings.emplace_back( m_wetDryCycles, m_config.m_bWetDryEnabled, 0.0f, false );
	rBindings.emplace_back( m_wetDrySusceptibility, m_config.m_bWetDryEnabled, 1.0f, false );
	rBindings.emplace_back( m_wetState, m_config.m_bWetDryEnabled, ( uint8_t ) ( 0 ), false );
	rBindings.emplace_back( m_exposureAge,
							m_config.m_bExposureAgeEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_alluvialDeposition,
							m_config.m_bAlluvialFanEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_debrisFlowDeposition,
							m_config.m_bDebrisFlowEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_duneDeposition,
							m_config.m_bDuneEnabled || m_config.m_bMaterialOutputEnabled,
							0.0f,
							false );
	rBindings.emplace_back( m_desertPavement, m_config.m_bDesertPavementEnabled, 0.0f, false );
	rBindings.emplace_back( m_cumulativeErosion, m_config.m_bStratigraphyEnabled, 0.0f, false );
	rBindings.emplace_back( m_surfaceLayerDepth,
							m_config.m_bStratigraphyEnabled,
							m_config.m_fSurfaceLayerDepthM,
							false );
	rBindings.emplace_back( m_bedrockHardness, m_config.m_bStratigraphyEnabled, m_config.m_fBedrockHardness, false );
	rBindings.emplace_back( m_materialTransitionKind,
							m_config.m_bMaterialOutputEnabled,
							( uint8_t ) ( TRANSITION_NONE ),
							false );
	rBindings.emplace_back( m_materialTransitionAmount, m_config.m_bMaterialOutputEnabled, 0.0f, false );
	rBindings.emplace_back( m_nextA, bNeedsNextA, 0.0f, false );
	rBindings.emplace_back( m_nextB, m_config.m_bHydraulicEnabled, 0.0f, false );
	rBindings.emplace_back( m_nextCoarse, m_config.m_bHydraulicEnabled, 0.0f, false );
	rBindings.emplace_back( m_nextSand, m_config.m_bHydraulicEnabled, 0.0f, false );
	rBindings.emplace_back( m_nextFine, m_config.m_bHydraulicEnabled, 0.0f, false );
	for ( auto &plane : m_szFlux ) {
		rBindings.emplace_back( plane, bNeedsFlux, 0.0f, true );
	}
	rBindings.emplace_back( m_transferAmount, bNeedsTransferAmount, 0.0f, true );
	rBindings.emplace_back( m_transferCoarse, bNeedsTransferCoarseSand, 0.0f, true );
	rBindings.emplace_back( m_transferSand, bNeedsTransferCoarseSand, 0.0f, true );
	rBindings.emplace_back( m_transferFine, bNeedsTransferFine, 0.0f, true );
	rBindings.emplace_back( m_transferDestination, bNeedsTransferDestination, ( int64_t ) ( -1 ), true );
	rBindings.emplace_back( m_stepHeightStart, m_config.m_bExposureAgeEnabled, 0.0f, true );
}

void Terrain3DErosionJob::clearBoundBuffers(
	const eastl::vector< PlaneBinding > &rBindings ) {
	for ( const auto &binding : rBindings ) {
		if ( binding.pFloat ) {
			binding.pFloat->clear();
		}
		if ( binding.pHalf ) {
			binding.pHalf->clear();
		}
		if ( binding.pByte ) {
			binding.pByte->clear();
		}
		if ( binding.pIndex ) {
			binding.pIndex->clear();
		}
	}
	m_persistentMemory.release();
	m_phaseMemory.release();
}

bool Terrain3DErosionJob::planBoundBuffers(
	const eastl::vector< PlaneBinding > &rBindings,
	Terrain3DLinearArenaLayout			&rPersistent,
	Terrain3DLinearArenaLayout			&rPhase ) const {
	bool bValid = true;
	for ( const auto &binding : rBindings ) {
		Terrain3DLinearArenaLayout &layout = binding.bPhase ? rPhase : rPersistent;
		if ( binding.pFloat ) {
			t3dErosionJobKeepValid( t3dErosionJobPlanFloatBuffer( layout, binding.bEnabled, m_nCellCount, 1u ),
									bValid );
		}
		if ( binding.pHalf ) {
			t3dErosionJobKeepValid( t3dErosionJobPlanHalfBuffer( layout, binding.bEnabled, m_nCellCount ), bValid );
		}
		if ( binding.pByte ) {
			t3dErosionJobKeepValid( t3dErosionJobPlanByteBuffer( layout, binding.bEnabled, m_nCellCount ), bValid );
		}
		if ( binding.pIndex ) {
			t3dErosionJobKeepValid( t3dErosionJobPlanIndexBuffer( layout, binding.bEnabled, m_nCellCount ), bValid );
		}
	}
	return bValid && rPersistent.valid() && rPhase.valid();
}

bool Terrain3DErosionJob::assignBoundBuffers(
	const eastl::vector< PlaneBinding > &rBindings ) {
	bool bValid = true;
	for ( const auto &binding : rBindings ) {
		Terrain3DLinearArena &arena = binding.bPhase ? m_phaseMemory : m_persistentMemory;
		if ( binding.pFloat ) {
			t3dErosionJobKeepValid( t3dErosionJobAssignFloatBuffer( arena,
																	*binding.pFloat,
																	binding.bEnabled,
																	m_nCellCount,
																	binding.fInitial ),
									bValid );
		}
		if ( binding.pHalf ) {
			t3dErosionJobKeepValid( t3dErosionJobAssignHalfBuffer( arena,
																   *binding.pHalf,
																   binding.bEnabled,
																   m_nCellCount,
																   binding.fInitial ),
									bValid );
		}
		if ( binding.pByte ) {
			t3dErosionJobKeepValid( t3dErosionJobAssignByteBuffer( arena,
																   *binding.pByte,
																   binding.bEnabled,
																   m_nCellCount,
																   ( uint8_t ) binding.fInitial ),
									bValid );
		}
		if ( binding.pIndex ) {
			t3dErosionJobKeepValid( t3dErosionJobAssignIndexBuffer( arena,
																	*binding.pIndex,
																	binding.bEnabled,
																	m_nCellCount,
																	( int64_t ) binding.fInitial ),
									bValid );
		}
	}
	return bValid;
}

bool Terrain3DErosionJob::allocateBuffers(
	void ) {
	eastl::vector< PlaneBinding > bindings;
	buildBufferBindings( bindings );
	clearBoundBuffers( bindings );
	Terrain3DLinearArenaLayout persistent_layout;
	Terrain3DLinearArenaLayout phase_layout;
	if ( !planBoundBuffers( bindings, persistent_layout, phase_layout ) ||
		 !m_persistentMemory.ensureCapacity( persistent_layout.size() ) ||
		 !m_phaseMemory.ensureCapacity( phase_layout.size() ) ) {
		clearBoundBuffers( bindings );
		setFailed( "Erosion buffer layout exceeds the configured address space or could not be allocated." );
		return false;
	}
	if ( !assignBoundBuffers( bindings ) || m_persistentMemory.used() != persistent_layout.size() ||
		 m_phaseMemory.used() != phase_layout.size() ) {
		clearBoundBuffers( bindings );
		setFailed( "Erosion buffer allocation did not match its exact arena layout." );
		return false;
	}
	if ( !allocatePreviewBuffer() ) {
		clearBoundBuffers( bindings );
		return false;
	}
	return true;
}

bool Terrain3DErosionJob::allocatePreviewBuffer(
	void ) {
	Terrain3DScopedLock lock( m_previewMutex );
	m_previewHeight.clear();
	m_previewMemory.release();
	m_previewRevision.store( 0 );
	m_previewStep.store( 0 );
	m_bPreviewWaiting.store( false );
	m_previewStepRequests.store( 0 );
	m_bPreviewPaused.store( m_config.m_bLivePreviewEnabled && m_config.m_bLivePreviewStartPaused );
	if ( !m_config.m_bLivePreviewEnabled ) {
		return true;
	}
	Terrain3DLinearArenaLayout layout;
	if ( !layout.addArray< float >( m_nCellCount ) || !layout.valid() ||
		 !m_previewMemory.ensureCapacity( layout.size() ) ||
		 !m_previewHeight.allocateUninitialized( m_previewMemory, m_nCellCount ) ||
		 m_previewMemory.used() != layout.size() ) {
		m_previewHeight.clear();
		m_previewMemory.release();
		setFailed( "Live erosion preview could not allocate its exact height snapshot." );
		return false;
	}
	return true;
}

void Terrain3DErosionJob::publishPreviewSnapshot(
	const int32_t nStep,
	const bool	  bForce ) {
	if ( !m_config.m_bLivePreviewEnabled || m_previewHeight.empty() || m_height.empty() ) {
		return;
	}
	const int32_t interval = MAX( 1, m_config.m_nLivePreviewIntervalSteps );
	if ( !bForce && !m_bPreviewPaused.load() && nStep > 0 && nStep < m_config.m_nSimulationSteps &&
		 nStep % interval != 0 ) {
		return;
	}
	{
		Terrain3DScopedLock lock( m_previewMutex );
		if ( !m_previewHeight.copyFrom( m_height ) ) {
			return;
		}
		m_previewStep.store( CLAMP( nStep, 0, m_config.m_nSimulationSteps ) );
		m_previewRevision.fetch_add( 1 );
	}
}

bool Terrain3DErosionJob::waitForPreviewPermission(
	void ) {
	if ( !m_config.m_bLivePreviewEnabled || !m_bPreviewPaused.load() ) {
		return !m_bCancelRequested.load();
	}
	Terrain3DUniqueLock lock( m_previewMutex );
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

Ref< Image > Terrain3DErosionJob::makeHeightImage(
	const FloatBuffer &rSource,
	const int32_t	   nRegionId,
	const bool		   bFullResolution ) const {
	if ( rSource.empty() || nRegionId < 0 || nRegionId >= ( int32_t ) ( m_regions.size() ) ) {
		return Ref< Image >();
	}
	const int32_t	size = bFullResolution ? T3D_REGION_SIZE : m_nSimulationRegionSize;
	const size_t	pixel_count = ( size_t ) ( size ) * ( size_t ) ( size );
	PackedByteArray image_data;
	image_data.resize( ( int64_t ) ( pixel_count * sizeof( float ) ) );
	uint8_t *const pDestination = image_data.ptrw();
	const size_t   source_offset = ( size_t ) ( nRegionId ) *m_nCellsPerRegion;
	if ( !bFullResolution ) {
		memcpy( pDestination, rSource.data() + source_offset, pixel_count * sizeof( float ) );
		return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
	}
	for ( int32_t y = 0; y < size; ++y ) {
		for ( int32_t x = 0; x < size; ++x ) {
			const float	 fValue = sampleOutput( rSource, nRegionId, ( float ) ( x ), ( float ) ( y ) );
			const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
			memcpy( pDestination + index * sizeof( float ), &fValue, sizeof( float ) );
		}
	}
	return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
}

bool Terrain3DErosionJob::compactCompletedResults(
	void ) {
	{
		Terrain3DScopedLock lock( m_statusMutex );
		m_bResultsCompacted = false;
		m_nRetainedResultBytes = 0u;
	}
	t3dErosionJobClearFloatBufferIfZero( m_sedimentCoarse );
	t3dErosionJobClearFloatBufferIfZero( m_sedimentSand );
	t3dErosionJobClearFloatBufferIfZero( m_sedimentFine );
	t3dErosionJobClearFloatBufferIfZero( m_oxidation );
	t3dErosionJobClearFloatBufferIfZero( m_flowAccumulation );
	t3dErosionJobClearFloatBufferIfZero( m_hydraulicDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_windDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_waveDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_weathering );
	t3dErosionJobClearFloatBufferIfZero( m_traffic );
	t3dErosionJobClearFloatBufferIfZero( m_alluvialDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_debrisFlowDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_duneDeposition );
	t3dErosionJobClearFloatBufferIfZero( m_saltWeathering );
	t3dErosionJobClearFloatBufferIfZero( m_wetDryCycles );
	t3dErosionJobClearFloatBufferIfZero( m_snowpack );
	t3dErosionJobClearFloatBufferIfZero( m_exposureAge );
	t3dErosionJobClearFloatBufferIfZero( m_desertPavement );
	t3dErosionJobClearFloatBufferIfZero( m_cumulativeErosion );

	if ( !m_materialTransitionKind.empty() ) {
		bool bHasTransition = false;
		for ( size_t iTransition = 0u; iTransition < ( size_t ) ( ( m_materialTransitionKind ).size() );
			  iTransition++ ) {
			const uint8_t kind = ( m_materialTransitionKind )[iTransition];
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

	t3dErosionResultBuffers_t  result;
	Terrain3DLinearArenaLayout layout;
	bool					   valid = true;
	t3dErosionJobKeepValid( t3dErosionJobPlanResultByte( layout, m_active ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_initialHeight ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_height ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_sedimentCoarse ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_sedimentSand ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_sedimentFine ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_surfaceCoarse ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_surfaceSand ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_surfaceFine ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_oxidation ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_flowAccumulation ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_hydraulicDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_windDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_waveDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_weathering ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_traffic ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_alluvialDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_debrisFlowDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_duneDeposition ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_saltWeathering ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_wetDryCycles ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_snowpack ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_exposureAge ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_desertPavement ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_cumulativeErosion ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultByte( layout, m_dominantTexture ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultByte( layout, m_materialTransitionKind ), valid );
	t3dErosionJobKeepValid( t3dErosionJobPlanResultFloat( layout, m_materialTransitionAmount ), valid );
	if ( !valid || !layout.valid() ) {
		return false;
	}

	m_resultMemory.release();
	if ( !m_resultMemory.ensureCapacity( layout.size() ) ) {
		return false;
	}
	t3dErosionJobKeepValid( t3dErosionJobCopyResultByte( m_resultMemory, m_active, result.m_bActive ), valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_initialHeight, result.m_initialHeight ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_height, result.m_height ), valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_sedimentCoarse, result.m_sedimentCoarse ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_sedimentSand, result.m_sedimentSand ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_sedimentFine, result.m_sedimentFine ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_surfaceCoarse, result.m_surfaceCoarse ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_surfaceSand, result.m_surfaceSand ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_surfaceFine, result.m_surfaceFine ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_oxidation, result.m_oxidation ), valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_flowAccumulation, result.m_flowAccumulation ),
		valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_hydraulicDeposition, result.m_hydraulicDeposition ),
		valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_windDeposition, result.m_windDeposition ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_waveDeposition, result.m_waveDeposition ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_weathering, result.m_weathering ), valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_traffic, result.m_traffic ), valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_alluvialDeposition, result.m_alluvialDeposition ),
		valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_debrisFlowDeposition, result.m_debrisFlowDeposition ),
		valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_duneDeposition, result.m_duneDeposition ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_saltWeathering, result.m_saltWeathering ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_wetDryCycles, result.m_wetDryCycles ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_snowpack, result.m_snowpack ), valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_exposureAge, result.m_exposureAge ),
							valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultFloat( m_resultMemory, m_desertPavement, result.m_desertPavement ),
							valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_cumulativeErosion, result.m_cumulativeErosion ),
		valid );
	t3dErosionJobKeepValid( t3dErosionJobCopyResultByte( m_resultMemory, m_dominantTexture, result.m_dominantTexture ),
							valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultByte( m_resultMemory, m_materialTransitionKind, result.m_materialTransitionKind ),
		valid );
	t3dErosionJobKeepValid(
		t3dErosionJobCopyResultFloat( m_resultMemory, m_materialTransitionAmount, result.m_materialTransitionAmount ),
		valid );
	if ( !valid || m_resultMemory.used() != layout.size() ) {
		m_resultMemory.release();
		return false;
	}

	m_active.swap( result.m_bActive );
	m_initialHeight.swap( result.m_initialHeight );
	m_height.swap( result.m_height );
	m_sedimentCoarse.swap( result.m_sedimentCoarse );
	m_sedimentSand.swap( result.m_sedimentSand );
	m_sedimentFine.swap( result.m_sedimentFine );
	m_surfaceCoarse.swap( result.m_surfaceCoarse );
	m_surfaceSand.swap( result.m_surfaceSand );
	m_surfaceFine.swap( result.m_surfaceFine );
	m_oxidation.swap( result.m_oxidation );
	m_flowAccumulation.swap( result.m_flowAccumulation );
	m_hydraulicDeposition.swap( result.m_hydraulicDeposition );
	m_windDeposition.swap( result.m_windDeposition );
	m_waveDeposition.swap( result.m_waveDeposition );
	m_weathering.swap( result.m_weathering );
	m_traffic.swap( result.m_traffic );
	m_alluvialDeposition.swap( result.m_alluvialDeposition );
	m_debrisFlowDeposition.swap( result.m_debrisFlowDeposition );
	m_duneDeposition.swap( result.m_duneDeposition );
	m_saltWeathering.swap( result.m_saltWeathering );
	m_wetDryCycles.swap( result.m_wetDryCycles );
	m_snowpack.swap( result.m_snowpack );
	m_exposureAge.swap( result.m_exposureAge );
	m_desertPavement.swap( result.m_desertPavement );
	m_cumulativeErosion.swap( result.m_cumulativeErosion );
	m_dominantTexture.swap( result.m_dominantTexture );
	m_materialTransitionKind.swap( result.m_materialTransitionKind );
	m_materialTransitionAmount.swap( result.m_materialTransitionAmount );

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
	for ( size_t iFlux = 0u; iFlux < T3D_ARRAY_COUNT( m_szFlux ); iFlux++ ) {
		FloatBuffer &plane = ( m_szFlux )[iFlux];
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
		Terrain3DScopedLock lock( m_statusMutex );
		m_bResultsCompacted = true;
		m_nRetainedResultBytes = ( uint64_t ) ( m_resultMemory.capacity() );
	}
	return true;
}

uint64_t Terrain3DErosionJob::hashRegionImages(
	const Ref< Image > &rHeight,
	const Ref< Image > &rControl ) const {
	uint64_t hash = T3D_EROSION_FNV_OFFSET;
	if ( rHeight.is_valid() ) {
		t3dErosionJobFnvHashPackedBytes( hash, rHeight->get_data() );
	}
	if ( rControl.is_valid() ) {
		t3dErosionJobFnvHashPackedBytes( hash, rControl->get_data() );
	} else {
		hash ^= ( uint64_t ) ( T3D_REGION_DEFAULT_CONTROL );
		hash *= T3D_EROSION_FNV_PRIME;
	}
	return hash;
}

uint64_t Terrain3DErosionJob::hashTextureProfiles(
	Terrain3DDataAccess *const pDataAccess ) const {
	Terrain3DDataAccess *const pData = pDataAccess;
	if ( !pData ) {
		return 0;
	}
	const Ref< Terrain3DAssets > pAssets = pData->getErosionAssets();
	if ( pAssets.is_null() ) {
		return 0;
	}

	uint64_t hash = T3D_EROSION_FNV_OFFSET;

	for ( int32_t texture_id = 0; texture_id < T3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
		const Ref< Terrain3DTextureAsset > pAsset = pAssets->getTextureAsset( texture_id );
		t3dErosionJobFnvHashU8( hash, pAsset.is_valid() ? 1u : 0u );
		if ( pAsset.is_null() ) {
			continue;
		}
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getId() ) );
		t3dErosionJobFnvHashU8( hash, pAsset->getErosionEnabled() ? 1u : 0u );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionHardness() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionHydraulicErodibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionWindErodibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionWaveErodibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionWeatheringSusceptibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionOxidationSusceptibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionSaltSusceptibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionWetDrySusceptibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionInfiltrationMultiplier() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionTrafficErodibility() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionCohesion() ) );
		t3dErosionJobFnvHashF64( hash, ( double ) ( pAsset->getErosionGrainSize() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionExposedTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionWeatheredTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionOxidizedTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionCoarseDepositTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionFineDepositTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionSaltWeatheredTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionPavementTextureId() ) );
		t3dErosionJobFnvHashI32( hash, ( int32_t ) ( pAsset->getErosionVarnishedTextureId() ) );
	}

	if ( m_config.m_bResolveAutoMaterials ) {
		const Ref< Terrain3DMaterial > pMaterial = pData->getErosionMaterial();
		t3dErosionJobFnvHashU8( hash, ( pMaterial.is_valid() && pMaterial->getAutoShaderEnabled() ) ? 1u : 0u );
		if ( pMaterial.is_valid() ) {
			t3dErosionJobFnvHashI32( hash,
									 ( int32_t ) ( ( int32_t ) ( pMaterial->getShaderParam( "auto_base_texture" ) ) ) );
			t3dErosionJobFnvHashI32(
				hash,
				( int32_t ) ( ( int32_t ) ( pMaterial->getShaderParam( "auto_overlay_texture" ) ) ) );
			t3dErosionJobFnvHashF64( hash, ( double ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_slope" ) ) ) );
			t3dErosionJobFnvHashF64(
				hash,
				( double ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_height_reduction" ) ) ) );
		}
	}
	return hash;
}

bool Terrain3DErosionJob::snapshotTextureProfiles(
	Terrain3DDataAccess *const pDataAccess ) {
	Terrain3DDataAccess *const pData = pDataAccess;
	if ( !pData ) {
		setFailed(
			"Erosion requires Terrain3DData initialized by a Terrain3D node so texture assets can be resolved." );
		return false;
	}
	const Ref< Terrain3DAssets > pAssets = pData->getErosionAssets();
	if ( pAssets.is_null() ) {
		setFailed( "Erosion could not read the Terrain3DAssets resource." );
		return false;
	}

	bool bHasTransition = false;
	for ( int32_t texture_id = 0; texture_id < T3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
		t3dTextureErosionProfile_t		  &profile = m_config.m_textureProfiles[( size_t ) ( texture_id )];
		const Ref< Terrain3DTextureAsset > pAsset = pAssets->getTextureAsset( texture_id );
		if ( pAsset.is_null() ) {
			continue;
		}
		profile.m_bValid = true;
		profile.m_bEnabled = pAsset->getErosionEnabled();
		profile.m_fHardness = ( float ) ( pAsset->getErosionHardness() );
		profile.m_fHydraulicErodibility = ( float ) ( pAsset->getErosionHydraulicErodibility() );
		profile.m_fWindErodibility = ( float ) ( pAsset->getErosionWindErodibility() );
		profile.m_fWaveErodibility = ( float ) ( pAsset->getErosionWaveErodibility() );
		profile.m_fWeatheringSusceptibility = ( float ) ( pAsset->getErosionWeatheringSusceptibility() );
		profile.m_fOxidationSusceptibility = ( float ) ( pAsset->getErosionOxidationSusceptibility() );
		profile.m_fSaltSusceptibility = ( float ) ( pAsset->getErosionSaltSusceptibility() );
		profile.m_fWetDrySusceptibility = ( float ) ( pAsset->getErosionWetDrySusceptibility() );
		profile.m_fInfiltrationMultiplier = ( float ) ( pAsset->getErosionInfiltrationMultiplier() );
		profile.m_fTrafficErodibility = ( float ) ( pAsset->getErosionTrafficErodibility() );
		profile.m_fCohesion = ( float ) ( pAsset->getErosionCohesion() );
		profile.m_fGrainSize = ( float ) ( pAsset->getErosionGrainSize() );
		profile.m_nExposedTextureId = pAsset->getErosionExposedTextureId();
		profile.m_nWeatheredTextureId = pAsset->getErosionWeatheredTextureId();
		profile.m_nOxidizedTextureId = pAsset->getErosionOxidizedTextureId();
		profile.m_nCoarseDepositTextureId = pAsset->getErosionCoarseDepositTextureId();
		profile.m_nFineDepositTextureId = pAsset->getErosionFineDepositTextureId();
		profile.m_nSaltWeatheredTextureId = pAsset->getErosionSaltWeatheredTextureId();
		profile.m_nPavementTextureId = pAsset->getErosionPavementTextureId();
		profile.m_nVarnishedTextureId = pAsset->getErosionVarnishedTextureId();

		const int32_t targets[8] = {
			profile.m_nExposedTextureId,
			profile.m_nWeatheredTextureId,
			profile.m_nOxidizedTextureId,
			profile.m_nCoarseDepositTextureId,
			profile.m_nFineDepositTextureId,
			profile.m_nSaltWeatheredTextureId,
			profile.m_nPavementTextureId,
			profile.m_nVarnishedTextureId,
		};
		for ( size_t iTarget = 0u; iTarget < T3D_ARRAY_COUNT( targets ); iTarget++ ) {
			const int32_t target = ( targets )[iTarget];
			if ( target < 0 ) {
				continue;
			}
			if ( target == texture_id ) {
				setFailed( String( "Texture asset " ) + itos( texture_id ) +
						   String( " contains an erosion transition to itself." ) );
				return false;
			}
			bHasTransition = true;
			if ( pAssets->getTextureAsset( target ).is_null() ) {
				setFailed( String( "Texture asset " ) + itos( texture_id ) +
						   String( " references missing erosion transition texture ID " ) + itos( target ) +
						   String( "." ) );
				return false;
			}
		}
	}

	if ( m_config.m_bResolveAutoMaterials ) {
		const Ref< Terrain3DMaterial > pMaterial = pData->getErosionMaterial();
		if ( pMaterial.is_valid() ) {
			m_config.m_bAutoShaderEnabled = pMaterial->getAutoShaderEnabled();
			m_config.m_nAutoBaseTexture = CLAMP( ( int32_t ) ( pMaterial->getShaderParam( "auto_base_texture" ) ),
												 0,
												 T3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
			m_config.m_nAutoOverlayTexture = CLAMP( ( int32_t ) ( pMaterial->getShaderParam( "auto_overlay_texture" ) ),
													0,
													T3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
			m_config.m_fAutoSlope = ( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_slope" ) ) );
			m_config.m_fAutoHeightReduction =
				( float ) ( ( real_t ) ( pMaterial->getShaderParam( "auto_height_reduction" ) ) );
		}
	}

	if ( m_config.m_bMaterialOutputEnabled && !bHasTransition ) {
		setFailed( "Material output is enabled, but no Terrain3DTextureAsset has an erosion transition texture "
				   "configured." );
		return false;
	}
	m_nAssetProfileHash = hashTextureProfiles( pData );
	return true;
}

bool Terrain3DErosionJob::regionSnapshotLess(
	const t3dRegionSnapshot_t &rA,
	const t3dRegionSnapshot_t &rB ) {
	return rA.m_location.y == rB.m_location.y ? rA.m_location.x < rB.m_location.x : rA.m_location.y < rB.m_location.y;
}

bool Terrain3DErosionJob::captureSourceRegion(
	Terrain3DDataAccess *pData,
	const Vector2i		&location,
	const int32_t		 region_id ) {
	const size_t texels = ( size_t ) ( T3D_REGION_SIZE ) * ( size_t ) ( T3D_REGION_SIZE );
	const size_t expected_height_bytes = texels * sizeof( uint16_t );
	const size_t expected_rf_bytes = texels * sizeof( float );

	if ( !pData->ensureCpuRegionResident( location ) ) {
		setFailed( "A terrain region could not be paged into the CPU map cache for erosion capture." );
		return false;
	}
	const Ref< Terrain3DRegion > pRegion = pData->getRegion( location );
	if ( pRegion.is_null() ) {
		setFailed( "An active terrain region disappeared while creating the erosion source snapshot." );
		return false;
	}
	const Ref< Image > pHeightMap = pRegion->getHeightMap();
	const Ref< Image > pControlMap = pRegion->getControlMap();
	if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
		 pHeightMap->get_width() != T3D_REGION_SIZE || pHeightMap->get_height() != T3D_REGION_SIZE ||
		 ( pControlMap.is_valid() &&
		   ( pControlMap->get_format() != Image::FORMAT_RF || pControlMap->get_width() != T3D_REGION_SIZE ||
			 pControlMap->get_height() != T3D_REGION_SIZE ) ) ) {
		setFailed( "Erosion requires compact R16 height maps and optional RF control maps." );
		return false;
	}

	const PackedByteArray encoded_height = pHeightMap->get_data();
	if ( ( size_t ) ( encoded_height.size() ) < expected_height_bytes ) {
		setFailed( "A Terrain3D region contains truncated R16 height data." );
		return false;
	}
	t3dRegionSnapshot_t &snapshot = m_regions[( size_t ) ( region_id )];
	snapshot.m_location = location;
	snapshot.m_nSourceOrder = region_id;
	snapshot.m_heightData.resize( ( int64_t ) ( expected_rf_bytes ) );
	uint8_t *const	pHeightDst = snapshot.m_heightData.ptrw();
	const uint8_t  *pHeightSrc = encoded_height.ptr();
	PackedByteArray control_source;
	const uint8_t  *pControlSrc = NULL;
	if ( pControlMap.is_valid() ) {
		snapshot.m_controlData.resize( ( int64_t ) ( expected_rf_bytes ) );
		control_source = pControlMap->get_data();
		if ( ( size_t ) ( control_source.size() ) < expected_rf_bytes ) {
			setFailed( "A Terrain3D region contains truncated control data." );
			return false;
		}
		pControlSrc = control_source.ptr();
		memcpy( snapshot.m_controlData.ptrw(), pControlSrc, expected_rf_bytes );
	} else {
		snapshot.m_controlData.clear();
	}
	for ( size_t i = 0; i < texels; ++i ) {
		t3dErosionJobWriteRf( pHeightDst,
							  i,
							  t3dRegionCodecReadHeightBytes( pHeightSrc, ( int ) ( i ), Image::FORMAT_R16 ) );
	}
	snapshot.m_nSourceHash = hashRegionImages( pHeightMap, pControlMap );
	const int32_t map_index = Terrain3DData::getRegionMapIndex( location );
	if ( map_index >= 0 ) {
		m_szRegionLookup[map_index] = ( int16_t ) ( region_id );
	}
	return true;
}

void Terrain3DErosionJob::indexSourceRegions(
	void ) {
	// Keep neighboring regions adjacent in every solver plane. Active region
	// order is editor/history dependent and otherwise turns cross-boundary reads
	// into arbitrary jumps through the working set.
	eastl::sort( m_regions.begin(), m_regions.end(), regionSnapshotLess );
	memset( m_szRegionLookup, 0xff, sizeof( m_szRegionLookup ) );
	for ( int32_t region_id = 0; region_id < ( int32_t ) ( m_regions.size() ); ++region_id ) {
		const int32_t map_index = Terrain3DData::getRegionMapIndex( m_regions[( size_t ) ( region_id )].m_location );
		if ( map_index >= 0 ) {
			m_szRegionLookup[map_index] = ( int16_t ) ( region_id );
		}
	}
	m_szRegionNeighbors.resize( m_regions.size() );
	for ( int32_t region_id = 0; region_id < ( int32_t ) ( m_regions.size() ); ++region_id ) {
		const Vector2i location = m_regions[( size_t ) ( region_id )].m_location;
		int16_t *const neighbors = m_szRegionNeighbors[( size_t ) ( region_id )].m_ids;
		for ( int32_t direction = 0; direction < T3D_EROSION_DIRECTION_COUNT; ++direction ) {
			const int32_t map_index = Terrain3DData::getRegionMapIndex(
				location + Vector2i( s_szErosionDirectionX[direction], s_szErosionDirectionY[direction] ) );
			neighbors[( size_t ) ( direction )] = ( int16_t ) ( map_index >= 0 ? m_szRegionLookup[map_index] : -1 );
		}
	}
}

bool Terrain3DErosionJob::captureSourceRegions(
	Terrain3DDataAccess *const pDataAccess ) {
	Terrain3DDataAccess *const pData = pDataAccess;
	if ( !pData ) {
		setFailed( "Erosion source capture requires valid Terrain3DData." );
		return false;
	}
	const TypedArray< Vector2i > locations = pData->getRegionLocations();
	m_regions.clear();
	m_regions.resize( ( size_t ) ( locations.size() ) );
	memset( m_szRegionLookup, 0xff, sizeof( m_szRegionLookup ) );

	for ( int32_t region_id = 0; region_id < locations.size(); ++region_id ) {
		if ( !captureSourceRegion( pData, locations[region_id], region_id ) ) {
			return false;
		}
	}
	indexSourceRegions();
	return true;
}

void Terrain3DErosionJob::releaseSourceSnapshots(
	void ) {
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_regions ).size() ); iRegion++ ) {
		t3dRegionSnapshot_t &snapshot = ( m_regions )[iRegion];
		snapshot.m_heightData.clear();
		snapshot.m_controlData.clear();
	}
	m_simulationMaskSnapshot.clear();
	m_trafficMaskSnapshot.clear();
	m_salinityMaskSnapshot.clear();
	m_surfaceLayerDepthSnapshot.clear();
	m_bedrockHardnessSnapshot.clear();
}

float Terrain3DErosionJob::sampleImageSnapshot(
	const t3dImageSnapshot_t &rImage,
	const Rect2				 &rWorldRect,
	const Vector2			 &rWorldPosition ) {
	if ( !rImage.isValid() || rWorldRect.size.x <= s_fErosionEpsilon || rWorldRect.size.y <= s_fErosionEpsilon ) {
		return 1.0f;
	}
	const Vector2 uv = ( rWorldPosition - rWorldRect.position ) / rWorldRect.size;
	if ( uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f ) {
		return 0.0f;
	}
	const float	   fPx = uv.x * ( float ) ( MAX( 1, rImage.m_size.x - 1 ) );
	const float	   fPy = uv.y * ( float ) ( MAX( 1, rImage.m_size.y - 1 ) );
	const int32_t  x0 = CLAMP( ( int32_t ) ( Math::floor( fPx ) ), 0, rImage.m_size.x - 1 );
	const int32_t  y0 = CLAMP( ( int32_t ) ( Math::floor( fPy ) ), 0, rImage.m_size.y - 1 );
	const int32_t  x1 = MIN( x0 + 1, rImage.m_size.x - 1 );
	const int32_t  y1 = MIN( y0 + 1, rImage.m_size.y - 1 );
	const float	   fFx = fPx - ( float ) ( x0 );
	const float	   fFy = fPy - ( float ) ( y0 );
	const uint8_t *pBytes = rImage.m_rfData.ptr();
	const size_t   stride = ( size_t ) ( rImage.m_size.x );
	const float	   fV00 = t3dErosionJobReadRf( pBytes, ( size_t ) ( y0 ) *stride + ( size_t ) ( x0 ) );
	const float	   fV10 = t3dErosionJobReadRf( pBytes, ( size_t ) ( y0 ) *stride + ( size_t ) ( x1 ) );
	const float	   fV01 = t3dErosionJobReadRf( pBytes, ( size_t ) ( y1 ) *stride + ( size_t ) ( x0 ) );
	const float	   fV11 = t3dErosionJobReadRf( pBytes, ( size_t ) ( y1 ) *stride + ( size_t ) ( x1 ) );
	return t3dErosionJobClamp01( Math::lerp( Math::lerp( fV00, fV10, fFx ), Math::lerp( fV01, fV11, fFx ), fFy ) );
}

bool Terrain3DErosionJob::buildSimulationSnapshot(
	void ) {
	const int32_t region_count = ( int32_t ) ( m_regions.size() );
	if ( region_count <= 0 ) {
		setFailed( "Erosion source snapshot contains no active regions." );
		return false;
	}

	Vector2i minimumLocation( INT32_MAX, INT32_MAX );
	Vector2i maximumLocation( INT32_MIN, INT32_MIN );
	for ( int32_t region_id = 0; region_id < region_count; ++region_id ) {
		const Vector2i location = m_regions[( size_t ) ( region_id )].m_location;
		minimumLocation.x = MIN( minimumLocation.x, location.x );
		minimumLocation.y = MIN( minimumLocation.y, location.y );
		maximumLocation.x = MAX( maximumLocation.x, location.x );
		maximumLocation.y = MAX( maximumLocation.y, location.y );
	}

	const Vector2 world_min =
		Vector2( minimumLocation.x, minimumLocation.y ) * ( float ) ( T3D_REGION_SIZE ) *m_fVertexSpacing;
	const Vector2 world_max =
		Vector2( maximumLocation.x + 1, maximumLocation.y + 1 ) * ( float ) ( T3D_REGION_SIZE ) *m_fVertexSpacing;
	m_terrainWorldRect = Rect2( world_min, world_max - world_min );
	if ( m_config.m_simulationMaskWorldRect.size.x <= s_fErosionEpsilon ||
		 m_config.m_simulationMaskWorldRect.size.y <= s_fErosionEpsilon ) {
		m_config.m_simulationMaskWorldRect = m_terrainWorldRect;
	}
	if ( m_config.m_trafficWorldRect.size.x <= s_fErosionEpsilon ||
		 m_config.m_trafficWorldRect.size.y <= s_fErosionEpsilon ) {
		m_config.m_trafficWorldRect = m_terrainWorldRect;
	}
	if ( m_config.m_salinityWorldRect.size.x <= s_fErosionEpsilon ||
		 m_config.m_salinityWorldRect.size.y <= s_fErosionEpsilon ) {
		m_config.m_salinityWorldRect = m_terrainWorldRect;
	}
	if ( m_config.m_surfaceLayerDepthWorldRect.size.x <= s_fErosionEpsilon ||
		 m_config.m_surfaceLayerDepthWorldRect.size.y <= s_fErosionEpsilon ) {
		m_config.m_surfaceLayerDepthWorldRect = m_terrainWorldRect;
	}
	if ( m_config.m_bedrockHardnessWorldRect.size.x <= s_fErosionEpsilon ||
		 m_config.m_bedrockHardnessWorldRect.size.y <= s_fErosionEpsilon ) {
		m_config.m_bedrockHardnessWorldRect = m_terrainWorldRect;
	}

	const t3dImageSnapshot_t &simulation_mask_image = m_simulationMaskSnapshot;
	const t3dImageSnapshot_t &traffic_image = m_trafficMaskSnapshot;
	const t3dImageSnapshot_t &salinity_image = m_salinityMaskSnapshot;
	const t3dImageSnapshot_t &surface_depth_image = m_surfaceLayerDepthSnapshot;
	const t3dImageSnapshot_t &bedrock_hardness_image = m_bedrockHardnessSnapshot;

	for ( int32_t region_id = 0; region_id < region_count; ++region_id ) {
		if ( m_bCancelRequested.load() ) {
			return false;
		}
		const t3dRegionSnapshot_t &snapshot = m_regions[( size_t ) ( region_id )];
		const uint8_t			  *pHeightBytes = snapshot.m_heightData.ptr();
		const uint8_t *pControlBytes = snapshot.m_controlData.is_empty() ? NULL : snapshot.m_controlData.ptr();

		for ( int32_t sy = 0; sy < m_nSimulationRegionSize; ++sy ) {
			for ( int32_t sx = 0; sx < m_nSimulationRegionSize; ++sx ) {
				double	fHeightSum = 0.0;
				int32_t valid_count = 0;
				int32_t hole_count = 0;
				for ( int32_t oy = 0; oy < m_nResolutionScale; ++oy ) {
					const int32_t source_y = sy * m_nResolutionScale + oy;
					for ( int32_t ox = 0; ox < m_nResolutionScale; ++ox ) {
						const int32_t source_x = sx * m_nResolutionScale + ox;
						const size_t  source_index =
							( size_t ) ( source_y ) * ( size_t ) ( T3D_REGION_SIZE ) + ( size_t ) ( source_x );
						const float	   fSourceHeight = t3dErosionJobReadRf( pHeightBytes, source_index );
						const uint32_t control = pControlBytes
													 ? t3dAsUint( t3dErosionJobReadRf( pControlBytes, source_index ) )
													 : T3D_REGION_DEFAULT_CONTROL;
						if ( t3dIsHole( control ) ) {
							hole_count += 1;
						}
						if ( !isnan( fSourceHeight ) ) {
							fHeightSum += fSourceHeight;
							valid_count += 1;
						}
					}
				}

				size_t index = ( size_t ) ( region_id ) *m_nCellsPerRegion +
							   ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
				bool bActive = valid_count > 0;
				if ( m_config.m_bPreserveHoles && hole_count * 2 >= m_nResolutionScale * m_nResolutionScale ) {
					bActive = false;
				}
				m_active[index] = bActive ? ( uint8_t ) ( 1 ) : ( uint8_t ) ( 0 );
				const float fValue = valid_count > 0 ? ( float ) ( fHeightSum / ( double ) ( valid_count ) ) : 0.0f;
				m_initialHeight[index] = fValue;
				m_height[index] = fValue;

				const Vector2i region_location = m_regions[( size_t ) ( region_id )].m_location;
				const Vector2  world_position =
					( Vector2(
						region_location.x * T3D_REGION_SIZE + sx * m_nResolutionScale + m_nResolutionScale * 0.5f,
						region_location.y * T3D_REGION_SIZE + sy * m_nResolutionScale + m_nResolutionScale * 0.5f ) ) *
					m_fVertexSpacing;

				float fSimulationWeight =
					sampleImageSnapshot( simulation_mask_image, m_config.m_simulationMaskWorldRect, world_position );
				if ( m_config.m_bInvertSimulationMask && simulation_mask_image.isValid() ) {
					fSimulationWeight = 1.0f - fSimulationWeight;
				}
				if ( m_config.m_bHeightRangeEnabled ) {
					const float fMinimumHeight = MIN( m_config.m_fMinimumHeightM, m_config.m_fMaximumHeightM );
					const float fMaximumHeight = MAX( m_config.m_fMinimumHeightM, m_config.m_fMaximumHeightM );
					float		fRangeWeight = 1.0f;
					if ( fValue < fMinimumHeight ) {
						fRangeWeight = m_config.m_fHeightRangeFalloffM > s_fErosionEpsilon
										   ? t3dErosionJobClamp01(
												 ( fValue - ( fMinimumHeight - m_config.m_fHeightRangeFalloffM ) ) /
												 m_config.m_fHeightRangeFalloffM )
										   : 0.0f;
					} else if ( fValue > fMaximumHeight ) {
						fRangeWeight = m_config.m_fHeightRangeFalloffM > s_fErosionEpsilon
										   ? t3dErosionJobClamp01(
												 ( ( fMaximumHeight + m_config.m_fHeightRangeFalloffM ) - fValue ) /
												 m_config.m_fHeightRangeFalloffM )
										   : 0.0f;
					}
					fSimulationWeight *= fRangeWeight;
				}
				if ( m_config.m_fEdgeFadeDistanceM > s_fErosionEpsilon ) {
					const float fLeft = world_position.x - m_terrainWorldRect.position.x;
					const float fRight = m_terrainWorldRect.get_end().x - world_position.x;
					const float fTop = world_position.y - m_terrainWorldRect.position.y;
					const float fBottom = m_terrainWorldRect.get_end().y - world_position.y;
					const float fEdgeDistance = MIN( MIN( fLeft, fRight ), MIN( fTop, fBottom ) );
					fSimulationWeight *= t3dErosionJobClamp01( fEdgeDistance / m_config.m_fEdgeFadeDistanceM );
				}
				m_simulationWeight[index] = t3dErosionJobClamp01( fSimulationWeight );

				if ( m_config.m_bTrafficEnabled && traffic_image.isValid() ) {
					const float fSampled =
						sampleImageSnapshot( traffic_image, m_config.m_trafficWorldRect, world_position );
					m_traffic[index] =
						fSampled > m_config.m_fTrafficThreshold
							? t3dErosionJobClamp01( ( fSampled - m_config.m_fTrafficThreshold ) /
													MAX( 1.0f - m_config.m_fTrafficThreshold, s_fErosionEpsilon ) )
							: 0.0f;
				}
				if ( m_config.m_bSaltWeatheringEnabled ) {
					const float fMappedSalinity =
						salinity_image.isValid()
							? sampleImageSnapshot( salinity_image, m_config.m_salinityWorldRect, world_position )
							: 0.0f;
					m_salinity[index] = t3dErosionJobClamp01(
						m_config.m_fBackgroundSalinity + fMappedSalinity * ( 1.0f - m_config.m_fBackgroundSalinity ) );
				}
				if ( m_config.m_bStratigraphyEnabled ) {
					const float fDepthFactor = surface_depth_image.isValid()
												   ? sampleImageSnapshot( surface_depth_image,
																		  m_config.m_surfaceLayerDepthWorldRect,
																		  world_position )
												   : 1.0f;
					m_surfaceLayerDepth[index] = MAX( 0.0f, m_config.m_fSurfaceLayerDepthM * fDepthFactor );
					m_bedrockHardness[index] =
						bedrock_hardness_image.isValid()
							? t3dErosionJobClamp01( sampleImageSnapshot( bedrock_hardness_image,
																		 m_config.m_bedrockHardnessWorldRect,
																		 world_position ) )
							: m_config.m_fBedrockHardness;
				}
			}
		}
		m_fProgress.store( 0.02f + 0.08f * ( float ) ( region_id + 1 ) / ( float ) ( MAX( 1, region_count ) ) );
	}

	if ( m_config.m_bTrafficEnabled && m_config.m_nTrafficWideningCells > 0 ) {
		for ( int32_t pass = 0; pass < m_config.m_nTrafficWideningCells; ++pass ) {
			for ( size_t index = 0; index < m_nCellCount; ++index ) {
				float fValue = m_traffic[index];
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor =
						neighborIndex( index, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
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
		const PackedByteArray &control_data = m_regions[( size_t ) ( region_id )].m_controlData;
		const uint8_t		  *pControlBytes = control_data.is_empty() ? NULL : control_data.ptr();

		for ( int32_t sy = 0; sy < m_nSimulationRegionSize; ++sy ) {
			for ( int32_t sx = 0; sx < m_nSimulationRegionSize; ++sx ) {
				const size_t index = ( size_t ) ( region_id ) *m_nCellsPerRegion +
									 ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
				double weights[T3D_EROSION_TEXTURE_PROFILE_COUNT];
				memset( weights, 0, sizeof( weights ) );
				const float fSlope = getSlope( index );
				const float fNormalY = 1.0f / Math::sqrt( 1.0f + fSlope * fSlope );
				const float fAutoBlend =
					t3dErosionJobClamp01( ( m_config.m_fAutoSlope * 2.0f * ( fNormalY - 1.0f ) + 1.0f ) -
										  m_config.m_fAutoHeightReduction * 0.01f * m_height[index] );

				for ( int32_t oy = 0; oy < m_nResolutionScale; ++oy ) {
					const int32_t source_y = sy * m_nResolutionScale + oy;
					for ( int32_t ox = 0; ox < m_nResolutionScale; ++ox ) {
						const int32_t source_x = sx * m_nResolutionScale + ox;
						const size_t  source_index =
							( size_t ) ( source_y ) * ( size_t ) ( T3D_REGION_SIZE ) + ( size_t ) ( source_x );
						const uint32_t control = pControlBytes
													 ? t3dAsUint( t3dErosionJobReadRf( pControlBytes, source_index ) )
													 : T3D_REGION_DEFAULT_CONTROL;
						if ( t3dIsHole( control ) ) {
							continue;
						}
						int32_t base = ( int32_t ) ( t3dGetBase( control ) );
						int32_t overlay = ( int32_t ) ( t3dGetOverlay( control ) );
						float	fBlend = ( float ) ( t3dGetBlend( control ) ) / s_fByteMaxFloat;
						if ( m_config.m_bResolveAutoMaterials && m_config.m_bAutoShaderEnabled &&
							 t3dIsAuto( control ) ) {
							base = m_config.m_nAutoBaseTexture;
							overlay = m_config.m_nAutoOverlayTexture;
							fBlend = fAutoBlend;
						}
						base = CLAMP( base, 0, T3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
						overlay = CLAMP( overlay, 0, T3D_EROSION_TEXTURE_PROFILE_COUNT - 1 );
						weights[( size_t ) ( base )] += ( double ) ( 1.0f - fBlend );
						weights[( size_t ) ( overlay )] += ( double ) ( fBlend );
					}
				}

				double fTotalWeight = 0.0;
				for ( int32_t texture_id = 0; texture_id < T3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
					fTotalWeight += weights[( size_t ) ( texture_id )];
				}
				int32_t dominant = 0;
				for ( int32_t texture_id = 1; texture_id < T3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
					if ( weights[( size_t ) ( texture_id )] > weights[( size_t ) ( dominant )] ) {
						dominant = texture_id;
					}
				}
				if ( !m_dominantTexture.empty() ) {
					m_dominantTexture[index] = ( uint8_t ) ( dominant );
				}
				if ( fTotalWeight <= s_fErosionEpsilon ) {
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
				for ( int32_t texture_id = 0; texture_id < T3D_EROSION_TEXTURE_PROFILE_COUNT; ++texture_id ) {
					const double fWeight = weights[( size_t ) ( texture_id )] / fTotalWeight;
					if ( fWeight <= 0.0 ) {
						continue;
					}
					const t3dTextureErosionProfile_t &profile = m_config.m_textureProfiles[( size_t ) ( texture_id )];
					const float						  fProfileHardness =
						  profile.m_fHardness >= 0.0f ? profile.m_fHardness : m_config.m_fBaseHardness;
					const bool enabled = profile.m_bValid ? profile.m_bEnabled : true;
					hardness += fWeight * ( double ) ( enabled ? fProfileHardness : 1.0f );
					fHydraulic += fWeight * ( double ) ( enabled ? profile.m_fHydraulicErodibility : 0.0f );
					fWind += fWeight * ( double ) ( enabled ? profile.m_fWindErodibility : 0.0f );
					fWave += fWeight * ( double ) ( enabled ? profile.m_fWaveErodibility : 0.0f );
					fWeathering += fWeight * ( double ) ( enabled ? profile.m_fWeatheringSusceptibility : 0.0f );
					fOxidation += fWeight * ( double ) ( enabled ? profile.m_fOxidationSusceptibility : 0.0f );
					fSalt += fWeight * ( double ) ( enabled ? profile.m_fSaltSusceptibility : 0.0f );
					fWetDry += fWeight * ( double ) ( enabled ? profile.m_fWetDrySusceptibility : 0.0f );
					fInfiltration += fWeight * ( double ) ( profile.m_fInfiltrationMultiplier );
					fTraffic += fWeight * ( double ) ( enabled ? profile.m_fTrafficErodibility : 0.0f );
					cohesion += fWeight * ( double ) ( enabled ? profile.m_fCohesion : 1.0f );
					grain_size += fWeight * ( double ) ( profile.m_fGrainSize );
				}

				const float fHardnessNoise =
					( t3dErosionJobHashUnit( m_config.m_nSeed, stochasticIndex( index ) ) * 2.0f - 1.0f ) *
					m_config.m_fHardnessVariation;
				if ( !m_substrateHardness.empty() ) {
					m_substrateHardness[index] = t3dErosionJobClamp01( ( float ) ( hardness ) + fHardnessNoise );
				}
				if ( !m_hardness.empty() ) {
					m_hardness[index] = !m_substrateHardness.empty()
											? m_substrateHardness[index]
											: t3dErosionJobClamp01( ( float ) ( hardness ) + fHardnessNoise );
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
					m_cohesion[index] = t3dErosionJobClamp01( ( float ) ( cohesion ) );
				}
				if ( !m_grainSize.empty() ) {
					m_grainSize[index] = t3dErosionJobClamp01( ( float ) ( grain_size ) );
				}
			}
		}
	}
	return true;
}

bool Terrain3DErosionJob::captureImageSnapshot(
	const Ref< Image > &rImage,
	t3dImageSnapshot_t &rSnapshot,
	const char		   *pszLabel ) {
	rSnapshot.clear();
	if ( rImage.is_null() ) {
		return true;
	}
	Ref< Image > pCopy = Image::create_from_data( rImage->get_width(),
												  rImage->get_height(),
												  rImage->has_mipmaps(),
												  rImage->get_format(),
												  rImage->get_data() );
	if ( pCopy.is_null() || pCopy->is_empty() ) {
		setFailed( String( "Failed to snapshot erosion input image: " ) + String( pszLabel ) + String( "." ) );
		return false;
	}
	pCopy->clear_mipmaps();
	if ( pCopy->get_format() != Image::FORMAT_RF ) {
		pCopy->convert( Image::FORMAT_RF );
	}
	rSnapshot.m_size = pCopy->get_size();
	rSnapshot.m_rfData = pCopy->get_data();
	if ( !rSnapshot.isValid() ) {
		setFailed( String( "Erosion input image produced invalid RF data: " ) + String( pszLabel ) + String( "." ) );
		return false;
	}
	return true;
}

Error Terrain3DErosionJob::configureSimulationGrid(
	Terrain3DDataAccess *pData ) {
	TypedArray< Vector2i >		 locations = pData->getRegionLocations();
	const Ref< Terrain3DRegion > pFirstRegion = pData->getRegion( Vector2i( locations[0] ) );
	if ( pFirstRegion.is_null() ) {
		setFailed( "Terrain3D's first active region could not be read." );
		return ERR_DOES_NOT_EXIST;
	}
	m_fVertexSpacing = pFirstRegion->getVertexSpacing();
	m_nSourceControlSnapshotBytes = 0;
	const uint64_t source_texel_count = ( uint64_t ) ( T3D_REGION_SIZE ) * ( uint64_t ) ( T3D_REGION_SIZE );
	const uint64_t control_bytes_per_region = source_texel_count * sizeof( float );
	for ( int32_t i = 0; i < locations.size(); ++i ) {
		const Vector2i location = Vector2i( locations[i] );
		if ( !pData->ensureCpuRegionResident( location ) ) {
			setFailed( "A terrain region could not be paged into the CPU map cache during erosion preflight." );
			return ERR_CANT_OPEN;
		}
		const Ref< Terrain3DRegion > pRegion = pData->getRegion( location );
		if ( pRegion.is_valid() && pRegion->getControlMap().is_valid() ) {
			if ( m_nSourceControlSnapshotBytes > eastl::numeric_limits< uint64_t >::max() - control_bytes_per_region ) {
				setFailed( "Erosion control snapshot dimensions overflow the memory preflight." );
				return ERR_OUT_OF_MEMORY;
			}
			m_nSourceControlSnapshotBytes += control_bytes_per_region;
		}
	}
	m_nResolutionScale = selectResolutionScale( locations.size() );
	if ( T3D_REGION_SIZE % m_nResolutionScale != 0 ) {
		setFailed( "The selected erosion resolution scale does not divide the Terrain3D region size." );
		return ERR_INVALID_DATA;
	}
	const uint64_t budget = ( uint64_t ) ( m_config.m_nMemoryBudgetMb ) * 1024ULL * 1024ULL;
	if ( estimateMemoryBytes( locations.size(), m_nResolutionScale ) > budget ) {
		setFailed( "The erosion working set exceeds memory_budget_mb even at the maximum supported resolution scale." );
		return ERR_OUT_OF_MEMORY;
	}

	m_nSimulationRegionSize = T3D_REGION_SIZE / m_nResolutionScale;
	if ( m_nSimulationRegionSize <= 0 || ( m_nSimulationRegionSize & ( m_nSimulationRegionSize - 1 ) ) != 0 ) {
		setFailed( "Erosion simulation regions must remain power-of-two for cache-local indexing." );
		return ERR_INVALID_DATA;
	}
	m_nSimulationRegionShift = ( int32_t ) ( t3dCountrZeroU32( ( uint32_t ) ( m_nSimulationRegionSize ) ) );
	m_nSimulationRegionMask = m_nSimulationRegionSize - 1;
	m_fCellSize = m_fVertexSpacing * ( float ) ( m_nResolutionScale );
	m_nCellsPerRegion = ( size_t ) ( m_nSimulationRegionSize ) * ( size_t ) ( m_nSimulationRegionSize );
	m_nCellsPerRegionShift = m_nSimulationRegionShift * 2;
	m_nCellsPerRegionMask = m_nCellsPerRegion - 1u;
	m_nCellCount = ( size_t ) ( locations.size() ) * m_nCellsPerRegion;

	return OK;
}

Error Terrain3DErosionJob::setup(
	Terrain3DDataAccess *const			   pDataAccess,
	const Ref< Terrain3DErosionSettings > &rSettings ) {
	Terrain3DDataAccess *const pData = pDataAccess;
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

	const Ref< Image > pSimulationMask = rSettings->getSimulationMask();
	const Ref< Image > pTrafficMask = rSettings->getTrafficMask();
	const Ref< Image > pSalinityMask = rSettings->getSalinityMask();
	const Ref< Image > pSurfaceLayerDepthMap = rSettings->getSurfaceLayerDepthMap();
	const Ref< Image > pBedrockHardnessMap = rSettings->getBedrockHardnessMap();
	if ( pSimulationMask.is_valid() && pSimulationMask->is_empty() ) {
		setFailed( "simulation_mask is assigned but empty." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.m_bHeightRangeEnabled &&
		 Math::is_equal_approx( m_config.m_fMinimumHeightM, m_config.m_fMaximumHeightM ) &&
		 m_config.m_fHeightRangeFalloffM <= s_fErosionEpsilon ) {
		setFailed( "Height-range limiting has zero width and no falloff." );
		return ERR_INVALID_PARAMETER;
	}

	if ( m_config.m_bTrafficEnabled ) {
		if ( pTrafficMask.is_null() || pTrafficMask->is_empty() ) {
			setFailed( "Foot traffic simulation is enabled, but traffic_mask is empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.m_bSaltWeatheringEnabled ) {
		if ( pSalinityMask.is_valid() && pSalinityMask->is_empty() ) {
			setFailed( "salinity_mask is assigned but empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.m_bStratigraphyEnabled ) {
		if ( ( pSurfaceLayerDepthMap.is_valid() && pSurfaceLayerDepthMap->is_empty() ) ||
			 ( pBedrockHardnessMap.is_valid() && pBedrockHardnessMap->is_empty() ) ) {
			setFailed( "A stratigraphy input map is assigned but empty." );
			return ERR_INVALID_PARAMETER;
		}
	}
	if ( m_config.m_bWetDryEnabled && m_config.m_fDryThreshold >= m_config.m_fWetThreshold ) {
		setFailed( "dry_threshold must be lower than wet_threshold for wet-dry hysteresis." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.m_bSnowmeltEnabled && m_config.m_fSnowMeltTemperature <= m_config.m_fSnowAccumulationTemperature ) {
		setFailed( "snow_melt_temperature must be greater than snow_accumulation_temperature." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.m_bWindEnabled && m_config.m_windDirection.length_squared() <= s_fErosionEpsilon ) {
		setFailed( "wind_direction must be non-zero when wind erosion is enabled." );
		return ERR_INVALID_PARAMETER;
	}
	if ( m_config.m_bWaveEnabled && m_config.m_waveDirection.length_squared() <= s_fErosionEpsilon ) {
		setFailed( "wave_direction must be non-zero when wave erosion is enabled." );
		return ERR_INVALID_PARAMETER;
	}
	m_nInputSnapshotBytes = 0;

	if ( !t3dErosionJobAddImageSnapshotBytes( m_nInputSnapshotBytes, pSimulationMask ) ||
		 !t3dErosionJobAddImageSnapshotBytes( m_nInputSnapshotBytes, pTrafficMask ) ||
		 !t3dErosionJobAddImageSnapshotBytes( m_nInputSnapshotBytes, pSalinityMask ) ||
		 !t3dErosionJobAddImageSnapshotBytes( m_nInputSnapshotBytes, pSurfaceLayerDepthMap ) ||
		 !t3dErosionJobAddImageSnapshotBytes( m_nInputSnapshotBytes, pBedrockHardnessMap ) ) {
		setFailed( "Erosion input image dimensions overflow the memory preflight." );
		return ERR_OUT_OF_MEMORY;
	}

	const Error gridError = configureSimulationGrid( pData );
	if ( gridError != OK ) {
		return gridError;
	}

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

int64_t Terrain3DErosionJob::neighborIndex(
	const size_t  iIndex,
	const int32_t nDx,
	const int32_t nDy ) const {
	const size_t local_index = iIndex & m_nCellsPerRegionMask;
	int32_t		 x = 0;
	int32_t		 y = 0;
	int32_t		 direction = -1;
	if ( nDx != 0 ) {
		x = ( int32_t ) ( local_index & ( size_t ) ( m_nSimulationRegionMask ) );
		if ( nDx < 0 ) {
			if ( x > 0 ) {
				return ( int64_t ) ( iIndex ) -1;
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
	const int32_t neighbor_region_id = m_szRegionNeighbors[( size_t ) ( region_id )].m_ids[( size_t ) ( direction )];
	if ( neighbor_region_id < 0 ) {
		return -1;
	}
	return ( int64_t ) ( ( size_t ) ( neighbor_region_id ) *m_nCellsPerRegion +
						 ( size_t ) ( y ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( x ) );
}

uint64_t Terrain3DErosionJob::stochasticIndex(
	const size_t iIndex ) const {
	const size_t region_id = iIndex >> m_nCellsPerRegionShift;
	const size_t local_index = iIndex & m_nCellsPerRegionMask;
	return ( uint64_t ) ( m_regions[region_id].m_nSourceOrder ) * ( uint64_t ) ( m_nCellsPerRegion ) +
		   ( uint64_t ) ( local_index );
}

void Terrain3DErosionJob::partitionMaterial(
	const float fGrainSize,
	const float fAmount,
	float	   &fCoarse,
	float	   &fSand,
	float	   &fFine ) const {
	const float fGrain = t3dErosionJobClamp01( fGrainSize );
	const float fFineWeight = t3dErosionJobClamp01( 1.0f - fGrain * 2.0f );
	const float fCoarseWeight = t3dErosionJobClamp01( fGrain * 2.0f - 1.0f );
	const float fSandWeight = MAX( 0.0f, 1.0f - fFineWeight - fCoarseWeight );
	fCoarse = fAmount * fCoarseWeight;
	fSand = fAmount * fSandWeight;
	fFine = fAmount * fFineWeight;
}

float Terrain3DErosionJob::getLooseMaterial(
	const size_t iIndex ) const {
	if ( m_surfaceCoarse.empty() ) {
		return 0.0f;
	}
	return MAX( 0.0f, m_surfaceCoarse[iIndex] + m_surfaceSand[iIndex] + m_surfaceFine[iIndex] );
}

float Terrain3DErosionJob::getSediment(
	const size_t iIndex ) const {
	if ( m_sedimentCoarse.empty() ) {
		return 0.0f;
	}
	return MAX( 0.0f, m_sedimentCoarse[iIndex] + m_sedimentSand[iIndex] + m_sedimentFine[iIndex] );
}

float Terrain3DErosionJob::entrainSurface(
	size_t		iIndex,
	const float fAmount,
	float	   &fCoarse,
	float	   &fSand,
	float	   &fFine ) {
	fCoarse = 0.0f;
	fSand = 0.0f;
	fFine = 0.0f;
	float fRemaining = MAX( 0.0f, fAmount );
	if ( fRemaining <= s_fErosionEpsilon ) {
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

	if ( fRemaining > s_fErosionEpsilon ) {
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

void Terrain3DErosionJob::depositSurface(
	size_t		iIndex,
	const float fCoarse,
	const float fSand,
	const float fFine ) {
	m_surfaceCoarse[iIndex] += MAX( 0.0f, fCoarse );
	m_surfaceSand[iIndex] += MAX( 0.0f, fSand );
	m_surfaceFine[iIndex] += MAX( 0.0f, fFine );
}

float Terrain3DErosionJob::getSlope(
	const size_t iIndex ) const {
	if ( !m_active[iIndex] ) {
		return 0.0f;
	}
	float fMaximumDifference = 0.0f;
	for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
		const int64_t neighbor = neighborIndex( iIndex, s_szErosionDirectionX[dir], s_szErosionDirectionY[dir] );
		if ( neighbor >= 0 && m_active[( size_t ) ( neighbor )] ) {
			fMaximumDifference =
				MAX( fMaximumDifference, Math::abs( m_height[iIndex] - m_height[( size_t ) ( neighbor )] ) );
		}
	}
	return t3dErosionJobSafeDivide( fMaximumDifference, m_fCellSize );
}

void t3dErosionKernels_s::runWeatheringPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const float				   step_progress = kernel_context.m_fStepProgress;
	// Weathering creates classed regolith. Oxidation, salt crystallization,
	// wet-dry cycling, exposure age, and stratigraphy feed back into hardness.
	if ( job.m_config.m_bWeatheringEnabled || job.m_config.m_bOxidationEnabled || job.m_config.m_bWetDryEnabled ||
		 job.m_config.m_bSaltWeatheringEnabled || job.m_config.m_bExposureAgeEnabled ||
		 job.m_config.m_bStratigraphyEnabled ) {
		job.setStage( STAGE_WEATHERING, 0.1f + 0.8f * step_progress );
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::WEATHERING,
								   job.m_nCellCount,
								   grain_alignment );

		if ( job.m_config.m_bWeatheringEnabled && job.m_config.m_fThermalCreepRate > 0.0f ) {
			for ( size_t iFlux = 0u; iFlux < T3D_ARRAY_COUNT( job.m_szFlux ); iFlux++ ) {
				Terrain3DErosionJob::FloatBuffer &plane = ( job.m_szFlux )[iFlux];
				eastl::fill( plane.begin(), plane.end(), 0.0f );
			}
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::THERMAL_FLUX,
									   job.m_nCellCount,
									   grain_alignment );
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::THERMAL_APPLY,
									   job.m_nCellCount,
									   grain_alignment );
		}
	}
}

void t3dErosionKernels_s::runHydraulicPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bHydraulicEnabled ) {
		job.setStage( STAGE_HYDRAULIC, 0.1f + 0.8f * step_progress );
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::RAINFALL,
								   job.m_nCellCount,
								   grain_alignment );

		for ( size_t iFlux = 0u; iFlux < T3D_ARRAY_COUNT( job.m_szFlux ); iFlux++ ) {
			Terrain3DErosionJob::FloatBuffer &plane = ( job.m_szFlux )[iFlux];
			eastl::fill( plane.begin(), plane.end(), 0.0f );
		}
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::HYDRAULIC_FLUX,
								   job.m_nCellCount,
								   grain_alignment );

		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::HYDRAULIC_WATER,
								   job.m_nCellCount,
								   grain_alignment );

		// Capacity is solved independently for coarse bedload, sand, and fines.
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::HYDRAULIC_CAPACITY,
								   job.m_nCellCount,
								   grain_alignment );

		// Alluvial fans sort and spread bedload where confined, steep flow emerges onto a flatter surface.
		if ( job.m_config.m_bAlluvialFanEnabled ) {
			eastl::fill( job.m_transferAmount.begin(), job.m_transferAmount.end(), 0.0f );
			eastl::fill( job.m_transferCoarse.begin(), job.m_transferCoarse.end(), 0.0f );
			eastl::fill( job.m_transferSand.begin(), job.m_transferSand.end(), 0.0f );
			eastl::fill( job.m_transferDestination.begin(), job.m_transferDestination.end(), -1 );
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::DEBRIS_TRANSFER,
									   job.m_nCellCount,
									   grain_alignment );
			// Deterministic apply pass permits multiple source channels to build the same fan lobe.
			for ( size_t source = 0; source < job.m_nCellCount; ++source ) {
				const float fCoarse = job.m_transferCoarse[source];
				const float fSand = job.m_transferSand[source];
				if ( fCoarse + fSand <= s_fErosionEpsilon ) {
					continue;
				}
				job.m_sedimentCoarse[source] = MAX( 0.0f, job.m_sedimentCoarse[source] - fCoarse );
				job.m_sedimentSand[source] = MAX( 0.0f, job.m_sedimentSand[source] - fSand );
				const float fSpread = job.m_transferAmount[source];
				const float fLocalCoarse = fCoarse * ( 1.0f - fSpread );
				const float fLocalSand = fSand * ( 1.0f - fSpread );
				job.depositSurface( source, fLocalCoarse, fLocalSand, 0.0f );
				job.m_height[source] += fLocalCoarse + fLocalSand;
				job.m_alluvialDeposition[source] += fLocalCoarse + fLocalSand;
				const int64_t destination = job.m_transferDestination[source];
				if ( destination >= 0 && fSpread > s_fErosionEpsilon ) {
					const float fSpreadCoarse = fCoarse * fSpread;
					const float fSpreadSand = fSand * fSpread;
					job.depositSurface( ( size_t ) ( destination ), fSpreadCoarse, fSpreadSand, 0.0f );
					job.m_height[( size_t ) ( destination )] += fSpreadCoarse + fSpreadSand;
					job.m_alluvialDeposition[( size_t ) ( destination )] += fSpreadCoarse + fSpreadSand;
				}
			}
		}

		// Advect all three suspended classes with the water _flux.
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::SEDIMENT_ADVECTION,
								   job.m_nCellCount,
								   grain_alignment );
		job.m_water.swap( job.m_nextA );
		job.m_sedimentCoarse.swap( job.m_nextCoarse );
		job.m_sedimentSand.swap( job.m_nextSand );
		job.m_sedimentFine.swap( job.m_nextFine );
	}
}

void t3dErosionKernels_s::runDebrisFlowPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bDebrisFlowEnabled && kernel_context.m_fDebrisCurve > 0.0f ) {
		job.setStage( STAGE_DEBRIS_FLOW, 0.1f + 0.8f * step_progress );
		eastl::fill( job.m_transferCoarse.begin(), job.m_transferCoarse.end(), 0.0f );
		eastl::fill( job.m_transferSand.begin(), job.m_transferSand.end(), 0.0f );
		eastl::fill( job.m_transferFine.begin(), job.m_transferFine.end(), 0.0f );
		eastl::fill( job.m_transferDestination.begin(), job.m_transferDestination.end(), -1 );
		// Deterministic serial source pass: runout paths may converge on one cell.
		for ( size_t index = 0; index < job.m_nCellCount; ++index ) {
			if ( !job.m_active[index] || job.m_water[index] <= s_fErosionEpsilon ) {
				continue;
			}
			const float fSuspended =
				job.m_sedimentCoarse[index] + job.m_sedimentSand[index] + job.m_sedimentFine[index];
			const float fMobile = fSuspended + job.getLooseMaterial( index );
			const float fConcentration = t3dErosionJobSafeDivide( fMobile, fMobile + job.m_water[index] );
			if ( fConcentration < job.m_config.m_fDebrisFlowConcentrationThreshold ||
				 job.getSlope( index ) < job.m_config.m_fDebrisFlowSlopeThreshold ) {
				continue;
			}
			int64_t destination = ( int64_t ) ( index );
			for ( int32_t travel = 0; travel < job.m_config.m_nDebrisFlowRunoutCells; ++travel ) {
				int64_t lowest = destination;
				float	fLowestHeight = job.m_height[( size_t ) ( destination )];
				for ( int32_t dir = 0; dir < T3D_EROSION_DIRECTION_COUNT; ++dir ) {
					const int64_t neighbor = job.neighborIndex( ( size_t ) ( destination ),
																s_szErosionDirectionX[dir],
																s_szErosionDirectionY[dir] );
					if ( neighbor >= 0 && job.m_active[( size_t ) ( neighbor )] &&
						 job.m_height[( size_t ) ( neighbor )] < fLowestHeight ) {
						lowest = neighbor;
						fLowestHeight = job.m_height[( size_t ) ( neighbor )];
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
			const float fAmount = MIN( job.m_config.m_fMaxHeightChangePerStep,
									   fMobile * job.m_config.m_fDebrisFlowRate * kernel_context.m_fDebrisCurve *
										   job.m_simulationWeight[index] );
			if ( fAmount <= s_fErosionEpsilon ) {
				continue;
			}
			float		fC = 0.0f, fS = 0.0f, fF = 0.0f;
			const float fSurfaceAmount = MIN( job.getLooseMaterial( index ), fAmount );
			job.entrainSurface( index, fSurfaceAmount, fC, fS, fF );
			job.m_height[index] -= fSurfaceAmount;
			const float fRemainder = fAmount - fSurfaceAmount;
			const float fSuspendedTotal = MAX( fSuspended, s_fErosionEpsilon );
			const float fSc =
				MIN( job.m_sedimentCoarse[index], fRemainder * job.m_sedimentCoarse[index] / fSuspendedTotal );
			const float fSs =
				MIN( job.m_sedimentSand[index], fRemainder * job.m_sedimentSand[index] / fSuspendedTotal );
			const float fSf =
				MIN( job.m_sedimentFine[index], fRemainder * job.m_sedimentFine[index] / fSuspendedTotal );
			job.m_sedimentCoarse[index] -= fSc;
			job.m_sedimentSand[index] -= fSs;
			job.m_sedimentFine[index] -= fSf;
			job.m_transferCoarse[index] = fC + fSc;
			job.m_transferSand[index] = fS + fSs;
			job.m_transferFine[index] = fF + fSf;
			job.m_transferDestination[index] = destination;
		}
		for ( size_t source = 0; source < job.m_nCellCount; ++source ) {
			const int64_t destination = job.m_transferDestination[source];
			if ( destination < 0 ) {
				continue;
			}
			const float fDepositFraction =
				t3dErosionJobClamp01( job.m_config.m_fDebrisFlowDepositionRate *
									  ( 1.0f - t3dErosionJobClamp01( job.getSlope( ( size_t ) ( destination ) ) ) ) );
			const float fC = job.m_transferCoarse[source] * fDepositFraction;
			const float fS = job.m_transferSand[source] * fDepositFraction;
			const float fF = job.m_transferFine[source] * fDepositFraction;
			job.depositSurface( ( size_t ) ( destination ), fC, fS, fF );
			job.m_height[( size_t ) ( destination )] += fC + fS + fF;
			job.m_debrisFlowDeposition[( size_t ) ( destination )] += fC + fS + fF;
			job.m_sedimentCoarse[( size_t ) ( destination )] += job.m_transferCoarse[source] - fC;
			job.m_sedimentSand[( size_t ) ( destination )] += job.m_transferSand[source] - fS;
			job.m_sedimentFine[( size_t ) ( destination )] += job.m_transferFine[source] - fF;
		}
	}
}

void t3dErosionKernels_s::runWindPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const size_t			   grain = rExecution.m_nGrain;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bWindEnabled && kernel_context.m_fWindCurve > 0.0f ) {
		job.setStage( STAGE_WIND, 0.1f + 0.8f * step_progress );
		const int32_t wind_dx = Math::abs( job.m_config.m_windDirection.x ) >= 0.25f
									? ( job.m_config.m_windDirection.x >= 0.0f ? 1 : -1 )
									: 0;
		const int32_t wind_dy = Math::abs( job.m_config.m_windDirection.y ) >= 0.25f
									? ( job.m_config.m_windDirection.y >= 0.0f ? 1 : -1 )
									: 0;
		kernel_context.m_nWindDx = wind_dx;
		kernel_context.m_nWindDy = wind_dy;
		eastl::fill( job.m_transferCoarse.begin(), job.m_transferCoarse.end(), 0.0f );
		eastl::fill( job.m_transferSand.begin(), job.m_transferSand.end(), 0.0f );
		eastl::fill( job.m_transferFine.begin(), job.m_transferFine.end(), 0.0f );
		eastl::fill( job.m_transferDestination.begin(), job.m_transferDestination.end(), -1 );
		// Wind travel has variable traversal length, so retain dynamic work stealing.
		t3dErosionRunDynamicKernel( executor,
									kernel_context,
									t3dErosionKernel_t::WIND_TRANSPORT,
									job.m_nCellCount,
									grain );
		// Gather incoming wind deposits by following the finite upwind transport corridor.
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::WIND_GATHER,
								   job.m_nCellCount,
								   grain_alignment );
	}
}

void t3dErosionKernels_s::runDunePhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bDuneEnabled ) {
		job.setStage( STAGE_DUNES, 0.1f + 0.8f * step_progress );
		for ( int32_t iteration = 0; iteration < job.m_config.m_nDuneRelaxationIterations; ++iteration ) {
			for ( size_t iFlux = 0u; iFlux < T3D_ARRAY_COUNT( job.m_szFlux ); iFlux++ ) {
				Terrain3DErosionJob::FloatBuffer &plane = ( job.m_szFlux )[iFlux];
				eastl::fill( plane.begin(), plane.end(), 0.0f );
			}
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::DUNE_FLUX,
									   job.m_nCellCount,
									   grain_alignment );
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::DUNE_APPLY,
									   job.m_nCellCount,
									   grain_alignment );
		}
	}
}

void t3dErosionKernels_s::runSurfaceAgingPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	// Desert pavement is a lag deposit: hydraulic or aeolian removal of fines leaves an armored coarse surface.
	if ( job.m_config.m_bDesertPavementEnabled ) {
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::PAVEMENT,
								   job.m_nCellCount,
								   grain_alignment );
	}
}

void t3dErosionKernels_s::runWavePhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const size_t			   grain = rExecution.m_nGrain;
	const int32_t			   step = rExecution.m_nStep;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bWaveEnabled && kernel_context.m_fWaveCurve > 0.0f ) {
		job.setStage( STAGE_WAVES, 0.1f + 0.8f * step_progress );
		const float	  current_sea_level = job.m_config.m_fSeaLevel + job.sampleCurve( CURVE_SEA_LEVEL, step );
		const int32_t wave_dx = Math::abs( job.m_config.m_waveDirection.x ) >= 0.25f
									? ( job.m_config.m_waveDirection.x >= 0.0f ? 1 : -1 )
									: 0;
		const int32_t wave_dy = Math::abs( job.m_config.m_waveDirection.y ) >= 0.25f
									? ( job.m_config.m_waveDirection.y >= 0.0f ? 1 : -1 )
									: 0;
		const int32_t fetch_cells = CLAMP( ( int32_t ) ( Math::ceil( job.m_config.m_fWaveFetchDistanceM /
																	 MAX( job.m_fCellSize, s_fErosionEpsilon ) ) ),
										   1,
										   256 );
		const float	  coastal_band = MAX( job.m_config.m_fCoastlineBandM, job.m_config.m_fTidalRange );
		kernel_context.m_fCurrentSeaLevel = current_sea_level;
		kernel_context.m_fCoastalBand = coastal_band;
		kernel_context.m_nWaveDx = wave_dx;
		kernel_context.m_nWaveDy = wave_dy;
		kernel_context.m_nFetchCells = fetch_cells;
		eastl::fill( job.m_transferAmount.begin(), job.m_transferAmount.end(), 0.0f );
		eastl::fill( job.m_transferDestination.begin(), job.m_transferDestination.end(), -1 );
		// Fetch length varies strongly with shoreline topology.
		t3dErosionRunDynamicKernel( executor,
									kernel_context,
									t3dErosionKernel_t::WAVE_TRANSPORT,
									job.m_nCellCount,
									grain );
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::WAVE_GATHER,
								   job.m_nCellCount,
								   grain_alignment );
		if ( job.m_config.m_fBeachSmoothing > 0.0f ) {
			t3dErosionRunStaticKernel( executor,
									   kernel_context,
									   t3dErosionKernel_t::BEACH_SMOOTH,
									   job.m_nCellCount,
									   grain_alignment );
			job.m_height.swap( job.m_nextA );
		}
	}
}

void t3dErosionKernels_s::runTrafficPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	const float				   step_progress = kernel_context.m_fStepProgress;
	if ( job.m_config.m_bTrafficEnabled && kernel_context.m_fTrafficCurve > 0.0f ) {
		job.setStage( STAGE_TRAFFIC, 0.1f + 0.8f * step_progress );
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::TRAFFIC,
								   job.m_nCellCount,
								   grain_alignment );
		job.m_height.swap( job.m_nextA );
	}
}

void t3dErosionKernels_s::runStratigraphyPhase(
	const t3dErosionStepExecution_t &rExecution ) {
	Terrain3DErosionJob		  &job = *rExecution.m_context.m_pJob;
	t3dErosionKernelContext_t &kernel_context = rExecution.m_context;
	const ParallelExecutor	  &executor = rExecution.m_executor;
	const size_t			   grain_alignment = rExecution.m_nGrainAlignment;
	if ( job.m_config.m_bStratigraphyEnabled ) {
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::STRATIGRAPHY,
								   job.m_nCellCount,
								   grain_alignment );
	}

	if ( job.m_config.m_bExposureAgeEnabled ) {
		t3dErosionRunStaticKernel( executor,
								   kernel_context,
								   t3dErosionKernel_t::EXPOSURE,
								   job.m_nCellCount,
								   grain_alignment );
	}
}

void Terrain3DErosionJob::run(
	void ) {
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

	const int32_t		   thread_count = t3dErosionJobResolveThreadCount( m_config.m_nThreadCount );
	const ParallelExecutor executor( thread_count );
	const size_t		   raw_grain = MAX( 256, m_nCellCount / ( size_t ) ( MAX( 1, thread_count * 32 ) ) );
	// Every task starts on a complete simulation row and on a cacheline boundary
	// for float, half and byte planes. Adjacent workers therefore never write the
	// same boundary line across the solver's parallel SoA channels.
	const size_t			  grain_alignment = MAX( 64u, ( size_t ) ( m_nSimulationRegionSize ) );
	const size_t			  grain = ( ( raw_grain + grain_alignment - 1u ) / grain_alignment ) * grain_alignment;
	t3dErosionKernelContext_t kernel_context;
	kernel_context.m_pJob = this;

	kernel_context.m_fYearsStep =
		( float ) ( m_config.m_fSimulationYears / ( double ) ( m_config.m_nSimulationSteps ) );
	kernel_context.m_fTalusGradient = Math::tan( Math::deg_to_rad( m_config.m_fTalusAngleDegrees ) );
	kernel_context.m_fDuneGradient = Math::tan( Math::deg_to_rad( m_config.m_fSandReposeAngleDegrees ) );
	float fPreviousTemperature = sampleCurve( CURVE_TEMPERATURE, 0 );

	for ( int32_t step = 0; step < m_config.m_nSimulationSteps; ++step ) {
		if ( !waitForPreviewPermission() || m_bCancelRequested.load() ) {
			m_state.store( STATE_CANCELLED );
			setStage( STAGE_CANCELLED, 1.0f );
			m_previewCondition.notifyAll();
			return;
		}
		if ( m_config.m_bExposureAgeEnabled && !m_stepHeightStart.copyFrom( m_height ) ) {
			setFailed( "Erosion exposure-age scratch buffer does not match the height layout." );
			return;
		}

		t3dErosionKernels_s::prepareStep( kernel_context, step, fPreviousTemperature );
		const t3dErosionStepExecution_t execution = { executor, kernel_context, grain_alignment, grain, step };
		t3dErosionKernels_s::runWeatheringPhase( execution );
		t3dErosionKernels_s::runHydraulicPhase( execution );
		t3dErosionKernels_s::runDebrisFlowPhase( execution );
		t3dErosionKernels_s::runWindPhase( execution );
		t3dErosionKernels_s::runDunePhase( execution );
		t3dErosionKernels_s::runSurfaceAgingPhase( execution );
		t3dErosionKernels_s::runWavePhase( execution );
		t3dErosionKernels_s::runTrafficPhase( execution );
		t3dErosionKernels_s::runStratigraphyPhase( execution );

		m_nCompletedSteps.store( step + 1 );
		m_fProgress.store( 0.1f + 0.8f * ( float ) ( step + 1 ) / ( float ) ( m_config.m_nSimulationSteps ) );
		publishPreviewSnapshot( step + 1 );
	}

	if ( m_bCancelRequested.load() ) {
		m_state.store( STATE_CANCELLED );
		setStage( STAGE_CANCELLED, 1.0f );
		return;
	}

	if ( m_config.m_bMaterialOutputEnabled ) {
		setStage( STAGE_MATERIALS, 0.92f );
		buildMaterialTransitions();
	}

	eastl::vector< t3dErosionFinalStatistics_t > finalStatistics( ( size_t ) ( MAX( 1, thread_count ) ) );
	kernel_context.m_pFinalStatistics = &finalStatistics;
	t3dErosionRunStaticKernel( executor,
							   kernel_context,
							   t3dErosionKernel_t::FINAL_STATISTICS,
							   m_nCellCount,
							   grain_alignment );
	double fTotalEroded = 0.0;
	double fTotalDeposited = 0.0;
	float  maximum_lowering = 0.0f;
	float  maximum_raising = 0.0f;
	for ( size_t iStatistic = 0u; iStatistic < ( size_t ) ( ( finalStatistics ).size() ); iStatistic++ ) {
		const t3dErosionFinalStatistics_t &local = ( finalStatistics )[iStatistic];
		fTotalEroded += local.m_fEroded;
		fTotalDeposited += local.m_fDeposited;
		maximum_lowering = MAX( maximum_lowering, local.m_fMaximumLowering );
		maximum_raising = MAX( maximum_raising, local.m_fMaximumRaising );
	}
	{
		Terrain3DScopedLock lock( m_statusMutex );
		m_fTotalEroded = fTotalEroded;
		m_fTotalDeposited = fTotalDeposited;
		m_fMaximumLowering = maximum_lowering;
		m_fMaximumRaising = maximum_raising;
	}

	publishPreviewSnapshot( m_config.m_nSimulationSteps, true );
	if ( !compactCompletedResults() ) {
		setFailed( "Erosion completed, but its retained result could not be compacted." );
		m_previewCondition.notifyAll();
		return;
	}

	m_state.store( STATE_COMPLETED );
	setStage( STAGE_COMPLETE, 1.0f );
	m_bPreviewWaiting.store( false );
	m_previewCondition.notifyAll();
	{
		Terrain3DScopedLock lock( m_previewMutex );
		m_previewHeight.clear();
		m_previewMemory.release();
	}
}

int32_t Terrain3DErosionJob::getTransitionTarget(
	const int32_t					  nTextureId,
	const t3dMaterialTransitionKind_t kind ) const {
	if ( nTextureId < 0 || nTextureId >= T3D_EROSION_TEXTURE_PROFILE_COUNT ) {
		return -1;
	}
	const t3dTextureErosionProfile_t &profile = m_config.m_textureProfiles[( size_t ) ( nTextureId )];
	if ( !profile.m_bValid ) {
		return -1;
	}
	switch ( kind ) {
	case TRANSITION_EXPOSED:
		return profile.m_nExposedTextureId;
	case TRANSITION_WEATHERED:
		return profile.m_nWeatheredTextureId;
	case TRANSITION_OXIDIZED:
		return profile.m_nOxidizedTextureId;
	case TRANSITION_COARSE_DEPOSIT:
		return profile.m_nCoarseDepositTextureId;
	case TRANSITION_FINE_DEPOSIT:
		return profile.m_nFineDepositTextureId;
	case TRANSITION_SALT_WEATHERED:
		return profile.m_nSaltWeatheredTextureId;
	case TRANSITION_DESERT_PAVEMENT:
		return profile.m_nPavementTextureId;
	case TRANSITION_VARNISHED:
		return profile.m_nVarnishedTextureId;
	default:
		return -1;
	}
}

void Terrain3DErosionJob::buildMaterialTransitions(
	void ) {
	eastl::fill( m_materialTransitionKind.begin(), m_materialTransitionKind.end(), ( uint8_t ) ( TRANSITION_NONE ) );
	eastl::fill( m_materialTransitionAmount.begin(), m_materialTransitionAmount.end(), 0.0f );

	const int32_t			  thread_count = t3dErosionJobResolveThreadCount( m_config.m_nThreadCount );
	const ParallelExecutor	  executor( thread_count );
	const size_t			  alignment = MAX( 64u, ( size_t ) ( m_nSimulationRegionSize ) );
	eastl::vector< float >	  workerMaximumFlow( ( size_t ) ( MAX( 1, thread_count ) ), s_fErosionEpsilon );
	t3dErosionKernelContext_t kernel_context;
	kernel_context.m_pJob = this;
	kernel_context.m_pWorkerMaximumFlow = &workerMaximumFlow;
	t3dErosionRunStaticKernel( executor, kernel_context, t3dErosionKernel_t::MAXIMUM_FLOW, m_nCellCount, alignment );
	float maximumFlow = s_fErosionEpsilon;
	for ( size_t iWorker = 0u; iWorker < ( size_t ) ( ( workerMaximumFlow ).size() ); iWorker++ ) {
		const float fLocalMaximum = ( workerMaximumFlow )[iWorker];
		maximumFlow = MAX( maximumFlow, fLocalMaximum );
	}
	kernel_context.m_fMaximumFlow = maximumFlow;

	t3dErosionRunStaticKernel( executor,
							   kernel_context,
							   t3dErosionKernel_t::MATERIAL_TRANSITIONS,
							   m_nCellCount,
							   alignment );
}

void Terrain3DErosionJob::cancel(
	void ) {
	m_bCancelRequested.store( true );
	m_previewCondition.notifyAll();
}

void Terrain3DErosionJob::setPreviewPaused(
	const bool bPaused ) {
	if ( !m_config.m_bLivePreviewEnabled || isComplete() ) {
		return;
	}
	m_bPreviewPaused.store( bPaused );
	if ( !bPaused ) {
		m_previewStepRequests.store( 0 );
	}
	m_previewCondition.notifyAll();
}

void Terrain3DErosionJob::requestPreviewStep(
	void ) {
	if ( !m_config.m_bLivePreviewEnabled || isComplete() ) {
		return;
	}
	m_bPreviewPaused.store( true );
	m_previewStepRequests.fetch_add( 1 );
	m_previewCondition.notifyAll();
}

bool Terrain3DErosionJob::verifySourceUnchanged(
	Terrain3DDataAccess *const pDataAccess ) const {
	Terrain3DDataAccess *const pData = pDataAccess;
	if ( !pData || pData->getRegionCount() != ( int32_t ) ( m_regions.size() ) ) {
		return false;
	}
	if ( hashTextureProfiles( pData ) != m_nAssetProfileHash ) {
		return false;
	}
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_regions ).size() ); iRegion++ ) {
		const t3dRegionSnapshot_t &snapshot = ( m_regions )[iRegion];
		if ( !pData->ensureCpuRegionResident( snapshot.m_location ) ) {
			return false;
		}
		const Ref< Terrain3DRegion > pRegion = pData->getRegion( snapshot.m_location );
		if ( pRegion.is_null() || T3D_REGION_SIZE != T3D_REGION_SIZE ||
			 !Math::is_equal_approx( pRegion->getVertexSpacing(), m_fVertexSpacing ) ) {
			return false;
		}
		const Ref< Image > pHeightMap = pRegion->getHeightMap();
		const Ref< Image > pControlMap = pRegion->getControlMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			 pHeightMap->get_width() != T3D_REGION_SIZE || pHeightMap->get_height() != T3D_REGION_SIZE ||
			 ( pControlMap.is_valid() &&
			   ( pControlMap->get_format() != Image::FORMAT_RF || pControlMap->get_width() != T3D_REGION_SIZE ||
				 pControlMap->get_height() != T3D_REGION_SIZE ) ) ) {
			return false;
		}
		if ( hashRegionImages( pHeightMap, pControlMap ) != snapshot.m_nSourceHash ) {
			return false;
		}
	}
	return true;
}

float Terrain3DErosionJob::sampleOutputCell(
	const FloatBuffer &rMap,
	const int32_t	   nRegionId,
	const int32_t	   iBaseX,
	const int32_t	   iBaseY,
	const int32_t	   iX,
	const int32_t	   iY ) const {
	const size_t base_index = ( size_t ) ( nRegionId ) *m_nCellsPerRegion +
							  ( size_t ) ( iBaseY ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( iBaseX );
	const int64_t resolved = neighborIndex( base_index, iX - iBaseX, iY - iBaseY );
	if ( resolved < 0 || !m_active[( size_t ) ( resolved )] ) {
		return rMap[base_index];
	}
	return rMap[( size_t ) ( resolved )];
}

float Terrain3DErosionJob::sampleOutput(
	const FloatBuffer &rMap,
	const int32_t	   nRegionId,
	const float		   fSourceX,
	const float		   fSourceY ) const {
	if ( rMap.size() < m_nCellCount || nRegionId < 0 || nRegionId >= ( int32_t ) ( m_regions.size() ) ) {
		return 0.0f;
	}
	const float	  fSimX = ( fSourceX + 0.5f ) / ( float ) ( m_nResolutionScale ) -0.5f;
	const float	  fSimY = ( fSourceY + 0.5f ) / ( float ) ( m_nResolutionScale ) -0.5f;
	const int32_t x0 = ( int32_t ) ( Math::floor( fSimX ) );
	const int32_t y0 = ( int32_t ) ( Math::floor( fSimY ) );
	const float	  fFx = fSimX - ( float ) ( x0 );
	const float	  fFy = fSimY - ( float ) ( y0 );
	const int32_t base_x = CLAMP( x0, 0, m_nSimulationRegionSize - 1 );
	const int32_t base_y = CLAMP( y0, 0, m_nSimulationRegionSize - 1 );
	const float	  fV00 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0, y0 );
	const float	  fV10 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0 + 1, y0 );
	const float	  fV01 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0, y0 + 1 );
	const float	  fV11 = sampleOutputCell( rMap, nRegionId, base_x, base_y, x0 + 1, y0 + 1 );
	return Math::lerp( Math::lerp( fV00, fV10, fFx ), Math::lerp( fV01, fV11, fFx ), fFy );
}

Error Terrain3DErosionJob::prepareApplyRegions(
	Terrain3DDataAccess						*pData,
	eastl::vector< Ref< Terrain3DRegion > > &pApplyRegions ) {
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_regions ).size() ); iRegion++ ) {
		const t3dRegionSnapshot_t &snapshot = ( m_regions )[iRegion];
		if ( !pData->ensureCpuRegionResident( snapshot.m_location ) ) {
			setErrorMessage( "A terrain region could not be paged into the CPU map cache before erosion apply." );
			return ERR_CANT_OPEN;
		}
		const Ref< Terrain3DRegion > pRegion = pData->getRegion( snapshot.m_location );
		if ( pRegion.is_null() ) {
			setErrorMessage( "A terrain region captured by the erosion snapshot no longer exists." );
			return ERR_DOES_NOT_EXIST;
		}
		const Ref< Image > pHeightMap = pRegion->getHeightMap();
		const Ref< Image > pControlMap = pRegion->getControlMap();
		const Ref< Image > pColorMap = pRegion->getColorMap();
		if ( pHeightMap.is_null() || pHeightMap->get_format() != Image::FORMAT_R16 ||
			 pHeightMap->get_width() != T3D_REGION_SIZE || pHeightMap->get_height() != T3D_REGION_SIZE ||
			 ( pControlMap.is_valid() &&
			   ( pControlMap->get_format() != Image::FORMAT_RF || pControlMap->get_width() != T3D_REGION_SIZE ||
				 pControlMap->get_height() != T3D_REGION_SIZE ) ) ) {
			setErrorMessage( "A terrain region no longer has compatible R16 height or optional RF control data." );
			return ERR_INVALID_DATA;
		}
		if ( m_config.m_bOxidationColorEnabled && pColorMap.is_valid() &&
			 ( pColorMap->get_width() != T3D_REGION_SIZE || pColorMap->get_height() != T3D_REGION_SIZE ) ) {
			setErrorMessage( "A terrain color map no longer matches the snapshotted region resolution." );
			return ERR_INVALID_DATA;
		}
		pApplyRegions.push_back( pRegion );
	}

	return OK;
}

Error Terrain3DErosionJob::applyRegionOutputs(
	Terrain3DDataAccess			 *pData,
	const Ref< Terrain3DRegion > &pRegion,
	const int32_t				  region_id,
	t3dErosionApplyBatch_t		 &rBatch ) {
	const Terrain3DParallelExecutor &executor = rBatch.m_executor;
	const size_t					 source_pixel_count = rBatch.m_nSourcePixelCount;
	const size_t					 apply_grain = rBatch.m_nGrain;
	auto							&height_buffer = rBatch.m_height;
	auto							&control_buffer = rBatch.m_control;
	auto							&color_buffer = rBatch.m_color;
	auto *const						 changed_locations = rBatch.m_changedLocations;
	bool							&bAnyControlChanged = rBatch.m_bControlChanged;
	bool							&bAnyColorChanged = rBatch.m_bColorChanged;
	if ( !pData->ensureCpuRegionResident( pRegion->getLocation() ) ) {
		setErrorMessage( "A terrain region could not be paged into the CPU map cache during erosion apply." );
		return ERR_CANT_OPEN;
	}
	Ref< Image > pHeightMap = pRegion->getHeightMap();
	const size_t simulation_begin = ( size_t ) ( region_id ) *m_nCellsPerRegion;
	const size_t simulation_end = simulation_begin + m_nCellsPerRegion;
	bool		 bRegionNeedsControl = false;
	if ( m_config.m_bMaterialOutputEnabled && !m_materialTransitionKind.empty() ) {
		for ( size_t index = simulation_begin; index < simulation_end; ++index ) {
			if ( m_materialTransitionKind[index] != ( uint8_t ) ( TRANSITION_NONE ) &&
				 m_materialTransitionAmount[index] > s_fErosionEpsilon ) {
				bRegionNeedsControl = true;
				break;
			}
		}
	}
	bool bRegionNeedsColor = false;
	if ( m_config.m_bOxidationColorEnabled && !m_oxidation.empty() &&
		 m_config.m_fOxidationTintStrength > s_fErosionEpsilon ) {
		for ( size_t index = simulation_begin; index < simulation_end; ++index ) {
			if ( m_oxidation[index] * m_config.m_fOxidationTintStrength > ( 0.5f / s_fByteMaxFloat ) ) {
				bRegionNeedsColor = true;
				break;
			}
		}
	}
	Ref< Image > pControlMap = bRegionNeedsControl ? pRegion->materializeMap( TYPE_CONTROL ) : pRegion->getControlMap();
	Ref< Image > pColorMap = bRegionNeedsColor ? pRegion->materializeMap( TYPE_COLOR ) : pRegion->getColorMap();

	const PackedByteArray encoded_height_data = pHeightMap->get_data();
	const size_t		  encoded_height_byte_count = source_pixel_count * sizeof( uint16_t );
	if ( ( size_t ) ( encoded_height_data.size() ) < encoded_height_byte_count ) {
		setErrorMessage( "A terrain height map returned an incomplete R16 data buffer." );
		return ERR_INVALID_DATA;
	}
	PackedByteArray source_height_data;
	source_height_data.resize( ( int64_t ) ( source_pixel_count * sizeof( float ) ) );
	uint8_t *const pSourceHeightWrite = source_height_data.ptrw();
	for ( size_t i = 0; i < source_pixel_count; ++i ) {
		t3dErosionJobWriteRf(
			pSourceHeightWrite,
			i,
			t3dRegionCodecReadHeightBytes( encoded_height_data.ptr(), ( int ) ( i ), Image::FORMAT_R16 ) );
	}
	PackedByteArray source_control_data;
	if ( bRegionNeedsControl ) {
		if ( pControlMap.is_valid() ) {
			source_control_data = pControlMap->get_data();
		} else {
			source_control_data.resize( ( int64_t ) ( source_pixel_count * sizeof( float ) ) );
			uint8_t *const pControlWrite = source_control_data.ptrw();
			for ( size_t i = 0; i < source_pixel_count; ++i ) {
				t3dErosionJobWriteRf( pControlWrite, i, t3dAsFloat( T3D_REGION_DEFAULT_CONTROL ) );
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
	if ( m_config.m_bApplyHeight ) {
		height_buffer.resize( height_byte_count );
		memcpy( height_buffer.data(), source_height_data.ptr(), height_byte_count );
	}
	if ( bRegionNeedsControl ) {
		control_buffer.resize( control_byte_count );
		memcpy( control_buffer.data(), source_control_data.ptr(), control_byte_count );
	}

	const bool bTintRgba8 = bRegionNeedsColor && pColorMap.is_valid() && pColorMap->get_format() == Image::FORMAT_RGBA8;
	PackedByteArray source_color_data;
	if ( bTintRgba8 ) {
		source_color_data = pColorMap->get_data();
		if ( ( size_t ) ( source_color_data.size() ) < source_pixel_count * 4U ) {
			setErrorMessage( "A terrain color map returned an incomplete RGBA8 data buffer." );
			return ERR_INVALID_DATA;
		}
		color_buffer.resize( ( size_t ) ( source_color_data.size() ) );
		memcpy( color_buffer.data(), source_color_data.ptr(), color_buffer.size() );
	}

	const uint8_t *height_source_bytes = source_height_data.ptr();
	const uint8_t *control_source_bytes = source_control_data.is_empty() ? NULL : source_control_data.ptr();
	uint8_t *const height_bytes = height_buffer.empty() ? NULL : height_buffer.data();
	uint8_t		  *control_bytes = control_buffer.empty() ? NULL : control_buffer.data();
	uint8_t		  *color_bytes = color_buffer.empty() ? NULL : color_buffer.data();

	t3dErosionApplyContext_t apply_context;
	apply_context.m_pJob = this;
	apply_context.m_nRegionId = region_id;
	apply_context.m_pHeightSourceBytes = height_source_bytes;
	apply_context.m_pControlSourceBytes = control_source_bytes;
	apply_context.m_pHeightBytes = height_bytes;
	apply_context.m_pControlBytes = control_bytes;
	apply_context.m_pColorBytes = color_bytes;
	executor.run( source_pixel_count, apply_grain, t3dErosionKernels_s::applyRegion, &apply_context );

	if ( bRegionNeedsColor && pColorMap.is_valid() && !bTintRgba8 ) {
		// Non-RGBA8 formats use Image accessors on the main thread. This is slower,
		// but avoids changing the user's color-map format or touching engine objects
		// from erosion worker threads.
		for ( int32_t y = 0; y < T3D_REGION_SIZE; ++y ) {
			for ( int32_t x = 0; x < T3D_REGION_SIZE; ++x ) {
				const size_t source_index = ( size_t ) ( y ) * ( size_t ) ( T3D_REGION_SIZE ) + ( size_t ) ( x );
				if ( isnan( t3dErosionJobReadRf( height_source_bytes, source_index ) ) ) {
					continue;
				}
				const float fOxidation =
					t3dErosionJobClamp01( sampleOutput( m_oxidation, region_id, ( float ) ( x ), ( float ) ( y ) ) *
										  m_config.m_fOxidationTintStrength );
				Color		source_color = pColorMap->get_pixel( x, y );
				const float fRoughness = source_color.a;
				source_color = source_color.lerp( m_config.m_oxidationTint, fOxidation );
				source_color.a = fRoughness;
				pColorMap->set_pixel( x, y, source_color );
			}
		}
	}

	if ( control_bytes && memcmp( control_bytes, source_control_data.ptr(), control_buffer.size() ) == 0 ) {
		control_buffer.clear();
		control_bytes = NULL;
		pRegion->compactMapIfDefault( TYPE_CONTROL );
	}
	if ( color_bytes && memcmp( color_bytes, source_color_data.ptr(), color_buffer.size() ) == 0 ) {
		color_buffer.clear();
		color_bytes = NULL;
		pRegion->compactMapIfDefault( TYPE_COLOR );
	}

	if ( height_bytes ) {
		PackedByteArray output_height_data;
		output_height_data.resize( ( int64_t ) ( source_pixel_count * sizeof( uint16_t ) ) );
		uint8_t *const pEncoded = output_height_data.ptrw();
		for ( size_t i = 0; i < source_pixel_count; ++i ) {
			const uint16_t value = t3dRegionCodecEncodeHeightU16( t3dErosionJobReadRf( height_bytes, i ) );
			memcpy( pEncoded + i * sizeof( uint16_t ), &value, sizeof( uint16_t ) );
		}
		pHeightMap->set_data( pHeightMap->get_width(),
							  pHeightMap->get_height(),
							  false,
							  Image::FORMAT_R16,
							  output_height_data );
	}
	if ( control_bytes ) {
		PackedByteArray output_control_data;
		output_control_data.resize( ( int64_t ) ( control_buffer.size() ) );
		memcpy( output_control_data.ptrw(), control_buffer.data(), control_buffer.size() );
		pControlMap->set_data( pControlMap->get_width(),
							   pControlMap->get_height(),
							   pControlMap->has_mipmaps(),
							   Image::FORMAT_RF,
							   output_control_data );
		pRegion->compactMapIfDefault( TYPE_CONTROL );
		bAnyControlChanged = true;
	}
	if ( color_bytes ) {
		PackedByteArray output_color_data;
		output_color_data.resize( ( int64_t ) ( color_buffer.size() ) );
		memcpy( output_color_data.ptrw(), color_buffer.data(), color_buffer.size() );
		pColorMap->set_data( pColorMap->get_width(),
							 pColorMap->get_height(),
							 pColorMap->has_mipmaps(),
							 Image::FORMAT_RGBA8,
							 output_color_data );
	}
	const bool bRegionColorChanged = color_bytes || ( bRegionNeedsColor && pColorMap.is_valid() && !bTintRgba8 );
	if ( bRegionColorChanged ) {
		pColorMap->clear_mipmaps();
		pRegion->compactMapIfDefault( TYPE_COLOR );
		bAnyColorChanged = true;
	}
	const bool bRegionChanged = height_bytes || control_bytes || bRegionColorChanged;
	if ( bRegionChanged ) {
		pRegion->setModified( true );
		pRegion->setEdited( true );
	}
	if ( m_config.m_bApplyHeight ) {
		pRegion->calcHeightRange();
	}
	const Vector2i location = pRegion->getLocation();
	if ( height_bytes ) {
		changed_locations[TYPE_HEIGHT].push_back( location );
	}
	if ( control_bytes ) {
		changed_locations[TYPE_CONTROL].push_back( location );
	}
	if ( bRegionColorChanged ) {
		changed_locations[TYPE_COLOR].push_back( location );
	}
	return OK;
}

Error Terrain3DErosionJob::apply(
	Object *const pDataObject,
	const bool	  bForce ) {
	Terrain3DData *const pData = Object::cast_to< Terrain3DData >( pDataObject );
	Terrain3DScopedLock	 applyLock( m_applyMutex );
	setErrorMessage( String() );
	if ( m_state.load() != STATE_COMPLETED ) {
		setErrorMessage( "Erosion results can be applied only from the completed state." );
		return ERR_UNCONFIGURED;
	}
	if ( !pData ) {
		setErrorMessage( "Erosion apply requires a valid Terrain3DData object." );
		return ERR_INVALID_PARAMETER;
	}
	if ( !m_config.m_bApplyHeight && !m_config.m_bMaterialOutputEnabled && !m_config.m_bOxidationColorEnabled ) {
		setErrorMessage( "This erosion job has no terrain outputs enabled; it is diagnostic-only." );
		return ERR_UNCONFIGURED;
	}
	if ( !bForce && !verifySourceUnchanged( pData ) ) {
		setErrorMessage( "Terrain data changed after the erosion snapshot was created. Refusing to apply stale "
						 "results; create a new job or pass force=true." );
		return ERR_INVALID_DATA;
	}

	eastl::vector< Ref< Terrain3DRegion > > pApplyRegions;
	pApplyRegions.reserve( m_regions.size() );
	const Error stagingError = prepareApplyRegions( pData, pApplyRegions );
	if ( stagingError != OK ) {
		return stagingError;
	}

	const int32_t		   thread_count = t3dErosionJobResolveThreadCount( m_config.m_nThreadCount );
	const ParallelExecutor executor( thread_count );
	const size_t		   source_pixel_count = ( size_t ) ( T3D_REGION_SIZE ) * ( size_t ) ( T3D_REGION_SIZE );
	const size_t		   apply_grain = MAX( 4096, source_pixel_count / ( size_t ) ( MAX( 1, thread_count * 16 ) ) );

	// Application is region-serial, so reuse three raw staging buffers instead of
	// returning them to the general heap after every region. Image/PackedByteArray
	// publication remains per-region because those buffers become resource state.
	t3dErosionApplyBatch_t batch{ executor, source_pixel_count, apply_grain };
	for ( auto &locations : batch.m_changedLocations ) {
		locations.reserve( m_regions.size() );
	}
	batch.m_height.reserve( source_pixel_count * sizeof( float ) );
	batch.m_control.reserve( source_pixel_count * sizeof( float ) );
	batch.m_color.reserve( source_pixel_count * 4U );

	for ( int32_t region_id = 0; region_id < ( int32_t ) ( pApplyRegions.size() ); ++region_id ) {
		const Error regionError = applyRegionOutputs( pData, pApplyRegions[( size_t ) region_id], region_id, batch );
		if ( regionError != OK ) {
			return regionError;
		}
	}

	const bool bHeightChanged = m_config.m_bApplyHeight;
	const bool bControlChanged = batch.m_bControlChanged;
	const bool bColorChanged = batch.m_bColorChanged;
	if ( !bHeightChanged && !bControlChanged && !bColorChanged ) {
		setErrorMessage( "The enabled erosion outputs did not produce any terrain-map changes." );
		return ERR_DOES_NOT_EXIST;
	}

	if ( bHeightChanged ) {
		pData->calcHeightRange( false );
	}
	const Vector2 height_range = pData->getHeightRange();
	AABB		  edited_area;
	edited_area.position = Vector3( m_terrainWorldRect.position.x, height_range.x, m_terrainWorldRect.position.y );
	edited_area.size = Vector3( m_terrainWorldRect.size.x,
								MAX( height_range.y - height_range.x, m_fVertexSpacing ),
								m_terrainWorldRect.size.y );

	auto *const changed_locations = batch.m_changedLocations;
	// Terrain3DData owns mutation finalization, sampling publication, derived
	// maps and render/MegaVT invalidation. Each channel carries its exact
	// changed-region set, including disjoint height and control edits.
	for ( int32_t iMap = TYPE_HEIGHT; iMap < REGION_TYPE_MAX; ++iMap ) {
		if ( changed_locations[iMap].empty() ) {
			continue;
		}
		uint32_t flags = MAP_EDIT_KEEP_EDITED;
		if ( iMap == TYPE_HEIGHT || iMap == TYPE_CONTROL ) {
			flags |= MAP_EDIT_UPDATE_INSTANCES | MAP_EDIT_UPDATE_COLLISION | MAP_EDIT_SNAP;
		}
		pData->commitMapEdits( ( t3dMapType_t ) ( iMap ), changed_locations[iMap], edited_area, flags );
	}
	m_state.store( STATE_APPLIED );
	return OK;
}

bool Terrain3DErosionJob::isSourceUnchanged(
	Object *pDataObject ) const {
	Terrain3DData *const pData = Object::cast_to< Terrain3DData >( pDataObject );
	return pData && verifySourceUnchanged( pData );
}

bool Terrain3DErosionJob::isComplete(
	void ) const {
	const int32_t state = m_state.load();
	return state == STATE_COMPLETED || state == STATE_APPLIED || state == STATE_CANCELLED || state == STATE_FAILED;
}

String Terrain3DErosionJob::getStageName(
	void ) const {
	switch ( ( t3dStage_t ) ( m_stage.load() ) ) {
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

Dictionary Terrain3DErosionJob::getStatistics(
	void ) const {
	double	 fTotalEroded = 0.0;
	double	 fTotalDeposited = 0.0;
	float	 fMaximumLowering = 0.0f;
	float	 fMaximumRaising = 0.0f;
	bool	 bResultsCompacted = false;
	uint64_t retained_result_bytes = 0u;
	String	 error_message;
	{
		Terrain3DScopedLock lock( m_statusMutex );
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
	statistics["source_region_size"] = T3D_REGION_SIZE;
	statistics["simulation_region_size"] = m_nSimulationRegionSize;
	statistics["resolution_scale"] = m_nResolutionScale;
	statistics["cell_count"] = ( int64_t ) ( m_nCellCount );
	statistics["completed_steps"] = m_nCompletedSteps.load();
	statistics["simulation_years"] = m_config.m_fSimulationYears;
	statistics["estimated_eroded_volume_m3"] = fTotalEroded;
	statistics["estimated_deposited_volume_m3"] = fTotalDeposited;
	statistics["maximum_lowering_m"] = fMaximumLowering;
	statistics["maximum_raising_m"] = fMaximumRaising;
	statistics["sediment_sorting"] = m_config.m_bSedimentSortingEnabled;
	statistics["alluvial_fans"] = m_config.m_bAlluvialFanEnabled;
	statistics["debris_flows"] = m_config.m_bDebrisFlowEnabled;
	statistics["dune_relaxation"] = m_config.m_bDuneEnabled;
	statistics["salt_weathering"] = m_config.m_bSaltWeatheringEnabled;
	statistics["wet_dry_cycles"] = m_config.m_bWetDryEnabled;
	statistics["snowmelt"] = m_config.m_bSnowmeltEnabled;
	statistics["stratigraphy"] = m_config.m_bStratigraphyEnabled;
	statistics["results_compacted"] = bResultsCompacted;
	statistics["retained_result_bytes"] = ( int64_t ) ( retained_result_bytes );
	statistics["error"] = error_message;
	return statistics;
}

String Terrain3DErosionJob::getErrorMessage(
	void ) const {
	Terrain3DScopedLock lock( m_statusMutex );
	return m_errorMessage;
}

TypedArray< Vector2i > Terrain3DErosionJob::getRegionLocations(
	void ) const {
	TypedArray< Vector2i > locations;
	for ( size_t iRegion = 0u; iRegion < ( size_t ) ( ( m_regions ).size() ); iRegion++ ) {
		const t3dRegionSnapshot_t &region = ( m_regions )[iRegion];
		locations.push_back( region.m_location );
	}
	return locations;
}

Ref< Image > Terrain3DErosionJob::getInitialRegionHeightMap(
	const Vector2i &rRegionLocation,
	const bool		bFullResolution ) const {
	const int32_t state = m_state.load();
	if ( ( state == STATE_RUNNING && m_previewRevision.load() <= 0 ) ||
		 ( state != STATE_RUNNING && state != STATE_COMPLETED && state != STATE_APPLIED ) ) {
		return Ref< Image >();
	}
	const int32_t map_index = Terrain3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref< Image >();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref< Image >();
	}
	Terrain3DScopedLock lock( m_previewMutex );
	return makeHeightImage( m_initialHeight, region_id, bFullResolution );
}

Ref< Image > Terrain3DErosionJob::getPreviewRegionHeightMap(
	const Vector2i &rRegionLocation,
	const bool		bFullResolution ) const {
	const int32_t map_index = Terrain3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref< Image >();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref< Image >();
	}

	const int32_t state = m_state.load();
	if ( state == STATE_COMPLETED || state == STATE_APPLIED ) {
		return makeHeightImage( m_height, region_id, bFullResolution );
	}
	if ( state != STATE_RUNNING || m_previewRevision.load() <= 0 ) {
		return Ref< Image >();
	}
	Terrain3DScopedLock lock( m_previewMutex );
	const int32_t		locked_state = m_state.load();
	if ( locked_state == STATE_COMPLETED || locked_state == STATE_APPLIED ) {
		return makeHeightImage( m_height, region_id, bFullResolution );
	}
	return makeHeightImage( m_previewHeight, region_id, bFullResolution );
}

Ref< Image > Terrain3DErosionJob::getRegionOutputMap(
	const Vector2i		&rRegionLocation,
	const t3dOutputMap_t output,
	const bool			 bFullResolution ) const {
	if ( m_state.load() != STATE_COMPLETED && m_state.load() != STATE_APPLIED ) {
		return Ref< Image >();
	}
	if ( output < 0 || output >= OUTPUT_MAX ) {
		return Ref< Image >();
	}
	const int32_t map_index = Terrain3DData::getRegionMapIndex( rRegionLocation );
	if ( map_index < 0 ) {
		return Ref< Image >();
	}
	const int32_t region_id = m_szRegionLookup[map_index];
	if ( region_id < 0 ) {
		return Ref< Image >();
	}

	const FloatBuffer *pSource = NULL;
	bool			   bDeriveSediment = false;
	bool			   bDeriveLooseMaterial = false;
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

	const int32_t	size = bFullResolution ? T3D_REGION_SIZE : m_nSimulationRegionSize;
	const size_t	pixel_count = ( size_t ) ( size ) * ( size_t ) ( size );
	PackedByteArray image_data;
	image_data.resize( ( int64_t ) ( pixel_count * sizeof( float ) ) );
	uint8_t *const pDestination = image_data.ptrw();
	for ( int32_t y = 0; y < size; ++y ) {
		for ( int32_t x = 0; x < size; ++x ) {
			float fValue = 0.0f;
			if ( output == OUTPUT_HEIGHT_DELTA ) {
				const float fFinalHeight = bFullResolution
											   ? sampleOutput( m_height, region_id, ( float ) ( x ), ( float ) ( y ) )
											   : m_height[( size_t ) ( region_id ) *m_nCellsPerRegion +
														  ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
				const float fInitialHeight =
					bFullResolution ? sampleOutput( m_initialHeight, region_id, ( float ) ( x ), ( float ) ( y ) )
									: m_initialHeight[( size_t ) ( region_id ) *m_nCellsPerRegion +
													  ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
				fValue = fFinalHeight - fInitialHeight;
			} else if ( output == OUTPUT_MATERIAL_ID ) {
				if ( !m_materialTransitionKind.empty() ) {
					const int32_t sx =
						bFullResolution ? CLAMP( x / m_nResolutionScale, 0, m_nSimulationRegionSize - 1 ) : x;
					const int32_t sy =
						bFullResolution ? CLAMP( y / m_nResolutionScale, 0, m_nSimulationRegionSize - 1 ) : y;
					const size_t index = ( size_t ) ( region_id ) *m_nCellsPerRegion +
										 ( size_t ) ( sy ) * ( size_t ) ( m_nSimulationRegionSize ) + ( size_t ) ( sx );
					const t3dMaterialTransitionKind_t kind =
						( t3dMaterialTransitionKind_t ) ( m_materialTransitionKind[index] );
					const int32_t target = getTransitionTarget( ( int32_t ) ( m_dominantTexture[index] ), kind );
					fValue = target >= 0 ? ( float ) ( target ) / 31.0f : 0.0f;
				}
			} else if ( bDeriveSediment ) {
				if ( bFullResolution ) {
					fValue = sampleOutput( m_sedimentCoarse, region_id, ( float ) ( x ), ( float ) ( y ) ) +
							 sampleOutput( m_sedimentSand, region_id, ( float ) ( x ), ( float ) ( y ) ) +
							 sampleOutput( m_sedimentFine, region_id, ( float ) ( x ), ( float ) ( y ) );
				} else {
					const size_t index = ( size_t ) ( region_id ) *m_nCellsPerRegion +
										 ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
					fValue = getSediment( index );
				}
			} else if ( bDeriveLooseMaterial ) {
				if ( bFullResolution ) {
					fValue = sampleOutput( m_surfaceCoarse, region_id, ( float ) ( x ), ( float ) ( y ) ) +
							 sampleOutput( m_surfaceSand, region_id, ( float ) ( x ), ( float ) ( y ) ) +
							 sampleOutput( m_surfaceFine, region_id, ( float ) ( x ), ( float ) ( y ) );
				} else {
					const size_t index = ( size_t ) ( region_id ) *m_nCellsPerRegion +
										 ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
					fValue = getLooseMaterial( index );
				}
			} else if ( pSource ) {
				fValue = bFullResolution ? sampleOutput( *pSource, region_id, ( float ) ( x ), ( float ) ( y ) )
										 : ( *pSource )[( size_t ) ( region_id ) *m_nCellsPerRegion +
														( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x )];
			}
			const size_t index = ( size_t ) ( y ) * ( size_t ) ( size ) + ( size_t ) ( x );
			memcpy( pDestination + index * sizeof( float ), &fValue, sizeof( float ) );
		}
	}
	return Image::create_from_data( size, size, false, Image::FORMAT_RF, image_data );
}

void Terrain3DErosionJob::_bind_methods(
	void ) {
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

	ClassDB::bind_method( D_METHOD( "run" ), &Terrain3DErosionJob::run );
	ClassDB::bind_method( D_METHOD( "cancel" ), &Terrain3DErosionJob::cancel );
	ClassDB::bind_method( D_METHOD( "set_preview_paused", "paused" ), &Terrain3DErosionJob::setPreviewPaused );
	ClassDB::bind_method( D_METHOD( "request_preview_step" ), &Terrain3DErosionJob::requestPreviewStep );
	ClassDB::bind_method( D_METHOD( "apply", "data", "force" ), &Terrain3DErosionJob::apply, DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_state" ), &Terrain3DErosionJob::getState );
	ClassDB::bind_method( D_METHOD( "get_progress" ), &Terrain3DErosionJob::getProgress );
	ClassDB::bind_method( D_METHOD( "get_stage_name" ), &Terrain3DErosionJob::getStageName );
	ClassDB::bind_method( D_METHOD( "get_error_message" ), &Terrain3DErosionJob::getErrorMessage );
	ClassDB::bind_method( D_METHOD( "is_complete" ), &Terrain3DErosionJob::isComplete );
	ClassDB::bind_method( D_METHOD( "is_source_unchanged", "data" ), &Terrain3DErosionJob::isSourceUnchanged );
	ClassDB::bind_method( D_METHOD( "was_cancelled" ), &Terrain3DErosionJob::wasCancelled );
	ClassDB::bind_method( D_METHOD( "get_effective_resolution_scale" ),
						  &Terrain3DErosionJob::getEffectiveResolutionScale );
	ClassDB::bind_method( D_METHOD( "get_completed_steps" ), &Terrain3DErosionJob::getCompletedSteps );
	ClassDB::bind_method( D_METHOD( "is_live_preview_enabled" ), &Terrain3DErosionJob::isLivePreviewEnabled );
	ClassDB::bind_method( D_METHOD( "is_preview_paused" ), &Terrain3DErosionJob::isPreviewPaused );
	ClassDB::bind_method( D_METHOD( "is_preview_waiting" ), &Terrain3DErosionJob::isPreviewWaiting );
	ClassDB::bind_method( D_METHOD( "get_preview_revision" ), &Terrain3DErosionJob::getPreviewRevision );
	ClassDB::bind_method( D_METHOD( "get_preview_step" ), &Terrain3DErosionJob::getPreviewStep );
	ClassDB::bind_method( D_METHOD( "get_simulation_steps" ), &Terrain3DErosionJob::getSimulationSteps );
	ClassDB::bind_method( D_METHOD( "get_simulation_region_size" ), &Terrain3DErosionJob::getSimulationRegionSize );
	ClassDB::bind_method( D_METHOD( "get_preview_cell_size" ), &Terrain3DErosionJob::getPreviewCellSize );
	ClassDB::bind_method( D_METHOD( "get_statistics" ), &Terrain3DErosionJob::getStatistics );
	ClassDB::bind_method( D_METHOD( "get_region_locations" ), &Terrain3DErosionJob::getRegionLocations );
	ClassDB::bind_method( D_METHOD( "get_initial_region_height_map", "region_location", "full_resolution" ),
						  &Terrain3DErosionJob::getInitialRegionHeightMap,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_preview_region_height_map", "region_location", "full_resolution" ),
						  &Terrain3DErosionJob::getPreviewRegionHeightMap,
						  DEFVAL( false ) );
	ClassDB::bind_method( D_METHOD( "get_region_output_map", "region_location", "output", "full_resolution" ),
						  &Terrain3DErosionJob::getRegionOutputMap,
						  DEFVAL( false ) );
}
