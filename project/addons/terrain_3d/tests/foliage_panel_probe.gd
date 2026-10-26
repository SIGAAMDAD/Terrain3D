@tool
extends "res://addons/terrain_3d/tools/foliage_panel.gd"

# Test-only seams: native generation/configuration remain the real panel code.
# Successful publication is intercepted so no editor undo actions are registered.
var publication_requested: bool = false
var fail_staging: bool = false
var fail_restore: bool = false
var use_real_commit: bool = false

func _commit(batches: Array) -> void:
	if use_real_commit:
		super._commit(batches)
	else:
		publication_requested = true

func commit_with_staging_failure(batches: Array) -> void:
	fail_staging = true
	super._commit(batches)

func _new_transaction():
	publication_requested = true
	var transaction = super._new_transaction()
	if fail_staging:
		transaction.failure_phase = "PREPARED"
	transaction.fail_restore = fail_restore
	return transaction
