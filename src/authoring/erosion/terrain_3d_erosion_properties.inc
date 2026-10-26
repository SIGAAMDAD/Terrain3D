// Intentionally included multiple times: define T3D_EROSION_GROUP and
// T3D_EROSION_PROPERTY before including. Order is the serialized Inspector order.
// Bounds apply only to CLAMPED rows. Defaults live in the configuration snapshot
// and curve constructor; masks and curves remain settings-owned resources.

T3D_EROSION_GROUP( "General", "" )
T3D_EROSION_PROPERTY( CLAMPED, double, SimulationYears, simulation_years, m_config.m_fSimulationYears, 0.000001, 1000000000.0,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.000001,1000000000,0.001,or_greater" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, SimulationSteps, simulation_steps, m_config.m_nSimulationSteps, 1, 4096,
        Variant::INT, PROPERTY_HINT_RANGE, "1,4096,1" )
T3D_EROSION_PROPERTY( CUSTOM_VALUE, int32_t, ResolutionScale, resolution_scale, m_config.m_nResolutionScale, 0, 0,
        Variant::INT, PROPERTY_HINT_ENUM, "Full resolution:1,Half:2,Quarter:4,Eighth:8,Sixteenth:16" )
T3D_EROSION_PROPERTY( VALUE, bool, AutoAdjustResolution, auto_adjust_resolution, m_config.m_bAutoAdjustResolution, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, MemoryBudgetMb, memory_budget_mb, m_config.m_nMemoryBudgetMb, 128, 32768,
        Variant::INT, PROPERTY_HINT_RANGE, "128,32768,128,suffix: MB" )
T3D_EROSION_PROPERTY( VALUE, bool, LivePreviewEnabled, live_preview_enabled, m_config.m_bLivePreviewEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, LivePreviewIntervalSteps, live_preview_interval_steps, m_config.m_nLivePreviewIntervalSteps, 1, 256,
        Variant::INT, PROPERTY_HINT_RANGE, "1,256,1,suffix: steps" )
T3D_EROSION_PROPERTY( VALUE, bool, LivePreviewStartPaused, live_preview_start_paused, m_config.m_bLivePreviewStartPaused, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, ThreadCount, thread_count, m_config.m_nThreadCount, 0, 64,
        Variant::INT, PROPERTY_HINT_RANGE, "0,64,1" )
T3D_EROSION_PROPERTY( VALUE, int64_t, Seed, seed, m_config.m_nSeed, 0, 0,
        Variant::INT, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CUSTOM_VALUE, t3dBoundaryMode_t, BoundaryMode, boundary_mode, m_config.m_boundaryMode, 0, 0,
        Variant::INT, PROPERTY_HINT_ENUM, "Closed,Open" )
