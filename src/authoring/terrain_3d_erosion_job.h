// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#pragma once

#ifndef TERRAIN3D_EROSION_JOB_CLASS_H
#define TERRAIN3D_EROSION_JOB_CLASS_H

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/ref_counted.hpp>

#include "eastl_config.h"
#include <EASTL/array.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <EASTL/vector.h>

#include "Terrain3DConstants.h"
#include "authoring/terrain_3d_erosion_settings.h"
#include "data/terrain_3d_half_buffer.h"
#include "memory/terrain_3d_linear_arena.h"

class T3DData;
typedef struct t3dErosionKernels_s t3dErosionKernels_t;


typedef enum t3dJobState_e {
	STATE_NEW,
	STATE_READY,
	STATE_RUNNING,
	STATE_COMPLETED,
	STATE_CANCELLED,
	STATE_FAILED,
	STATE_APPLIED,
} t3dJobState_t;

typedef enum t3dOutputMap_e {
	OUTPUT_HEIGHT_DELTA,
	OUTPUT_FLOW_ACCUMULATION,
	OUTPUT_SEDIMENT,
	OUTPUT_COARSE_SEDIMENT,
	OUTPUT_SAND_SEDIMENT,
	OUTPUT_FINE_SEDIMENT,
	OUTPUT_LOOSE_MATERIAL,
	OUTPUT_OXIDATION,
	OUTPUT_WEATHERING,
	OUTPUT_HYDRAULIC_DEPOSITION,
	OUTPUT_WIND_DEPOSITION,
	OUTPUT_WAVE_DEPOSITION,
	OUTPUT_TRAFFIC,
	OUTPUT_ALLUVIAL_DEPOSITION,
	OUTPUT_DEBRIS_FLOW_DEPOSITION,
	OUTPUT_DUNE_DEPOSITION,
	OUTPUT_SALT_WEATHERING,
	OUTPUT_WET_DRY_CYCLES,
	OUTPUT_EXPOSURE_AGE,
	OUTPUT_SNOWPACK,
	OUTPUT_DESERT_PAVEMENT,
	OUTPUT_STRATIGRAPHY,
	OUTPUT_MATERIAL_ID,
	OUTPUT_MAX,
} t3dOutputMap_t;

typedef enum t3dMaterialTransitionKind_e : uint8_t {
	TRANSITION_NONE,
	TRANSITION_EXPOSED,
	TRANSITION_WEATHERED,
	TRANSITION_OXIDIZED,
	TRANSITION_COARSE_DEPOSIT,
	TRANSITION_FINE_DEPOSIT,
	TRANSITION_SALT_WEATHERED,
	TRANSITION_DESERT_PAVEMENT,
	TRANSITION_VARNISHED,
} t3dMaterialTransitionKind_t;

typedef enum t3dStage_e {
	STAGE_IDLE,
	STAGE_SNAPSHOT,
	STAGE_WEATHERING,
	STAGE_HYDRAULIC,
	STAGE_DEBRIS_FLOW,
	STAGE_WIND,
	STAGE_DUNES,
	STAGE_WAVES,
	STAGE_TRAFFIC,
	STAGE_MATERIALS,
	STAGE_COMPLETE,
	STAGE_CANCELLED,
	STAGE_FAILED,
} t3dStage_t;

typedef struct t3dRegionSnapshot_s {
	Vector2i location;
	int32_t sourceOrder = 0;
	uint64_t sourceHash = 0;
	PackedByteArray heightData;
	PackedByteArray controlData;
} t3dRegionSnapshot_t;

typedef struct t3dImageSnapshot_s {
	Vector2i nSize;
	PackedByteArray rfData;

	EA_FORCE_INLINE bool isValid( void ) const { return nSize.x > 0 && nSize.y > 0 && rfData.size() >= ( int64_t ) ( nSize.x ) * ( int64_t ) ( nSize.y ) * ( int64_t ) ( sizeof( float ) ); }
	EA_FORCE_INLINE void clear( void ) { nSize = Vector2i(); rfData.clear(); }
} t3dImageSnapshot_t;

