#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;
using Godot.Collections;

namespace TokisanGames;

[Tool]
public partial class Terrain3DErosionJob : RefCounted
{
    private new static readonly StringName NativeName = new StringName("Terrain3DErosionJob");

    [Obsolete("Wrapper types cannot be constructed with constructors; use Instantiate() instead.")]
    protected Terrain3DErosionJob() { }

    private static CSharpScript _wrapperScriptAsset;

    public new static Terrain3DErosionJob Bind(GodotObject godotObject)
    {
        if (!IsInstanceValid(godotObject))
            return null;
        if (godotObject is Terrain3DErosionJob wrapperScriptInstance)
            return wrapperScriptInstance;
#if DEBUG
        var currentObjectClassName = godotObject.GetClass();
        if (!ClassDB.IsParentClass(nameof(Terrain3DErosionJob), currentObjectClassName))
            throw new InvalidOperationException($"The supplied GodotObject ({currentObjectClassName}) is not Terrain3DErosionJob.");
#endif
        if (_wrapperScriptAsset is null)
        {
            var scriptPathAttribute = typeof(Terrain3DErosionJob).GetCustomAttributes<ScriptPathAttribute>().FirstOrDefault();
            if (scriptPathAttribute is null) throw new UnreachableException();
            _wrapperScriptAsset = ResourceLoader.Load<CSharpScript>(scriptPathAttribute.Path);
        }
        var instanceId = godotObject.GetInstanceId();
        godotObject.SetScript(_wrapperScriptAsset);
        return (Terrain3DErosionJob)InstanceFromId(instanceId);
    }

    public new static Terrain3DErosionJob Instantiate() => Bind(ClassDB.Instantiate(NativeName).As<GodotObject>());

    public enum JobState
    {
        New = 0,
        Ready = 1,
        Running = 2,
        Completed = 3,
        Cancelled = 4,
        Failed = 5,
        Applied = 6,
    }

    public enum OutputMap
    {
        HeightDelta = 0,
        FlowAccumulation = 1,
        Sediment = 2,
        CoarseSediment = 3,
        SandSediment = 4,
        FineSediment = 5,
        LooseMaterial = 6,
        Oxidation = 7,
        Weathering = 8,
        HydraulicDeposition = 9,
        WindDeposition = 10,
        WaveDeposition = 11,
        Traffic = 12,
        AlluvialDeposition = 13,
        DebrisFlowDeposition = 14,
        DuneDeposition = 15,
        SaltWeathering = 16,
        WetDryCycles = 17,
        ExposureAge = 18,
        Snowpack = 19,
        DesertPavement = 20,
        Stratigraphy = 21,
        MaterialId = 22,
        Max = 23,
    }

    public new class GDExtensionMethodName : RefCounted.MethodName
    {
        public new static readonly StringName Run = "run";
        public new static readonly StringName Cancel = "cancel";
        public new static readonly StringName Apply = "apply";
        public new static readonly StringName GetState = "get_state";
        public new static readonly StringName GetProgress = "get_progress";
        public new static readonly StringName GetStageName = "get_stage_name";
        public new static readonly StringName GetErrorMessage = "get_error_message";
        public new static readonly StringName IsComplete = "is_complete";
        public new static readonly StringName IsSourceUnchanged = "is_source_unchanged";
        public new static readonly StringName WasCancelled = "was_cancelled";
        public new static readonly StringName GetEffectiveResolutionScale = "get_effective_resolution_scale";
        public new static readonly StringName GetCompletedSteps = "get_completed_steps";
        public new static readonly StringName GetStatistics = "get_statistics";
        public new static readonly StringName GetRegionLocations = "get_region_locations";
        public new static readonly StringName GetRegionOutputMap = "get_region_output_map";
    }

    public new void Run() => Call(GDExtensionMethodName.Run, []);
    public new void Cancel() => Call(GDExtensionMethodName.Cancel, []);
    public new Error Apply(Terrain3DData data, bool force = false) => Call(GDExtensionMethodName.Apply, [data, force]).As<Error>();
    public new JobState GetState() => (JobState)Call(GDExtensionMethodName.GetState, []).As<long>();
    public new double GetProgress() => Call(GDExtensionMethodName.GetProgress, []).As<double>();
    public new string GetStageName() => Call(GDExtensionMethodName.GetStageName, []).As<string>();
    public new string GetErrorMessage() => Call(GDExtensionMethodName.GetErrorMessage, []).As<string>();
    public new bool IsComplete() => Call(GDExtensionMethodName.IsComplete, []).As<bool>();
    public new bool IsSourceUnchanged(Terrain3DData data) => Call(GDExtensionMethodName.IsSourceUnchanged, [data]).As<bool>();
    public new bool WasCancelled() => Call(GDExtensionMethodName.WasCancelled, []).As<bool>();
    public new long GetEffectiveResolutionScale() => Call(GDExtensionMethodName.GetEffectiveResolutionScale, []).As<long>();
    public new long GetCompletedSteps() => Call(GDExtensionMethodName.GetCompletedSteps, []).As<long>();
    public new Dictionary GetStatistics() => Call(GDExtensionMethodName.GetStatistics, []).As<Dictionary>();
    public new Godot.Collections.Array GetRegionLocations() => Call(GDExtensionMethodName.GetRegionLocations, []).As<Godot.Collections.Array>();
    public new Image GetRegionOutputMap(Vector2I regionLocation, OutputMap output, bool fullResolution = false) =>
        Call(GDExtensionMethodName.GetRegionOutputMap, [regionLocation, Variant.From(output), fullResolution]).As<Image>();
}
