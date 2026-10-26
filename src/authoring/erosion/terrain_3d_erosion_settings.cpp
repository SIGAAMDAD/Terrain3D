// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "authoring/erosion/terrain_3d_erosion_settings.h"

#include <godot_cpp/core/class_db.hpp>

static inline int32_t t3dErosionSettingsSanitizeResolutionScale( int32_t nValue ) {
	if ( nValue <= 1 ) {
		return 1;
	}
	if ( nValue <= 2 ) {
		return 2;
	}
	if ( nValue <= 4 ) {
		return 4;
	}
	if ( nValue <= 8 ) {
		return 8;
	}
	return 16;
}

Ref<Curve> Terrain3DErosionSettings::makeFlatCurve( const float fValue, const float fMin, const float fMax ) {
	Ref<Curve> pCurve;
	pCurve.instantiate();
	pCurve->set_min_value( fMin );
	pCurve->set_max_value( fMax );
	pCurve->add_point( Vector2( 0.0f, fValue ) );
	pCurve->add_point( Vector2( 1.0f, fValue ) );
	return pCurve;
}

void Terrain3DErosionSettings::setCurve( Ref<Curve> &rTargetCurve, const Ref<Curve> &rSourceCurve ) {
	if ( rTargetCurve == rSourceCurve ) {
		return;
	}
	rTargetCurve = rSourceCurve;
	emit_changed();
}

Terrain3DErosionSettings::Terrain3DErosionSettings( void ) {
	m_masterIntensityCurve = makeFlatCurve( 1.0f );
	m_rainfallCurve = makeFlatCurve( 1.0f );
	m_weatheringCurve = makeFlatCurve( 1.0f );
	m_oxidationCurve = makeFlatCurve( 1.0f );
	m_windCurve = makeFlatCurve( 1.0f );
	m_waveCurve = makeFlatCurve( 1.0f );
	m_trafficCurve = makeFlatCurve( 1.0f );
	m_temperatureCurve = makeFlatCurve( 0.5f, -1.0f, 1.0f );
	m_seaLevelCurve = makeFlatCurve( 0.0f, -1000000.0f, 1000000.0f );
	m_snowmeltCurve = makeFlatCurve( 1.0f );
	m_saltWeatheringCurve = makeFlatCurve( 1.0f );
	m_debrisFlowCurve = makeFlatCurve( 1.0f );
}