typedef struct t3dTextureErosionProfile_s {
	bool bValid = false;
	bool bEnabled = true;
	float fHardness = -1.0f;
	float fHydraulicErodibility = 1.0f;
	float fWindErodibility = 1.0f;
	float fWaveErodibility = 1.0f;
	float fWeatheringSusceptibility = 1.0f;
	float fOxidationSusceptibility = 1.0f;
	float fSaltSusceptibility = 1.0f;
	float fWetDrySusceptibility = 1.0f;
	float fInfiltrationMultiplier = 1.0f;
	float fTrafficErodibility = 1.0f;
	float fCohesion = 0.0f;
	float fGrainSize = 0.5f;
	int32_t exposedTextureId = -1;
	int32_t weatheredTextureId = -1;
	int32_t oxidizedTextureId = -1;
	int32_t coarseDepositTextureId = -1;
	int32_t fineDepositTextureId = -1;
	int32_t saltWeatheredTextureId = -1;
	int32_t pavementTextureId = -1;
	int32_t varnishedTextureId = -1;
} t3dTextureErosionProfile_t;

typedef enum t3dCurveId_e : uint8_t {
	CURVE_MASTER,
	CURVE_RAINFALL,
	CURVE_WEATHERING,
	CURVE_OXIDATION,
	CURVE_WIND,
	CURVE_WAVE,
	CURVE_TRAFFIC,
	CURVE_TEMPERATURE,
	CURVE_SEA_LEVEL,
	CURVE_SNOWMELT,
	CURVE_SALT_WEATHERING,
	CURVE_DEBRIS_FLOW,
	CURVE_COUNT,
} t3dCurveId_t;

