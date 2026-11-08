// GLSL ES 1.00 -> Cg translation for the Vita (vitaGL takes raw Cg shaders).
//
// vitaGL has its own GLSL translator, but it rewrites every '*' without knowing
// the operand types, which breaks the engine's skinning and texture-matrix
// math. This one preprocesses the source itself, tracks which expressions are
// matrices and turns only matrix products into mul() calls.
//
// Conventions:
// - Matrix uniforms are uploaded as the game passes them (column-major), so a
//   GLSL column is a Cg row: GLSL "M * v" becomes "mul(v, M)" and "A * B"
//   becomes "mul(B, A)" (Options::swap_mul = false flips this, for testing).
// - Attributes keep their names as the entry point's parameters, so the
//   engine finds them with glGetActiveAttrib / glGetAttribLocation.
// - Varyings get TEXCOORD0..9 semantics from a map shared by the vertex and
//   fragment shader of one program (see varyings()).
// - Uniform names that are Cg keywords get a '_' suffix (renamed_uniform()).
#pragma once

#include <map>
#include <string>
#include <vector>

namespace glsl {

struct Options {
    bool swap_mul = true;
};

// Names of the varyings a shader declares (after preprocessing), in order.
std::vector<std::string> varyings(const std::string& source, bool vertex);

// The vertex shader's attributes as {GLSL type, name}, in declaration order.
std::vector<std::pair<std::string, std::string>> attributes(const std::string& source);

// Translate; varying_slots maps each varying name to its TEXCOORD index.
// Returns false and sets error when the shader uses something unsupported.
bool to_cg(const std::string& source, bool vertex, const std::map<std::string, int>& varying_slots,
           const Options& opt, std::string& out, std::string& error);

// The Cg name of a uniform, if it had to be renamed (else the name itself).
std::string renamed_uniform(const std::string& name);

}  // namespace glsl