T3D_EROSION_PROPERTY( CLAMPED, float, MaxHeightChangePerStep, max_height_change_per_step,
        m_config.m_fMaxHeightChangePerStep, 0.000001f, 10000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.000001,10000,0.001,suffix: m" )
T3D_EROSION_PROPERTY( VALUE, bool, PreserveHoles, preserve_holes, m_config.m_bPreserveHoles, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, ApplyHeight, apply_height, m_config.m_bApplyHeight, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, HeightOutputStrength, height_output_strength, m_config.m_fHeightOutputStrength, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, GlobalIntensity, global_intensity, m_config.m_fGlobalIntensity, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )

T3D_EROSION_GROUP( "Spatial Limits", "" )
T3D_EROSION_PROPERTY( REF, Ref<Image>, SimulationMask, simulation_mask, m_simulationMask, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Image" )
T3D_EROSION_PROPERTY( REF, Rect2, SimulationMaskWorldRect, simulation_mask_world_rect, m_config.m_simulationMaskWorldRect, 0, 0,
        Variant::RECT2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( VALUE, bool, InvertSimulationMask, invert_simulation_mask, m_config.m_bInvertSimulationMask, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, EdgeFadeDistanceM, edge_fade_distance_m, m_config.m_fEdgeFadeDistanceM, 0.0f, 10000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10000000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( TOGGLE, bool, HeightRangeEnabled, height_range_enabled, m_config.m_bHeightRangeEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, MinimumHeightM, minimum_height_m, m_config.m_fMinimumHeightM, -1000000.0f, 1000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, MaximumHeightM, maximum_height_m, m_config.m_fMaximumHeightM, -1000000.0f, 1000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, HeightRangeFalloffM, height_range_falloff_m, m_config.m_fHeightRangeFalloffM, 0.0f, 1000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000000,0.1,suffix: m" )

T3D_EROSION_GROUP( "Hydraulic Erosion", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, HydraulicEnabled, hydraulic_enabled, m_config.m_bHydraulicEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, RainfallMPerYear, rainfall_m_per_year, m_config.m_fRainfallMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, RainfallVariation, rainfall_variation, m_config.m_fRainfallVariation, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, EvaporationFraction, evaporation_fraction, m_config.m_fEvaporationFraction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, InfiltrationFraction, infiltration_fraction, m_config.m_fInfiltrationFraction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, FlowRate, flow_rate, m_config.m_fFlowRate, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SedimentCapacity, sediment_capacity, m_config.m_fSedimentCapacity, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, HydraulicErosionMPerYear, hydraulic_erosion_m_per_year,
        m_config.m_fHydraulicErosionMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, HydraulicDepositionRate, hydraulic_deposition_rate, m_config.m_fHydraulicDepositionRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, MinimumSlope, minimum_slope, m_config.m_fMinimumSlope, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.0001" )
T3D_EROSION_PROPERTY( CLAMPED, float, StormFrequency, storm_frequency, m_config.m_fStormFrequency, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, StormIntensity, storm_intensity, m_config.m_fStormIntensity, 0.0f, 100.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, ChannelIncisionStrength, channel_incision_strength, m_config.m_fChannelIncisionStrength, 0.0f, 16.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,16,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, StandingWaterDepositionRate, standing_water_deposition_rate,
        m_config.m_fStandingWaterDepositionRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, SedimentSortingEnabled, sediment_sorting_enabled, m_config.m_bSedimentSortingEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, CoarseTransportMultiplier, coarse_transport_multiplier,
        m_config.m_fCoarseTransportMultiplier, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SandTransportMultiplier, sand_transport_multiplier, m_config.m_fSandTransportMultiplier, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, FineTransportMultiplier, fine_transport_multiplier, m_config.m_fFineTransportMultiplier, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, CoarseSettlingRate, coarse_settling_rate, m_config.m_fCoarseSettlingRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SandSettlingRate, sand_settling_rate, m_config.m_fSandSettlingRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, FineSettlingRate, fine_settling_rate, m_config.m_fFineSettlingRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, AlluvialFanEnabled, alluvial_fan_enabled, m_config.m_bAlluvialFanEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, AlluvialFanStrength, alluvial_fan_strength, m_config.m_fAlluvialFanStrength, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, AlluvialFanSlopeDrop, alluvial_fan_slope_drop, m_config.m_fAlluvialFanSlopeDrop, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.001" )
T3D_EROSION_PROPERTY( CLAMPED, float, AlluvialFanSpread, alluvial_fan_spread, m_config.m_fAlluvialFanSpread, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, DebrisFlowEnabled, debris_flow_enabled, m_config.m_bDebrisFlowEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, DebrisFlowConcentrationThreshold, debris_flow_concentration_threshold,
        m_config.m_fDebrisFlowConcentrationThreshold, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, DebrisFlowSlopeThreshold, debris_flow_slope_threshold,
        m_config.m_fDebrisFlowSlopeThreshold, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.001" )
T3D_EROSION_PROPERTY( CLAMPED, float, DebrisFlowRate, debris_flow_rate, m_config.m_fDebrisFlowRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, DebrisFlowDepositionRate, debris_flow_deposition_rate,
        m_config.m_fDebrisFlowDepositionRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, DebrisFlowRunoutCells, debris_flow_runout_cells, m_config.m_nDebrisFlowRunoutCells, 1, 64,
        Variant::INT, PROPERTY_HINT_RANGE, "1,64,1,suffix: cells" )

T3D_EROSION_GROUP( "Weathering and Oxidation", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, WeatheringEnabled, weathering_enabled, m_config.m_bWeatheringEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, PhysicalWeatheringMPerYear, physical_weathering_m_per_year,
        m_config.m_fPhysicalWeatheringMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, ChemicalWeatheringMPerYear, chemical_weathering_m_per_year,
        m_config.m_fChemicalWeatheringMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, FreezeThawStrength, freeze_thaw_strength, m_config.m_fFreezeThawStrength, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, TalusAngleDegrees, talus_angle_degrees, m_config.m_fTalusAngleDegrees, 0.0f, 89.9f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,89.9,0.1,suffix: °" )
T3D_EROSION_PROPERTY( CLAMPED, float, ThermalCreepRate, thermal_creep_rate, m_config.m_fThermalCreepRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, BaseHardness, base_hardness, m_config.m_fBaseHardness, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, HardnessVariation, hardness_variation, m_config.m_fHardnessVariation, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, OxidationEnabled, oxidation_enabled, m_config.m_bOxidationEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, OxidationRatePerYear, oxidation_rate_per_year, m_config.m_fOxidationRatePerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001" )
T3D_EROSION_PROPERTY( CLAMPED, float, OxidationWeakening, oxidation_weakening, m_config.m_fOxidationWeakening, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WeatheringSlopeBias, weathering_slope_bias, m_config.m_fWeatheringSlopeBias, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, ChemicalMoistureExponent, chemical_moisture_exponent,
        m_config.m_fChemicalMoistureExponent, 0.1f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.1,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, FreezePoint, freeze_point, m_config.m_fFreezePoint, -1.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, WetDryEnabled, wet_dry_enabled, m_config.m_bWetDryEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, WetDryWeatheringMPerCycle, wet_dry_weathering_m_per_cycle,
        m_config.m_fWetDryWeatheringMPerCycle, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/cycle" )
T3D_EROSION_PROPERTY( CLAMPED, float, WetThreshold, wet_threshold, m_config.m_fWetThreshold, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.001" )
T3D_EROSION_PROPERTY( CLAMPED, float, DryThreshold, dry_threshold, m_config.m_fDryThreshold, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.001" )
T3D_EROSION_PROPERTY( TOGGLE, bool, SaltWeatheringEnabled, salt_weathering_enabled, m_config.m_bSaltWeatheringEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( REF, Ref<Image>, SalinityMask, salinity_mask, m_salinityMask, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Image" )
T3D_EROSION_PROPERTY( REF, Rect2, SalinityWorldRect, salinity_world_rect, m_config.m_salinityWorldRect, 0, 0,
        Variant::RECT2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, BackgroundSalinity, background_salinity, m_config.m_fBackgroundSalinity, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SaltWeatheringMPerYear, salt_weathering_m_per_year, m_config.m_fSaltWeatheringMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, SaltCrystallizationStrength, salt_crystallization_strength,
        m_config.m_fSaltCrystallizationStrength, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SaltEvaporationBias, salt_evaporation_bias, m_config.m_fSaltEvaporationBias, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SaltCohesionLoss, salt_cohesion_loss, m_config.m_fSaltCohesionLoss, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, ExposureAgeEnabled, exposure_age_enabled, m_config.m_bExposureAgeEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, VarnishMaturationYears, varnish_maturation_years,
        m_config.m_fVarnishMaturationYears, 1.0f, 1000000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "1,1000000000,1,suffix: years" )
T3D_EROSION_PROPERTY( CLAMPED, float, ExposureResetDepthM, exposure_reset_depth_m, m_config.m_fExposureResetDepthM, 0.000001f, 10000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.000001,10000,0.001,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, VarnishStabilitySlope, varnish_stability_slope, m_config.m_fVarnishStabilitySlope, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, VarnishOxidationMultiplier, varnish_oxidation_multiplier,
        m_config.m_fVarnishOxidationMultiplier, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( TOGGLE, bool, SnowmeltEnabled, snowmelt_enabled, m_config.m_bSnowmeltEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, SnowfallFraction, snowfall_fraction, m_config.m_fSnowfallFraction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SnowAccumulationTemperature, snow_accumulation_temperature,
        m_config.m_fSnowAccumulationTemperature, -1.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SnowMeltTemperature, snow_melt_temperature, m_config.m_fSnowMeltTemperature, -1.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SnowMeltFractionPerStep, snow_melt_fraction_per_step, m_config.m_fSnowMeltFractionPerStep, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, SnowSublimationFraction, snow_sublimation_fraction, m_config.m_fSnowSublimationFraction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )

T3D_EROSION_GROUP( "Wind", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, WindEnabled, wind_enabled, m_config.m_bWindEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CUSTOM_REF, Vector2, WindDirection, wind_direction, m_config.m_windDirection, 0, 0,
        Variant::VECTOR2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindStrength, wind_strength, m_config.m_fWindStrength, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindVariation, wind_variation, m_config.m_fWindVariation, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindErosionMPerYear, wind_erosion_m_per_year, m_config.m_fWindErosionMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindTransportRate, wind_transport_rate, m_config.m_fWindTransportRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindDepositionRate, wind_deposition_rate, m_config.m_fWindDepositionRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindShelterStrength, wind_shelter_strength, m_config.m_fWindShelterStrength, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindThreshold, wind_threshold, m_config.m_fWindThreshold, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, WindTransportDistanceCells, wind_transport_distance_cells,
        m_config.m_nWindTransportDistanceCells, 1, 32,
        Variant::INT, PROPERTY_HINT_RANGE, "1,32,1,suffix: cells" )
T3D_EROSION_PROPERTY( CLAMPED, float, WindDepositionSlopeBias, wind_deposition_slope_bias, m_config.m_fWindDepositionSlopeBias, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )

T3D_EROSION_GROUP( "Dune Relaxation", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, DuneEnabled, dune_enabled, m_config.m_bDuneEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, SandReposeAngleDegrees, sand_repose_angle_degrees, m_config.m_fSandReposeAngleDegrees, 1.0f, 89.9f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "1,89.9,0.1,suffix: °" )
T3D_EROSION_PROPERTY( CLAMPED, float, DuneAvalancheRate, dune_avalanche_rate, m_config.m_fDuneAvalancheRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, DuneRelaxationIterations, dune_relaxation_iterations, m_config.m_nDuneRelaxationIterations, 1, 32,
        Variant::INT, PROPERTY_HINT_RANGE, "1,32,1" )
T3D_EROSION_PROPERTY( CLAMPED, float, DuneMinSandDepthM, dune_min_sand_depth_m, m_config.m_fDuneMinSandDepthM, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.001,suffix: m" )

T3D_EROSION_GROUP( "Desert Pavement", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, DesertPavementEnabled, desert_pavement_enabled, m_config.m_bDesertPavementEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, PavementFormationRate, pavement_formation_rate, m_config.m_fPavementFormationRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, PavementFineRemovalThreshold, pavement_fine_removal_threshold,
        m_config.m_fPavementFineRemovalThreshold, 0.001f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.001,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, PavementArmorStrength, pavement_armor_strength, m_config.m_fPavementArmorStrength, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, PavementInfiltrationReduction, pavement_infiltration_reduction,
        m_config.m_fPavementInfiltrationReduction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )

T3D_EROSION_GROUP( "Wave Erosion", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, WaveEnabled, wave_enabled, m_config.m_bWaveEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, SeaLevel, sea_level, m_config.m_fSeaLevel, -1000000.0f, 1000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "-1000000,1000000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, TidalRange, tidal_range, m_config.m_fTidalRange, 0.0f, 100000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( CUSTOM_REF, Vector2, WaveDirection, wave_direction, m_config.m_waveDirection, 0, 0,
        Variant::VECTOR2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, WaveStrength, wave_strength, m_config.m_fWaveStrength, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, WaveErosionMPerYear, wave_erosion_m_per_year, m_config.m_fWaveErosionMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, WaveDepositionRate, wave_deposition_rate, m_config.m_fWaveDepositionRate, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, CoastlineBandM, coastline_band_m, m_config.m_fCoastlineBandM, 0.001f, 100000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.001,100000,0.1,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, WaveFetchDistanceM, wave_fetch_distance_m, m_config.m_fWaveFetchDistanceM, 0.0f, 10000000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10000000,1,suffix: m" )
T3D_EROSION_PROPERTY( CLAMPED, float, WaveUndercutBias, wave_undercut_bias, m_config.m_fWaveUndercutBias, 0.0f, 10.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,10,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, BeachSmoothing, beach_smoothing, m_config.m_fBeachSmoothing, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )

T3D_EROSION_GROUP( "Foot Traffic", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, TrafficEnabled, traffic_enabled, m_config.m_bTrafficEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( REF, Ref<Image>, TrafficMask, traffic_mask, m_trafficMask, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Image" )
T3D_EROSION_PROPERTY( REF, Rect2, TrafficWorldRect, traffic_world_rect, m_config.m_trafficWorldRect, 0, 0,
        Variant::RECT2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, TrafficErosionMPerYear, traffic_erosion_m_per_year, m_config.m_fTrafficErosionMPerYear, 0.0f, 1000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1000,0.00001,suffix: m/year" )
T3D_EROSION_PROPERTY( CLAMPED, float, TrafficCompaction, traffic_compaction, m_config.m_fTrafficCompaction, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, TrafficSmoothing, traffic_smoothing, m_config.m_fTrafficSmoothing, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, TrafficRunoffMultiplier, traffic_runoff_multiplier, m_config.m_fTrafficRunoffMultiplier, 0.0f, 100.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, TrafficThreshold, traffic_threshold, m_config.m_fTrafficThreshold, 0.0f, 0.999f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,0.999,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, int32_t, TrafficWideningCells, traffic_widening_cells, m_config.m_nTrafficWideningCells, 0, 32,
        Variant::INT, PROPERTY_HINT_RANGE, "0,32,1,suffix: cells" )

T3D_EROSION_GROUP( "Layered Substrate", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, StratigraphyEnabled, stratigraphy_enabled, m_config.m_bStratigraphyEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, SurfaceLayerDepthM, surface_layer_depth_m, m_config.m_fSurfaceLayerDepthM, 0.0f, 100000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,100000,0.01,suffix: m" )
T3D_EROSION_PROPERTY( REF, Ref<Image>, SurfaceLayerDepthMap, surface_layer_depth_map, m_surfaceLayerDepthMap, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Image" )
T3D_EROSION_PROPERTY( REF, Rect2, SurfaceLayerDepthWorldRect, surface_layer_depth_world_rect, m_config.m_surfaceLayerDepthWorldRect, 0, 0,
        Variant::RECT2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, BedrockHardness, bedrock_hardness, m_config.m_fBedrockHardness, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( REF, Ref<Image>, BedrockHardnessMap, bedrock_hardness_map, m_bedrockHardnessMap, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Image" )
T3D_EROSION_PROPERTY( REF, Rect2, BedrockHardnessWorldRect, bedrock_hardness_world_rect, m_config.m_bedrockHardnessWorldRect, 0, 0,
        Variant::RECT2, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, StratigraphyTransitionWidthM, stratigraphy_transition_width_m,
        m_config.m_fStratigraphyTransitionWidthM, 0.000001f, 100000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.000001,100000,0.01,suffix: m" )

T3D_EROSION_GROUP( "Material Transitions", "" )
T3D_EROSION_PROPERTY( TOGGLE, bool, MaterialOutputEnabled, material_output_enabled, m_config.m_bMaterialOutputEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( VALUE, bool, PreserveControlFlags, preserve_control_flags, m_config.m_bPreserveControlFlags, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( VALUE, bool, ResolveAutoMaterials, resolve_auto_materials, m_config.m_bResolveAutoMaterials, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, MaterialTransitionStrength, material_transition_strength,
        m_config.m_fMaterialTransitionStrength, 0.0f, 8.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,8,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, MaterialTransitionThreshold, material_transition_threshold,
        m_config.m_fMaterialTransitionThreshold, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )
T3D_EROSION_PROPERTY( CLAMPED, float, MaterialTransitionDepthM, material_transition_depth_m,
        m_config.m_fMaterialTransitionDepthM, 0.001f, 10000.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0.001,10000,0.01,suffix: m" )
T3D_EROSION_PROPERTY( TOGGLE, bool, OxidationColorEnabled, oxidation_color_enabled, m_config.m_bOxidationColorEnabled, 0, 0,
        Variant::BOOL, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( REF, Color, OxidationTint, oxidation_tint, m_config.m_oxidationTint, 0, 0,
        Variant::COLOR, PROPERTY_HINT_NONE, "" )
T3D_EROSION_PROPERTY( CLAMPED, float, OxidationTintStrength, oxidation_tint_strength, m_config.m_fOxidationTintStrength, 0.0f, 1.0f,
        Variant::FLOAT, PROPERTY_HINT_RANGE, "0,1,0.01" )

T3D_EROSION_GROUP( "Time Curves", "" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, MasterIntensityCurve, master_intensity_curve, m_masterIntensityCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, RainfallCurve, rainfall_curve, m_rainfallCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, WeatheringCurve, weathering_curve, m_weatheringCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, OxidationCurve, oxidation_curve, m_oxidationCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, WindCurve, wind_curve, m_windCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, WaveCurve, wave_curve, m_waveCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, TrafficCurve, traffic_curve, m_trafficCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, TemperatureCurve, temperature_curve, m_temperatureCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, SeaLevelCurve, sea_level_curve, m_seaLevelCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, SnowmeltCurve, snowmelt_curve, m_snowmeltCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, SaltWeatheringCurve, salt_weathering_curve, m_saltWeatheringCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
T3D_EROSION_PROPERTY( CURVE, Ref<Curve>, DebrisFlowCurve, debris_flow_curve, m_debrisFlowCurve, 0, 0,
        Variant::OBJECT, PROPERTY_HINT_RESOURCE_TYPE, "Curve" )