typedef struct t3dConfig_s {
	double fSimulationYears = 1000.0;
	int32_t simulationSteps = 128;
	int32_t requestedResolutionScale = 8;
	bool bAutoAdjustResolution = true;
	int32_t memoryBudgetMb = 1024;
	bool bLivePreviewEnabled = false;
	int32_t livePreviewIntervalSteps = 4;
	bool bLivePreviewStartPaused = false;
	int32_t nThreadCount = 0;
	uint64_t seed = 1;
	t3dBoundaryMode_t boundaryMode = BOUNDARY_CLOSED;
	float fMaxHeightChangePerStep = 0.5f;
	bool bPreserveHoles = true;
	bool bApplyHeight = true;
	float fHeightOutputStrength = 1.0f;
	float fGlobalIntensity = 1.0f;
	Rect2 simulationMaskWorldRect;
	bool bInvertSimulationMask = false;
	float fEdgeFadeDistanceM = 0.0f;
	bool bHeightRangeEnabled = false;
	float fMinimumHeightM = -1000000.0f;
	float fMaximumHeightM = 1000000.0f;
	float fHeightRangeFalloffM = 0.0f;

	bool bHydraulicEnabled = true;
	float fRainfallMPerYear = 0.002f;
	float fRainfallVariation = 0.25f;
	float fEvaporationFraction = 0.18f;
	float fInfiltrationFraction = 0.05f;
	float fFlowRate = 0.65f;
	float fSedimentCapacity = 1.25f;
	float fHydraulicErosionMPerYear = 0.0002f;
	float fHydraulicDepositionRate = 0.35f;
	float fMinimumSlope = 0.001f;
	float fStormFrequency = 1.0f;
	float fStormIntensity = 1.0f;
	float fChannelIncisionStrength = 0.0f;
	float fStandingWaterDepositionRate = 0.0f;
	bool bSedimentSortingEnabled = true;
	float fCoarseTransportMultiplier = 0.35f;
	float fSandTransportMultiplier = 1.0f;
	float fFineTransportMultiplier = 1.5f;
	float fCoarseSettlingRate = 0.75f;
	float fSandSettlingRate = 0.35f;
	float fFineSettlingRate = 0.08f;
	bool bAlluvialFanEnabled = false;
	float fAlluvialFanStrength = 0.75f;
	float fAlluvialFanSlopeDrop = 0.02f;
	float fAlluvialFanSpread = 0.35f;
	bool bDebrisFlowEnabled = false;
	float fDebrisFlowConcentrationThreshold = 0.35f;
	float fDebrisFlowSlopeThreshold = 0.02f;
	float fDebrisFlowRate = 0.35f;
	float fDebrisFlowDepositionRate = 0.65f;
	int32_t debrisFlowRunoutCells = 4;

	bool bWeatheringEnabled = true;
	float fPhysicalWeatheringMPerYear = 0.00005f;
	float fChemicalWeatheringMPerYear = 0.00002f;
	float fFreezeThawStrength = 0.25f;
	float fTalusAngleDegrees = 35.0f;
	float fThermalCreepRate = 0.2f;
	float fBaseHardness = 0.72f;
	float fHardnessVariation = 0.12f;
	bool bOxidationEnabled = true;
	float fOxidationRatePerYear = 0.0001f;
	float fOxidationWeakening = 0.2f;
	float fWeatheringSlopeBias = 1.0f;
	float fChemicalMoistureExponent = 1.0f;
	float fFreezePoint = 0.0f;
	bool bWetDryEnabled = false;
	float fWetDryWeatheringMPerCycle = 0.0005f;
	float fWetThreshold = 0.05f;
	float fDryThreshold = 0.01f;
	bool bSaltWeatheringEnabled = false;
	Rect2 salinityWorldRect;
	float fBackgroundSalinity = 0.0f;
	float fSaltWeatheringMPerYear = 0.00005f;
	float fSaltCrystallizationStrength = 1.0f;
	float fSaltEvaporationBias = 1.0f;
	float fSaltCohesionLoss = 0.25f;
	bool bExposureAgeEnabled = false;
	float fVarnishMaturationYears = 5000.0f;
	float fExposureResetDepthM = 0.02f;
	float fVarnishStabilitySlope = 0.25f;
	float fVarnishOxidationMultiplier = 1.0f;
	bool bSnowmeltEnabled = false;
	float fSnowfallFraction = 0.65f;
	float fSnowAccumulationTemperature = 0.0f;
	float fSnowMeltTemperature = 0.1f;
	float fSnowMeltFractionPerStep = 0.25f;
	float fSnowSublimationFraction = 0.02f;

	bool bWindEnabled = false;
	Vector2 windDirection = Vector2( 1.0f, 0.0f );
	float fWindStrength = 1.0f;
	float fWindVariation = 0.2f;
	float fWindErosionMPerYear = 0.00005f;
	float fWindTransportRate = 0.25f;
	float fWindDepositionRate = 0.4f;
	float fWindShelterStrength = 0.6f;
	float fWindThreshold = 0.0f;
	int32_t windTransportDistanceCells = 1;
	float fWindDepositionSlopeBias = 0.0f;
	bool bDuneEnabled = false;
	float fSandReposeAngleDegrees = 33.0f;
	float fDuneAvalancheRate = 0.35f;
	int32_t duneRelaxationIterations = 2;
	float fDuneMinSandDepthM = 0.02f;
	bool bDesertPavementEnabled = false;
	float fPavementFormationRate = 0.25f;
	float fPavementFineRemovalThreshold = 0.4f;
	float fPavementArmorStrength = 0.65f;
	float fPavementInfiltrationReduction = 0.25f;

	bool bWaveEnabled = false;
	float fSeaLevel = 0.0f;
	float fTidalRange = 2.0f;
	Vector2 waveDirection = Vector2( 1.0f, 0.0f );
	float fWaveStrength = 1.0f;
	float fWaveErosionMPerYear = 0.001f;
	float fWaveDepositionRate = 0.65f;
	float fCoastlineBandM = 12.0f;
	float fWaveFetchDistanceM = 512.0f;
	float fWaveUndercutBias = 1.5f;
	float fBeachSmoothing = 0.0f;

	bool bTrafficEnabled = false;
	Rect2 trafficWorldRect;
	float fTrafficErosionMPerYear = 0.0002f;
	float fTrafficCompaction = 0.75f;
	float fTrafficSmoothing = 0.2f;
	float fTrafficRunoffMultiplier = 1.5f;
	float fTrafficThreshold = 0.0f;
	int32_t trafficWideningCells = 0;

	bool bStratigraphyEnabled = false;
	float fSurfaceLayerDepthM = 2.0f;
	Rect2 surfaceLayerDepthWorldRect;
	float fBedrockHardness = 0.9f;
	Rect2 bedrockHardnessWorldRect;
	float fStratigraphyTransitionWidthM = 0.5f;

	bool bMaterialOutputEnabled = false;
	bool bPreserveControlFlags = true;
	bool bResolveAutoMaterials = true;
	float fMaterialTransitionStrength = 1.0f;
	float fMaterialTransitionThreshold = 0.05f;
	float fMaterialTransitionDepthM = 0.25f;
	bool bAutoShaderEnabled = false;
	int32_t autoBaseTexture = 0;
	int32_t autoOverlayTexture = 1;
	float fAutoSlope = 1.0f;
	float fAutoHeightReduction = 0.0f;
	eastl::array<t3dTextureErosionProfile_t, TERRAIN3D_EROSION_TEXTURE_PROFILE_COUNT> szTextureProfiles;
	bool bOxidationColorEnabled = false;
	Color oxidationTint = Color( 0.45f, 0.19f, 0.07f, 1.0f );
	float fOxidationTintStrength = 0.45f;

	eastl::vector<float> fCurveSamples;
	int32_t curveStride = 0;
} t3dConfig_t;

