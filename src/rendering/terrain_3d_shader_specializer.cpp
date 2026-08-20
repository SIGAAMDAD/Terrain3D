// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#include "core/terrain_3d_compiler.h"
#include "rendering/terrain_3d_shader_specializer.h"
#include "Terrain3DConstants.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

using namespace godot;

namespace {

typedef enum t3dconditionalOwnership_e {
	T3D_CONDITIONAL_PRESERVED,
	T3D_CONDITIONAL_SPECIALIZED,
} t3dconditionalOwnership_t;

typedef struct t3dconditionalFrame_s {
	t3dconditionalOwnership_t ownership = T3D_CONDITIONAL_PRESERVED;
	bool bParentEmit = true;
	bool bBranchTaken = false;
} t3dconditionalFrame_t;

class T3DStaticExpressionParser {
	const String &m_expression;
	const t3dshaderStaticState_t &m_state;
	int m_index = 0;
	String m_error;

	void skipWhitespace( void ) {
		while ( m_index < m_expression.length() ) {
			const char32_t c = m_expression[m_index];
			if ( c != ' ' && c != '\t' && c != '\r' ) {
				break;
			}
			++m_index;
		}
	}

	bool consume( const String &rToken ) {
		skipWhitespace();
		if ( m_expression.substr( m_index, rToken.length() ) != rToken ) {
			return false;
		}
		m_index += rToken.length();
		return true;
	}

	bool getMacroValue( const String &rName, int64_t &nValue ) {
		if ( rName == "T3D_TERRAIN_SAMPLING_MODE" ) {
			nValue = m_state.terrainSamplingMode;
		} else if ( rName == "T3D_TERRAIN_NORMAL_MODE" ) {
			nValue = m_state.terrainNormalMode;
		} else if ( rName == "T3D_COLOR_SAMPLING_MODE" ) {
			nValue = m_state.colorSamplingMode;
		} else if ( rName == "T3D_PROJECTION_MODE" ) {
			nValue = m_state.projectionMode;
		} else if ( rName == "T3D_TEXTURE_SURFACE_MODE" ) {
			nValue = m_state.textureSurfaceMode;
		} else if ( rName == "T3D_SAMPLE_SURFACE_DATA" ) {
			nValue = m_state.bSampleSurfaceData ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_ROUGHNESS" ) {
			nValue = m_state.bOutputRoughness ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_SPECULAR" ) {
			nValue = m_state.bOutputSpecular ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_NORMAL_MAP" ) {
			nValue = m_state.bOutputNormalMap ? 1 : 0;
		} else if ( rName == "T3D_OUTPUT_AO" ) {
			nValue = m_state.bOutputAo ? 1 : 0;
		} else if ( rName == "T3D_SHADING_MODE" ) {
			nValue = m_state.terrainShadingMode;
		} else if ( rName == "T3D_DETILING_MODE" ) {
			nValue = m_state.detilingMode;
		} else if ( rName == "T3D_WORLD_NOISE_SHADER_MODE" ) {
			nValue = m_state.worldNoiseShaderMode;
		} else if ( rName == "T3D_FAST_BLEND_CURVE" ) {
			nValue = m_state.bFastBlendCurve ? 1 : 0;
		} else if ( rName == "T3D_CONTROL_MAPS_IMPLICIT" ) {
			nValue = m_state.bControlMapsImplicit ? 1 : 0;
		} else if ( rName == "T3D_COLOR_MAPS_IMPLICIT" ) {
			nValue = m_state.bColorMapsImplicit ? 1 : 0;
		} else if ( rName == "T3D_AUTO_SHADER_ENABLED" ) {
			nValue = m_state.bAutoShaderEnabled ? 1 : 0;
		} else {
			m_error = "Unknown Terrain3D shader macro: " + rName;
			return false;
		}
		return true;
	}

