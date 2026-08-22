@tool
class_name Terrain3DRegionWorkbenchCoverage
extends RefCounted

var minimum: Vector2i = Vector2i.ZERO
var maximum: Vector2i = Vector2i.ZERO


static func create(p_minimum: Vector2i, p_maximum: Vector2i) -> Terrain3DRegionWorkbenchCoverage:
	var result := Terrain3DRegionWorkbenchCoverage.new()
	result.minimum = p_minimum
	result.maximum = p_maximum
	return result
