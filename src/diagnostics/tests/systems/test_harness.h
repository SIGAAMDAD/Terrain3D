#ifndef TERRAIN3D_SYSTEM_TEST_HARNESS_H
#define TERRAIN3D_SYSTEM_TEST_HARNESS_H

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <initializer_list>
#include <math.h>

namespace terrain3d_tests {
using namespace godot;

// Each case owns its fixtures. A failed requirement returns from that case;
// ordinary checks continue so related failures appear in the same report.
class TestContext {
  public:
	int assertions = 0;
	Array failures;

	bool check(bool condition, const char *expression, const char *file, int line) {
		++assertions;
		if (!condition) {
			Dictionary failure;
			failure["check"] = expression;
			failure["file"] = file;
			failure["line"] = line;
			failures.push_back(failure);
			UtilityFunctions::print("    FAIL ", expression, " (", file, ":", line, ")");
		}
		return condition;
	}
};

class TestRunner {
  public:
	String suite_filter;
	String case_filter;
	bool list_only = false;
	Array cases;
	int passed = 0;
	int failed = 0;
	int assertions = 0;

	template <typename Function> void run(const char *suite, const char *name, Function function) {
		if ((!suite_filter.is_empty() && suite_filter != suite) ||
		    (!case_filter.is_empty() && String(name).find(case_filter) < 0)) {
			return;
		}
		Dictionary result;
		result["suite"] = suite;
		result["name"] = name;
		if (list_only) {
			cases.push_back(result);
			return;
		}
		TestContext test;
		function(test);
		const bool success = test.failures.is_empty() && test.assertions > 0;
		if (success) {
			++passed;
		} else {
			++failed;
		}
		assertions += test.assertions;
		result["passed"] = success;
		result["assertions"] = test.assertions;
		result["failures"] = test.failures;
		cases.push_back(result);
		UtilityFunctions::print(success ? "PASS " : "FAIL ", suite, "/", name);
	}
};

inline bool near(double actual, double expected, double tolerance = 0.00001) {
	return isfinite(actual) && isfinite(expected) && fabs(actual - expected) <= tolerance;
}

void register_instancing_tests(TestRunner &runner);
void register_memory_arena_tests(TestRunner &runner);
void register_memory_budget_tests(TestRunner &runner);
void register_virtual_texturing_tests(TestRunner &runner);
void register_codec_tests(TestRunner &runner);
void register_cooking_tests(TestRunner &runner);
void register_render_upload_tests(TestRunner &runner);
} // namespace terrain3d_tests

#define T3D_CHECK(test, ...) (test).check((__VA_ARGS__), #__VA_ARGS__, __FILE__, __LINE__)
#define T3D_REQUIRE(test, ...)                                                                                         \
	do {                                                                                                               \
		if (!T3D_CHECK(test, __VA_ARGS__)) {                                                                           \
			return;                                                                                                    \
		}                                                                                                              \
	} while (0)

#endif
