#include "diagnostics/tests/systems/test_harness.h"
#include "rendering/rd/core/terrain_3d_rd_context.h"

namespace terrain3d_tests {
void register_render_upload_tests(TestRunner &runner) {
	runner.run("render_uploads", "all_passes_and_viewports_accumulate_in_one_render_frame", [](TestContext &test) {
		Terrain3DRdContext context;
		context.beginFrame(41, 9);
		context.recordBufferUpdate(512, T3D_RD_CONTEXT_OWNER_TERRAIN, T3D_GPU_HOT_FRAME);
		context.beginFrame(41, 10); // Another scene pass/view, same device frame.
		context.recordBufferUpdate(1024, T3D_RD_CONTEXT_OWNER_INSTANCES, T3D_GPU_HISTORY);
		context.recordTextureUpload(65536, T3D_RD_CONTEXT_OWNER_VT);
		const auto diagnostics = context.getGpuUploadDiagnostics();
		T3D_CHECK(test, int64_t(diagnostics["bytes_uploaded_frame"]) == 67072);
		T3D_CHECK(test, int64_t(diagnostics["upload_calls_frame"]) == 3);
		const Dictionary owners = diagnostics["frame_by_owner_and_policy"];
		T3D_CHECK(test, int64_t(owners["terrain_hot_frame_bytes"]) == 512);
		T3D_CHECK(test, int64_t(owners["instances_history_bytes"]) == 1024);
		T3D_CHECK(test, int64_t(owners["vt_dirty_upload_bytes"]) == 65536);
	});
	runner.run("render_uploads", "completed_frames_survive_polling_and_idle_frames_report_zero", [](TestContext &test) {
		Terrain3DRdContext context;
		context.beginFrame(7, 1);
		context.recordBufferUpdate(128);
		context.recordTextureUpload(4096, T3D_RD_CONTEXT_OWNER_TERRAIN);
		context.beginFrame(8, 2);
		const auto idle = context.getGpuUploadDiagnostics();
		T3D_CHECK(test, int64_t(idle["completed_frame_id"]) == 7);
		T3D_CHECK(test, int64_t(idle["bytes_uploaded_completed_frame"]) == 4224);
		T3D_CHECK(test, int64_t(idle["bytes_uploaded_frame"]) == 0);
		T3D_CHECK(test, context.getGpuUploadDiagnostics() == idle); // Read-only polling.
		context.beginFrame(9, 2);
		const auto next = context.getGpuUploadDiagnostics();
		T3D_CHECK(test, int64_t(next["bytes_uploaded_completed_frame"]) == 0);
		T3D_CHECK(test, int64_t(next["peak_completed_frame_bytes"]) == 4224);
		T3D_CHECK(test, int64_t(next["buffer_upload_bytes"]) == 128);
		T3D_CHECK(test, int64_t(next["texture_upload_bytes"]) == 4096);
		T3D_CHECK(test, int64_t(next["bytes_uploaded"]) == 4224);
	});
	runner.run("render_uploads", "zero_byte_operations_do_not_inflate_transfer_counts", [](TestContext &test) {
		Terrain3DRdContext context;
		context.beginFrame(0, 0);
		context.recordBufferUpdate(0);
		context.recordTextureUpload(0, T3D_RD_CONTEXT_OWNER_VT);
		const auto diagnostics = context.getGpuUploadDiagnostics();
		T3D_CHECK(test, int64_t(diagnostics["upload_calls_frame"]) == 0);
		T3D_CHECK(test, int64_t(diagnostics["bytes_uploaded"]) == 0);
		T3D_CHECK(test, int64_t(context.getDiagnostics()["buffer_updates"]) == 0);
	});
}
} // namespace terrain3d_tests
