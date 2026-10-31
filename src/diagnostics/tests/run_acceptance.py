#!/usr/bin/env python3
"""Portable P15 C++17 syntax and sanitizer test matrix for the Terrain3D src tree.

Usage:
  python src/diagnostics/tests/run_acceptance.py \
      --deps /path/to/dependency_root --output /tmp/terrain3d-p15

Dependency root requires godot-cpp/include, godot-cpp/gen/include,
thirdparty/EASTL/include, and thirdparty/EABase/include/Common.
The project root supplied here contains only src/, so this script performs
syntax and self-contained tests, not a Godot/Windows linked build.
"""

import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import subprocess
import time

SRC = Path( __file__ ).resolve().parents[2]
CONFIGS = {
	"editor": [ "-DGDEXTENSION", "-DT3D_EDITOR_NATIVE", "-DTOOLS_ENABLED", "-DT3D_TESTS_ENABLED" ],
	"release": [ "-DGDEXTENSION", "-DT3D_RUNTIME_NATIVE" ],
	"debug": [ "-DGDEXTENSION", "-DT3D_RUNTIME_NATIVE", "-DDEBUG_ENABLED", "-DDEBUG_METHODS_ENABLED" ],
}
PORTABLE = {
	"raster": ( "diagnostics/tests/test_raster_categorical_pick.cpp", [] ),
	"profiler": ( "diagnostics/tests/test_streaming_profiler.cpp", [] ),
	"forward_profile": ( "diagnostics/tests/test_forward_profile_stages.cpp", [] ),
	"transfer_budget": ( "diagnostics/tests/systems/gpu_transfer_budget_tests.cpp", [ "-DT3D_GPU_BUDGET_TEST_MAIN" ] ),
	"feedback_epoch": ( "diagnostics/tests/test_vt_feedback_epoch.cpp", [] ),
	"feedback_grid": ( "diagnostics/tests/test_vt_feedback_grid.cpp", [] ),
	"feedback_reducer": ( "diagnostics/tests/test_vt_feedback_reducer.cpp", [] ),
}


def run_command( command, timeout ):
	start = time.monotonic()
	try:
		result = subprocess.run( command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
			text=True, timeout=timeout, check=False )
		return { "exit": result.returncode, "seconds": round( time.monotonic() - start, 3 ),
			"output": result.stdout + result.stderr }
	except subprocess.TimeoutExpired:
		return { "exit": 124, "seconds": round( time.monotonic() - start, 3 ),
			"output": "Timeout" }


def main():
	parser = argparse.ArgumentParser( description=__doc__ )
	parser.add_argument( "--deps", required=True, type=Path )
	parser.add_argument( "--output", required=True, type=Path )
	parser.add_argument( "--profiles", nargs="+", choices=CONFIGS.keys(), default=list( CONFIGS ) )
	parser.add_argument( "--compilers", nargs="+", default=[ "clang++", "g++" ] )
	parser.add_argument( "--workers", type=int, default=4 )
	parser.add_argument( "--timeout", type=int, default=100 )
	parser.add_argument( "--skip-syntax", action="store_true" )
	parser.add_argument( "--skip-portable", action="store_true" )
	args = parser.parse_args()
	args.output.mkdir( parents=True, exist_ok=True )
	include = [ "-I", str( SRC ) ]
	for folder in [ "godot-cpp/include", "godot-cpp/gen/include", "thirdparty/EASTL/include", "thirdparty/EABase/include/Common" ]:
		dependency = args.deps / folder
		if not dependency.is_dir():
			parser.error( "Required dependency directory missing: " + str( dependency ) )
		include.extend( [ "-isystem", str( dependency ) ] )
	results = []
	if not args.skip_syntax:
		sources = sorted( SRC.rglob( "*.cpp" ) )
		jobs = [ ( compiler, profile, source ) for profile in args.profiles
			for compiler in args.compilers for source in sources ]

		def compile_one( spec ):
			compiler, profile, source = spec
			command = [ compiler, "-std=c++17", "-fsyntax-only", "-Werror=return-type",
				*CONFIGS[profile], *include, str( source ) ]
			result = run_command( command, args.timeout )
			return { "compiler": compiler, "profile": profile, "source": str( source.relative_to( SRC ) ),
				"command": command, **result }

		with concurrent.futures.ThreadPoolExecutor( max_workers=max( 1, args.workers ) ) as pool:
			for result in pool.map( compile_one, jobs ):
				results.append( result )
				if result["exit"]:
					print( "FAIL", result["compiler"], result["profile"], result["source"], flush=True )
		print( "Syntax passes", sum( r["exit"] == 0 for r in results ), "/", len( results ), flush=True )
	(args.output / "syntax_matrix.json").write_text( json.dumps( results, indent=2 ) )
	portable = []
	if not args.skip_portable:
		for compiler in args.compilers:
			for name, ( source, flags ) in PORTABLE.items():
				binary = args.output / ( compiler.replace( "/", "_" ) + "_" + name )
				command = [ compiler, "-std=c++17", "-O1", "-g", "-pthread",
					"-fno-omit-frame-pointer", "-fsanitize=address,undefined",
					*flags, "-I", str( SRC ), str( SRC / source ), "-o", str( binary ) ]
				build = run_command( command, args.timeout )
				test = run_command( [ str( binary ) ], args.timeout ) if build["exit"] == 0 else None
				entry = { "compiler": compiler, "test": name, "source": source,
					"build": build, "run": test, "build_command": command }
				portable.append( entry )
				print( compiler, name, "PASS" if test and test["exit"] == 0 else "FAIL", flush=True )
	(args.output / "portable_results.json").write_text( json.dumps( portable, indent=2 ) )
	failed = [ r for r in results if r["exit"] ]
	failed += [ p for p in portable if p["run"] is None or p["run"]["exit"] ]
	summary = { "source_translation_units": len( list( SRC.rglob( "*.cpp" ) ) ),
		"syntax_checks": len( results ), "syntax_failures": sum( r["exit"] != 0 for r in results ),
		"portable_tests": len( portable ), "portable_failures": len( failed ) - sum( r["exit"] != 0 for r in results ) }
	(args.output / "summary.json").write_text( json.dumps( summary, indent=2 ) )
	print( json.dumps( summary ), flush=True )
	return 1 if failed else 0


if __name__ == "__main__":
	raise SystemExit( main() )