	bool parseIdentifierOrInteger( int64_t &nValue ) {
		skipWhitespace();
		if ( m_index >= m_expression.length() ) {
			m_error = "Unexpected end of Terrain3D shader expression";
			return false;
		}
		const int start = m_index;
		if ( m_expression[m_index] == '-' || ( m_expression[m_index] >= '0' && m_expression[m_index] <= '9' ) ) {
			if ( m_expression[m_index] == '-' ) {
				++m_index;
			}
			const int digits = m_index;
			while ( m_index < m_expression.length() && m_expression[m_index] >= '0' && m_expression[m_index] <= '9' ) {
				++m_index;
			}
			if ( digits == m_index ) {
				m_error = "Invalid integer in Terrain3D shader expression";
				return false;
			}
			nValue = m_expression.substr( start, m_index - start ).to_int();
			return true;
		}

		while ( m_index < m_expression.length() ) {
			const char32_t c = m_expression[m_index];
			const bool identifier_char = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) ||
				( c >= '0' && c <= '9' ) || c == '_';
			if ( !identifier_char ) {
				break;
			}
			++m_index;
		}
		if ( start == m_index ) {
			m_error = "Expected macro or integer in Terrain3D shader expression";
			return false;
		}
		return getMacroValue( m_expression.substr( start, m_index - start ), nValue );
	}

	bool parseValue( int64_t &nValue ) {
		skipWhitespace();
		if ( consume( "(" ) ) {
			bool bNested = false;
			if ( !parseOr( bNested ) || !consume( ")" ) ) {
				if ( m_error.is_empty() ) {
					m_error = "Unbalanced parentheses in Terrain3D shader expression";
				}
				return false;
			}
			nValue = bNested ? 1 : 0;
			return true;
		}
		return parseIdentifierOrInteger( nValue );
	}

	bool parseComparison( bool &bValue ) {
		int64_t lhs = 0;
		if ( !parseValue( lhs ) ) {
			return false;
		}
		if ( consume( "==" ) ) {
			int64_t rhs = 0;
			if ( !parseValue( rhs ) ) {
				return false;
			}
			bValue = lhs == rhs;
			return true;
		}
		if ( consume( "!=" ) ) {
			int64_t rhs = 0;
			if ( !parseValue( rhs ) ) {
				return false;
			}
			bValue = lhs != rhs;
			return true;
		}
		bValue = lhs != 0;
		return true;
	}

	bool parseUnary( bool &bValue ) {
		if ( consume( "!" ) ) {
			if ( !parseUnary( bValue ) ) {
				return false;
			}
			bValue = !bValue;
			return true;
		}
		return parseComparison( bValue );
	}

	bool parseAnd( bool &bValue ) {
		if ( !parseUnary( bValue ) ) {
			return false;
		}
		while ( consume( "&&" ) ) {
			bool rhs = false;
			if ( !parseUnary( rhs ) ) {
				return false;
			}
			bValue = bValue && rhs;
		}
		return true;
	}

	bool parseOr( bool &bValue ) {
		if ( !parseAnd( bValue ) ) {
			return false;
		}
		while ( consume( "||" ) ) {
			bool rhs = false;
			if ( !parseAnd( rhs ) ) {
				return false;
			}
			bValue = bValue || rhs;
		}
		return true;
	}

public:
	EA_FORCE_INLINE T3DStaticExpressionParser( const String &rExpression, const t3dshaderStaticState_t &rState ) : m_expression( rExpression ), m_state( rState ) {}

	bool evaluate( bool &bValue, String &rFailureSummary ) {
		if ( !parseOr( bValue ) ) {
			rFailureSummary = m_error;
			return false;
		}
		skipWhitespace();
		if ( m_index != m_expression.length() ) {
			rFailureSummary = "Unsupported trailing syntax in Terrain3D shader expression: " +
				m_expression.substr( m_index );
			return false;
		}
		return true;
	}
};

bool containsEnabledFeature( const PackedStringArray &rFeatures, const String &rFeature ) {
	for ( int index = 0; index < rFeatures.size(); ++index ) {
		if ( rFeatures[index] == rFeature ) {
			return true;
		}
	}
	return false;
}