class T3DErosionJob : public RefCounted {
	GDCLASS( T3DErosionJob, RefCounted );
	CLASS_NAME();
	friend class T3DData;
	friend struct t3dErosionKernels_s;

public:




private:








	using FloatBuffer = T3DArenaBuffer<float>;
	using HalfBuffer = T3DHalfBuffer;
	using ByteBuffer = T3DArenaBuffer<uint8_t>;
	using IndexBuffer = T3DArenaBuffer<int64_t>;







	t3dConfig_t m_config;
	eastl::vector<t3dRegionSnapshot_t> m_regions;
	eastl::vector<eastl::array<int32_t, 4>> m_szRegionNeighbors;
	int32_t m_szRegionLookup[TERRAIN3D_EROSION_REGION_LOOKUP_CAPACITY] = {};
	int32_t m_nSourceRegionSize = 0;
	int32_t m_nSimulationRegionSize = 0;
	int32_t m_nSimulationRegionShift = 0;
	int32_t m_nSimulationRegionMask = 0;
	int32_t m_nCellsPerRegionShift = 0;
	size_t m_nCellsPerRegionMask = 0u;
	int32_t m_resolutionScale = 1;
	float m_fVertexSpacing = 1.0f;
	float m_fCellSize = 1.0f;
	size_t m_nCellsPerRegion = 0;
	size_t m_nCellCount = 0;
	Rect2 m_terrainWorldRect;
	t3dImageSnapshot_t m_simulationMaskSnapshot;
	t3dImageSnapshot_t m_trafficMaskSnapshot;
	t3dImageSnapshot_t m_salinityMaskSnapshot;
	t3dImageSnapshot_t m_surfaceLayerDepthSnapshot;
	t3dImageSnapshot_t m_bedrockHardnessSnapshot;
	uint64_t m_nInputSnapshotBytes = 0;
	uint64_t m_nSourceControlSnapshotBytes = 0;

	// The persistent and scratch maps use independent exact-size slabs. Both
	// reject upstream allocations, so the configured budget is enforceable.
	T3DLinearArena m_persistentMemory;
	T3DLinearArena m_phaseMemory;
	T3DLinearArena m_resultMemory;
	T3DLinearArena m_previewMemory;
	bool m_bResultsCompacted = false;
	uint64_t m_nRetainedResultBytes = 0u;

	ByteBuffer m_active;
	FloatBuffer m_simulationWeight;
	FloatBuffer m_initialHeight;
	FloatBuffer m_height;
	FloatBuffer m_water;
	FloatBuffer m_sedimentCoarse;
	FloatBuffer m_sedimentSand;
	FloatBuffer m_sedimentFine;
	FloatBuffer m_surfaceCoarse;
	FloatBuffer m_surfaceSand;
	FloatBuffer m_surfaceFine;
	HalfBuffer m_substrateHardness;
	FloatBuffer m_hardness;
	FloatBuffer m_oxidation;
	FloatBuffer m_flowAccumulation;
	FloatBuffer m_hydraulicDeposition;
	FloatBuffer m_windDeposition;
	FloatBuffer m_waveDeposition;
	FloatBuffer m_weathering;
	FloatBuffer m_traffic;
	FloatBuffer m_compaction;
	FloatBuffer m_alluvialDeposition;
	FloatBuffer m_debrisFlowDeposition;
	FloatBuffer m_duneDeposition;
	FloatBuffer m_saltWeathering;
	FloatBuffer m_wetDryCycles;
	FloatBuffer m_previousMoisture;
	ByteBuffer m_wetState;
	HalfBuffer m_salinity;
	FloatBuffer m_snowpack;
	FloatBuffer m_exposureAge;
	FloatBuffer m_desertPavement;
	FloatBuffer m_cumulativeErosion;
	FloatBuffer m_surfaceLayerDepth;
	HalfBuffer m_bedrockHardness;

