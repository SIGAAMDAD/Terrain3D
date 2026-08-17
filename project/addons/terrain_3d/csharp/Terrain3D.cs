#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;
using Godot.Collections;

namespace TokisanGames;

[Tool]
public partial class Terrain3D : Node3D
{

	private new static readonly StringName NativeName = new StringName("Terrain3D");

	[Obsolete("Wrapper types cannot be constructed with constructors (it only instantiate the underlying Terrain3D object), please use the Instantiate() method instead.")]
	protected Terrain3D() { }

	private static CSharpScript _wrapperScriptAsset;

	/// <summary>
	/// Try to cast the script on the supplied <paramref name="godotObject"/> to the <see cref="Terrain3D"/> wrapper type,
	/// if no script has attached to the type, or the script attached to the type does not inherit the <see cref="Terrain3D"/> wrapper type,
	/// a new instance of the <see cref="Terrain3D"/> wrapper script will get attaches to the <paramref name="godotObject"/>.
	/// </summary>
	/// <remarks>The developer should only supply the <paramref name="godotObject"/> that represents the correct underlying GDExtension type.</remarks>
	/// <param name="godotObject">The <paramref name="godotObject"/> that represents the correct underlying GDExtension type.</param>
	/// <returns>The existing or a new instance of the <see cref="Terrain3D"/> wrapper script attached to the supplied <paramref name="godotObject"/>.</returns>
	public new static Terrain3D Bind(GodotObject godotObject)
	{
		if (!IsInstanceValid(godotObject))
			return null;

		if (godotObject is Terrain3D wrapperScriptInstance)
			return wrapperScriptInstance;

#if DEBUG
		var expectedType = typeof(Terrain3D);
		var currentObjectClassName = godotObject.GetClass();
		if (!ClassDB.IsParentClass(expectedType.Name, currentObjectClassName))
			throw new InvalidOperationException($"The supplied GodotObject ({currentObjectClassName}) is not the {expectedType.Name} type.");
#endif

		if (_wrapperScriptAsset is null)
		{
			var scriptPathAttribute = typeof(Terrain3D).GetCustomAttributes<ScriptPathAttribute>().FirstOrDefault();
			if (scriptPathAttribute is null) throw new UnreachableException();
			_wrapperScriptAsset = ResourceLoader.Load<CSharpScript>(scriptPathAttribute.Path);
		}

		var instanceId = godotObject.GetInstanceId();
		godotObject.SetScript(_wrapperScriptAsset);
		return (Terrain3D)InstanceFromId(instanceId);
	}

	/// <summary>
	/// Creates an instance of the GDExtension <see cref="Terrain3D"/> type, and attaches a wrapper script instance to it.
	/// </summary>
	/// <returns>The wrapper instance linked to the underlying GDExtension "Terrain3D" type.</returns>
	public new static Terrain3D Instantiate() => Bind(ClassDB.Instantiate(NativeName).As<GodotObject>());

	public enum DebugLevelEnum
	{
		Error = 0,
		Info = 1,
		Debug = 2,
		Extreme = 3,
	}

	public enum RegionSizeEnum
	{
		Size64 = 64,
		Size128 = 128,
		Size256 = 256,
		Size512 = 512,
		Size1024 = 1024,
		Size2048 = 2048,
	}

	public enum EditorExecutionModeEnum
	{
		Authoring = 0,
		RuntimeProfile = 1,
	}

	public enum EnvironmentShadowQualityEnum
	{
		Low = 0,
		Normal = 1,
		High = 2,
		Ultra = 3,
	}

	public new class GDExtensionSignalName : Node3D.SignalName
	{
		/// <summary>
		/// Cached name for the 'material_changed' member.
		/// </summary>
		public new static readonly StringName MaterialChanged = "material_changed";
		/// <summary>
		/// Cached name for the 'assets_changed' member.
		/// </summary>
		public new static readonly StringName AssetsChanged = "assets_changed";
		/// <summary>Cached name for editor execution-policy changes.</summary>
		public new static readonly StringName EditorExecutionModeChanged = "editor_execution_mode_changed";
	}

	public new delegate void MaterialChangedSignalHandler();
	private MaterialChangedSignalHandler _materialChangedSignal;
	private Callable _materialChangedSignalCallable;
	public event MaterialChangedSignalHandler MaterialChangedSignal
	{
		add
		{
			if (_materialChangedSignal is null)
			{
				_materialChangedSignalCallable = Callable.From(() => 
					_materialChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.MaterialChanged, _materialChangedSignalCallable);
			}
			_materialChangedSignal += value;
		}
		remove
		{
			_materialChangedSignal -= value;
			if (_materialChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.MaterialChanged, _materialChangedSignalCallable);
			_materialChangedSignalCallable = default;
		}
	}

	public new delegate void AssetsChangedSignalHandler();
	private AssetsChangedSignalHandler _assetsChangedSignal;
	private Callable _assetsChangedSignalCallable;
	public event AssetsChangedSignalHandler AssetsChangedSignal
	{
		add
		{
			if (_assetsChangedSignal is null)
			{
				_assetsChangedSignalCallable = Callable.From(() => 
					_assetsChangedSignal?.Invoke());
				Connect(GDExtensionSignalName.AssetsChanged, _assetsChangedSignalCallable);
			}
			_assetsChangedSignal += value;
		}
		remove
		{
			_assetsChangedSignal -= value;
			if (_assetsChangedSignal is not null) return;
			Disconnect(GDExtensionSignalName.AssetsChanged, _assetsChangedSignalCallable);
			_assetsChangedSignalCallable = default;
		}
	}