#define SET_CLAMPED( methodName, methodSymbol, field_name, value_type, min_value, max_value )                                              \
	void Terrain3DErosionSettings::methodSymbol( value_type valueArg ) {                                                                   \
		value_type value = CLAMP( valueArg, value_type( min_value ), value_type( max_value ) );                                            \
		if ( field_name == value ) {                                                                                                       \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = value;                                                                                                                \
		emit_changed();                                                                                                                    \
	}

#define SET_VALUE( methodName, methodSymbol, field_name, value_type )                                                                      \
	void Terrain3DErosionSettings::methodSymbol( value_type valueArg ) {                                                                   \
		if ( field_name == valueArg ) {                                                                                                    \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = valueArg;                                                                                                             \
		emit_changed();                                                                                                                    \
	}

#define SET_REF_VALUE( methodName, methodSymbol, field_name, value_type )                                                                  \
	void Terrain3DErosionSettings::methodSymbol( const value_type &valueArg ) {                                                            \
		if ( field_name == valueArg ) {                                                                                                    \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = valueArg;                                                                                                             \
		emit_changed();                                                                                                                    \
	}

#define SET_INSPECTOR_TOGGLE( methodName, methodSymbol, field_name )                                                                       \
	void Terrain3DErosionSettings::methodSymbol( bool bValueArg ) {                                                                        \
		if ( field_name == bValueArg ) {                                                                                                   \
			return;                                                                                                                        \
		}                                                                                                                                  \
		field_name = bValueArg;                                                                                                            \
		emit_changed();                                                                                                                    \
		notify_property_list_changed();                                                                                                    \
	}

SET_CLAMPED( simulation_years, setSimulationYears, m_config.m_fSimulationYears, double, 0.000001, 1000000000.0 )
SET_CLAMPED( simulation_steps, setSimulationSteps, m_config.m_nSimulationSteps, int32_t, 1, 4096 )
void Terrain3DErosionSettings::setResolutionScale( const int32_t nValue ) {
	const int32_t value = t3dErosionSettingsSanitizeResolutionScale( nValue );
	if ( m_config.m_nResolutionScale != value ) {
		m_config.m_nResolutionScale = value;
		emit_changed();
	}
}
SET_VALUE( auto_adjust_resolution, setAutoAdjustResolution, m_config.m_bAutoAdjustResolution, bool )
SET_CLAMPED( memory_budget_mb, setMemoryBudgetMb, m_config.m_nMemoryBudgetMb, int32_t, 128, 32768 )
SET_VALUE( live_preview_enabled, setLivePreviewEnabled, m_config.m_bLivePreviewEnabled, bool )
SET_CLAMPED( live_preview_interval_steps, setLivePreviewIntervalSteps, m_config.m_nLivePreviewIntervalSteps, int32_t, 1, 256 )
SET_VALUE( live_preview_start_paused, setLivePreviewStartPaused, m_config.m_bLivePreviewStartPaused, bool )
SET_CLAMPED( thread_count, setThreadCount, m_config.m_nThreadCount, int32_t, 0, 64 )
SET_VALUE( seed, setSeed, m_config.m_nSeed, int64_t )
void Terrain3DErosionSettings::setBoundaryMode( const t3dBoundaryMode_t eValue ) {
	const t3dBoundaryMode_t value =
	        ( t3dBoundaryMode_t )( CLAMP( ( int32_t )( eValue ), ( int32_t )( BOUNDARY_CLOSED ), ( int32_t )( BOUNDARY_OPEN ) ) );
	if ( m_config.m_boundaryMode != value ) {
		m_config.m_boundaryMode = value;
		emit_changed();
	}
}
SET_CLAMPED( max_height_change_per_step, setMaxHeightChangePerStep, m_config.m_fMaxHeightChangePerStep, float, 0.000001f, 10000.0f )
SET_VALUE( preserve_holes, setPreserveHoles, m_config.m_bPreserveHoles, bool )
SET_INSPECTOR_TOGGLE( apply_height, setApplyHeight, m_config.m_bApplyHeight )
SET_CLAMPED( height_output_strength, setHeightOutputStrength, m_config.m_fHeightOutputStrength, float, 0.0f, 10.0f )
SET_CLAMPED( global_intensity, setGlobalIntensity, m_config.m_fGlobalIntensity, float, 0.0f, 10.0f )
SET_REF_VALUE( simulation_mask, setSimulationMask, m_simulationMask, Ref<Image> )
SET_REF_VALUE( simulation_mask_world_rect, setSimulationMaskWorldRect, m_config.m_simulationMaskWorldRect, Rect2 )
SET_VALUE( invert_simulation_mask, setInvertSimulationMask, m_config.m_bInvertSimulationMask, bool )
SET_CLAMPED( edge_fade_distance_m, setEdgeFadeDistanceM, m_config.m_fEdgeFadeDistanceM, float, 0.0f, 10000000.0f )
SET_INSPECTOR_TOGGLE( height_range_enabled, setHeightRangeEnabled, m_config.m_bHeightRangeEnabled )
SET_CLAMPED( minimum_height_m, setMinimumHeightM, m_config.m_fMinimumHeightM, float, -1000000.0f, 1000000.0f )
SET_CLAMPED( maximum_height_m, setMaximumHeightM, m_config.m_fMaximumHeightM, float, -1000000.0f, 1000000.0f )
SET_CLAMPED( height_range_falloff_m, setHeightRangeFalloffM, m_config.m_fHeightRangeFalloffM, float, 0.0f, 1000000.0f )

SET_INSPECTOR_TOGGLE( hydraulic_enabled, setHydraulicEnabled, m_config.m_bHydraulicEnabled )
SET_CLAMPED( rainfall_m_per_year, setRainfallMPerYear, m_config.m_fRainfallMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( rainfall_variation, setRainfallVariation, m_config.m_fRainfallVariation, float, 0.0f, 1.0f )
SET_CLAMPED( evaporation_fraction, setEvaporationFraction, m_config.m_fEvaporationFraction, float, 0.0f, 1.0f )
SET_CLAMPED( infiltration_fraction, setInfiltrationFraction, m_config.m_fInfiltrationFraction, float, 0.0f, 1.0f )
SET_CLAMPED( flow_rate, setFlowRate, m_config.m_fFlowRate, float, 0.0f, 10.0f )
SET_CLAMPED( sediment_capacity, setSedimentCapacity, m_config.m_fSedimentCapacity, float, 0.0f, 1000.0f )
SET_CLAMPED( hydraulic_erosion_m_per_year, setHydraulicErosionMPerYear, m_config.m_fHydraulicErosionMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( hydraulic_deposition_rate, setHydraulicDepositionRate, m_config.m_fHydraulicDepositionRate, float, 0.0f, 1.0f )
SET_CLAMPED( minimum_slope, setMinimumSlope, m_config.m_fMinimumSlope, float, 0.0f, 10.0f )
SET_CLAMPED( storm_frequency, setStormFrequency, m_config.m_fStormFrequency, float, 0.0f, 1.0f )
SET_CLAMPED( storm_intensity, setStormIntensity, m_config.m_fStormIntensity, float, 0.0f, 100.0f )
SET_CLAMPED( channel_incision_strength, setChannelIncisionStrength, m_config.m_fChannelIncisionStrength, float, 0.0f, 16.0f )
SET_CLAMPED( standing_water_deposition_rate, setStandingWaterDepositionRate, m_config.m_fStandingWaterDepositionRate, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( sediment_sorting_enabled, setSedimentSortingEnabled, m_config.m_bSedimentSortingEnabled )
SET_CLAMPED( coarse_transport_multiplier, setCoarseTransportMultiplier, m_config.m_fCoarseTransportMultiplier, float, 0.0f, 8.0f )
SET_CLAMPED( sand_transport_multiplier, setSandTransportMultiplier, m_config.m_fSandTransportMultiplier, float, 0.0f, 8.0f )
SET_CLAMPED( fine_transport_multiplier, setFineTransportMultiplier, m_config.m_fFineTransportMultiplier, float, 0.0f, 8.0f )
SET_CLAMPED( coarse_settling_rate, setCoarseSettlingRate, m_config.m_fCoarseSettlingRate, float, 0.0f, 1.0f )
SET_CLAMPED( sand_settling_rate, setSandSettlingRate, m_config.m_fSandSettlingRate, float, 0.0f, 1.0f )
SET_CLAMPED( fine_settling_rate, setFineSettlingRate, m_config.m_fFineSettlingRate, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( alluvial_fan_enabled, setAlluvialFanEnabled, m_config.m_bAlluvialFanEnabled )
SET_CLAMPED( alluvial_fan_strength, setAlluvialFanStrength, m_config.m_fAlluvialFanStrength, float, 0.0f, 8.0f )
SET_CLAMPED( alluvial_fan_slope_drop, setAlluvialFanSlopeDrop, m_config.m_fAlluvialFanSlopeDrop, float, 0.0f, 10.0f )
SET_CLAMPED( alluvial_fan_spread, setAlluvialFanSpread, m_config.m_fAlluvialFanSpread, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( debris_flow_enabled, setDebrisFlowEnabled, m_config.m_bDebrisFlowEnabled )
SET_CLAMPED(
        debris_flow_concentration_threshold, setDebrisFlowConcentrationThreshold, m_config.m_fDebrisFlowConcentrationThreshold, float, 0.0f, 1.0f )
SET_CLAMPED( debris_flow_slope_threshold, setDebrisFlowSlopeThreshold, m_config.m_fDebrisFlowSlopeThreshold, float, 0.0f, 10.0f )
SET_CLAMPED( debris_flow_rate, setDebrisFlowRate, m_config.m_fDebrisFlowRate, float, 0.0f, 1.0f )
SET_CLAMPED( debris_flow_deposition_rate, setDebrisFlowDepositionRate, m_config.m_fDebrisFlowDepositionRate, float, 0.0f, 1.0f )
SET_CLAMPED( debris_flow_runout_cells, setDebrisFlowRunoutCells, m_config.m_nDebrisFlowRunoutCells, int32_t, 1, 64 )

SET_INSPECTOR_TOGGLE( weathering_enabled, setWeatheringEnabled, m_config.m_bWeatheringEnabled )
SET_CLAMPED( physical_weathering_m_per_year, setPhysicalWeatheringMPerYear, m_config.m_fPhysicalWeatheringMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( chemical_weathering_m_per_year, setChemicalWeatheringMPerYear, m_config.m_fChemicalWeatheringMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( freeze_thaw_strength, setFreezeThawStrength, m_config.m_fFreezeThawStrength, float, 0.0f, 10.0f )
SET_CLAMPED( talus_angle_degrees, setTalusAngleDegrees, m_config.m_fTalusAngleDegrees, float, 0.0f, 89.9f )
SET_CLAMPED( thermal_creep_rate, setThermalCreepRate, m_config.m_fThermalCreepRate, float, 0.0f, 1.0f )
SET_CLAMPED( base_hardness, setBaseHardness, m_config.m_fBaseHardness, float, 0.0f, 1.0f )
SET_CLAMPED( hardness_variation, setHardnessVariation, m_config.m_fHardnessVariation, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( oxidation_enabled, setOxidationEnabled, m_config.m_bOxidationEnabled )
SET_CLAMPED( oxidation_rate_per_year, setOxidationRatePerYear, m_config.m_fOxidationRatePerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( oxidation_weakening, setOxidationWeakening, m_config.m_fOxidationWeakening, float, 0.0f, 1.0f )
SET_CLAMPED( weathering_slope_bias, setWeatheringSlopeBias, m_config.m_fWeatheringSlopeBias, float, 0.0f, 8.0f )
SET_CLAMPED( chemical_moisture_exponent, setChemicalMoistureExponent, m_config.m_fChemicalMoistureExponent, float, 0.1f, 8.0f )
SET_CLAMPED( freeze_point, setFreezePoint, m_config.m_fFreezePoint, float, -1.0f, 1.0f )
SET_INSPECTOR_TOGGLE( wet_dry_enabled, setWetDryEnabled, m_config.m_bWetDryEnabled )
SET_CLAMPED( wet_dry_weathering_m_per_cycle, setWetDryWeatheringMPerCycle, m_config.m_fWetDryWeatheringMPerCycle, float, 0.0f, 1000.0f )
SET_CLAMPED( wet_threshold, setWetThreshold, m_config.m_fWetThreshold, float, 0.0f, 1.0f )
SET_CLAMPED( dry_threshold, setDryThreshold, m_config.m_fDryThreshold, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( salt_weathering_enabled, setSaltWeatheringEnabled, m_config.m_bSaltWeatheringEnabled )
SET_REF_VALUE( salinity_mask, setSalinityMask, m_salinityMask, Ref<Image> )
SET_REF_VALUE( salinity_world_rect, setSalinityWorldRect, m_config.m_salinityWorldRect, Rect2 )
SET_CLAMPED( background_salinity, setBackgroundSalinity, m_config.m_fBackgroundSalinity, float, 0.0f, 1.0f )
SET_CLAMPED( salt_weathering_m_per_year, setSaltWeatheringMPerYear, m_config.m_fSaltWeatheringMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( salt_crystallization_strength, setSaltCrystallizationStrength, m_config.m_fSaltCrystallizationStrength, float, 0.0f, 8.0f )
SET_CLAMPED( salt_evaporation_bias, setSaltEvaporationBias, m_config.m_fSaltEvaporationBias, float, 0.0f, 8.0f )
SET_CLAMPED( salt_cohesion_loss, setSaltCohesionLoss, m_config.m_fSaltCohesionLoss, float, 0.0f, 1.0f )
SET_INSPECTOR_TOGGLE( exposure_age_enabled, setExposureAgeEnabled, m_config.m_bExposureAgeEnabled )
SET_CLAMPED( varnish_maturation_years, setVarnishMaturationYears, m_config.m_fVarnishMaturationYears, float, 1.0f, 1000000000.0f )
SET_CLAMPED( exposure_reset_depth_m, setExposureResetDepthM, m_config.m_fExposureResetDepthM, float, 0.000001f, 10000.0f )
SET_CLAMPED( varnish_stability_slope, setVarnishStabilitySlope, m_config.m_fVarnishStabilitySlope, float, 0.0f, 10.0f )
SET_CLAMPED( varnish_oxidation_multiplier, setVarnishOxidationMultiplier, m_config.m_fVarnishOxidationMultiplier, float, 0.0f, 8.0f )
SET_INSPECTOR_TOGGLE( snowmelt_enabled, setSnowmeltEnabled, m_config.m_bSnowmeltEnabled )
SET_CLAMPED( snowfall_fraction, setSnowfallFraction, m_config.m_fSnowfallFraction, float, 0.0f, 1.0f )
SET_CLAMPED( snow_accumulation_temperature, setSnowAccumulationTemperature, m_config.m_fSnowAccumulationTemperature, float, -1.0f, 1.0f )
SET_CLAMPED( snow_melt_temperature, setSnowMeltTemperature, m_config.m_fSnowMeltTemperature, float, -1.0f, 1.0f )
SET_CLAMPED( snow_melt_fraction_per_step, setSnowMeltFractionPerStep, m_config.m_fSnowMeltFractionPerStep, float, 0.0f, 1.0f )
SET_CLAMPED( snow_sublimation_fraction, setSnowSublimationFraction, m_config.m_fSnowSublimationFraction, float, 0.0f, 1.0f )

SET_INSPECTOR_TOGGLE( wind_enabled, setWindEnabled, m_config.m_bWindEnabled )
void Terrain3DErosionSettings::setWindDirection( const Vector2 &vValue ) {
	const Vector2 value = vValue.length_squared() > CMP_EPSILON ? vValue.normalized() : Vector2( 1.0f, 0.0f );
	if ( !m_config.m_windDirection.is_equal_approx( value ) ) {
		m_config.m_windDirection = value;
		emit_changed();
	}
}
SET_CLAMPED( wind_strength, setWindStrength, m_config.m_fWindStrength, float, 0.0f, 1000.0f )
SET_CLAMPED( wind_variation, setWindVariation, m_config.m_fWindVariation, float, 0.0f, 1.0f )
SET_CLAMPED( wind_erosion_m_per_year, setWindErosionMPerYear, m_config.m_fWindErosionMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( wind_transport_rate, setWindTransportRate, m_config.m_fWindTransportRate, float, 0.0f, 1.0f )
SET_CLAMPED( wind_deposition_rate, setWindDepositionRate, m_config.m_fWindDepositionRate, float, 0.0f, 1.0f )
SET_CLAMPED( wind_shelter_strength, setWindShelterStrength, m_config.m_fWindShelterStrength, float, 0.0f, 1.0f )
SET_CLAMPED( wind_threshold, setWindThreshold, m_config.m_fWindThreshold, float, 0.0f, 1000.0f )
SET_CLAMPED( wind_transport_distance_cells, setWindTransportDistanceCells, m_config.m_nWindTransportDistanceCells, int32_t, 1, 32 )
SET_CLAMPED( wind_deposition_slope_bias, setWindDepositionSlopeBias, m_config.m_fWindDepositionSlopeBias, float, 0.0f, 8.0f )
SET_INSPECTOR_TOGGLE( dune_enabled, setDuneEnabled, m_config.m_bDuneEnabled )
SET_CLAMPED( sand_repose_angle_degrees, setSandReposeAngleDegrees, m_config.m_fSandReposeAngleDegrees, float, 1.0f, 89.9f )
SET_CLAMPED( dune_avalanche_rate, setDuneAvalancheRate, m_config.m_fDuneAvalancheRate, float, 0.0f, 1.0f )
SET_CLAMPED( dune_relaxation_iterations, setDuneRelaxationIterations, m_config.m_nDuneRelaxationIterations, int32_t, 1, 32 )
SET_CLAMPED( dune_min_sand_depth_m, setDuneMinSandDepthM, m_config.m_fDuneMinSandDepthM, float, 0.0f, 1000.0f )
SET_INSPECTOR_TOGGLE( desert_pavement_enabled, setDesertPavementEnabled, m_config.m_bDesertPavementEnabled )
SET_CLAMPED( pavement_formation_rate, setPavementFormationRate, m_config.m_fPavementFormationRate, float, 0.0f, 1.0f )
SET_CLAMPED( pavement_fine_removal_threshold, setPavementFineRemovalThreshold, m_config.m_fPavementFineRemovalThreshold, float, 0.001f, 1.0f )
SET_CLAMPED( pavement_armor_strength, setPavementArmorStrength, m_config.m_fPavementArmorStrength, float, 0.0f, 1.0f )
SET_CLAMPED( pavement_infiltration_reduction, setPavementInfiltrationReduction, m_config.m_fPavementInfiltrationReduction, float, 0.0f, 1.0f )

SET_INSPECTOR_TOGGLE( wave_enabled, setWaveEnabled, m_config.m_bWaveEnabled )
SET_CLAMPED( sea_level, setSeaLevel, m_config.m_fSeaLevel, float, -1000000.0f, 1000000.0f )
SET_CLAMPED( tidal_range, setTidalRange, m_config.m_fTidalRange, float, 0.0f, 100000.0f )
void Terrain3DErosionSettings::setWaveDirection( const Vector2 &vValue ) {
	const Vector2 value = vValue.length_squared() > CMP_EPSILON ? vValue.normalized() : Vector2( 1.0f, 0.0f );
	if ( !m_config.m_waveDirection.is_equal_approx( value ) ) {
		m_config.m_waveDirection = value;
		emit_changed();
	}
}
SET_CLAMPED( wave_strength, setWaveStrength, m_config.m_fWaveStrength, float, 0.0f, 1000.0f )
SET_CLAMPED( wave_erosion_m_per_year, setWaveErosionMPerYear, m_config.m_fWaveErosionMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( wave_deposition_rate, setWaveDepositionRate, m_config.m_fWaveDepositionRate, float, 0.0f, 1.0f )
SET_CLAMPED( coastline_band_m, setCoastlineBandM, m_config.m_fCoastlineBandM, float, 0.001f, 100000.0f )
SET_CLAMPED( wave_fetch_distance_m, setWaveFetchDistanceM, m_config.m_fWaveFetchDistanceM, float, 0.0f, 10000000.0f )
SET_CLAMPED( wave_undercut_bias, setWaveUndercutBias, m_config.m_fWaveUndercutBias, float, 0.0f, 10.0f )
SET_CLAMPED( beach_smoothing, setBeachSmoothing, m_config.m_fBeachSmoothing, float, 0.0f, 1.0f )

SET_INSPECTOR_TOGGLE( traffic_enabled, setTrafficEnabled, m_config.m_bTrafficEnabled )
SET_REF_VALUE( traffic_mask, setTrafficMask, m_trafficMask, Ref<Image> )
SET_REF_VALUE( traffic_world_rect, setTrafficWorldRect, m_config.m_trafficWorldRect, Rect2 )
SET_CLAMPED( traffic_erosion_m_per_year, setTrafficErosionMPerYear, m_config.m_fTrafficErosionMPerYear, float, 0.0f, 1000.0f )
SET_CLAMPED( traffic_compaction, setTrafficCompaction, m_config.m_fTrafficCompaction, float, 0.0f, 1.0f )
SET_CLAMPED( traffic_smoothing, setTrafficSmoothing, m_config.m_fTrafficSmoothing, float, 0.0f, 1.0f )
SET_CLAMPED( traffic_runoff_multiplier, setTrafficRunoffMultiplier, m_config.m_fTrafficRunoffMultiplier, float, 0.0f, 100.0f )
SET_CLAMPED( traffic_threshold, setTrafficThreshold, m_config.m_fTrafficThreshold, float, 0.0f, 0.999f )
SET_CLAMPED( traffic_widening_cells, setTrafficWideningCells, m_config.m_nTrafficWideningCells, int32_t, 0, 32 )

SET_INSPECTOR_TOGGLE( stratigraphy_enabled, setStratigraphyEnabled, m_config.m_bStratigraphyEnabled )
SET_CLAMPED( surface_layer_depth_m, setSurfaceLayerDepthM, m_config.m_fSurfaceLayerDepthM, float, 0.0f, 100000.0f )
SET_REF_VALUE( surface_layer_depth_map, setSurfaceLayerDepthMap, m_surfaceLayerDepthMap, Ref<Image> )
SET_REF_VALUE( surface_layer_depth_world_rect, setSurfaceLayerDepthWorldRect, m_config.m_surfaceLayerDepthWorldRect, Rect2 )
SET_CLAMPED( bedrock_hardness, setBedrockHardness, m_config.m_fBedrockHardness, float, 0.0f, 1.0f )
SET_REF_VALUE( bedrock_hardness_map, setBedrockHardnessMap, m_bedrockHardnessMap, Ref<Image> )
SET_REF_VALUE( bedrock_hardness_world_rect, setBedrockHardnessWorldRect, m_config.m_bedrockHardnessWorldRect, Rect2 )
SET_CLAMPED(
        stratigraphy_transition_width_m, setStratigraphyTransitionWidthM, m_config.m_fStratigraphyTransitionWidthM, float, 0.000001f, 100000.0f )

SET_INSPECTOR_TOGGLE( material_output_enabled, setMaterialOutputEnabled, m_config.m_bMaterialOutputEnabled )
SET_VALUE( preserve_control_flags, setPreserveControlFlags, m_config.m_bPreserveControlFlags, bool )
SET_VALUE( resolve_auto_materials, setResolveAutoMaterials, m_config.m_bResolveAutoMaterials, bool )
SET_CLAMPED( material_transition_strength, setMaterialTransitionStrength, m_config.m_fMaterialTransitionStrength, float, 0.0f, 8.0f )
SET_CLAMPED( material_transition_threshold, setMaterialTransitionThreshold, m_config.m_fMaterialTransitionThreshold, float, 0.0f, 1.0f )
SET_CLAMPED( material_transition_depth_m, setMaterialTransitionDepthM, m_config.m_fMaterialTransitionDepthM, float, 0.001f, 10000.0f )
SET_INSPECTOR_TOGGLE( oxidation_color_enabled, setOxidationColorEnabled, m_config.m_bOxidationColorEnabled )
SET_REF_VALUE( oxidation_tint, setOxidationTint, m_config.m_oxidationTint, Color )
SET_CLAMPED( oxidation_tint_strength, setOxidationTintStrength, m_config.m_fOxidationTintStrength, float, 0.0f, 1.0f )

void Terrain3DErosionSettings::setMasterIntensityCurve( const Ref<Curve> &rValue ) {
	setCurve( m_masterIntensityCurve, rValue );
}
void Terrain3DErosionSettings::setRainfallCurve( const Ref<Curve> &rValue ) {
	setCurve( m_rainfallCurve, rValue );
}
void Terrain3DErosionSettings::setWeatheringCurve( const Ref<Curve> &rValue ) {
	setCurve( m_weatheringCurve, rValue );
}
void Terrain3DErosionSettings::setOxidationCurve( const Ref<Curve> &rValue ) {
	setCurve( m_oxidationCurve, rValue );
}
void Terrain3DErosionSettings::setWindCurve( const Ref<Curve> &rValue ) {
	setCurve( m_windCurve, rValue );
}
void Terrain3DErosionSettings::setWaveCurve( const Ref<Curve> &rValue ) {
	setCurve( m_waveCurve, rValue );
}
void Terrain3DErosionSettings::setTrafficCurve( const Ref<Curve> &rValue ) {
	setCurve( m_trafficCurve, rValue );
}
void Terrain3DErosionSettings::setTemperatureCurve( const Ref<Curve> &rValue ) {
	setCurve( m_temperatureCurve, rValue );
}
void Terrain3DErosionSettings::setSeaLevelCurve( const Ref<Curve> &rValue ) {
	setCurve( m_seaLevelCurve, rValue );
}
void Terrain3DErosionSettings::setSnowmeltCurve( const Ref<Curve> &rValue ) {
	setCurve( m_snowmeltCurve, rValue );
}
void Terrain3DErosionSettings::setSaltWeatheringCurve( const Ref<Curve> &rValue ) {
	setCurve( m_saltWeatheringCurve, rValue );
}
void Terrain3DErosionSettings::setDebrisFlowCurve( const Ref<Curve> &rValue ) {
	setCurve( m_debrisFlowCurve, rValue );
}

#undef SET_INSPECTOR_TOGGLE
#undef SET_REF_VALUE
#undef SET_VALUE
#undef SET_CLAMPED

// Inspector dependencies are data, not a branch for every property.  Literal
// names keep the table independent of Godot object initialization order.
typedef struct t3dErosionPropertyRule_s {
	const char *m_pName;
	bool t3dErosionConfigSnapshot_t::*m_pEnabled;
} t3dErosionPropertyRule_t;

void Terrain3DErosionSettings::_validate_property( PropertyInfo &rProperty ) const {
	static const t3dErosionPropertyRule_t rules[] = {
		{ "height_output_strength", &t3dErosionConfigSnapshot_t::m_bApplyHeight },
		{ "minimum_height_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "maximum_height_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "height_range_falloff_m", &t3dErosionConfigSnapshot_t::m_bHeightRangeEnabled },
		{ "rainfall_m_per_year", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "rainfall_variation", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "evaporation_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "infiltration_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "flow_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sediment_capacity", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "hydraulic_erosion_m_per_year", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "hydraulic_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "minimum_slope", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "storm_frequency", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "storm_intensity", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "channel_incision_strength", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "standing_water_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sediment_sorting_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sand_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "fine_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "sand_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "fine_settling_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_strength", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_slope_drop", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "alluvial_fan_spread", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_concentration_threshold", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_slope_threshold", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_deposition_rate", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "debris_flow_runout_cells", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snowmelt_enabled", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snowfall_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_accumulation_temperature", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_melt_temperature", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_melt_fraction_per_step", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "snow_sublimation_fraction", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "coarse_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "sand_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "fine_transport_multiplier", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "coarse_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "sand_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "fine_settling_rate", &t3dErosionConfigSnapshot_t::m_bSedimentSortingEnabled },
		{ "alluvial_fan_strength", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "alluvial_fan_slope_drop", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "alluvial_fan_spread", &t3dErosionConfigSnapshot_t::m_bAlluvialFanEnabled },
		{ "debris_flow_concentration_threshold", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_slope_threshold", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_rate", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_deposition_rate", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "debris_flow_runout_cells", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
		{ "snowfall_fraction", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_accumulation_temperature", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_melt_temperature", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_melt_fraction_per_step", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "snow_sublimation_fraction", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "physical_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "chemical_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "freeze_thaw_strength", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "talus_angle_degrees", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "thermal_creep_rate", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "weathering_slope_bias", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "chemical_moisture_exponent", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "freeze_point", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "wet_dry_weathering_m_per_cycle", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "wet_threshold", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "dry_threshold", &t3dErosionConfigSnapshot_t::m_bWetDryEnabled },
		{ "salinity_mask", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salinity_world_rect", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "background_salinity", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_weathering_m_per_year", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_crystallization_strength", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_evaporation_bias", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "salt_cohesion_loss", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "varnish_maturation_years", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "exposure_reset_depth_m", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "varnish_stability_slope", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "varnish_oxidation_multiplier", &t3dErosionConfigSnapshot_t::m_bExposureAgeEnabled },
		{ "oxidation_rate_per_year", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "oxidation_weakening", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "sand_repose_angle_degrees", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_avalanche_rate", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_relaxation_iterations", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "dune_min_sand_depth_m", &t3dErosionConfigSnapshot_t::m_bDuneEnabled },
		{ "sea_level", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "tidal_range", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_direction", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_strength", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_erosion_m_per_year", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_deposition_rate", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "coastline_band_m", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_fetch_distance_m", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "wave_undercut_bias", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "beach_smoothing", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "surface_layer_depth_m", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "surface_layer_depth_map", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "surface_layer_depth_world_rect", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness_map", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "bedrock_hardness_world_rect", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "stratigraphy_transition_width_m", &t3dErosionConfigSnapshot_t::m_bStratigraphyEnabled },
		{ "preserve_control_flags", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "resolve_auto_materials", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_strength", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_threshold", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "material_transition_depth_m", &t3dErosionConfigSnapshot_t::m_bMaterialOutputEnabled },
		{ "oxidation_tint", &t3dErosionConfigSnapshot_t::m_bOxidationColorEnabled },
		{ "oxidation_tint_strength", &t3dErosionConfigSnapshot_t::m_bOxidationColorEnabled },
		{ "rainfall_curve", &t3dErosionConfigSnapshot_t::m_bHydraulicEnabled },
		{ "weathering_curve", &t3dErosionConfigSnapshot_t::m_bWeatheringEnabled },
		{ "oxidation_curve", &t3dErosionConfigSnapshot_t::m_bOxidationEnabled },
		{ "wind_curve", &t3dErosionConfigSnapshot_t::m_bWindEnabled },
		{ "wave_curve", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "sea_level_curve", &t3dErosionConfigSnapshot_t::m_bWaveEnabled },
		{ "traffic_curve", &t3dErosionConfigSnapshot_t::m_bTrafficEnabled },
		{ "snowmelt_curve", &t3dErosionConfigSnapshot_t::m_bSnowmeltEnabled },
		{ "salt_weathering_curve", &t3dErosionConfigSnapshot_t::m_bSaltWeatheringEnabled },
		{ "debris_flow_curve", &t3dErosionConfigSnapshot_t::m_bDebrisFlowEnabled },
	};
	const StringName name = rProperty.name;
	for ( size_t i = 0; i < sizeof( rules ) / sizeof( rules[0] ); ++i ) {
		const t3dErosionPropertyRule_t &rRule = rules[i];
		if ( !( m_config.*rRule.m_pEnabled ) && name == StringName( rRule.m_pName ) ) {
			rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
			return;
		}
	}

	// Prefix-gated groups exclude their own enable switch, as before.
	if ( !m_config.m_bWindEnabled && name != StringName( "wind_enabled" ) && name.begins_with( "wind_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if ( !m_config.m_bDesertPavementEnabled && name.begins_with( "pavement_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	} else if ( !m_config.m_bTrafficEnabled && name != StringName( "traffic_enabled" ) && name.begins_with( "traffic_" ) ) {
		rProperty.usage = PROPERTY_USAGE_NO_EDITOR;
	}
}

#define BIND_PROP( type, name, setter, getter )                                                                                            \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &Terrain3DErosionSettings::setter );                                          \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &Terrain3DErosionSettings::getter );                                                   \
	ADD_PROPERTY( PropertyInfo( type, #name ), "set_" #name, "get_" #name )

#define BIND_PROP_HINT( type, name, setter, getter, hint, hint_string )                                                                    \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &Terrain3DErosionSettings::setter );                                          \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &Terrain3DErosionSettings::getter );                                                   \
	ADD_PROPERTY( PropertyInfo( type, #name, hint, hint_string ), "set_" #name, "get_" #name )

#define BIND_CURVE( name, setter, getter )                                                                                                 \
	ClassDB::bind_method( D_METHOD( "set_" #name, "value" ), &Terrain3DErosionSettings::setter );                                          \
	ClassDB::bind_method( D_METHOD( "get_" #name ), &Terrain3DErosionSettings::getter );                                                   \
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, #name, PROPERTY_HINT_RESOURCE_TYPE, "Curve" ), "set_" #name, "get_" #name )

void Terrain3DErosionSettings::_bind_methods( void ) {
	BIND_ENUM_CONSTANT( BOUNDARY_CLOSED );
	BIND_ENUM_CONSTANT( BOUNDARY_OPEN );



	ADD_GROUP( "General", "" );
	BIND_PROP_HINT( Variant::FLOAT,
	        simulation_years,
	        setSimulationYears,
	        getSimulationYears,
	        PROPERTY_HINT_RANGE,
	        "0.000001,1000000000,0.001,or_greater" );
	BIND_PROP_HINT( Variant::INT, simulation_steps, setSimulationSteps, getSimulationSteps, PROPERTY_HINT_RANGE, "1,4096,1" );
	BIND_PROP_HINT( Variant::INT,
	        resolution_scale,
	        setResolutionScale,
	        getResolutionScale,
	        PROPERTY_HINT_ENUM,
	        "Full resolution:1,Half:2,Quarter:4,Eighth:8,Sixteenth:16" );
	BIND_PROP( Variant::BOOL, auto_adjust_resolution, setAutoAdjustResolution, getAutoAdjustResolution );
	BIND_PROP_HINT( Variant::INT, memory_budget_mb, setMemoryBudgetMb, getMemoryBudgetMb, PROPERTY_HINT_RANGE, "128,32768,128,suffix: MB" );
	BIND_PROP( Variant::BOOL, live_preview_enabled, setLivePreviewEnabled, getLivePreviewEnabled );
	BIND_PROP_HINT( Variant::INT,
	        live_preview_interval_steps,
	        setLivePreviewIntervalSteps,
	        getLivePreviewIntervalSteps,
	        PROPERTY_HINT_RANGE,
	        "1,256,1,suffix: steps" );
	BIND_PROP( Variant::BOOL, live_preview_start_paused, setLivePreviewStartPaused, getLivePreviewStartPaused );
	BIND_PROP_HINT( Variant::INT, thread_count, setThreadCount, getThreadCount, PROPERTY_HINT_RANGE, "0,64,1" );
	BIND_PROP( Variant::INT, seed, setSeed, getSeed );
	BIND_PROP_HINT( Variant::INT, boundary_mode, setBoundaryMode, getBoundaryMode, PROPERTY_HINT_ENUM, "Closed,Open" );
	BIND_PROP_HINT( Variant::FLOAT,
	        max_height_change_per_step,
	        setMaxHeightChangePerStep,
	        getMaxHeightChangePerStep,
	        PROPERTY_HINT_RANGE,
	        "0.000001,10000,0.001,suffix: m" );
	BIND_PROP( Variant::BOOL, preserve_holes, setPreserveHoles, getPreserveHoles );
	BIND_PROP( Variant::BOOL, apply_height, setApplyHeight, getApplyHeight );
	BIND_PROP_HINT(
	        Variant::FLOAT, height_output_strength, setHeightOutputStrength, getHeightOutputStrength, PROPERTY_HINT_RANGE, "0,10,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, global_intensity, setGlobalIntensity, getGlobalIntensity, PROPERTY_HINT_RANGE, "0,10,0.01" );

	ADD_GROUP( "Spatial Limits", "" );
	ClassDB::bind_method( D_METHOD( "set_simulation_mask", "value" ), &Terrain3DErosionSettings::setSimulationMask );
	ClassDB::bind_method( D_METHOD( "get_simulation_mask" ), &Terrain3DErosionSettings::getSimulationMask );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "simulation_mask", PROPERTY_HINT_RESOURCE_TYPE, "Image" ),
	        "set_simulation_mask",
	        "get_simulation_mask" );
	BIND_PROP( Variant::RECT2, simulation_mask_world_rect, setSimulationMaskWorldRect, getSimulationMaskWorldRect );
	BIND_PROP( Variant::BOOL, invert_simulation_mask, setInvertSimulationMask, getInvertSimulationMask );
	BIND_PROP_HINT( Variant::FLOAT,
	        edge_fade_distance_m,
	        setEdgeFadeDistanceM,
	        getEdgeFadeDistanceM,
	        PROPERTY_HINT_RANGE,
	        "0,10000000,0.1,suffix: m" );
	BIND_PROP( Variant::BOOL, height_range_enabled, setHeightRangeEnabled, getHeightRangeEnabled );
	BIND_PROP_HINT(
	        Variant::FLOAT, minimum_height_m, setMinimumHeightM, getMinimumHeightM, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" );
	BIND_PROP_HINT(
	        Variant::FLOAT, maximum_height_m, setMaximumHeightM, getMaximumHeightM, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" );
	BIND_PROP_HINT( Variant::FLOAT,
	        height_range_falloff_m,
	        setHeightRangeFalloffM,
	        getHeightRangeFalloffM,
	        PROPERTY_HINT_RANGE,
	        "0,1000000,0.1,suffix: m" );

	ADD_GROUP( "Hydraulic Erosion", "" );
	BIND_PROP( Variant::BOOL, hydraulic_enabled, setHydraulicEnabled, getHydraulicEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        rainfall_m_per_year,
	        setRainfallMPerYear,
	        getRainfallMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT, rainfall_variation, setRainfallVariation, getRainfallVariation, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, evaporation_fraction, setEvaporationFraction, getEvaporationFraction, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, infiltration_fraction, setInfiltrationFraction, getInfiltrationFraction, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, flow_rate, setFlowRate, getFlowRate, PROPERTY_HINT_RANGE, "0,10,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, sediment_capacity, setSedimentCapacity, getSedimentCapacity, PROPERTY_HINT_RANGE, "0,1000,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        hydraulic_erosion_m_per_year,
	        setHydraulicErosionMPerYear,
	        getHydraulicErosionMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT,
	        hydraulic_deposition_rate,
	        setHydraulicDepositionRate,
	        getHydraulicDepositionRate,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, minimum_slope, setMinimumSlope, getMinimumSlope, PROPERTY_HINT_RANGE, "0,10,0.0001" );
	BIND_PROP_HINT( Variant::FLOAT, storm_frequency, setStormFrequency, getStormFrequency, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, storm_intensity, setStormIntensity, getStormIntensity, PROPERTY_HINT_RANGE, "0,100,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        channel_incision_strength,
	        setChannelIncisionStrength,
	        getChannelIncisionStrength,
	        PROPERTY_HINT_RANGE,
	        "0,16,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        standing_water_deposition_rate,
	        setStandingWaterDepositionRate,
	        getStandingWaterDepositionRate,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );

	ADD_SUBGROUP( "Sediment Sorting", "" );
	BIND_PROP( Variant::BOOL, sediment_sorting_enabled, setSedimentSortingEnabled, getSedimentSortingEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        coarse_transport_multiplier,
	        setCoarseTransportMultiplier,
	        getCoarseTransportMultiplier,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        sand_transport_multiplier,
	        setSandTransportMultiplier,
	        getSandTransportMultiplier,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        fine_transport_multiplier,
	        setFineTransportMultiplier,
	        getFineTransportMultiplier,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, coarse_settling_rate, setCoarseSettlingRate, getCoarseSettlingRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, sand_settling_rate, setSandSettlingRate, getSandSettlingRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, fine_settling_rate, setFineSettlingRate, getFineSettlingRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	ADD_SUBGROUP( "Alluvial Fans", "" );
	BIND_PROP( Variant::BOOL, alluvial_fan_enabled, setAlluvialFanEnabled, getAlluvialFanEnabled );
	BIND_PROP_HINT(
	        Variant::FLOAT, alluvial_fan_strength, setAlluvialFanStrength, getAlluvialFanStrength, PROPERTY_HINT_RANGE, "0,8,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, alluvial_fan_slope_drop, setAlluvialFanSlopeDrop, getAlluvialFanSlopeDrop, PROPERTY_HINT_RANGE, "0,10,0.001" );
	BIND_PROP_HINT( Variant::FLOAT, alluvial_fan_spread, setAlluvialFanSpread, getAlluvialFanSpread, PROPERTY_HINT_RANGE, "0,1,0.01" );
	ADD_SUBGROUP( "Debris Flows", "" );
	BIND_PROP( Variant::BOOL, debris_flow_enabled, setDebrisFlowEnabled, getDebrisFlowEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        debris_flow_concentration_threshold,
	        setDebrisFlowConcentrationThreshold,
	        getDebrisFlowConcentrationThreshold,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        debris_flow_slope_threshold,
	        setDebrisFlowSlopeThreshold,
	        getDebrisFlowSlopeThreshold,
	        PROPERTY_HINT_RANGE,
	        "0,10,0.001" );
	BIND_PROP_HINT( Variant::FLOAT, debris_flow_rate, setDebrisFlowRate, getDebrisFlowRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        debris_flow_deposition_rate,
	        setDebrisFlowDepositionRate,
	        getDebrisFlowDepositionRate,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );
	BIND_PROP_HINT( Variant::INT,
	        debris_flow_runout_cells,
	        setDebrisFlowRunoutCells,
	        getDebrisFlowRunoutCells,
	        PROPERTY_HINT_RANGE,
	        "1,64,1,suffix: cells" );

	ADD_GROUP( "Weathering and Oxidation", "" );
	BIND_PROP( Variant::BOOL, weathering_enabled, setWeatheringEnabled, getWeatheringEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        physical_weathering_m_per_year,
	        setPhysicalWeatheringMPerYear,
	        getPhysicalWeatheringMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT,
	        chemical_weathering_m_per_year,
	        setChemicalWeatheringMPerYear,
	        getChemicalWeatheringMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT, freeze_thaw_strength, setFreezeThawStrength, getFreezeThawStrength, PROPERTY_HINT_RANGE, "0,10,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, talus_angle_degrees, setTalusAngleDegrees, getTalusAngleDegrees, PROPERTY_HINT_RANGE, "0,89.9,0.1,suffix: °" );
	BIND_PROP_HINT( Variant::FLOAT, thermal_creep_rate, setThermalCreepRate, getThermalCreepRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, base_hardness, setBaseHardness, getBaseHardness, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, hardness_variation, setHardnessVariation, getHardnessVariation, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP( Variant::BOOL, oxidation_enabled, setOxidationEnabled, getOxidationEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        oxidation_rate_per_year,
	        setOxidationRatePerYear,
	        getOxidationRatePerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001" );
	BIND_PROP_HINT( Variant::FLOAT, oxidation_weakening, setOxidationWeakening, getOxidationWeakening, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, weathering_slope_bias, setWeatheringSlopeBias, getWeatheringSlopeBias, PROPERTY_HINT_RANGE, "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        chemical_moisture_exponent,
	        setChemicalMoistureExponent,
	        getChemicalMoistureExponent,
	        PROPERTY_HINT_RANGE,
	        "0.1,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, freeze_point, setFreezePoint, getFreezePoint, PROPERTY_HINT_RANGE, "-1,1,0.01" );
	ADD_SUBGROUP( "Wet-Dry Cycling", "" );
	BIND_PROP( Variant::BOOL, wet_dry_enabled, setWetDryEnabled, getWetDryEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        wet_dry_weathering_m_per_cycle,
	        setWetDryWeatheringMPerCycle,
	        getWetDryWeatheringMPerCycle,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/cycle" );
	BIND_PROP_HINT( Variant::FLOAT, wet_threshold, setWetThreshold, getWetThreshold, PROPERTY_HINT_RANGE, "0,1,0.001" );
	BIND_PROP_HINT( Variant::FLOAT, dry_threshold, setDryThreshold, getDryThreshold, PROPERTY_HINT_RANGE, "0,1,0.001" );
	ADD_SUBGROUP( "Salt Weathering", "" );
	BIND_PROP( Variant::BOOL, salt_weathering_enabled, setSaltWeatheringEnabled, getSaltWeatheringEnabled );
	ClassDB::bind_method( D_METHOD( "set_salinity_mask", "value" ), &Terrain3DErosionSettings::setSalinityMask );
	ClassDB::bind_method( D_METHOD( "get_salinity_mask" ), &Terrain3DErosionSettings::getSalinityMask );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "salinity_mask", PROPERTY_HINT_RESOURCE_TYPE, "Image" ),
	        "set_salinity_mask",
	        "get_salinity_mask" );
	BIND_PROP( Variant::RECT2, salinity_world_rect, setSalinityWorldRect, getSalinityWorldRect );
	BIND_PROP_HINT( Variant::FLOAT, background_salinity, setBackgroundSalinity, getBackgroundSalinity, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        salt_weathering_m_per_year,
	        setSaltWeatheringMPerYear,
	        getSaltWeatheringMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT,
	        salt_crystallization_strength,
	        setSaltCrystallizationStrength,
	        getSaltCrystallizationStrength,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, salt_evaporation_bias, setSaltEvaporationBias, getSaltEvaporationBias, PROPERTY_HINT_RANGE, "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, salt_cohesion_loss, setSaltCohesionLoss, getSaltCohesionLoss, PROPERTY_HINT_RANGE, "0,1,0.01" );
	ADD_SUBGROUP( "Exposure Age and Varnish", "" );
	BIND_PROP( Variant::BOOL, exposure_age_enabled, setExposureAgeEnabled, getExposureAgeEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        varnish_maturation_years,
	        setVarnishMaturationYears,
	        getVarnishMaturationYears,
	        PROPERTY_HINT_RANGE,
	        "1,1000000000,1,suffix: years" );
	BIND_PROP_HINT( Variant::FLOAT,
	        exposure_reset_depth_m,
	        setExposureResetDepthM,
	        getExposureResetDepthM,
	        PROPERTY_HINT_RANGE,
	        "0.000001,10000,0.001,suffix: m" );
	BIND_PROP_HINT(
	        Variant::FLOAT, varnish_stability_slope, setVarnishStabilitySlope, getVarnishStabilitySlope, PROPERTY_HINT_RANGE, "0,10,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        varnish_oxidation_multiplier,
	        setVarnishOxidationMultiplier,
	        getVarnishOxidationMultiplier,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	ADD_SUBGROUP( "Snow and Meltwater", "" );
	BIND_PROP( Variant::BOOL, snowmelt_enabled, setSnowmeltEnabled, getSnowmeltEnabled );
	BIND_PROP_HINT( Variant::FLOAT, snowfall_fraction, setSnowfallFraction, getSnowfallFraction, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        snow_accumulation_temperature,
	        setSnowAccumulationTemperature,
	        getSnowAccumulationTemperature,
	        PROPERTY_HINT_RANGE,
	        "-1,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, snow_melt_temperature, setSnowMeltTemperature, getSnowMeltTemperature, PROPERTY_HINT_RANGE, "-1,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        snow_melt_fraction_per_step,
	        setSnowMeltFractionPerStep,
	        getSnowMeltFractionPerStep,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        snow_sublimation_fraction,
	        setSnowSublimationFraction,
	        getSnowSublimationFraction,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );

	ADD_GROUP( "Wind", "" );
	BIND_PROP( Variant::BOOL, wind_enabled, setWindEnabled, getWindEnabled );
	BIND_PROP( Variant::VECTOR2, wind_direction, setWindDirection, getWindDirection );
	BIND_PROP_HINT( Variant::FLOAT, wind_strength, setWindStrength, getWindStrength, PROPERTY_HINT_RANGE, "0,1000,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, wind_variation, setWindVariation, getWindVariation, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        wind_erosion_m_per_year,
	        setWindErosionMPerYear,
	        getWindErosionMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT, wind_transport_rate, setWindTransportRate, getWindTransportRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, wind_deposition_rate, setWindDepositionRate, getWindDepositionRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, wind_shelter_strength, setWindShelterStrength, getWindShelterStrength, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, wind_threshold, setWindThreshold, getWindThreshold, PROPERTY_HINT_RANGE, "0,1000,0.01" );
	BIND_PROP_HINT( Variant::INT,
	        wind_transport_distance_cells,
	        setWindTransportDistanceCells,
	        getWindTransportDistanceCells,
	        PROPERTY_HINT_RANGE,
	        "1,32,1,suffix: cells" );
	BIND_PROP_HINT( Variant::FLOAT,
	        wind_deposition_slope_bias,
	        setWindDepositionSlopeBias,
	        getWindDepositionSlopeBias,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	ADD_GROUP( "Dune Relaxation", "" );
	BIND_PROP( Variant::BOOL, dune_enabled, setDuneEnabled, getDuneEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        sand_repose_angle_degrees,
	        setSandReposeAngleDegrees,
	        getSandReposeAngleDegrees,
	        PROPERTY_HINT_RANGE,
	        "1,89.9,0.1,suffix: °" );
	BIND_PROP_HINT( Variant::FLOAT, dune_avalanche_rate, setDuneAvalancheRate, getDuneAvalancheRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::INT,
	        dune_relaxation_iterations,
	        setDuneRelaxationIterations,
	        getDuneRelaxationIterations,
	        PROPERTY_HINT_RANGE,
	        "1,32,1" );
	BIND_PROP_HINT( Variant::FLOAT,
	        dune_min_sand_depth_m,
	        setDuneMinSandDepthM,
	        getDuneMinSandDepthM,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.001,suffix: m" );
	ADD_GROUP( "Desert Pavement", "" );
	BIND_PROP( Variant::BOOL, desert_pavement_enabled, setDesertPavementEnabled, getDesertPavementEnabled );
	BIND_PROP_HINT(
	        Variant::FLOAT, pavement_formation_rate, setPavementFormationRate, getPavementFormationRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        pavement_fine_removal_threshold,
	        setPavementFineRemovalThreshold,
	        getPavementFineRemovalThreshold,
	        PROPERTY_HINT_RANGE,
	        "0.001,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, pavement_armor_strength, setPavementArmorStrength, getPavementArmorStrength, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        pavement_infiltration_reduction,
	        setPavementInfiltrationReduction,
	        getPavementInfiltrationReduction,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );

	ADD_GROUP( "Wave Erosion", "" );
	BIND_PROP( Variant::BOOL, wave_enabled, setWaveEnabled, getWaveEnabled );
	BIND_PROP_HINT( Variant::FLOAT, sea_level, setSeaLevel, getSeaLevel, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" );
	BIND_PROP_HINT( Variant::FLOAT, tidal_range, setTidalRange, getTidalRange, PROPERTY_HINT_RANGE, "0,100000,0.1,suffix: m" );
	BIND_PROP( Variant::VECTOR2, wave_direction, setWaveDirection, getWaveDirection );
	BIND_PROP_HINT( Variant::FLOAT, wave_strength, setWaveStrength, getWaveStrength, PROPERTY_HINT_RANGE, "0,1000,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        wave_erosion_m_per_year,
	        setWaveErosionMPerYear,
	        getWaveErosionMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT, wave_deposition_rate, setWaveDepositionRate, getWaveDepositionRate, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT(
	        Variant::FLOAT, coastline_band_m, setCoastlineBandM, getCoastlineBandM, PROPERTY_HINT_RANGE, "0.001,100000,0.1,suffix: m" );
	BIND_PROP_HINT( Variant::FLOAT,
	        wave_fetch_distance_m,
	        setWaveFetchDistanceM,
	        getWaveFetchDistanceM,
	        PROPERTY_HINT_RANGE,
	        "0,10000000,1,suffix: m" );
	BIND_PROP_HINT( Variant::FLOAT, wave_undercut_bias, setWaveUndercutBias, getWaveUndercutBias, PROPERTY_HINT_RANGE, "0,10,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, beach_smoothing, setBeachSmoothing, getBeachSmoothing, PROPERTY_HINT_RANGE, "0,1,0.01" );

	ADD_GROUP( "Foot Traffic", "" );
	BIND_PROP( Variant::BOOL, traffic_enabled, setTrafficEnabled, getTrafficEnabled );
	ClassDB::bind_method( D_METHOD( "set_traffic_mask", "value" ), &Terrain3DErosionSettings::setTrafficMask );
	ClassDB::bind_method( D_METHOD( "get_traffic_mask" ), &Terrain3DErosionSettings::getTrafficMask );
	ADD_PROPERTY(
	        PropertyInfo( Variant::OBJECT, "traffic_mask", PROPERTY_HINT_RESOURCE_TYPE, "Image" ), "set_traffic_mask", "get_traffic_mask" );
	BIND_PROP( Variant::RECT2, traffic_world_rect, setTrafficWorldRect, getTrafficWorldRect );
	BIND_PROP_HINT( Variant::FLOAT,
	        traffic_erosion_m_per_year,
	        setTrafficErosionMPerYear,
	        getTrafficErosionMPerYear,
	        PROPERTY_HINT_RANGE,
	        "0,1000,0.00001,suffix: m/year" );
	BIND_PROP_HINT( Variant::FLOAT, traffic_compaction, setTrafficCompaction, getTrafficCompaction, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, traffic_smoothing, setTrafficSmoothing, getTrafficSmoothing, PROPERTY_HINT_RANGE, "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        traffic_runoff_multiplier,
	        setTrafficRunoffMultiplier,
	        getTrafficRunoffMultiplier,
	        PROPERTY_HINT_RANGE,
	        "0,100,0.01" );
	BIND_PROP_HINT( Variant::FLOAT, traffic_threshold, setTrafficThreshold, getTrafficThreshold, PROPERTY_HINT_RANGE, "0,0.999,0.01" );
	BIND_PROP_HINT( Variant::INT,
	        traffic_widening_cells,
	        setTrafficWideningCells,
	        getTrafficWideningCells,
	        PROPERTY_HINT_RANGE,
	        "0,32,1,suffix: cells" );

	ADD_GROUP( "Layered Substrate", "" );
	BIND_PROP( Variant::BOOL, stratigraphy_enabled, setStratigraphyEnabled, getStratigraphyEnabled );
	BIND_PROP_HINT( Variant::FLOAT,
	        surface_layer_depth_m,
	        setSurfaceLayerDepthM,
	        getSurfaceLayerDepthM,
	        PROPERTY_HINT_RANGE,
	        "0,100000,0.01,suffix: m" );
	ClassDB::bind_method( D_METHOD( "set_surface_layer_depth_map", "value" ), &Terrain3DErosionSettings::setSurfaceLayerDepthMap );
	ClassDB::bind_method( D_METHOD( "get_surface_layer_depth_map" ), &Terrain3DErosionSettings::getSurfaceLayerDepthMap );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "surface_layer_depth_map", PROPERTY_HINT_RESOURCE_TYPE, "Image" ),
	        "set_surface_layer_depth_map",
	        "get_surface_layer_depth_map" );
	BIND_PROP( Variant::RECT2, surface_layer_depth_world_rect, setSurfaceLayerDepthWorldRect, getSurfaceLayerDepthWorldRect );
	BIND_PROP_HINT( Variant::FLOAT, bedrock_hardness, setBedrockHardness, getBedrockHardness, PROPERTY_HINT_RANGE, "0,1,0.01" );
	ClassDB::bind_method( D_METHOD( "set_bedrock_hardness_map", "value" ), &Terrain3DErosionSettings::setBedrockHardnessMap );
	ClassDB::bind_method( D_METHOD( "get_bedrock_hardness_map" ), &Terrain3DErosionSettings::getBedrockHardnessMap );
	ADD_PROPERTY( PropertyInfo( Variant::OBJECT, "bedrock_hardness_map", PROPERTY_HINT_RESOURCE_TYPE, "Image" ),
	        "set_bedrock_hardness_map",
	        "get_bedrock_hardness_map" );
	BIND_PROP( Variant::RECT2, bedrock_hardness_world_rect, setBedrockHardnessWorldRect, getBedrockHardnessWorldRect );
	BIND_PROP_HINT( Variant::FLOAT,
	        stratigraphy_transition_width_m,
	        setStratigraphyTransitionWidthM,
	        getStratigraphyTransitionWidthM,
	        PROPERTY_HINT_RANGE,
	        "0.000001,100000,0.01,suffix: m" );

	ADD_GROUP( "Material Transitions", "" );
	BIND_PROP( Variant::BOOL, material_output_enabled, setMaterialOutputEnabled, getMaterialOutputEnabled );
	BIND_PROP( Variant::BOOL, preserve_control_flags, setPreserveControlFlags, getPreserveControlFlags );
	BIND_PROP( Variant::BOOL, resolve_auto_materials, setResolveAutoMaterials, getResolveAutoMaterials );
	BIND_PROP_HINT( Variant::FLOAT,
	        material_transition_strength,
	        setMaterialTransitionStrength,
	        getMaterialTransitionStrength,
	        PROPERTY_HINT_RANGE,
	        "0,8,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        material_transition_threshold,
	        setMaterialTransitionThreshold,
	        getMaterialTransitionThreshold,
	        PROPERTY_HINT_RANGE,
	        "0,1,0.01" );
	BIND_PROP_HINT( Variant::FLOAT,
	        material_transition_depth_m,
	        setMaterialTransitionDepthM,
	        getMaterialTransitionDepthM,
	        PROPERTY_HINT_RANGE,
	        "0.001,10000,0.01,suffix: m" );
	BIND_PROP( Variant::BOOL, oxidation_color_enabled, setOxidationColorEnabled, getOxidationColorEnabled );
	BIND_PROP( Variant::COLOR, oxidation_tint, setOxidationTint, getOxidationTint );
	BIND_PROP_HINT(
	        Variant::FLOAT, oxidation_tint_strength, setOxidationTintStrength, getOxidationTintStrength, PROPERTY_HINT_RANGE, "0,1,0.01" );

	ADD_GROUP( "Time Curves", "" );
	BIND_CURVE( master_intensity_curve, setMasterIntensityCurve, getMasterIntensityCurve );
	BIND_CURVE( rainfall_curve, setRainfallCurve, getRainfallCurve );
	BIND_CURVE( weathering_curve, setWeatheringCurve, getWeatheringCurve );
	BIND_CURVE( oxidation_curve, setOxidationCurve, getOxidationCurve );
	BIND_CURVE( wind_curve, setWindCurve, getWindCurve );
	BIND_CURVE( wave_curve, setWaveCurve, getWaveCurve );
	BIND_CURVE( traffic_curve, setTrafficCurve, getTrafficCurve );
	BIND_CURVE( temperature_curve, setTemperatureCurve, getTemperatureCurve );
	BIND_CURVE( sea_level_curve, setSeaLevelCurve, getSeaLevelCurve );
	BIND_CURVE( snowmelt_curve, setSnowmeltCurve, getSnowmeltCurve );
	BIND_CURVE( salt_weathering_curve, setSaltWeatheringCurve, getSaltWeatheringCurve );
	BIND_CURVE( debris_flow_curve, setDebrisFlowCurve, getDebrisFlowCurve );
#undef BIND_CURVE
#undef BIND_PROP_HINT
#undef BIND_PROP
}