EA_FORCE_INLINE bool isTerrain3dFeatureMacro( const String &rName ) { return rName.begins_with( "T3D_ENABLE_" ); }

EA_FORCE_INLINE bool isTerrain3dStaticExpression( const String &rExpression ) { return rExpression.find( "T3D_" ) >= 0; }

String getShaderDirectiveCode( const String &rLine ) {
	String directive = rLine.strip_edges();
	if ( !directive.begins_with( "#" ) ) {
		return directive;
	}
	const int comment = directive.find( " //" );
	if ( comment >= 0 ) {
		directive = directive.substr( 0, comment ).strip_edges();
	}
	return directive;
}

bool isPathInStack( const Vector<String> &rStack, const String &rPath ) {
	for ( int index = 0; index < rStack.size(); ++index ) {
		if ( rStack[index] == rPath ) {
			return true;
		}
	}
	return false;
}

bool expandShaderSourceRecursive(
	const String &rPath, Vector<String> &rStack,
	String &rOutputSource, String &rFailureSummary
) {
	if ( rStack.size() >= TERRAIN3D_SHADER_MAX_CONDITIONAL_DEPTH ) {
		rFailureSummary = "Terrain3D shader include depth exceeded while expanding: " + rPath;
		return false;
	}
	if ( isPathInStack( rStack, rPath ) ) {
		rFailureSummary = "Terrain3D shader include cycle detected at: " + rPath;
		return false;
	}

	const Ref<FileAccess> file = FileAccess::open( rPath, FileAccess::READ );
	if ( file.is_null() ) {
		rFailureSummary = "Failed to open Terrain3D shader source: " + rPath;
		return false;
	}
	const String source = file->get_as_text();
	if ( source.is_empty() ) {
		rFailureSummary = "Terrain3D shader source is empty: " + rPath;
		return false;
	}

	rStack.push_back( rPath );
	const PackedStringArray lines = source.split( "\n", true );
	PackedStringArray output;
	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String &line = lines[line_index];
		const String directive = getShaderDirectiveCode( line );
		if ( !directive.begins_with( "#include" ) ) {
			output.push_back( line );
			continue;
		}

		const int quote_start = directive.find( "\"" );
		const int quote_end = quote_start >= 0 ? directive.find( "\"", quote_start + 1 ) : -1;
		if ( quote_start < 0 || quote_end <= quote_start + 1 ) {
			rStack.remove_at( rStack.size() - 1 );
			rFailureSummary = "Malformed Terrain3D shader include in " + rPath + ": " + directive;
			return false;
		}
		String include_path = directive.substr( quote_start + 1, quote_end - quote_start - 1 );
		if ( !include_path.begins_with( "res://" ) ) {
			include_path = rPath.get_base_dir().path_join( include_path ).simplify_path();
		}
		String expanded_include;
		if ( !expandShaderSourceRecursive( include_path, rStack, expanded_include, rFailureSummary ) ) {
			rStack.remove_at( rStack.size() - 1 );
			return false;
		}
		output.push_back( expanded_include );
	}
	rStack.remove_at( rStack.size() - 1 );

	rOutputSource = String( "\n" ).join( output );
	if ( source.ends_with( "\n" ) ) {
		rOutputSource += "\n";
	}
	return true;
}

} // namespace

bool terrain3dExpandShaderIncludes(
	const String &rRootPath, String &rOutputSource, String &rFailureSummary
) {
	Vector<String> stack;
	rOutputSource = String();
	rFailureSummary = String();
	return expandShaderSourceRecursive( rRootPath, stack, rOutputSource, rFailureSummary );
}