	ByteBuffer m_dominantTexture;
	HalfBuffer m_hydraulicErodibility;
	HalfBuffer m_windErodibility;
	HalfBuffer m_waveErodibility;
	HalfBuffer m_weatheringSusceptibility;
	HalfBuffer m_oxidationSusceptibility;
	HalfBuffer m_saltSusceptibility;
	HalfBuffer m_wetDrySusceptibility;
	HalfBuffer m_infiltrationMultiplier;
	HalfBuffer m_trafficErodibility;
	HalfBuffer m_cohesion;
	HalfBuffer m_grainSize;

	ByteBuffer m_materialTransitionKind;
	FloatBuffer m_materialTransitionAmount;

	// Solver scratch is allocated with the persistent channels in one guarded
	// phase. run() and snapshot construction do not issue additional large heap
	// allocations, and next_a is reused for traffic-mask dilation.
	eastl::array<FloatBuffer, 4> m_szFlux;
	FloatBuffer m_nextA;
	FloatBuffer m_nextB;
	FloatBuffer m_nextCoarse;
	FloatBuffer m_nextSand;
	FloatBuffer m_nextFine;
	FloatBuffer m_transferAmount;
	FloatBuffer m_transferCoarse;
	FloatBuffer m_transferSand;
	FloatBuffer m_transferFine;
	IndexBuffer m_transferDestination;
	FloatBuffer m_stepHeightStart;
	FloatBuffer m_previewHeight;

	mutable std::mutex m_previewMutex;
	std::condition_variable m_previewCondition;
	std::atomic<int32_t> m_previewRevision{0};
	std::atomic<int32_t> m_previewStep{0};
	std::atomic<bool> m_bPreviewPaused{false};
	std::atomic<bool> m_bPreviewWaiting{false};
	std::atomic<int32_t> m_previewStepRequests{0};

	uint64_t m_assetProfileHash = 0;

	std::atomic<int32_t> m_state{STATE_NEW};
	std::atomic<int32_t> m_stage{STAGE_IDLE};
	std::atomic<float> m_fProgress{0.0f};
	std::atomic<bool> m_bCancelRequested{false};
	mutable std::mutex m_statusMutex;
	std::mutex m_applyMutex;
	String m_errorMessage;
	double m_fTotalEroded = 0.0;
	double m_fTotalDeposited = 0.0;
	float m_fMaximumLowering = 0.0f;
	float m_fMaximumRaising = 0.0f;
	std::atomic<int32_t> m_nCompletedSteps{0};

