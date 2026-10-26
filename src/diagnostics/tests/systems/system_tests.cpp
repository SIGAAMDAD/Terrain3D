#include "diagnostics/tests/systems/test_harness.h"

godot::Dictionary t3dRunSystemUnitTests(
	const godot::String &suite_filter,
	const godot::String &case_filter,
	bool				 list_only ) {
	terrain3d_tests::TestRunner runner;
	runner.suite_filter = suite_filter;
	runner.case_filter = case_filter;
	runner.list_only = list_only;
	terrain3d_tests::register_instancing_tests( runner );
	terrain3d_tests::register_memory_arena_tests( runner );
	terrain3d_tests::register_memory_budget_tests( runner );
	terrain3d_tests::register_virtual_texturing_tests( runner );
	terrain3d_tests::register_codec_tests( runner );
	terrain3d_tests::register_cooking_tests( runner );
	terrain3d_tests::register_localized_correctness_tests( runner );
	terrain3d_tests::register_render_upload_tests( runner );
	terrain3d_tests::register_gpu_single_copy_tests( runner );
	terrain3d_tests::register_stream_source_cache_tests( runner );
	terrain3d_tests::register_manifest_safety_tests( runner );
	terrain3d_tests::register_stream_io_rollout_tests( runner );
	terrain3d_tests::register_vt_spatial_demand_tests( runner );
	terrain3d_tests::register_vt_visibility_baker_tests( runner );
	terrain3d_tests::register_hierarchy_scheduler_tests( runner );
	terrain3d_tests::register_vt_visibility_aggregation_tests( runner );
	terrain3d_tests::register_gpu_page_reduction_tests( runner );
	terrain3d_tests::register_gpu_page_upload_tests( runner );
	terrain3d_tests::register_gpu_page_grouped_cull_tests( runner );
	terrain3d_tests::register_gpu_page_scan_tests( runner );
	godot::Dictionary report;
	report["cases"] = runner.cases;
	report["case_count"] = runner.cases.size();
	report["passed"] = runner.passed;
	report["failed"] = runner.failed;
	report["skipped"] = runner.skipped;
	report["assertions"] = runner.assertions;
	report["list_only"] = list_only;
	report["success"] = !runner.cases.is_empty() && runner.failed == 0;
	return report;
}
