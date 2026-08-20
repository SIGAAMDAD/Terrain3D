#pragma warning disable CS0109
using System;
using System.Diagnostics;
using System.Linq;
using System.Reflection;
using Godot;
using Godot.Collections;

namespace TokisanGames;

[Tool]
public partial class T3DEditor : GodotObject
{

	private new static readonly StringName NativeName = new StringName("T3DEditor");

	[Obsolete("Wrapper types cannot be constructed with constructors (it only instantiate the underlying T3DEditor object), please use the Instantiate() method instead.")]
	protected T3DEditor() { }

	private static CSharpScript _wrapperScriptAsset;

	/// <summary>
	/// Try to cast the script on the supplied <paramref name="godotObject"/> to the <see cref="T3DEditor"/> wrapper type,
	/// if no script has attached to the type, or the script attached to the type does not inherit the <see cref="T3DEditor"/> wrapper type,
	/// a new instance of the <see cref="T3DEditor"/> wrapper script will get attaches to the <paramref name="godotObject"/>.
	/// </summary>
	/// <remarks>The developer should only supply the <paramref name="godotObject"/> that represents the correct underlying GDExtension type.</remarks>
	/// <param name="godotObject">The <paramref name="godotObject"/> that represents the correct underlying GDExtension type.</param>
	/// <returns>The existing or a new instance of the <see cref="T3DEditor"/> wrapper script attached to the supplied <paramref name="godotObject"/>.</returns>
	public new static T3DEditor Bind(GodotObject godotObject)
	{
		if (!IsInstanceValid(godotObject))
			return null;

		if (godotObject is T3DEditor wrapperScriptInstance)
			return wrapperScriptInstance;

#if DEBUG
		var expectedType = typeof(T3DEditor);
		var currentObjectClassName = godotObject.GetClass();
		if (!ClassDB.IsParentClass(expectedType.Name, currentObjectClassName))
			throw new InvalidOperationException($"The supplied GodotObject ({currentObjectClassName}) is not the {expectedType.Name} type.");
#endif

		if (_wrapperScriptAsset is null)
		{
			var scriptPathAttribute = typeof(T3DEditor).GetCustomAttributes<ScriptPathAttribute>().FirstOrDefault();
			if (scriptPathAttribute is null) throw new UnreachableException();
			_wrapperScriptAsset = ResourceLoader.Load<CSharpScript>(scriptPathAttribute.Path);
		}

		var instanceId = godotObject.GetInstanceId();
		godotObject.SetScript(_wrapperScriptAsset);
		return (T3DEditor)InstanceFromId(instanceId);
	}

	/// <summary>
	/// Creates an instance of the GDExtension <see cref="T3DEditor"/> type, and attaches a wrapper script instance to it.
	/// </summary>
	/// <returns>The wrapper instance linked to the underlying GDExtension "T3DEditor" type.</returns>
	public new static T3DEditor Instantiate() => Bind(ClassDB.Instantiate(NativeName).As<GodotObject>());

	public enum Operation
	{
		Add = 0,
		Subtract = 1,
		Replace = 2,
		Average = 3,
		Gradient = 4,
		OpMax = 5,
	}

	public enum Tool
	{
		Sculpt = 1,
		Height = 2,
		Texture = 3,
		Color = 4,
		Roughness = 5,
		Angle = 10,
		Scale = 11,
		Autoshader = 6,
		Holes = 7,
		Navigation = 8,
		Instancer = 9,
		Region = 0,
		Max = 12,
	}

	public new class GDExtensionSignalName : GodotObject.SignalName
	{
	}

	public new class GDExtensionPropertyName : GodotObject.PropertyName
	{
	}