	Error setup( T3DData *pData, const Ref<T3DErosionSettings> &rSettings );
	void copyConfig( const Ref<T3DErosionSettings> &rSettings );
	void sampleCurves( const Ref<T3DErosionSettings> &rSettings );
	float sampleCurve( t3dCurveId_t curve, int32_t nStep ) const;
	int32_t selectResolutionScale( int32_t nRegionCount ) const;
	uint64_t estimateMemoryBytes( int32_t nRegionCount, int32_t nScale ) const;
	bool allocateBuffers( void );
	bool compactCompletedResults( void );
	bool captureSourceRegions( T3DData *pData );
	static bool regionSnapshotLess( const t3dRegionSnapshot_t &rA, const t3dRegionSnapshot_t &rB );
	static float sampleImageSnapshot(
		const t3dImageSnapshot_t &rImage, const Rect2 &rWorldRect,
		const Vector2 &rWorldPosition
	);
	bool captureImageSnapshot( const Ref<Image> &rImage, t3dImageSnapshot_t &rSnapshot, const char *pszLabel );
	bool buildSimulationSnapshot( void );
	void releaseSourceSnapshots( void );
	bool verifySourceUnchanged( T3DData *pData ) const;
	uint64_t hashRegionImages( const Ref<Image> &rHeight, const Ref<Image> &rControl ) const;
	int64_t neighborIndex( size_t iIndex, int32_t nDx, int32_t nDy ) const;
	uint64_t stochasticIndex( size_t iIndex ) const;
	float getSlope( size_t iIndex ) const;
	float sampleOutputCell(
		const FloatBuffer &rMap, int32_t nRegionId, int32_t iBaseX, int32_t iBaseY,
		int32_t iX, int32_t iY
	) const;
	float sampleOutput( const FloatBuffer &rMap, int32_t nRegionId, float fSourceX, float fSourceY ) const;
	bool snapshotTextureProfiles( T3DData *pData );
	uint64_t hashTextureProfiles( T3DData *pData ) const;
	int32_t getTransitionTarget( int32_t nTextureId, t3dMaterialTransitionKind_t kind ) const;
	void partitionMaterial( float fGrainSize, float fAmount, float &fCoarse, float &fSand, float &fFine ) const;
	float getLooseMaterial( size_t iIndex ) const;
	float getSediment( size_t iIndex ) const;
	float entrainSurface( size_t iIndex, float fAmount, float &fCoarse, float &fSand, float &fFine );
	void depositSurface( size_t iIndex, float fCoarse, float fSand, float fFine );
	void buildMaterialTransitions( void );
	void setErrorMessage( const String &rMessage );
	void setFailed( const String &rMessage );
	void setStage( t3dStage_t stage, float fProgress );
	bool allocatePreviewBuffer( void );
	void publishPreviewSnapshot( int32_t nStep, bool bForce = false );
	bool waitForPreviewPermission( void );
	Ref<Image> makeHeightImage( const FloatBuffer &rSource, int32_t nRegionId, bool bFullResolution ) const;

public:
	T3DErosionJob( void );
	~T3DErosionJob( void ) = default;

	void run( void );
	void cancel( void );
	void setPreviewPaused( bool bPaused );
	void requestPreviewStep( void );
	Error apply( T3DData *pData, bool bForce = false );
	EA_FORCE_INLINE t3dJobState_t getState( void ) const { return ( t3dJobState_t ) ( m_state.load() ); }
	EA_FORCE_INLINE float getProgress( void ) const { return m_fProgress.load(); }
	String getStageName( void ) const;
	String getErrorMessage( void ) const;
	bool isComplete( void ) const;
	bool isSourceUnchanged( T3DData *pData ) const;
	EA_FORCE_INLINE bool wasCancelled( void ) const { return m_state.load() == STATE_CANCELLED; }
	EA_FORCE_INLINE int32_t getEffectiveResolutionScale( void ) const { return m_resolutionScale; }
	EA_FORCE_INLINE int32_t getCompletedSteps( void ) const { return m_nCompletedSteps.load(); }
	EA_FORCE_INLINE bool isLivePreviewEnabled( void ) const { return m_config.bLivePreviewEnabled; }
	EA_FORCE_INLINE bool isPreviewPaused( void ) const { return m_bPreviewPaused.load(); }
	EA_FORCE_INLINE bool isPreviewWaiting( void ) const { return m_bPreviewWaiting.load(); }
	EA_FORCE_INLINE int32_t getPreviewRevision( void ) const { return m_previewRevision.load(); }
	EA_FORCE_INLINE int32_t getPreviewStep( void ) const { return m_previewStep.load(); }
	EA_FORCE_INLINE int32_t getSimulationSteps( void ) const { return m_config.simulationSteps; }
	EA_FORCE_INLINE int32_t getSimulationRegionSize( void ) const { return m_nSimulationRegionSize; }
	EA_FORCE_INLINE float getPreviewCellSize( void ) const { return m_fCellSize; }
	Dictionary getStatistics( void ) const;
	TypedArray<Vector2i> getRegionLocations( void ) const;
	Ref<Image> getInitialRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref<Image> getPreviewRegionHeightMap( const Vector2i &rRegionLocation, bool bFullResolution = false ) const;
	Ref<Image> getRegionOutputMap(
		const Vector2i &rRegionLocation, t3dOutputMap_t output,
		bool bFullResolution = false
	) const;

protected:
	static void _bind_methods( void );
};

VARIANT_ENUM_CAST( t3dJobState_t );
VARIANT_ENUM_CAST( t3dOutputMap_t );
#endif // TERRAIN3D_EROSION_JOB_CLASS_H
