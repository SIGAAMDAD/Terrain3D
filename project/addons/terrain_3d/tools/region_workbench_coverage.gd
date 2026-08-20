@tool
class_name T3DRegionWorkbenchCoverage
extends RefCounted

var minimum: Vector2i = Vector2i.ZERO
var maximum: Vector2i = Vector2i.ZERO


static func create(p_minimum: Vector2i, p_maximum: Vector2i) -> T3DRegionWorkbenchCoverage:
	var result := T3DRegionWorkbenchCoverage.new()
	result.minimum = p_minimum
	result.maximum = p_maximum
	return result