bool terrain3dValidateAssembledShaderSource(
	const String &rSource, String &rFailureSummary
) {
	const PackedStringArray lines = rSource.split( "\n", true );
	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String directive = getShaderDirectiveCode( lines[line_index] );
		if ( directive.begins_with( "#include" ) ) {
			rFailureSummary = "Unexpanded shader include remains at assembled line " +
				String::num_int64( line_index + 1 );
			return false;
		}
		if ( directive.find( "T3D_STANDALONE_DEFAULTS" ) >= 0 ||
			directive.find( "T3D_TEMPLATE_CONTEXT" ) >= 0 ||
			( directive.begins_with( "#" ) && directive.find( "T3D_" ) >= 0 ) ) {
			rFailureSummary = "Unresolved Terrain3D preprocessing state remains at assembled line " +
				String::num_int64( line_index + 1 ) + ": " + directive;
			return false;
		}
	}
	return true;
}

bool terrain3dAssembleShaderSource(
	const String &rExpandedSource, const t3dshaderStaticState_t &rState,
	const PackedStringArray &rEnabledFeatures, String &rOutputSource,
	String &rFailureSummary
) {
	const PackedStringArray lines = rExpandedSource.split( "\n", true );
	PackedStringArray output;
	t3dconditionalFrame_t stack[TERRAIN3D_SHADER_MAX_CONDITIONAL_DEPTH] = {};
	int depth = 0;
	bool bEmit = true;
	bool bSkipStandaloneDefaults = false;

	for ( int line_index = 0; line_index < lines.size(); ++line_index ) {
		const String &line = lines[line_index];
		const String directive = getShaderDirectiveCode( line );
		if ( directive == "// T3D_STANDALONE_DEFAULTS_BEGIN" ) {
			if ( bSkipStandaloneDefaults ) {
				rFailureSummary = "Nested Terrain3D standalone-default block at line " + String::num_int64( line_index + 1 );
				return false;
			}
			bSkipStandaloneDefaults = true;
			continue;
		}
		if ( directive == "// T3D_STANDALONE_DEFAULTS_END" ) {
			if ( !bSkipStandaloneDefaults ) {
				rFailureSummary = "Unmatched Terrain3D standalone-default terminator at line " + String::num_int64( line_index + 1 );
				return false;
			}
			bSkipStandaloneDefaults = false;
			continue;
		}
		if ( bSkipStandaloneDefaults ) {
			continue;
		}

		const bool bIfdef = directive.begins_with( "#ifdef " );
		const bool bIfndef = directive.begins_with( "#ifndef " );
		if ( bIfdef || bIfndef ) {
			if ( depth >= TERRAIN3D_SHADER_MAX_CONDITIONAL_DEPTH ) {
				rFailureSummary = "Terrain3D shader conditional depth exceeded at line " + String::num_int64( line_index + 1 );
				return false;
			}
			const String macro = directive.substr( bIfdef ? 7 : 8 ).strip_edges();
			bool bOwned = false;
			bool bValue = false;
			if ( macro == "T3D_TEMPLATE_CONTEXT" ) {
				bOwned = true;
				bValue = true;
			} else if ( isTerrain3dFeatureMacro( macro ) ) {
				bOwned = true;
				const String feature = macro.substr( String( "T3D_ENABLE_" ).length() );
				bValue = containsEnabledFeature( rEnabledFeatures, feature );
			}
			if ( bIfndef ) {
				bValue = !bValue;
			}

			t3dconditionalFrame_t &frame = stack[depth++];
			frame.bParentEmit = bEmit;
			frame.bBranchTaken = bOwned && bValue;
			frame.ownership = bOwned ? T3D_CONDITIONAL_SPECIALIZED : T3D_CONDITIONAL_PRESERVED;
			if ( bOwned ) {
				bEmit = bEmit && bValue;
			} else if ( bEmit ) {
				output.push_back( line );
			}
			continue;
		}

		if ( directive.begins_with( "#if " ) ) {
			if ( depth >= TERRAIN3D_SHADER_MAX_CONDITIONAL_DEPTH ) {
				rFailureSummary = "Terrain3D shader conditional depth exceeded at line " + String::num_int64( line_index + 1 );
				return false;
			}
			const String expression = directive.substr( 4 ).strip_edges();
			const bool bOwned = isTerrain3dStaticExpression( expression );
			bool bValue = false;
			if ( bOwned ) {
				T3DStaticExpressionParser parser( expression, rState );
				if ( !parser.evaluate( bValue, rFailureSummary ) ) {
					rFailureSummary = "Terrain3D shader expression error at line " + String::num_int64( line_index + 1 ) +
						": " + rFailureSummary + " [" + expression + "]";
					return false;
				}
			}
			t3dconditionalFrame_t &frame = stack[depth++];
			frame.bParentEmit = bEmit;
			frame.bBranchTaken = bOwned && bValue;
			frame.ownership = bOwned ? T3D_CONDITIONAL_SPECIALIZED : T3D_CONDITIONAL_PRESERVED;
			if ( bOwned ) {
				bEmit = bEmit && bValue;
			} else if ( bEmit ) {
				output.push_back( line );
			}
			continue;
		}

		if ( directive.begins_with( "#elif " ) ) {
			if ( depth <= 0 ) {
				rFailureSummary = "Unmatched #elif at line " + String::num_int64( line_index + 1 );
				return false;
			}
			t3dconditionalFrame_t &frame = stack[depth - 1];
			if ( frame.ownership == T3D_CONDITIONAL_PRESERVED ) {
				if ( frame.bParentEmit ) {
					output.push_back( line );
				}
				bEmit = frame.bParentEmit;
				continue;
			}
			const String expression = directive.substr( 6 ).strip_edges();
			if ( !isTerrain3dStaticExpression( expression ) ) {
				rFailureSummary = "Terrain3D-owned conditional mixes with non-Terrain3D #elif at line " +
					String::num_int64( line_index + 1 );
				return false;
			}
			bool bValue = false;
			T3DStaticExpressionParser parser( expression, rState );
			if ( !parser.evaluate( bValue, rFailureSummary ) ) {
				rFailureSummary = "Terrain3D shader #elif expression error at line " + String::num_int64( line_index + 1 ) +
					": " + rFailureSummary + " [" + expression + "]";
				return false;
			}
			const bool bSelect = !frame.bBranchTaken && bValue;
			frame.bBranchTaken = frame.bBranchTaken || bValue;
			bEmit = frame.bParentEmit && bSelect;
			continue;
		}

		if ( directive == "#else" ) {
			if ( depth <= 0 ) {
				rFailureSummary = "Unmatched #else at line " + String::num_int64( line_index + 1 );
				return false;
			}
			t3dconditionalFrame_t &frame = stack[depth - 1];
			if ( frame.ownership == T3D_CONDITIONAL_PRESERVED ) {
				if ( frame.bParentEmit ) {
					output.push_back( line );
				}
				bEmit = frame.bParentEmit;
				continue;
			}
			bEmit = frame.bParentEmit && !frame.bBranchTaken;
			frame.bBranchTaken = true;
			continue;
		}

		if ( directive == "#endif" ) {
			if ( depth <= 0 ) {
				rFailureSummary = "Unmatched #endif at line " + String::num_int64( line_index + 1 );
				return false;
			}
			const t3dconditionalFrame_t frame = stack[--depth];
			if ( frame.ownership == T3D_CONDITIONAL_PRESERVED && frame.bParentEmit ) {
				output.push_back( line );
			}
			bEmit = frame.bParentEmit;
			continue;
		}

		if ( bEmit ) {
			output.push_back( line );
		}
	}

	if ( depth != 0 ) {
		rFailureSummary = "Unbalanced shader conditional stack after Terrain3D assembly";
		return false;
	}
	if ( bSkipStandaloneDefaults ) {
		rFailureSummary = "Unterminated Terrain3D standalone-default block";
		return false;
	}

	String assembled = String( "\n" ).join( output );
	if ( rExpandedSource.ends_with( "\n" ) ) {
		assembled += "\n";
	}
	if ( !terrain3dValidateAssembledShaderSource( assembled, rFailureSummary ) ) {
		return false;
	}
	rOutputSource = assembled;
	return true;
}