	public new class GDExtensionPropertyName : Node3D.PropertyName
	{
		/// <summary>
		/// Cached name for the 'version' member.
		/// </summary>
		public new static readonly StringName Version = "version";
		/// <summary>
		/// Cached name for the 'debug_level' member.
		/// </summary>
		public new static readonly StringName DebugLevel = "debug_level";
		/// <summary>
		/// Cached name for the 'data_directory' member.
		/// </summary>
		public new static readonly StringName DataDirectory = "data_directory";
		/// <summary>Cached name for continuous runtime diagnostic sampling.</summary>
		public new static readonly StringName RuntimeProfilingEnabled = "runtime_profiling_enabled";
		/// <summary>Cached name for adaptive runtime work-budget control.</summary>
		public new static readonly StringName RuntimeAdaptiveWorkBudgetsEnabled = "runtime_adaptive_work_budgets_enabled";
		/// <summary>Cached name for the adaptive terrain CPU-time target.</summary>
		public new static readonly StringName RuntimeAdaptiveTargetMs = "runtime_adaptive_target_ms";
		/// <summary>Cached name for the adaptive terrain CPU-time hard maximum.</summary>
		public new static readonly StringName RuntimeAdaptiveHardMaxMs = "runtime_adaptive_hard_max_ms";
		/// <summary>Cached name for the adaptive adjustment window.</summary>
		public new static readonly StringName RuntimeAdaptiveAdjustmentWindowFrames = "runtime_adaptive_adjustment_window_frames";
		/// <summary>Cached name for the minimum adaptive GPU publication budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinGpuPages = "runtime_adaptive_min_gpu_pages";
		/// <summary>Cached name for the minimum adaptive CPU page-load budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinCpuPages = "runtime_adaptive_min_cpu_pages";
		/// <summary>Cached name for the minimum adaptive instance-page budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinInstancePages = "runtime_adaptive_min_instance_pages";
		/// <summary>Cached name for the minimum adaptive instancer-cell budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinInstancerCells = "runtime_adaptive_min_instancer_cells";
		/// <summary>Cached name for the minimum adaptive dynamic-node load budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinNodeLoadRegions = "runtime_adaptive_min_node_load_regions";
		/// <summary>Cached name for the minimum adaptive dynamic-node unload budget.</summary>
		public new static readonly StringName RuntimeAdaptiveMinNodeUnloadRegions = "runtime_adaptive_min_node_unload_regions";
		/// <summary>Cached name for boundary-triggered secondary work phasing.</summary>
		public new static readonly StringName RuntimeBoundaryWorkPhasingEnabled = "runtime_boundary_work_phasing_enabled";
		/// <summary>Cached name for the dynamic-node boundary delay.</summary>
		public new static readonly StringName RuntimeBoundaryNodeDelayFrames = "runtime_boundary_node_delay_frames";
		/// <summary>Cached name for the instancer boundary delay.</summary>
		public new static readonly StringName RuntimeBoundaryInstancerDelayFrames = "runtime_boundary_instancer_delay_frames";
		/// <summary>Cached name for the editor authoring/runtime-profile policy.</summary>
		public new static readonly StringName EditorExecutionMode = "editor_execution_mode";
		/// <summary>Cached legacy alias for explicit runtime streaming preview in the editor.</summary>
		public new static readonly StringName EditorRuntimeStreamingPreview = "editor_runtime_streaming_preview";
		/// <summary>Cached name for the compiled near/middle/far runtime shader split.</summary>
		public new static readonly StringName RuntimeShaderTieringEnabled = "runtime_shader_tiering_enabled";
		/// <summary>Cached name for the number of full-detail clipmap levels.</summary>
		public new static readonly StringName RuntimeNearShaderLods = "runtime_near_shader_lods";
		/// <summary>Cached name for the ring-quantized near-tier endpoint in metres.</summary>
		public new static readonly StringName RuntimeNearRangeEndM = "runtime_near_range_end_m";
		/// <summary>Cached name for the ring-quantized middle-tier endpoint in metres.</summary>
		public new static readonly StringName RuntimeMiddleRangeEndM = "runtime_middle_range_end_m";
		/// <summary>Cached name for automatic detailed-residency coupling to the near tier.</summary>
		public new static readonly StringName RuntimeSyncDetailLods = "runtime_sync_detail_lods";
		/// <summary>Cached name for the number of macro-surface clipmap levels.</summary>
		public new static readonly StringName RuntimeMiddleShaderLods = "runtime_middle_shader_lods";
		/// <summary>Cached name for the deferred clipmap transform budget.</summary>
		public new static readonly StringName RuntimeClipmapUpdateBudget = "runtime_clipmap_update_budget";
		/// <summary>Cached name for cooked-page preparation worker count.</summary>
		public new static readonly StringName RuntimePagePrepareThreads = "runtime_page_prepare_threads";
		/// <summary>Cached name for bounded physical GPU terrain-region residency.</summary>
		public new static readonly StringName GpuRegionCacheEnabled = "gpu_region_cache_enabled";
		/// <summary>Cached name for the physical region-map memory budget.</summary>
		public new static readonly StringName GpuRegionCacheBudgetMb = "gpu_region_cache_budget_mb";
		/// <summary>Cached name for the per-tick physical page publication limit.</summary>
		public new static readonly StringName GpuRegionUploadBudgetLayers = "gpu_region_upload_budget_layers";
		/// <summary>Cached name for the per-tick terrain upload byte budget.</summary>
		public new static readonly StringName GpuRegionUploadBudgetMb = "gpu_region_upload_budget_mb";
		/// <summary>Cached name for unreferenced physical pages reserved for upload and retirement.</summary>
		public new static readonly StringName GpuRegionSparePages = "gpu_region_spare_pages";
		/// <summary>Cached name for the in-flight render-frame retirement delay.</summary>
		public new static readonly StringName GpuRegionRetireFrames = "gpu_region_retire_frames";
		/// <summary>Cached name for clipmap LODs requiring detailed physical region pages.</summary>
		public new static readonly StringName GpuRegionDetailLods = "gpu_region_detail_lods";
		/// <summary>Cached name for the requested detailed physical-residency endpoint in metres.</summary>
		public new static readonly StringName GpuRegionDetailRangeEndM = "gpu_region_detail_range_end_m";
		/// <summary>Cached name for the maximum supported detailed clipmap LOD count.</summary>
		public new static readonly StringName GpuRegionMaxSupportedDetailLods = "gpu_region_max_supported_detail_lods";
		/// <summary>Cached name for the maximum supported detailed-residency endpoint in metres.</summary>
		public new static readonly StringName GpuRegionMaxSupportedDetailRangeM = "gpu_region_max_supported_detail_range_m";
		/// <summary>Cached name for the physical pages required by the requested detailed footprint.</summary>
		public new static readonly StringName GpuRegionRequiredCorePages = "gpu_region_required_core_pages";
		/// <summary>Cached name for mapped physical page capacity.</summary>
		public new static readonly StringName GpuRegionMappedCapacity = "gpu_region_mapped_capacity";
		/// <summary>Cached name for detailed-footprint validity under the current cache layout.</summary>
		public new static readonly StringName GpuRegionDetailRangeValid = "gpu_region_detail_range_valid";
		/// <summary>Cached name for the physical residency guard-band width.</summary>
		public new static readonly StringName GpuRegionGuardRegions = "gpu_region_guard_regions";
		/// <summary>Cached name for velocity-directed prefetch rows.</summary>
		public new static readonly StringName GpuRegionPrefetchRows = "gpu_region_prefetch_rows";
		/// <summary>Cached name for physical page retention after leaving the footprint.</summary>
		public new static readonly StringName GpuRegionHysteresisTicks = "gpu_region_hysteresis_ticks";
		/// <summary>Cached name for physical color-page spatial reduction.</summary>
		public new static readonly StringName GpuRegionColorResolutionDivisor = "gpu_region_color_resolution_divisor";
		/// <summary>Cached name for physical derived-page spatial reduction.</summary>
		public new static readonly StringName GpuRegionDerivedResolutionDivisor = "gpu_region_derived_resolution_divisor";
		/// <summary>
		/// Cached name for the 'gpu_region_cooked_pages_enabled' property.
		/// </summary>
		public new static readonly StringName GpuRegionCookedPagesEnabled = "gpu_region_cooked_pages_enabled";
		/// <summary>
		/// Cached name for the 'gpu_region_cook_on_save' property.
		/// </summary>
		public new static readonly StringName GpuRegionCookOnSave = "gpu_region_cook_on_save";
		/// <summary>Cached name for CPU terrain-region eviction.</summary>
		public new static readonly StringName CpuRegionCacheEnabled = "cpu_region_cache_enabled";
		/// <summary>Cached name for CPU terrain-region cache memory.</summary>
		public new static readonly StringName CpuRegionCacheBudgetMb = "cpu_region_cache_budget_mb";
		/// <summary>Cached name for per-tick CPU terrain-region loads.</summary>
		public new static readonly StringName CpuRegionLoadBudgetLayers = "cpu_region_load_budget_layers";
		/// <summary>Cached name for per-tick CPU terrain-region bytes.</summary>
		public new static readonly StringName CpuRegionLoadBudgetMb = "cpu_region_load_budget_mb";
		/// <summary>Cached name for instance-region eviction.</summary>
		public new static readonly StringName InstanceRegionCacheEnabled = "instance_region_cache_enabled";
		/// <summary>Cached name for instance-region cache memory.</summary>
		public new static readonly StringName InstanceRegionCacheBudgetMb = "instance_region_cache_budget_mb";
		/// <summary>Cached name for per-tick instance-region page loads.</summary>
		public new static readonly StringName InstanceRegionLoadBudgetPages = "instance_region_load_budget_pages";
		/// <summary>Cached name for per-tick instance-region bytes.</summary>
		public new static readonly StringName InstanceRegionLoadBudgetMb = "instance_region_load_budget_mb";
		/// <summary>Cached name for dynamic-node streaming.</summary>
		public new static readonly StringName DynamicNodeStreamingEnabled = "dynamic_node_streaming_enabled";
		/// <summary>Cached name for dynamic-node region radius.</summary>
		public new static readonly StringName DynamicNodeStreamRadius = "dynamic_node_stream_radius";
		/// <summary>Cached name for per-tick dynamic-node region loads.</summary>
		public new static readonly StringName DynamicNodeLoadRegionBudget = "dynamic_node_load_region_budget";
		/// <summary>Cached name for per-tick dynamic-node instantiation.</summary>
		public new static readonly StringName DynamicNodeLoadNodeBudget = "dynamic_node_load_node_budget";
		/// <summary>Cached name for per-tick dynamic-node region unloads.</summary>
		public new static readonly StringName DynamicNodeUnloadRegionBudget = "dynamic_node_unload_region_budget";
		/// <summary>Cached name for streamed LightmapGI package residency.</summary>
		public new static readonly StringName LightmapStreamingEnabled = "lightmap_streaming_enabled";
		/// <summary>Cached name for regional baked-lighting request radius.</summary>
		public new static readonly StringName LightmapStreamRadius = "lightmap_stream_radius";
		/// <summary>Cached name for per-update baked-lighting package activation budget.</summary>
		public new static readonly StringName LightmapLoadBudget = "lightmap_load_budget";
		/// <summary>Cached name for per-update baked-lighting package retirement budget.</summary>
		public new static readonly StringName LightmapUnloadBudget = "lightmap_unload_budget";
		/// <summary>Cached name for semantic environment shadow-caster policy.</summary>
		public new static readonly StringName SemanticShadowPolicyEnabled = "semantic_shadow_policy_enabled";
		/// <summary>Cached name for semantic environment shadow quality.</summary>
		public new static readonly StringName EnvironmentShadowQuality = "environment_shadow_quality";
		/// <summary>Cached name for non-Terrain3D render layers retained in the directional shadow caster mask.</summary>
		public new static readonly StringName EnvironmentShadowExternalMask = "environment_shadow_external_mask";
		/// <summary>Cached name for explicit semantic sun/moon reception policy.</summary>
		public new static readonly StringName SemanticDirectionalReceptionEnabled = "semantic_directional_reception_enabled";
		/// <summary>Cached name for external render layers that continue receiving the managed directional light.</summary>
		public new static readonly StringName EnvironmentDirectionalExternalMask = "environment_directional_external_mask";
		/// <summary>Cached name for the persisted instancer cell size.</summary>
		public new static readonly StringName InstancerCellSize = "instancer_cell_size";
		/// <summary>Cached name for static-instance cell streaming.</summary>
		public new static readonly StringName InstancerCellStreamingEnabled = "instancer_cell_streaming_enabled";
		/// <summary>Cached name for single-LOD instance buffers.</summary>
		public new static readonly StringName InstancerSingleLodBuffers = "instancer_single_lod_buffers";
		/// <summary>Cached name for instance-cell streaming margin.</summary>
		public new static readonly StringName InstancerCellStreamingMargin = "instancer_cell_streaming_margin";
		/// <summary>Cached name for instance streaming movement threshold.</summary>
		public new static readonly StringName InstancerCellStreamingUpdateDistance = "instancer_cell_streaming_update_distance";
		/// <summary>Cached name for instance-cell update budget.</summary>
		public new static readonly StringName InstancerUpdateCellBudget = "instancer_update_cell_budget";
		/// <summary>
		/// Cached name for the 'material' member.
		/// </summary>
		public new static readonly StringName Material = "material";
		/// <summary>
		/// Cached name for the 'assets' member.
		/// </summary>
		public new static readonly StringName Assets = "assets";
		/// <summary>
		/// Cached name for the 'data' member.
		/// </summary>
		public new static readonly StringName Data = "data";
		/// <summary>
		/// Cached name for the 'collision' member.
		/// </summary>
		public new static readonly StringName Collision = "collision";
		/// <summary>
		/// Cached name for the 'instancer' member.
		/// </summary>
		public new static readonly StringName Instancer = "instancer";
		/// <summary>
		/// Cached name for the 'light_target' member.
		/// </summary>
		public new static readonly StringName LightTarget = "light_target";
		/// <summary>
		/// Cached name for the 'region_size' member.
		/// </summary>
		public new static readonly StringName RegionSize = "region_size";
		/// <summary>
		/// Cached name for the 'save_16_bit' member.
		/// </summary>
		public new static readonly StringName Save16Bit = "save_16_bit";
		/// <summary>
		/// Cached name for the 'label_distance' member.
		/// </summary>
		public new static readonly StringName LabelDistance = "label_distance";
		/// <summary>
		/// Cached name for the 'label_size' member.
		/// </summary>
		public new static readonly StringName LabelSize = "label_size";
		/// <summary>
		/// Cached name for the 'show_grid' member.
		/// </summary>
		public new static readonly StringName ShowGrid = "show_grid";
		/// <summary>
		/// Cached name for the 'collision_mode' member.
		/// </summary>
		public new static readonly StringName CollisionMode = "collision_mode";
		/// <summary>
		/// Cached name for the 'collision_shape_size' member.
		/// </summary>
		public new static readonly StringName CollisionShapeSize = "collision_shape_size";
		/// <summary>
		/// Cached name for the 'collision_radius' member.
		/// </summary>
		public new static readonly StringName CollisionRadius = "collision_radius";
		/// <summary>
		/// Cached name for the 'collision_target' member.
		/// </summary>
		public new static readonly StringName CollisionTarget = "collision_target";
		/// <summary>
		/// Cached name for the 'collision_layer' member.
		/// </summary>
		public new static readonly StringName CollisionLayer = "collision_layer";
		/// <summary>
		/// Cached name for the 'collision_mask' member.
		/// </summary>
		public new static readonly StringName CollisionMask = "collision_mask";
		/// <summary>
		/// Cached name for the 'collision_priority' member.
		/// </summary>
		public new static readonly StringName CollisionPriority = "collision_priority";
		/// <summary>
		/// Cached name for the 'physics_material' member.
		/// </summary>
		public new static readonly StringName PhysicsMaterial = "physics_material";
		/// <summary>
		/// Cached name for the 'clipmap_target' member.
		/// </summary>
		public new static readonly StringName ClipmapTarget = "clipmap_target";
		/// <summary>
		/// Cached name for the 'mesh_lods' member.
		/// </summary>
		public new static readonly StringName MeshLods = "mesh_lods";
		/// <summary>
		/// Cached name for the 'tessellation_level' member.
		/// </summary>
		public new static readonly StringName TessellationLevel = "tessellation_level";
		public new static readonly StringName ComputeDisplacementEnabled = "compute_displacement_enabled";
		/// <summary>
		/// Cached name for the 'mesh_size' member.
		/// </summary>
		public new static readonly StringName MeshSize = "mesh_size";
		/// <summary>
		/// Cached name for the 'vertex_spacing' member.
		/// </summary>
		public new static readonly StringName VertexSpacing = "vertex_spacing";
		/// <summary>
		/// Cached name for the 'cull_margin' member.
		/// </summary>
		public new static readonly StringName CullMargin = "cull_margin";
		/// <summary>
		/// Cached name for the 'cast_shadows' member.
		/// </summary>
		public new static readonly StringName CastShadows = "cast_shadows";
		public new static readonly StringName ShadowLodCount = "shadow_lod_count";
		public new static readonly StringName NearShadowEnabled = "near_shadow_enabled";
		public new static readonly StringName MiddleShadowEnabled = "middle_shadow_enabled";
		public new static readonly StringName FarShadowEnabled = "far_shadow_enabled";
		/// <summary>
		/// Cached name for the 'gi_mode' member.
		/// </summary>
		public new static readonly StringName GiMode = "gi_mode";
		/// <summary>
		/// Cached name for the 'render_layers' member.
		/// </summary>
		public new static readonly StringName RenderLayers = "render_layers";
		/// <summary>
		/// Cached name for the 'displacement_scale' member.
		/// </summary>
		public new static readonly StringName DisplacementScale = "displacement_scale";
		/// <summary>
		/// Cached name for the 'displacement_sharpness' member.
		/// </summary>
		public new static readonly StringName DisplacementSharpness = "displacement_sharpness";
		/// <summary>
		/// Cached name for the 'buffer_shader_override_enabled' member.
		/// </summary>
		public new static readonly StringName BufferShaderOverrideEnabled = "buffer_shader_override_enabled";
		/// <summary>
		/// Cached name for the 'buffer_shader_override' member.
		/// </summary>
		public new static readonly StringName BufferShaderOverride = "buffer_shader_override";
		/// <summary>
		/// Cached name for the 'ocean_enabled' member.
		/// </summary>
		public new static readonly StringName OceanEnabled = "ocean_enabled";
		/// <summary>
		/// Cached name for the 'ocean_mesh_lods' member.
		/// </summary>
		public new static readonly StringName OceanMeshLods = "ocean_mesh_lods";
		/// <summary>
		/// Cached name for the 'ocean_tessellation_level' member.
		/// </summary>
		public new static readonly StringName OceanTessellationLevel = "ocean_tessellation_level";
		/// <summary>
		/// Cached name for the 'ocean_mesh_size' member.
		/// </summary>
		public new static readonly StringName OceanMeshSize = "ocean_mesh_size";
		/// <summary>
		/// Cached name for the 'ocean_vertex_spacing' member.
		/// </summary>
		public new static readonly StringName OceanVertexSpacing = "ocean_vertex_spacing";
		/// <summary>
		/// Cached name for the 'ocean_cull_margin' member.
		/// </summary>
		public new static readonly StringName OceanCullMargin = "ocean_cull_margin";
		/// <summary>
		/// Cached name for the 'ocean_cast_shadows' member.
		/// </summary>
		public new static readonly StringName OceanCastShadows = "ocean_cast_shadows";
		/// <summary>
		/// Cached name for the 'ocean_gi_mode' member.
		/// </summary>
		public new static readonly StringName OceanGiMode = "ocean_gi_mode";
		/// <summary>
		/// Cached name for the 'ocean_render_layers' member.
		/// </summary>
		public new static readonly StringName OceanRenderLayers = "ocean_render_layers";
		/// <summary>
		/// Cached name for the 'ocean_material' member.
		/// </summary>
		public new static readonly StringName OceanMaterial = "ocean_material";
		/// <summary>
		/// Cached name for the 'mouse_layer' member.
		/// </summary>
		public new static readonly StringName MouseLayer = "mouse_layer";
		/// <summary>
		/// Cached name for the 'free_editor_textures' member.
		/// </summary>
		public new static readonly StringName FreeEditorTextures = "free_editor_textures";
		/// <summary>
		/// Cached name for the 'instancer_mode' member.
		/// </summary>
		public new static readonly StringName InstancerMode = "instancer_mode";
		/// <summary>
		/// Cached name for the 'show_region_grid' member.
		/// </summary>
		public new static readonly StringName ShowRegionGrid = "show_region_grid";
		/// <summary>
		/// Cached name for the 'show_instancer_grid' member.
		/// </summary>
		public new static readonly StringName ShowInstancerGrid = "show_instancer_grid";
		/// <summary>
		/// Cached name for the 'show_vertex_grid' member.
		/// </summary>
		public new static readonly StringName ShowVertexGrid = "show_vertex_grid";
		/// <summary>
		/// Cached name for the 'show_contours' member.
		/// </summary>
		public new static readonly StringName ShowContours = "show_contours";
		/// <summary>
		/// Cached name for the 'show_navigation' member.
		/// </summary>
		public new static readonly StringName ShowNavigation = "show_navigation";
		/// <summary>
		/// Cached name for the 'show_checkered' member.
		/// </summary>
		public new static readonly StringName ShowCheckered = "show_checkered";
		/// <summary>
		/// Cached name for the 'show_grey' member.
		/// </summary>
		public new static readonly StringName ShowGrey = "show_grey";
		/// <summary>
		/// Cached name for the 'show_heightmap' member.
		/// </summary>
		public new static readonly StringName ShowHeightmap = "show_heightmap";
		/// <summary>
		/// Cached name for the 'show_jaggedness' member.
		/// </summary>
		public new static readonly StringName ShowJaggedness = "show_jaggedness";
		/// <summary>
		/// Cached name for the 'show_autoshader' member.
		/// </summary>
		public new static readonly StringName ShowAutoshader = "show_autoshader";
		/// <summary>
		/// Cached name for the 'show_control_texture' member.
		/// </summary>
		public new static readonly StringName ShowControlTexture = "show_control_texture";
		/// <summary>
		/// Cached name for the 'show_control_blend' member.
		/// </summary>
		public new static readonly StringName ShowControlBlend = "show_control_blend";
		/// <summary>
		/// Cached name for the 'show_control_angle' member.
		/// </summary>
		public new static readonly StringName ShowControlAngle = "show_control_angle";
		/// <summary>
		/// Cached name for the 'show_control_scale' member.
		/// </summary>
		public new static readonly StringName ShowControlScale = "show_control_scale";
		/// <summary>
		/// Cached name for the 'show_colormap' member.
		/// </summary>
		public new static readonly StringName ShowColormap = "show_colormap";
		/// <summary>
		/// Cached name for the 'show_roughmap' member.
		/// </summary>
		public new static readonly StringName ShowRoughmap = "show_roughmap";
		/// <summary>
		/// Cached name for the 'show_displacement_buffer' member.
		/// </summary>
		public new static readonly StringName ShowDisplacementBuffer = "show_displacement_buffer";
		/// <summary>
		/// Cached name for the 'show_texture_albedo' member.
		/// </summary>
		public new static readonly StringName ShowTextureAlbedo = "show_texture_albedo";
		/// <summary>
		/// Cached name for the 'show_texture_height' member.
		/// </summary>
		public new static readonly StringName ShowTextureHeight = "show_texture_height";
		/// <summary>
		/// Cached name for the 'show_texture_normal' member.
		/// </summary>
		public new static readonly StringName ShowTextureNormal = "show_texture_normal";
		/// <summary>
		/// Cached name for the 'show_texture_rough' member.
		/// </summary>
		public new static readonly StringName ShowTextureRough = "show_texture_rough";
		/// <summary>
		/// Cached name for the 'show_texture_ao' member.
		/// </summary>
		public new static readonly StringName ShowTextureAo = "show_texture_ao";
	}

