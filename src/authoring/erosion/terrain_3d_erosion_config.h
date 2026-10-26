// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_EROSION_CONFIG_H
#define TERRAIN3D_EROSION_CONFIG_H

#include "Terrain3DTypes.h"

#include "Terrain3DConstants.h"

#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/vector2.hpp>

// t3dBoundaryMode_e centralized in Terrain3DTypes.h.


// Canonical value-only erosion configuration. Terrain3DErosionSettings owns this
// snapshot directly; Terrain3DErosionJob copies it before asynchronous work starts.
// Resource references (masks/curves) remain settings-owned and are captured/sampled
// separately so the worker configuration stays detached from mutable Godot objects.
typedef struct t3dErosionConfigSnapshot_s {

	// General.
	double m_fSimulationYears = 1000.0;
	int32_t m_nSimulationSteps = 128;
	int32_t m_nResolutionScale = 8;
	bool m_bAutoAdjustResolution = true;
	int32_t m_nMemoryBudgetMb = 1024;
	bool m_bLivePreviewEnabled = true;
	int32_t m_nLivePreviewIntervalSteps = 4;
	bool m_bLivePreviewStartPaused = false;
	int32_t m_nThreadCount = 0;
	int64_t m_nSeed = 1;
	t3dBoundaryMode_t m_boundaryMode = BOUNDARY_CLOSED;
	float m_fMaxHeightChangePerStep = 0.5f;
	bool m_bPreserveHoles = true;
	bool m_bApplyHeight = true;
	float m_fHeightOutputStrength = 1.0f;
	float m_fGlobalIntensity = 1.0f;
	Rect2 m_simulationMaskWorldRect;
	bool m_bInvertSimulationMask = false;
	float m_fEdgeFadeDistanceM = 0.0f;
	bool m_bHeightRangeEnabled = false;
	float m_fMinimumHeightM = -1000000.0f;
	float m_fMaximumHeightM = 1000000.0f;
	float m_fHeightRangeFalloffM = 0.0f;

	// Hydraulic erosion.
	bool m_bHydraulicEnabled = true;
	float m_fRainfallMPerYear = 0.002f;
	float m_fRainfallVariation = 0.25f;
	float m_fEvaporationFraction = 0.18f;
	float m_fInfiltrationFraction = 0.05f;
	float m_fFlowRate = 0.65f;
	float m_fSedimentCapacity = 1.25f;
	float m_fHydraulicErosionMPerYear = 0.0002f;
	float m_fHydraulicDepositionRate = 0.35f;
	float m_fMinimumSlope = 0.001f;
	float m_fStormFrequency = 1.0f;
	float m_fStormIntensity = 1.0f;
	float m_fChannelIncisionStrength = 0.0f;
	float m_fStandingWaterDepositionRate = 0.0f;

	// Desert sediment sorting, fans, and debris flows.
	bool m_bSedimentSortingEnabled = true;
	float m_fCoarseTransportMultiplier = 0.35f;
	float m_fSandTransportMultiplier = 1.0f;
	float m_fFineTransportMultiplier = 1.5f;
	float m_fCoarseSettlingRate = 0.75f;
	float m_fSandSettlingRate = 0.35f;
	float m_fFineSettlingRate = 0.08f;
	bool m_bAlluvialFanEnabled = false;
	float m_fAlluvialFanStrength = 0.75f;
	float m_fAlluvialFanSlopeDrop = 0.02f;
	float m_fAlluvialFanSpread = 0.35f;
	bool m_bDebrisFlowEnabled = false;
	float m_fDebrisFlowConcentrationThreshold = 0.35f;
	float m_fDebrisFlowSlopeThreshold = 0.02f;
	float m_fDebrisFlowRate = 0.35f;
	float m_fDebrisFlowDepositionRate = 0.65f;
	int32_t m_nDebrisFlowRunoutCells = 4;

	// Weathering and oxidation.
	bool m_bWeatheringEnabled = true;
	float m_fPhysicalWeatheringMPerYear = 0.00005f;
	float m_fChemicalWeatheringMPerYear = 0.00002f;
	float m_fFreezeThawStrength = 0.25f;
	float m_fTalusAngleDegrees = 35.0f;
	float m_fThermalCreepRate = 0.2f;
	float m_fBaseHardness = 0.72f;
	float m_fHardnessVariation = 0.12f;
	bool m_bOxidationEnabled = true;
	float m_fOxidationRatePerYear = 0.0001f;
	float m_fOxidationWeakening = 0.2f;
	float m_fWeatheringSlopeBias = 1.0f;
	float m_fChemicalMoistureExponent = 1.0f;
	float m_fFreezePoint = 0.0f;

	// Arid weathering, salinity, exposure age, and seasonal melt.
	bool m_bWetDryEnabled = false;
	float m_fWetDryWeatheringMPerCycle = 0.0005f;
	float m_fWetThreshold = 0.05f;
	float m_fDryThreshold = 0.01f;
	bool m_bSaltWeatheringEnabled = false;
	Rect2 m_salinityWorldRect;
	float m_fBackgroundSalinity = 0.0f;
	float m_fSaltWeatheringMPerYear = 0.00005f;
	float m_fSaltCrystallizationStrength = 1.0f;
	float m_fSaltEvaporationBias = 1.0f;
	float m_fSaltCohesionLoss = 0.25f;
	bool m_bExposureAgeEnabled = false;
	float m_fVarnishMaturationYears = 5000.0f;
	float m_fExposureResetDepthM = 0.02f;
	float m_fVarnishStabilitySlope = 0.25f;
	float m_fVarnishOxidationMultiplier = 1.0f;
	bool m_bSnowmeltEnabled = false;
	float m_fSnowfallFraction = 0.65f;
	float m_fSnowAccumulationTemperature = 0.0f;
	float m_fSnowMeltTemperature = 0.1f;
	float m_fSnowMeltFractionPerStep = 0.25f;
	float m_fSnowSublimationFraction = 0.02f;

	// Wind.
	bool m_bWindEnabled = false;
	Vector2 m_windDirection = Vector2( 1.0f, 0.0f );
	float m_fWindStrength = 1.0f;
	float m_fWindVariation = 0.2f;
	float m_fWindErosionMPerYear = 0.00005f;
	float m_fWindTransportRate = 0.25f;
	float m_fWindDepositionRate = 0.4f;
	float m_fWindShelterStrength = 0.6f;
	float m_fWindThreshold = 0.0f;
	int32_t m_nWindTransportDistanceCells = 1;
	float m_fWindDepositionSlopeBias = 0.0f;

	// Dune relaxation and desert pavement.
	bool m_bDuneEnabled = false;
	float m_fSandReposeAngleDegrees = 33.0f;
	float m_fDuneAvalancheRate = 0.35f;
	int32_t m_nDuneRelaxationIterations = 2;
	float m_fDuneMinSandDepthM = 0.02f;
	bool m_bDesertPavementEnabled = false;
	float m_fPavementFormationRate = 0.25f;
	float m_fPavementFineRemovalThreshold = 0.4f;
	float m_fPavementArmorStrength = 0.65f;
	float m_fPavementInfiltrationReduction = 0.25f;

	// Waves.
	bool m_bWaveEnabled = false;
	float m_fSeaLevel = 0.0f;
	float m_fTidalRange = 2.0f;
	Vector2 m_waveDirection = Vector2( 1.0f, 0.0f );
	float m_fWaveStrength = 1.0f;
	float m_fWaveErosionMPerYear = 0.001f;
	float m_fWaveDepositionRate = 0.65f;
	float m_fCoastlineBandM = 12.0f;
	float m_fWaveFetchDistanceM = 512.0f;
	float m_fWaveUndercutBias = 1.5f;
	float m_fBeachSmoothing = 0.0f;

	// Foot traffic.
	bool m_bTrafficEnabled = false;
	Rect2 m_trafficWorldRect;
	float m_fTrafficErosionMPerYear = 0.0002f;
	float m_fTrafficCompaction = 0.75f;
	float m_fTrafficSmoothing = 0.2f;
	float m_fTrafficRunoffMultiplier = 1.5f;
	float m_fTrafficThreshold = 0.0f;
	int32_t m_nTrafficWideningCells = 0;

	// Layered substrate / stratigraphy.
	bool m_bStratigraphyEnabled = false;
	float m_fSurfaceLayerDepthM = 2.0f;
	Rect2 m_surfaceLayerDepthWorldRect;
	float m_fBedrockHardness = 0.9f;
	Rect2 m_bedrockHardnessWorldRect;
	float m_fStratigraphyTransitionWidthM = 0.5f;

	// Material transitions.
	bool m_bMaterialOutputEnabled = false;
	bool m_bPreserveControlFlags = true;
	bool m_bResolveAutoMaterials = true;
	float m_fMaterialTransitionStrength = 1.0f;
	float m_fMaterialTransitionThreshold = 0.05f;
	float m_fMaterialTransitionDepthM = 0.25f;
	bool m_bOxidationColorEnabled = false;
	Color m_oxidationTint = Color( 0.45f, 0.19f, 0.07f, 1.0f );
	float m_fOxidationTintStrength = 0.45f;

} t3dErosionConfigSnapshot_t;


#endif // TERRAIN3D_EROSION_CONFIG_H