	public new class GDExtensionMethodName : GodotObject.MethodName
	{
		/// <summary>
		/// Cached name for the 'set_terrain' member.
		/// </summary>
		public new static readonly StringName SetTerrain = "set_terrain";
		/// <summary>
		/// Cached name for the 'get_terrain' member.
		/// </summary>
		public new static readonly StringName GetTerrain = "get_terrain";
		/// <summary>
		/// Cached name for the 'set_brush_data' member.
		/// </summary>
		public new static readonly StringName SetBrushData = "set_brush_data";
		/// <summary>
		/// Cached name for the 'set_tool' member.
		/// </summary>
		public new static readonly StringName SetTool = "set_tool";
		/// <summary>
		/// Cached name for the 'get_tool' member.
		/// </summary>
		public new static readonly StringName GetTool = "get_tool";
		/// <summary>
		/// Cached name for the 'set_operation' member.
		/// </summary>
		public new static readonly StringName SetOperation = "set_operation";
		/// <summary>
		/// Cached name for the 'get_operation' member.
		/// </summary>
		public new static readonly StringName GetOperation = "get_operation";
		/// <summary>
		/// Cached name for the 'start_operation' member.
		/// </summary>
		public new static readonly StringName StartOperation = "start_operation";
		/// <summary>
		/// Cached name for the 'is_operating' member.
		/// </summary>
		public new static readonly StringName IsOperating = "is_operating";
		/// <summary>
		/// Cached name for the 'operate' member.
		/// </summary>
		public new static readonly StringName Operate = "operate";
		/// <summary>
		/// Cached name for the 'backup_region' member.
		/// </summary>
		public new static readonly StringName BackupRegion = "backup_region";
		/// <summary>
		/// Cached name for the 'stop_operation' member.
		/// </summary>
		public new static readonly StringName StopOperation = "stop_operation";
		/// <summary>
		/// Cached name for the 'apply_undo' member.
		/// </summary>
		public new static readonly StringName ApplyUndo = "apply_undo";
	}

	public new void SetTerrain(T3DTerrain terrain) =>
		Call(GDExtensionMethodName.SetTerrain, [terrain]);

	public new T3DTerrain GetTerrain() =>
		T3DTerrain.Bind(Call(GDExtensionMethodName.GetTerrain, []).As<Node3D>());

	public new void SetBrushData(Godot.Collections.Dictionary data) =>
		Call(GDExtensionMethodName.SetBrushData, [data]);


	public new void SetTool(T3DEditor.Tool tool) =>
		Call(GDExtensionMethodName.SetTool, [Variant.From(tool)]);

	public new T3DEditor.Tool GetTool() =>
		Call(GDExtensionMethodName.GetTool, []).As<T3DEditor.Tool>();

	public new void SetOperation(T3DEditor.Operation operation) =>
		Call(GDExtensionMethodName.SetOperation, [Variant.From(operation)]);

	public new T3DEditor.Operation GetOperation() =>
		Call(GDExtensionMethodName.GetOperation, []).As<T3DEditor.Operation>();

	public new void StartOperation(Vector3 position) =>
		Call(GDExtensionMethodName.StartOperation, [position]);

	public new bool IsOperating() =>
		Call(GDExtensionMethodName.IsOperating, []).As<bool>();

	public new void Operate(Vector3 position, double cameraDirection) =>
		Call(GDExtensionMethodName.Operate, [position, cameraDirection]);

	public new void BackupRegion(T3DRegion region) =>
		Call(GDExtensionMethodName.BackupRegion, [region]);

	public new void StopOperation() =>
		Call(GDExtensionMethodName.StopOperation, []);

	public new void ApplyUndo(Godot.Collections.Dictionary data) =>
		Call(GDExtensionMethodName.ApplyUndo, [data]);

}

file static class OperationExtensions
{
public static int SafeAsInt32(this T3DEditor.Operation enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this T3DEditor.Operation enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this T3DEditor.Operation? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}

file static class ToolExtensions
{
public static int SafeAsInt32(this T3DEditor.Tool enumValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this T3DEditor.Tool enumValue, int defaultValue) =>
Convert.ToInt32(enumValue);

public static int SafeAsInt32(this T3DEditor.Tool? enumValue, int defaultValue = 0) =>
enumValue.HasValue ? Convert.ToInt32(enumValue.Value) : defaultValue;
}