	public new string Version
	{
		get => Get(GDExtensionPropertyName.Version).As<string>();
	}

	public new Terrain3D.DebugLevelEnum DebugLevel
	{
		get => Get(GDExtensionPropertyName.DebugLevel).As<Terrain3D.DebugLevelEnum>();
		set => Set(GDExtensionPropertyName.DebugLevel, Variant.From(value));
	}

	public new string DataDirectory
	{
		get => Get(GDExtensionPropertyName.DataDirectory).As<string>();
		set => Set(GDExtensionPropertyName.DataDirectory, value);
	}

	/// <summary>Continuously records live terrain subsystem timings and counters.</summary>
	public new bool RuntimeProfilingEnabled
	{
		get => Get(GDExtensionPropertyName.RuntimeProfilingEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.RuntimeProfilingEnabled, value);
	}

	/// <summary>Allows Terrain3D to adjust bounded work while preserving configured values as hard ceilings.</summary>
	public new bool RuntimeAdaptiveWorkBudgetsEnabled
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveWorkBudgetsEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveWorkBudgetsEnabled, value);
	}

	/// <summary>Target Terrain3D CPU time used by the adaptive controller.</summary>
	public new double RuntimeAdaptiveTargetMs
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveTargetMs).As<double>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveTargetMs, value);
	}

	/// <summary>Hard Terrain3D CPU-time limit used to reduce adaptive work.</summary>
	public new double RuntimeAdaptiveHardMaxMs
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveHardMaxMs).As<double>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveHardMaxMs, value);
	}

	public new long RuntimeAdaptiveAdjustmentWindowFrames
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveAdjustmentWindowFrames).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveAdjustmentWindowFrames, value);
	}

	public new long RuntimeAdaptiveMinGpuPages
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinGpuPages).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinGpuPages, value);
	}

	public new long RuntimeAdaptiveMinCpuPages
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinCpuPages).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinCpuPages, value);
	}

	public new long RuntimeAdaptiveMinInstancePages
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinInstancePages).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinInstancePages, value);
	}

	public new long RuntimeAdaptiveMinInstancerCells
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinInstancerCells).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinInstancerCells, value);
	}

	public new long RuntimeAdaptiveMinNodeLoadRegions
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinNodeLoadRegions).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinNodeLoadRegions, value);
	}

	public new long RuntimeAdaptiveMinNodeUnloadRegions
	{
		get => Get(GDExtensionPropertyName.RuntimeAdaptiveMinNodeUnloadRegions).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeAdaptiveMinNodeUnloadRegions, value);
	}

	public new bool RuntimeBoundaryWorkPhasingEnabled
	{
		get => Get(GDExtensionPropertyName.RuntimeBoundaryWorkPhasingEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.RuntimeBoundaryWorkPhasingEnabled, value);
	}

	public new long RuntimeBoundaryNodeDelayFrames
	{
		get => Get(GDExtensionPropertyName.RuntimeBoundaryNodeDelayFrames).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeBoundaryNodeDelayFrames, value);
	}

	public new long RuntimeBoundaryInstancerDelayFrames
	{
		get => Get(GDExtensionPropertyName.RuntimeBoundaryInstancerDelayFrames).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeBoundaryInstancerDelayFrames, value);
	}

	/// <summary>Selects stable rapid authoring or the exact runtime residency policy used for profiling.</summary>
	public new EditorExecutionModeEnum EditorExecutionMode
	{
		get => Get(GDExtensionPropertyName.EditorExecutionMode).As<EditorExecutionModeEnum>();
		set => Set(GDExtensionPropertyName.EditorExecutionMode, Variant.From(value));
	}

	/// <summary>Legacy boolean alias for EditorExecutionMode.</summary>
	[Obsolete("Use EditorExecutionMode instead.")]
	public new bool EditorRuntimeStreamingPreview
	{
		get => Get(GDExtensionPropertyName.EditorRuntimeStreamingPreview).As<bool>();
		set => Set(GDExtensionPropertyName.EditorRuntimeStreamingPreview, value);
	}

	/// <summary>Uses independently compiled near, middle and far terrain materials in runtime/profile mode.</summary>
	public new bool RuntimeShaderTieringEnabled
	{
		get => Get(GDExtensionPropertyName.RuntimeShaderTieringEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.RuntimeShaderTieringEnabled, value);
	}

	/// <summary>Number of innermost clipmap levels using the complete terrain material.</summary>
	public new long RuntimeNearShaderLods
	{
		get => Get(GDExtensionPropertyName.RuntimeNearShaderLods).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeNearShaderLods, value);
	}

	/// <summary>Requested near-tier endpoint in metres, rounded upward to a clipmap-ring boundary.</summary>
	public new double RuntimeNearRangeEndM
	{
		get => Get(GDExtensionPropertyName.RuntimeNearRangeEndM).As<double>();
		set => Set(GDExtensionPropertyName.RuntimeNearRangeEndM, value);
	}

	/// <summary>Requested middle-tier endpoint in metres, rounded upward to a clipmap-ring boundary.</summary>
	public new double RuntimeMiddleRangeEndM
	{
		get => Get(GDExtensionPropertyName.RuntimeMiddleRangeEndM).As<double>();
		set => Set(GDExtensionPropertyName.RuntimeMiddleRangeEndM, value);
	}

	/// <summary>Automatically keeps detailed GPU residency at least as large as the near tier.</summary>
	public new bool RuntimeSyncDetailLods
	{
		get => Get(GDExtensionPropertyName.RuntimeSyncDetailLods).As<bool>();
		set => Set(GDExtensionPropertyName.RuntimeSyncDetailLods, value);
	}

	/// <summary>Number of clipmap levels using the vertex-resolved macro-surface material.</summary>
	public new long RuntimeMiddleShaderLods
	{
		get => Get(GDExtensionPropertyName.RuntimeMiddleShaderLods).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeMiddleShaderLods, value);
	}

	/// <summary>Maximum deferred middle/far clipmap transforms submitted during one snap tick.</summary>
	public new long RuntimeClipmapUpdateBudget
	{
		get => Get(GDExtensionPropertyName.RuntimeClipmapUpdateBudget).As<long>();
		set => Set(GDExtensionPropertyName.RuntimeClipmapUpdateBudget, value);
	}

	/// <summary>Maximum worker tasks used to decode cooked terrain pages before serialized GPU upload.</summary>
	public new long RuntimePagePrepareThreads
	{
		get => Get(GDExtensionPropertyName.RuntimePagePrepareThreads).As<long>();
		set => Set(GDExtensionPropertyName.RuntimePagePrepareThreads, value);
	}

	/// <summary>Uses a bounded logical-to-physical cache for terrain region textures.</summary>
	public new bool GpuRegionCacheEnabled
	{
		get => Get(GDExtensionPropertyName.GpuRegionCacheEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.GpuRegionCacheEnabled, value);
	}

	/// <summary>Maximum MiB reserved for physical terrain region-map pages.</summary>
	public new long GpuRegionCacheBudgetMb
	{
		get => Get(GDExtensionPropertyName.GpuRegionCacheBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionCacheBudgetMb, value);
	}

	/// <summary>Maximum physical terrain pages published during one residency update.</summary>
	public new long GpuRegionUploadBudgetLayers
	{
		get => Get(GDExtensionPropertyName.GpuRegionUploadBudgetLayers).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionUploadBudgetLayers, value);
	}

	/// <summary>Maximum MiB uploaded by terrain region publication during one residency update.</summary>
	public new long GpuRegionUploadBudgetMb
	{
		get => Get(GDExtensionPropertyName.GpuRegionUploadBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionUploadBudgetMb, value);
	}

	/// <summary>Physical pages kept outside the logical page table for transactional uploads and delayed retirement.</summary>
	public new long GpuRegionSparePages
	{
		get => Get(GDExtensionPropertyName.GpuRegionSparePages).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionSparePages, value);
	}

	/// <summary>Rendered frames an unmapped physical page remains unavailable before reuse.</summary>
	public new long GpuRegionRetireFrames
	{
		get => Get(GDExtensionPropertyName.GpuRegionRetireFrames).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionRetireFrames, value);
	}

	/// <summary>Number of inner clipmap LODs that require detailed physical region pages; zero uses all LODs.</summary>
	public new long GpuRegionDetailLods
	{
		get => Get(GDExtensionPropertyName.GpuRegionDetailLods).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionDetailLods, value);
	}

	/// <summary>Requested detailed GPU-residency endpoint in metres.</summary>
	public new double GpuRegionDetailRangeEndM
	{
		get => Get(GDExtensionPropertyName.GpuRegionDetailRangeEndM).As<double>();
		set => Set(GDExtensionPropertyName.GpuRegionDetailRangeEndM, value);
	}

	/// <summary>Maximum detailed clipmap LOD count supported by the current physical page layout.</summary>
	public new long GpuRegionMaxSupportedDetailLods =>
		Get(GDExtensionPropertyName.GpuRegionMaxSupportedDetailLods).As<long>();

	/// <summary>Maximum detailed-residency endpoint supported by the current physical page layout.</summary>
	public new double GpuRegionMaxSupportedDetailRangeM =>
		Get(GDExtensionPropertyName.GpuRegionMaxSupportedDetailRangeM).As<double>();

	/// <summary>Core physical pages required by the requested detailed footprint.</summary>
	public new long GpuRegionRequiredCorePages =>
		Get(GDExtensionPropertyName.GpuRegionRequiredCorePages).As<long>();

	/// <summary>Mapped physical page capacity under the current cache budget and channel layout.</summary>
	public new long GpuRegionMappedCapacity =>
		Get(GDExtensionPropertyName.GpuRegionMappedCapacity).As<long>();

	/// <summary>Whether the requested detailed footprint fits the current mapped capacity.</summary>
	public new bool GpuRegionDetailRangeValid =>
		Get(GDExtensionPropertyName.GpuRegionDetailRangeValid).As<bool>();

	/// <summary>Additional region rings retained around the sampled detailed clipmap footprint.</summary>
	public new long GpuRegionGuardRegions
	{
		get => Get(GDExtensionPropertyName.GpuRegionGuardRegions).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionGuardRegions, value);
	}

	/// <summary>Additional rows prefetched in the dominant movement direction.</summary>
	public new long GpuRegionPrefetchRows
	{
		get => Get(GDExtensionPropertyName.GpuRegionPrefetchRows).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionPrefetchRows, value);
	}

	/// <summary>Residency updates that a recently exited page may remain available before eviction.</summary>
	public new long GpuRegionHysteresisTicks
	{
		get => Get(GDExtensionPropertyName.GpuRegionHysteresisTicks).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionHysteresisTicks, value);
	}

	/// <summary>Spatial resolution divisor applied to physical color pages.</summary>
	public new long GpuRegionColorResolutionDivisor
	{
		get => Get(GDExtensionPropertyName.GpuRegionColorResolutionDivisor).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionColorResolutionDivisor, value);
	}

	/// <summary>Spatial resolution divisor applied to physical derived-normal pages.</summary>
	public new long GpuRegionDerivedResolutionDivisor
	{
		get => Get(GDExtensionPropertyName.GpuRegionDerivedResolutionDivisor).As<long>();
		set => Set(GDExtensionPropertyName.GpuRegionDerivedResolutionDivisor, value);
	}
	/// <summary>
	/// Uses immutable cooked region pages when their version and source timestamp are valid.
	/// </summary>
	public new bool GpuRegionCookedPagesEnabled
	{
		get => Get(GDExtensionPropertyName.GpuRegionCookedPagesEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.GpuRegionCookedPagesEnabled, value);
	}
	/// <summary>
	/// Rebuilds the immutable GPU region page after a region is saved successfully.
	/// </summary>
	public new bool GpuRegionCookOnSave
	{
		get => Get(GDExtensionPropertyName.GpuRegionCookOnSave).As<bool>();
		set => Set(GDExtensionPropertyName.GpuRegionCookOnSave, value);
	}

	public new bool CpuRegionCacheEnabled
	{
		get => Get(GDExtensionPropertyName.CpuRegionCacheEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.CpuRegionCacheEnabled, value);
	}

	public new long CpuRegionCacheBudgetMb
	{
		get => Get(GDExtensionPropertyName.CpuRegionCacheBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.CpuRegionCacheBudgetMb, value);
	}

	public new long CpuRegionLoadBudgetLayers
	{
		get => Get(GDExtensionPropertyName.CpuRegionLoadBudgetLayers).As<long>();
		set => Set(GDExtensionPropertyName.CpuRegionLoadBudgetLayers, value);
	}

	public new long CpuRegionLoadBudgetMb
	{
		get => Get(GDExtensionPropertyName.CpuRegionLoadBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.CpuRegionLoadBudgetMb, value);
	}

	public new bool InstanceRegionCacheEnabled
	{
		get => Get(GDExtensionPropertyName.InstanceRegionCacheEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.InstanceRegionCacheEnabled, value);
	}

	public new long InstanceRegionCacheBudgetMb
	{
		get => Get(GDExtensionPropertyName.InstanceRegionCacheBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.InstanceRegionCacheBudgetMb, value);
	}

	public new long InstanceRegionLoadBudgetPages
	{
		get => Get(GDExtensionPropertyName.InstanceRegionLoadBudgetPages).As<long>();
		set => Set(GDExtensionPropertyName.InstanceRegionLoadBudgetPages, value);
	}

	public new long InstanceRegionLoadBudgetMb
	{
		get => Get(GDExtensionPropertyName.InstanceRegionLoadBudgetMb).As<long>();
		set => Set(GDExtensionPropertyName.InstanceRegionLoadBudgetMb, value);
	}

	public new bool DynamicNodeStreamingEnabled
	{
		get => Get(GDExtensionPropertyName.DynamicNodeStreamingEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.DynamicNodeStreamingEnabled, value);
	}

	public new long DynamicNodeStreamRadius
	{
		get => Get(GDExtensionPropertyName.DynamicNodeStreamRadius).As<long>();
		set => Set(GDExtensionPropertyName.DynamicNodeStreamRadius, value);
	}

	public new long DynamicNodeLoadRegionBudget
	{
		get => Get(GDExtensionPropertyName.DynamicNodeLoadRegionBudget).As<long>();
		set => Set(GDExtensionPropertyName.DynamicNodeLoadRegionBudget, value);
	}

	public new long DynamicNodeLoadNodeBudget
	{
		get => Get(GDExtensionPropertyName.DynamicNodeLoadNodeBudget).As<long>();
		set => Set(GDExtensionPropertyName.DynamicNodeLoadNodeBudget, value);
	}

	public new long DynamicNodeUnloadRegionBudget
	{
		get => Get(GDExtensionPropertyName.DynamicNodeUnloadRegionBudget).As<long>();
		set => Set(GDExtensionPropertyName.DynamicNodeUnloadRegionBudget, value);
	}

	public new bool LightmapStreamingEnabled
	{
		get => Get(GDExtensionPropertyName.LightmapStreamingEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.LightmapStreamingEnabled, value);
	}

	public new long LightmapStreamRadius
	{
		get => Get(GDExtensionPropertyName.LightmapStreamRadius).As<long>();
		set => Set(GDExtensionPropertyName.LightmapStreamRadius, value);
	}

	public new long LightmapLoadBudget
	{
		get => Get(GDExtensionPropertyName.LightmapLoadBudget).As<long>();
		set => Set(GDExtensionPropertyName.LightmapLoadBudget, value);
	}

	public new long LightmapUnloadBudget
	{
		get => Get(GDExtensionPropertyName.LightmapUnloadBudget).As<long>();
		set => Set(GDExtensionPropertyName.LightmapUnloadBudget, value);
	}

	public new bool SemanticShadowPolicyEnabled
	{
		get => Get(GDExtensionPropertyName.SemanticShadowPolicyEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.SemanticShadowPolicyEnabled, value);
	}

	public new EnvironmentShadowQualityEnum EnvironmentShadowQuality
	{
		get => Get(GDExtensionPropertyName.EnvironmentShadowQuality).As<EnvironmentShadowQualityEnum>();
		set => Set(GDExtensionPropertyName.EnvironmentShadowQuality, Variant.From(value));
	}

	public new long EnvironmentShadowExternalMask
	{
		get => Get(GDExtensionPropertyName.EnvironmentShadowExternalMask).As<long>();
		set => Set(GDExtensionPropertyName.EnvironmentShadowExternalMask, value);
	}

	/// <summary>When enabled, streamed environment geometry receives the managed directional light only when its descriptor opts in through receive_dynamic_directional, plus layers in EnvironmentDirectionalExternalMask.</summary>
	public new bool SemanticDirectionalReceptionEnabled
	{
		get => Get(GDExtensionPropertyName.SemanticDirectionalReceptionEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.SemanticDirectionalReceptionEnabled, value);
	}

	/// <summary>Non-Terrain3D render layers that remain in the managed DirectionalLight3D cull mask while semantic directional reception is enabled.</summary>
	public new long EnvironmentDirectionalExternalMask
	{
		get => Get(GDExtensionPropertyName.EnvironmentDirectionalExternalMask).As<long>();
		set => Set(GDExtensionPropertyName.EnvironmentDirectionalExternalMask, value);
	}

	/// <summary>Width and depth, in terrain samples, of one static-instance culling cell.</summary>
	public new long InstancerCellSize
	{
		get => Get(GDExtensionPropertyName.InstancerCellSize).As<long>();
		set => Set(GDExtensionPropertyName.InstancerCellSize, value);
	}

	/// <summary>Enables distance-based static-instance cell publication.</summary>
	public new bool InstancerCellStreamingEnabled
	{
		get => Get(GDExtensionPropertyName.InstancerCellStreamingEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.InstancerCellStreamingEnabled, value);
	}

	/// <summary>Keeps one active LOD MultiMesh buffer per visible cell.</summary>
	public new bool InstancerSingleLodBuffers
	{
		get => Get(GDExtensionPropertyName.InstancerSingleLodBuffers).As<bool>();
		set => Set(GDExtensionPropertyName.InstancerSingleLodBuffers, value);
	}

	/// <summary>Additional world-space distance retained around visible instance cells.</summary>
	public new double InstancerCellStreamingMargin
	{
		get => Get(GDExtensionPropertyName.InstancerCellStreamingMargin).As<double>();
		set => Set(GDExtensionPropertyName.InstancerCellStreamingMargin, value);
	}

	/// <summary>Minimum stream-target movement before static-instance selection is recalculated.</summary>
	public new double InstancerCellStreamingUpdateDistance
	{
		get => Get(GDExtensionPropertyName.InstancerCellStreamingUpdateDistance).As<double>();
		set => Set(GDExtensionPropertyName.InstancerCellStreamingUpdateDistance, value);
	}

	/// <summary>Maximum static-instance cells published during one pre-draw update.</summary>
	public new long InstancerUpdateCellBudget
	{
		get => Get(GDExtensionPropertyName.InstancerUpdateCellBudget).As<long>();
		set => Set(GDExtensionPropertyName.InstancerUpdateCellBudget, value);
	}

	public new Terrain3DMaterial Material
	{
		get => Terrain3DMaterial.Bind(Get(GDExtensionPropertyName.Material).As<Resource>());
		set => Set(GDExtensionPropertyName.Material, value);
	}

	public new Terrain3DAssets Assets
	{
		get => Terrain3DAssets.Bind(Get(GDExtensionPropertyName.Assets).As<Resource>());
		set => Set(GDExtensionPropertyName.Assets, value);
	}

	public new Terrain3DData Data
	{
		get => Terrain3DData.Bind(Get(GDExtensionPropertyName.Data).As<GodotObject>());
	}

	public new Terrain3DCollision Collision
	{
		get => Terrain3DCollision.Bind(Get(GDExtensionPropertyName.Collision).As<GodotObject>());
	}

	public new Terrain3DInstancer Instancer
	{
		get => Terrain3DInstancer.Bind(Get(GDExtensionPropertyName.Instancer).As<GodotObject>());
	}

	public new Node3D LightTarget
	{
		get => Get(GDExtensionPropertyName.LightTarget).As<Node3D>();
		set => Set(GDExtensionPropertyName.LightTarget, value);
	}

	public new Terrain3D.RegionSizeEnum RegionSize
	{
		get => Get(GDExtensionPropertyName.RegionSize).As<Terrain3D.RegionSizeEnum>();
		set => Set(GDExtensionPropertyName.RegionSize, Variant.From(value));
	}

	public new bool Save16Bit
	{
		get => Get(GDExtensionPropertyName.Save16Bit).As<bool>();
		set => Set(GDExtensionPropertyName.Save16Bit, value);
	}

	public new double LabelDistance
	{
		get => Get(GDExtensionPropertyName.LabelDistance).As<double>();
		set => Set(GDExtensionPropertyName.LabelDistance, value);
	}

	public new long LabelSize
	{
		get => Get(GDExtensionPropertyName.LabelSize).As<long>();
		set => Set(GDExtensionPropertyName.LabelSize, value);
	}

	public new bool ShowGrid
	{
		get => Get(GDExtensionPropertyName.ShowGrid).As<bool>();
		set => Set(GDExtensionPropertyName.ShowGrid, value);
	}

	public new Terrain3DCollision.CollisionMode CollisionMode
	{
		get => Get(GDExtensionPropertyName.CollisionMode).As<Terrain3DCollision.CollisionMode>();
		set => Set(GDExtensionPropertyName.CollisionMode, Variant.From(value));
	}

	public new long CollisionShapeSize
	{
		get => Get(GDExtensionPropertyName.CollisionShapeSize).As<long>();
		set => Set(GDExtensionPropertyName.CollisionShapeSize, value);
	}

	public new long CollisionRadius
	{
		get => Get(GDExtensionPropertyName.CollisionRadius).As<long>();
		set => Set(GDExtensionPropertyName.CollisionRadius, value);
	}

	public new Node3D CollisionTarget
	{
		get => Get(GDExtensionPropertyName.CollisionTarget).As<Node3D>();
		set => Set(GDExtensionPropertyName.CollisionTarget, value);
	}

	public new long CollisionLayer
	{
		get => Get(GDExtensionPropertyName.CollisionLayer).As<long>();
		set => Set(GDExtensionPropertyName.CollisionLayer, value);
	}

	public new long CollisionMask
	{
		get => Get(GDExtensionPropertyName.CollisionMask).As<long>();
		set => Set(GDExtensionPropertyName.CollisionMask, value);
	}

	public new double CollisionPriority
	{
		get => Get(GDExtensionPropertyName.CollisionPriority).As<double>();
		set => Set(GDExtensionPropertyName.CollisionPriority, value);
	}

	public new PhysicsMaterial PhysicsMaterial
	{
		get => Get(GDExtensionPropertyName.PhysicsMaterial).As<PhysicsMaterial>();
		set => Set(GDExtensionPropertyName.PhysicsMaterial, value);
	}

	public new Node3D ClipmapTarget
	{
		get => Get(GDExtensionPropertyName.ClipmapTarget).As<Node3D>();
		set => Set(GDExtensionPropertyName.ClipmapTarget, value);
	}

	public new long MeshLods
	{
		get => Get(GDExtensionPropertyName.MeshLods).As<long>();
		set => Set(GDExtensionPropertyName.MeshLods, value);
	}

	public new long TessellationLevel
	{
		get => Get(GDExtensionPropertyName.TessellationLevel).As<long>();
		set => Set(GDExtensionPropertyName.TessellationLevel, value);
	}

	public new bool ComputeDisplacementEnabled
	{
		get => Get(GDExtensionPropertyName.ComputeDisplacementEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.ComputeDisplacementEnabled, value);
	}

	public new long MeshSize
	{
		get => Get(GDExtensionPropertyName.MeshSize).As<long>();
		set => Set(GDExtensionPropertyName.MeshSize, value);
	}

	public new double VertexSpacing
	{
		get => Get(GDExtensionPropertyName.VertexSpacing).As<double>();
		set => Set(GDExtensionPropertyName.VertexSpacing, value);
	}

	public new double CullMargin
	{
		get => Get(GDExtensionPropertyName.CullMargin).As<double>();
		set => Set(GDExtensionPropertyName.CullMargin, value);
	}

	public new RenderingServer.ShadowCastingSetting CastShadows
	{
		get => Get(GDExtensionPropertyName.CastShadows).As<RenderingServer.ShadowCastingSetting>();
		set => Set(GDExtensionPropertyName.CastShadows, Variant.From(value));
	}

	public new long ShadowLodCount
	{
		get => Get(GDExtensionPropertyName.ShadowLodCount).As<long>();
		set => Set(GDExtensionPropertyName.ShadowLodCount, value);
	}

	public new bool NearShadowEnabled
	{
		get => Get(GDExtensionPropertyName.NearShadowEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.NearShadowEnabled, value);
	}

	public new bool MiddleShadowEnabled
	{
		get => Get(GDExtensionPropertyName.MiddleShadowEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.MiddleShadowEnabled, value);
	}

	public new bool FarShadowEnabled
	{
		get => Get(GDExtensionPropertyName.FarShadowEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.FarShadowEnabled, value);
	}

	public new GeometryInstance3D.GIModeEnum GiMode
	{
		get => Get(GDExtensionPropertyName.GiMode).As<GeometryInstance3D.GIModeEnum>();
		set => Set(GDExtensionPropertyName.GiMode, Variant.From(value));
	}

	public new long RenderLayers
	{
		get => Get(GDExtensionPropertyName.RenderLayers).As<long>();
		set => Set(GDExtensionPropertyName.RenderLayers, value);
	}

	public new double DisplacementScale
	{
		get => Get(GDExtensionPropertyName.DisplacementScale).As<double>();
		set => Set(GDExtensionPropertyName.DisplacementScale, value);
	}

	public new double DisplacementSharpness
	{
		get => Get(GDExtensionPropertyName.DisplacementSharpness).As<double>();
		set => Set(GDExtensionPropertyName.DisplacementSharpness, value);
	}

	public new bool BufferShaderOverrideEnabled
	{
		get => Get(GDExtensionPropertyName.BufferShaderOverrideEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.BufferShaderOverrideEnabled, value);
	}

	public new Shader BufferShaderOverride
	{
		get => Get(GDExtensionPropertyName.BufferShaderOverride).As<Shader>();
		set => Set(GDExtensionPropertyName.BufferShaderOverride, value);
	}

	public new bool OceanEnabled
	{
		get => Get(GDExtensionPropertyName.OceanEnabled).As<bool>();
		set => Set(GDExtensionPropertyName.OceanEnabled, value);
	}

	public new long OceanMeshLods
	{
		get => Get(GDExtensionPropertyName.OceanMeshLods).As<long>();
		set => Set(GDExtensionPropertyName.OceanMeshLods, value);
	}

	public new long OceanTessellationLevel
	{
		get => Get(GDExtensionPropertyName.OceanTessellationLevel).As<long>();
		set => Set(GDExtensionPropertyName.OceanTessellationLevel, value);
	}

	public new long OceanMeshSize
	{
		get => Get(GDExtensionPropertyName.OceanMeshSize).As<long>();
		set => Set(GDExtensionPropertyName.OceanMeshSize, value);
	}

	public new double OceanVertexSpacing
	{
		get => Get(GDExtensionPropertyName.OceanVertexSpacing).As<double>();
		set => Set(GDExtensionPropertyName.OceanVertexSpacing, value);
	}

	public new double OceanCullMargin
	{
		get => Get(GDExtensionPropertyName.OceanCullMargin).As<double>();
		set => Set(GDExtensionPropertyName.OceanCullMargin, value);
	}

	public new RenderingServer.ShadowCastingSetting OceanCastShadows
	{
		get => Get(GDExtensionPropertyName.OceanCastShadows).As<RenderingServer.ShadowCastingSetting>();
		set => Set(GDExtensionPropertyName.OceanCastShadows, Variant.From(value));
	}

	public new GeometryInstance3D.GIModeEnum OceanGiMode
	{
		get => Get(GDExtensionPropertyName.OceanGiMode).As<GeometryInstance3D.GIModeEnum>();
		set => Set(GDExtensionPropertyName.OceanGiMode, Variant.From(value));
	}

	public new long OceanRenderLayers
	{
		get => Get(GDExtensionPropertyName.OceanRenderLayers).As<long>();
		set => Set(GDExtensionPropertyName.OceanRenderLayers, value);
	}

	public new Material OceanMaterial
	{
		get => Get(GDExtensionPropertyName.OceanMaterial).As<Material>();
		set => Set(GDExtensionPropertyName.OceanMaterial, value);
	}

	public new long MouseLayer
	{
		get => Get(GDExtensionPropertyName.MouseLayer).As<long>();
		set => Set(GDExtensionPropertyName.MouseLayer, value);
	}

	public new bool FreeEditorTextures
	{
		get => Get(GDExtensionPropertyName.FreeEditorTextures).As<bool>();
		set => Set(GDExtensionPropertyName.FreeEditorTextures, value);
	}

	public new Terrain3DInstancer.InstancerMode InstancerMode
	{
		get => Get(GDExtensionPropertyName.InstancerMode).As<Terrain3DInstancer.InstancerMode>();
		set => Set(GDExtensionPropertyName.InstancerMode, Variant.From(value));
	}

	public new bool ShowRegionGrid
	{
		get => Get(GDExtensionPropertyName.ShowRegionGrid).As<bool>();
		set => Set(GDExtensionPropertyName.ShowRegionGrid, value);
	}

	public new bool ShowInstancerGrid
	{
		get => Get(GDExtensionPropertyName.ShowInstancerGrid).As<bool>();
		set => Set(GDExtensionPropertyName.ShowInstancerGrid, value);
	}

	public new bool ShowVertexGrid
	{
		get => Get(GDExtensionPropertyName.ShowVertexGrid).As<bool>();
		set => Set(GDExtensionPropertyName.ShowVertexGrid, value);
	}

	public new bool ShowContours
	{
		get => Get(GDExtensionPropertyName.ShowContours).As<bool>();
		set => Set(GDExtensionPropertyName.ShowContours, value);
	}

	public new bool ShowNavigation
	{
		get => Get(GDExtensionPropertyName.ShowNavigation).As<bool>();
		set => Set(GDExtensionPropertyName.ShowNavigation, value);
	}

	public new bool ShowCheckered
	{
		get => Get(GDExtensionPropertyName.ShowCheckered).As<bool>();
		set => Set(GDExtensionPropertyName.ShowCheckered, value);
	}

	public new bool ShowGrey
	{
		get => Get(GDExtensionPropertyName.ShowGrey).As<bool>();
		set => Set(GDExtensionPropertyName.ShowGrey, value);
	}

	public new bool ShowHeightmap
	{
		get => Get(GDExtensionPropertyName.ShowHeightmap).As<bool>();
		set => Set(GDExtensionPropertyName.ShowHeightmap, value);
	}

	public new bool ShowJaggedness
	{
		get => Get(GDExtensionPropertyName.ShowJaggedness).As<bool>();
		set => Set(GDExtensionPropertyName.ShowJaggedness, value);
	}

	public new bool ShowAutoshader
	{
		get => Get(GDExtensionPropertyName.ShowAutoshader).As<bool>();
		set => Set(GDExtensionPropertyName.ShowAutoshader, value);
	}

	public new bool ShowControlTexture
	{
		get => Get(GDExtensionPropertyName.ShowControlTexture).As<bool>();
		set => Set(GDExtensionPropertyName.ShowControlTexture, value);
	}

	public new bool ShowControlBlend
	{
		get => Get(GDExtensionPropertyName.ShowControlBlend).As<bool>();
		set => Set(GDExtensionPropertyName.ShowControlBlend, value);
	}

	public new bool ShowControlAngle
	{
		get => Get(GDExtensionPropertyName.ShowControlAngle).As<bool>();
		set => Set(GDExtensionPropertyName.ShowControlAngle, value);
	}

	public new bool ShowControlScale
	{
		get => Get(GDExtensionPropertyName.ShowControlScale).As<bool>();
		set => Set(GDExtensionPropertyName.ShowControlScale, value);
	}

	public new bool ShowColormap
	{
		get => Get(GDExtensionPropertyName.ShowColormap).As<bool>();
		set => Set(GDExtensionPropertyName.ShowColormap, value);
	}

	public new bool ShowRoughmap
	{
		get => Get(GDExtensionPropertyName.ShowRoughmap).As<bool>();
		set => Set(GDExtensionPropertyName.ShowRoughmap, value);
	}

	public new bool ShowDisplacementBuffer
	{
		get => Get(GDExtensionPropertyName.ShowDisplacementBuffer).As<bool>();
		set => Set(GDExtensionPropertyName.ShowDisplacementBuffer, value);
	}

	public new bool ShowTextureAlbedo
	{
		get => Get(GDExtensionPropertyName.ShowTextureAlbedo).As<bool>();
		set => Set(GDExtensionPropertyName.ShowTextureAlbedo, value);
	}

	public new bool ShowTextureHeight
	{
		get => Get(GDExtensionPropertyName.ShowTextureHeight).As<bool>();
		set => Set(GDExtensionPropertyName.ShowTextureHeight, value);
	}

	public new bool ShowTextureNormal
	{
		get => Get(GDExtensionPropertyName.ShowTextureNormal).As<bool>();
		set => Set(GDExtensionPropertyName.ShowTextureNormal, value);
	}

	public new bool ShowTextureRough
	{
		get => Get(GDExtensionPropertyName.ShowTextureRough).As<bool>();
		set => Set(GDExtensionPropertyName.ShowTextureRough, value);
	}

	public new bool ShowTextureAo
	{
		get => Get(GDExtensionPropertyName.ShowTextureAo).As<bool>();
		set => Set(GDExtensionPropertyName.ShowTextureAo, value);
	}

	public new class GDExtensionMethodName : Node3D.MethodName
	{
		/// <summary>Cached name for setting the editor execution policy.</summary>
		public new static readonly StringName SetEditorExecutionMode = "set_editor_execution_mode";
		/// <summary>Cached name for reading the editor execution policy.</summary>
		public new static readonly StringName GetEditorExecutionMode = "get_editor_execution_mode";
		/// <summary>Cached name for retrieving the live nested diagnostics snapshot.</summary>
		public new static readonly StringName GetRuntimeDiagnostics = "get_runtime_diagnostics";
		/// <summary>Cached name for retrieving the current Terrain3D-owned memory estimate.</summary>
		public new static readonly StringName GetMemoryEstimate = "get_memory_estimate";
		/// <summary>Cached name for regional baked-lighting residency diagnostics.</summary>
		public new static readonly StringName GetLightmapStreamingStats = "get_lightmap_streaming_stats";
		/// <summary>Cached name for generating temporary regional Terrain3D bake geometry with UV2.</summary>
		public new static readonly StringName BakeRegionLightmapMesh = "bake_region_lightmap_mesh";
		/// <summary>Cached name for retrieving adaptive work-budget state.</summary>
		public new static readonly StringName GetRuntimeAdaptiveBudgetStats = "get_runtime_adaptive_budget_stats";
		/// <summary>Cached name for retrieving boundary work-phasing state.</summary>
		public new static readonly StringName GetRuntimeBoundaryPhasingStats = "get_runtime_boundary_phasing_stats";
		/// <summary>Cached name for retrieving recent fixed-buffer runtime samples.</summary>
		public new static readonly StringName GetRuntimeProfileHistory = "get_runtime_profile_history";
		/// <summary>Cached name for retrieving only samples newer than a frame sequence.</summary>
		public new static readonly StringName GetRuntimeProfileHistorySince = "get_runtime_profile_history_since";
		/// <summary>Cached name for resetting live diagnostic counters.</summary>
		public new static readonly StringName ResetRuntimeDiagnostics = "reset_runtime_diagnostics";
		/// <summary>Cached name for querying effective GPU-region paging.</summary>
		public new static readonly StringName IsGpuRegionCacheEffective = "is_gpu_region_cache_effective";
		/// <summary>Cached name for querying effective CPU-region eviction.</summary>
		public new static readonly StringName IsCpuRegionCacheEffective = "is_cpu_region_cache_effective";
		/// <summary>Cached name for querying effective instance-region eviction.</summary>
		public new static readonly StringName IsInstanceRegionCacheEffective = "is_instance_region_cache_effective";
		/// <summary>Cached name for querying effective instancer-cell streaming.</summary>
		public new static readonly StringName IsInstancerCellStreamingEffective = "is_instancer_cell_streaming_effective";
		/// <summary>Cached name for querying effective dynamic-node streaming.</summary>
		public new static readonly StringName IsDynamicNodeStreamingEffective = "is_dynamic_node_streaming_effective";

		/// <summary>
		/// Cached name for the 'set_editor' member.
		/// </summary>
		public new static readonly StringName SetEditor = "set_editor";
		/// <summary>
		/// Cached name for the 'get_editor' member.
		/// </summary>
		public new static readonly StringName GetEditor = "get_editor";
		/// <summary>
		/// Cached name for the 'set_plugin' member.
		/// </summary>
		public new static readonly StringName SetPlugin = "set_plugin";
		/// <summary>
		/// Cached name for the 'get_plugin' member.
		/// </summary>
		public new static readonly StringName GetPlugin = "get_plugin";
		/// <summary>
		/// Cached name for the 'set_camera' member.
		/// </summary>
		public new static readonly StringName SetCamera = "set_camera";
		/// <summary>
		/// Cached name for the 'get_camera' member.
		/// </summary>
		public new static readonly StringName GetCamera = "get_camera";
		/// <summary>
		/// Cached name for the 'get_clipmap_target_position' member.
		/// </summary>
		public new static readonly StringName GetClipmapTargetPosition = "get_clipmap_target_position";
		/// <summary>
		/// Cached name for the 'get_collision_target_position' member.
		/// </summary>
		public new static readonly StringName GetCollisionTargetPosition = "get_collision_target_position";
		/// <summary>
		/// Cached name for the 'snap' member.
		/// </summary>
		public new static readonly StringName Snap = "snap";
		/// <summary>
		/// Cached name for the 'get_intersection' member.
		/// </summary>
		public new static readonly StringName GetIntersection = "get_intersection";
		/// <summary>
		/// Cached name for the 'get_raycast_result' member.
		/// </summary>
		public new static readonly StringName GetRaycastResult = "get_raycast_result";
		/// <summary>
		/// Cached name for the 'bake_mesh' member.
		/// </summary>
		public new static readonly StringName BakeMesh = "bake_mesh";
		/// <summary>
		/// Cached name for the 'bake_occluder' member.
		/// </summary>
		public new static readonly StringName BakeOccluder = "bake_occluder";
		/// <summary>
		/// Cached name for the 'generate_nav_mesh_source_geometry' member.
		/// </summary>
		public new static readonly StringName GenerateNavMeshSourceGeometry = "generate_nav_mesh_source_geometry";
	}

	public new void SetEditorExecutionMode(EditorExecutionModeEnum mode) =>
		Call(GDExtensionMethodName.SetEditorExecutionMode, [Variant.From(mode)]);

	public new EditorExecutionModeEnum GetEditorExecutionMode() =>
		Call(GDExtensionMethodName.GetEditorExecutionMode, []).As<EditorExecutionModeEnum>();

	public new Godot.Collections.Dictionary GetRuntimeDiagnostics() =>
		Call(GDExtensionMethodName.GetRuntimeDiagnostics, []).As<Godot.Collections.Dictionary>();

	public new Godot.Collections.Dictionary GetMemoryEstimate() =>
		Call(GDExtensionMethodName.GetMemoryEstimate, []).As<Godot.Collections.Dictionary>();

	public new Godot.Collections.Dictionary GetLightmapStreamingStats() =>
		Call(GDExtensionMethodName.GetLightmapStreamingStats, []).As<Godot.Collections.Dictionary>();

	public new Mesh BakeRegionLightmapMesh(Vector2I regionLocation, long lod = 2,
		long contextRadiusRegions = 1, double texelSize = 1.0) =>
		Call(GDExtensionMethodName.BakeRegionLightmapMesh,
			[regionLocation, lod, contextRadiusRegions, texelSize]).As<Mesh>();


	public new Godot.Collections.Dictionary GetRuntimeAdaptiveBudgetStats() =>
		Call(GDExtensionMethodName.GetRuntimeAdaptiveBudgetStats, []).As<Godot.Collections.Dictionary>();

	public new Godot.Collections.Dictionary GetRuntimeBoundaryPhasingStats() =>
		Call(GDExtensionMethodName.GetRuntimeBoundaryPhasingStats, []).As<Godot.Collections.Dictionary>();

	public new Godot.Collections.Array GetRuntimeProfileHistory(long maxSamples = 600) =>
		Call(GDExtensionMethodName.GetRuntimeProfileHistory, [maxSamples]).As<Godot.Collections.Array>();

	public new Godot.Collections.Array GetRuntimeProfileHistorySince(long afterSequence, long maxSamples = 600) =>
		Call(GDExtensionMethodName.GetRuntimeProfileHistorySince, [afterSequence, maxSamples]).As<Godot.Collections.Array>();

	public new void ResetRuntimeDiagnostics() =>
		Call(GDExtensionMethodName.ResetRuntimeDiagnostics, []);

	public new bool IsGpuRegionCacheEffective() =>
		Call(GDExtensionMethodName.IsGpuRegionCacheEffective, []).As<bool>();

	public new bool IsCpuRegionCacheEffective() =>
		Call(GDExtensionMethodName.IsCpuRegionCacheEffective, []).As<bool>();

	public new bool IsInstanceRegionCacheEffective() =>
		Call(GDExtensionMethodName.IsInstanceRegionCacheEffective, []).As<bool>();

	public new bool IsInstancerCellStreamingEffective() =>
		Call(GDExtensionMethodName.IsInstancerCellStreamingEffective, []).As<bool>();

	public new bool IsDynamicNodeStreamingEffective() =>
		Call(GDExtensionMethodName.IsDynamicNodeStreamingEffective, []).As<bool>();

	public new void SetEditor(Terrain3DEditor editor) => 
		Call(GDExtensionMethodName.SetEditor, [editor]);

	public new Terrain3DEditor GetEditor() => 
		Terrain3DEditor.Bind(Call(GDExtensionMethodName.GetEditor, []).As<GodotObject>());

	public new void SetPlugin(GodotObject plugin) => 
		Call(GDExtensionMethodName.SetPlugin, [plugin]);

	public new GodotObject GetPlugin() => 
		Call(GDExtensionMethodName.GetPlugin, []).As<GodotObject>();

	public new void SetCamera(Camera3D camera) => 
		Call(GDExtensionMethodName.SetCamera, [camera]);

	public new Camera3D GetCamera() => 
		Call(GDExtensionMethodName.GetCamera, []).As<Camera3D>();

	public new Vector3 GetClipmapTargetPosition() => 
		Call(GDExtensionMethodName.GetClipmapTargetPosition, []).As<Vector3>();

	public new Vector3 GetCollisionTargetPosition() => 
		Call(GDExtensionMethodName.GetCollisionTargetPosition, []).As<Vector3>();

	public new void Snap() => 
		Call(GDExtensionMethodName.Snap, []);

	public new Vector3 GetIntersection(Vector3 srcPos, Vector3 direction, bool gpuMode = false) => 
		Call(GDExtensionMethodName.GetIntersection, [srcPos, direction, gpuMode]).As<Vector3>();

	public new Godot.Collections.Dictionary GetRaycastResult(Vector3 srcPos, Vector3 direction, long collisionMask = 4294967295, bool excludeTerrain = false) => 
		Call(GDExtensionMethodName.GetRaycastResult, [srcPos, direction, collisionMask, excludeTerrain]).As<Godot.Collections.Dictionary>();

	public new Mesh BakeMesh(long lod, Terrain3DData.HeightFilter filter = Terrain3DData.HeightFilter.Nearest) => 
		Call(GDExtensionMethodName.BakeMesh, [lod, Variant.From(filter)]).As<Mesh>();

	public new ArrayOccluder3D BakeOccluder(long lod) =>
		Call(GDExtensionMethodName.BakeOccluder, [lod]).As<ArrayOccluder3D>();

	public new Vector3[] GenerateNavMeshSourceGeometry(Aabb globalAabb, bool requireNav = true) => 
		Call(GDExtensionMethodName.GenerateNavMeshSourceGeometry, [globalAabb, requireNav]).As<Vector3[]>();

}

file static class DebugLevelEnumExtensions
{
public static int SafeAsInt32(this Terrain3D.DebugLevelEnum enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.DebugLevelEnum enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.DebugLevelEnum? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}

file static class RegionSizeEnumExtensions
{
public static int SafeAsInt32(this Terrain3D.RegionSizeEnum enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.RegionSizeEnum enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.RegionSizeEnum? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}

file static class EditorExecutionModeEnumExtensions
{
public static int SafeAsInt32(this Terrain3D.EditorExecutionModeEnum enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.EditorExecutionModeEnum enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this Terrain3D.EditorExecutionModeEnum? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}

