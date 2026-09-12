#pragma once

// draw_assets.hpp -- REN-38-F7 (D-007): the debug-draw shader suite as AUTHORED DECLARATIONS. The 339-line
// hand-written CKIR builder (`ckir_draw.hpp`) is DELETED; these are the same programs -- line_aa's screen-space
// quad expansion, triangle_solid's corner select, the infinite grid -- as `.crdv` vertex declarations (the F7
// procedural vocabulary: `position = "node:..."` + `[expand]` + field/header/corner inputs) and `.crdm`
// materials (the line falloff, the colour passthrough, the cell-factor grid over the new `fwidth` node).
// THE DELETION IS THE PROOF the F7 vocabulary can express them; the shipped copies under `assets/` are the
// editable form, pinned by the drift gate through `builtin_draw_asset_text`.
//
// THE DATA CONTRACT is unchanged (one u32 storage buffer at set 0 / binding 0):
//   words [0..31] HEADER: [0..15] view_proj . [16,17] viewport_px . [18] category_mask . [19] time_s
//     [20..22] camera_pos . [23] plane_y . [24] primary_color . [25] secondary_color . [26] primary_cell
//     [27] secondary_cell . [28] fade_distance . [29] axis_x_color . [30] axis_z_color . [31] reserved
//   words [32..) INSTANCES: line 9 words (sx sy sz ex ey ez color flags width) . tri 11 words (3x pos3 color flags)
// Vertex counts: lines 6*N, triangles 3*N, the grid a fixed 6.

#include <crd/containers/string.hpp>
#include <crd/core/types.hpp>

namespace crd::draw
{

inline constexpr crd::u32 kHeaderWords       = 32U;
inline constexpr crd::u32 kLineInstanceWords = 9U;
inline constexpr crd::u32 kTriInstanceWords  = 11U;

inline constexpr const char* kDrawLineVs = R"(schema   = 1
name     = "crd://vertex/draw_line"
position = "node:clip"

[header]
view_proj = 0

[expand]
verts_per_instance = 6
instance_words     = 9
instance_off       = 32
category_field     = 7
category_mask_word = 18

[[varying]]
name       = "v_color"
location   = 0
interp     = "smooth"
source     = ["node:col"]
node_comps = [4]

[[varying]]
name       = "v_quad"
location   = 1
interp     = "smooth"
source     = ["node:quadv"]
node_comps = [2]

[[node]]
name   = "cxc"
op     = "ifequal"
inputs = ["@corner", 5.0, 1.0, 0.0]

[[node]]
name   = "cxb"
op     = "ifequal"
inputs = ["@corner", 4.0, 1.0, "cxc"]

[[node]]
name   = "cx"
op     = "ifequal"
inputs = ["@corner", 1.0, 1.0, "cxb"]

[[node]]
name   = "cyc"
op     = "ifequal"
inputs = ["@corner", 5.0, 1.0, -1.0]

[[node]]
name   = "cyb"
op     = "ifequal"
inputs = ["@corner", 3.0, 1.0, "cyc"]

[[node]]
name   = "cy"
op     = "ifequal"
inputs = ["@corner", 2.0, 1.0, "cyb"]

[[node]]
name   = "sp"
op     = "combine3"
inputs = ["field:0", "field:1", "field:2"]

[[node]]
name   = "ep"
op     = "combine3"
inputs = ["field:3", "field:4", "field:5"]

[[node]]
name   = "ca"
op     = "view_proj"
inputs = ["sp"]

[[node]]
name   = "cb"
op     = "view_proj"
inputs = ["ep"]

[[node]]
name   = "cax"
op     = "extract"
inputs = ["ca", 0]

[[node]]
name   = "cay"
op     = "extract"
inputs = ["ca", 1]

[[node]]
name   = "caz"
op     = "extract"
inputs = ["ca", 2]

[[node]]
name   = "caw"
op     = "extract"
inputs = ["ca", 3]

[[node]]
name   = "cbx"
op     = "extract"
inputs = ["cb", 0]

[[node]]
name   = "cby"
op     = "extract"
inputs = ["cb", 1]

[[node]]
name   = "cbz"
op     = "extract"
inputs = ["cb", 2]

[[node]]
name   = "cbw"
op     = "extract"
inputs = ["cb", 3]

[[node]]
name   = "hvx"
op     = "multiply"
inputs = ["hdr:16", 0.5]

[[node]]
name   = "hvy"
op     = "multiply"
inputs = ["hdr:17", 0.5]

[[node]]
name   = "ndax"
op     = "divide"
inputs = ["cax", "caw"]

[[node]]
name   = "ax"
op     = "multiply"
inputs = ["ndax", "hvx"]

[[node]]
name   = "nday"
op     = "divide"
inputs = ["cay", "caw"]

[[node]]
name   = "ay"
op     = "multiply"
inputs = ["nday", "hvy"]

[[node]]
name   = "ndbx"
op     = "divide"
inputs = ["cbx", "cbw"]

[[node]]
name   = "bx"
op     = "multiply"
inputs = ["ndbx", "hvx"]

[[node]]
name   = "ndby"
op     = "divide"
inputs = ["cby", "cbw"]

[[node]]
name   = "by"
op     = "multiply"
inputs = ["ndby", "hvy"]

[[node]]
name   = "dx0"
op     = "subtract"
inputs = ["bx", "ax"]

[[node]]
name   = "dy0"
op     = "subtract"
inputs = ["by", "ay"]

[[node]]
name   = "l2a"
op     = "multiply"
inputs = ["dx0", "dx0"]

[[node]]
name   = "l2b"
op     = "multiply"
inputs = ["dy0", "dy0"]

[[node]]
name   = "l2"
op     = "add"
inputs = ["l2a", "l2b"]

[[node]]
name   = "len0"
op     = "sqrt"
inputs = ["l2"]

[[node]]
name   = "len"
op     = "ifgreater"
inputs = [0.001, "len0", 1.0, "len0"]

[[node]]
name   = "dirx0"
op     = "divide"
inputs = ["dx0", "len"]

[[node]]
name   = "dirx"
op     = "ifgreater"
inputs = [0.001, "len0", 1.0, "dirx0"]

[[node]]
name   = "diry0"
op     = "divide"
inputs = ["dy0", "len"]

[[node]]
name   = "diry"
op     = "ifgreater"
inputs = [0.001, "len0", 0.0, "diry0"]

[[node]]
name   = "halfw"
op     = "multiply"
inputs = ["field:8", 0.5]

[[node]]
name   = "bmx"
op     = "multiply"
inputs = ["dx0", "cx"]

[[node]]
name   = "basex"
op     = "add"
inputs = ["ax", "bmx"]

[[node]]
name   = "bmy"
op     = "multiply"
inputs = ["dy0", "cx"]

[[node]]
name   = "basey"
op     = "add"
inputs = ["ay", "bmy"]

[[node]]
name   = "push"
op     = "multiply"
inputs = ["cy", "halfw"]

[[node]]
name   = "nx"
op     = "multiply"
inputs = ["diry", -1.0]

[[node]]
name   = "fxm"
op     = "multiply"
inputs = ["nx", "push"]

[[node]]
name   = "fx"
op     = "add"
inputs = ["basex", "fxm"]

[[node]]
name   = "fym"
op     = "multiply"
inputs = ["dirx", "push"]

[[node]]
name   = "fy"
op     = "add"
inputs = ["basey", "fym"]

[[node]]
name   = "fx2"
op     = "multiply"
inputs = ["fx", 2.0]

[[node]]
name   = "ndcx"
op     = "divide"
inputs = ["fx2", "hdr:16"]

[[node]]
name   = "fy2"
op     = "multiply"
inputs = ["fy", 2.0]

[[node]]
name   = "ndcy"
op     = "divide"
inputs = ["fy2", "hdr:17"]

[[node]]
name   = "dw"
op     = "subtract"
inputs = ["cbw", "caw"]

[[node]]
name   = "wmx"
op     = "multiply"
inputs = ["dw", "cx"]

[[node]]
name   = "w"
op     = "add"
inputs = ["caw", "wmx"]

[[node]]
name   = "za"
op     = "divide"
inputs = ["caz", "caw"]

[[node]]
name   = "zb"
op     = "divide"
inputs = ["cbz", "cbw"]

[[node]]
name   = "dz"
op     = "subtract"
inputs = ["zb", "za"]

[[node]]
name   = "zmx"
op     = "multiply"
inputs = ["dz", "cx"]

[[node]]
name   = "z"
op     = "add"
inputs = ["za", "zmx"]

[[node]]
name   = "pxw"
op     = "multiply"
inputs = ["ndcx", "w"]

[[node]]
name   = "pyw"
op     = "multiply"
inputs = ["ndcy", "w"]

[[node]]
name   = "pzw"
op     = "multiply"
inputs = ["z", "w"]

[[node]]
name   = "vis"
op     = "multiply"
inputs = ["@category", 1.0]

[[node]]
name   = "pxd"
op     = "subtract"
inputs = ["pxw", 2.0]

[[node]]
name   = "pxm"
op     = "multiply"
inputs = ["vis", "pxd"]

[[node]]
name   = "px"
op     = "add"
inputs = ["pxm", 2.0]

[[node]]
name   = "pyd"
op     = "subtract"
inputs = ["pyw", 2.0]

[[node]]
name   = "pym"
op     = "multiply"
inputs = ["vis", "pyd"]

[[node]]
name   = "py"
op     = "add"
inputs = ["pym", 2.0]

[[node]]
name   = "pzd"
op     = "subtract"
inputs = ["pzw", 2.0]

[[node]]
name   = "pzm"
op     = "multiply"
inputs = ["vis", "pzd"]

[[node]]
name   = "pz"
op     = "add"
inputs = ["pzm", 2.0]

[[node]]
name   = "pwd"
op     = "subtract"
inputs = ["w", 1.0]

[[node]]
name   = "pwm"
op     = "multiply"
inputs = ["vis", "pwd"]

[[node]]
name   = "pw"
op     = "add"
inputs = ["pwm", 1.0]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["px", "py", "pz", "pw"]

[[node]]
name   = "col"
op     = "multiply"
inputs = ["fieldc:6", [1.0, 1.0, 1.0, 1.0]]

[[node]]
name   = "hwm"
op     = "maximum"
inputs = ["halfw", 0.5]

[[node]]
name   = "fade"
op     = "divide"
inputs = [1.0, "hwm"]

[[node]]
name   = "quadv"
op     = "combine2"
inputs = ["cy", "fade"]
)";

inline constexpr const char* kDrawTriVs = R"(schema   = 1
name     = "crd://vertex/draw_tri"
position = "node:clip"

[header]
view_proj = 0

[expand]
verts_per_instance = 3
instance_words     = 11
instance_off       = 32
category_field     = 10
category_mask_word = 18

[[varying]]
name       = "v_color"
location   = 0
interp     = "smooth"
source     = ["node:col"]
node_comps = [4]

[[node]]
name   = "w0"
op     = "ifequal"
inputs = ["@corner", 0.0, 1.0, 0.0]

[[node]]
name   = "w1"
op     = "ifequal"
inputs = ["@corner", 1.0, 1.0, 0.0]

[[node]]
name   = "w01"
op     = "add"
inputs = ["w0", "w1"]

[[node]]
name   = "w2"
op     = "subtract"
inputs = [1.0, "w01"]

[[node]]
name   = "wsel"
op     = "combine3"
inputs = ["w0", "w1", "w2"]

[[node]]
name   = "px3"
op     = "combine3"
inputs = ["field:0", "field:3", "field:6"]

[[node]]
name   = "py3"
op     = "combine3"
inputs = ["field:1", "field:4", "field:7"]

[[node]]
name   = "pz3"
op     = "combine3"
inputs = ["field:2", "field:5", "field:8"]

[[node]]
name   = "wx"
op     = "dotproduct"
inputs = ["wsel", "px3"]

[[node]]
name   = "wy"
op     = "dotproduct"
inputs = ["wsel", "py3"]

[[node]]
name   = "wz"
op     = "dotproduct"
inputs = ["wsel", "pz3"]

[[node]]
name   = "wp"
op     = "combine3"
inputs = ["wx", "wy", "wz"]

[[node]]
name   = "cp"
op     = "view_proj"
inputs = ["wp"]

[[node]]
name   = "cpx"
op     = "extract"
inputs = ["cp", 0]

[[node]]
name   = "cpy"
op     = "extract"
inputs = ["cp", 1]

[[node]]
name   = "cpz"
op     = "extract"
inputs = ["cp", 2]

[[node]]
name   = "cpw"
op     = "extract"
inputs = ["cp", 3]

[[node]]
name   = "vis"
op     = "multiply"
inputs = ["@category", 1.0]

[[node]]
name   = "pxd"
op     = "subtract"
inputs = ["cpx", 2.0]

[[node]]
name   = "pxm"
op     = "multiply"
inputs = ["vis", "pxd"]

[[node]]
name   = "px"
op     = "add"
inputs = ["pxm", 2.0]

[[node]]
name   = "pyd"
op     = "subtract"
inputs = ["cpy", 2.0]

[[node]]
name   = "pym"
op     = "multiply"
inputs = ["vis", "pyd"]

[[node]]
name   = "py"
op     = "add"
inputs = ["pym", 2.0]

[[node]]
name   = "pzd"
op     = "subtract"
inputs = ["cpz", 2.0]

[[node]]
name   = "pzm"
op     = "multiply"
inputs = ["vis", "pzd"]

[[node]]
name   = "pz"
op     = "add"
inputs = ["pzm", 2.0]

[[node]]
name   = "pwd"
op     = "subtract"
inputs = ["cpw", 1.0]

[[node]]
name   = "pwm"
op     = "multiply"
inputs = ["vis", "pwd"]

[[node]]
name   = "pw"
op     = "add"
inputs = ["pwm", 1.0]

[[node]]
name   = "clip"
op     = "combine4"
inputs = ["px", "py", "pz", "pw"]

[[node]]
name   = "col"
op     = "multiply"
inputs = ["fieldc:9", [1.0, 1.0, 1.0, 1.0]]
)";

inline constexpr const char* kDrawGridVs = R"(schema   = 1
name     = "crd://vertex/draw_grid"
position = "node:clip"

[header]
view_proj = 0

[expand]
verts_per_instance = 6

[[varying]]
name       = "v_world"
location   = 0
interp     = "smooth"
source     = ["node:wp"]
node_comps = [3]

[[varying]]
name       = "v_camrel"
location   = 1
interp     = "smooth"
source     = ["node:cr3"]
node_comps = [3]

[[varying]]
name       = "v_cells"
location   = 2
interp     = "smooth"
source     = ["node:cells"]
node_comps = [2]

[[varying]]
name       = "v_colp"
location   = 3
interp     = "smooth"
source     = ["node:colp"]
node_comps = [4]

[[varying]]
name       = "v_cols"
location   = 4
interp     = "smooth"
source     = ["node:cols"]
node_comps = [4]

[[varying]]
name       = "v_colax"
location   = 5
interp     = "smooth"
source     = ["node:colax"]
node_comps = [4]

[[varying]]
name       = "v_colaz"
location   = 6
interp     = "smooth"
source     = ["node:colaz"]
node_comps = [4]

[[node]]
name   = "cxb"
op     = "ifequal"
inputs = ["@corner", 4.0, 1.0, -1.0]

[[node]]
name   = "cxa"
op     = "ifequal"
inputs = ["@corner", 2.0, 1.0, "cxb"]

[[node]]
name   = "cx"
op     = "ifequal"
inputs = ["@corner", 1.0, 1.0, "cxa"]

[[node]]
name   = "czb"
op     = "ifequal"
inputs = ["@corner", 5.0, 1.0, -1.0]

[[node]]
name   = "cza"
op     = "ifequal"
inputs = ["@corner", 4.0, 1.0, "czb"]

[[node]]
name   = "cz"
op     = "ifequal"
inputs = ["@corner", 2.0, 1.0, "cza"]

[[node]]
name   = "half"
op     = "maximum"
inputs = ["hdr:28", 1.0]

[[node]]
name   = "wxm"
op     = "multiply"
inputs = ["cx", "half"]

[[node]]
name   = "wx"
op     = "add"
inputs = ["hdr:20", "wxm"]

[[node]]
name   = "wzm"
op     = "multiply"
inputs = ["cz", "half"]

[[node]]
name   = "wz"
op     = "add"
inputs = ["hdr:22", "wzm"]

[[node]]
name   = "wy"
op     = "multiply"
inputs = ["hdr:23", 1.0]

[[node]]
name   = "wp"
op     = "combine3"
inputs = ["wx", "wy", "wz"]

[[node]]
name   = "clip"
op     = "view_proj"
inputs = ["wp"]

[[node]]
name   = "crx"
op     = "subtract"
inputs = ["wx", "hdr:20"]

[[node]]
name   = "crz"
op     = "subtract"
inputs = ["wz", "hdr:22"]

[[node]]
name   = "cr3"
op     = "combine3"
inputs = ["crx", "crz", "half"]

[[node]]
name   = "cells"
op     = "combine2"
inputs = ["hdr:26", "hdr:27"]

[[node]]
name   = "colp"
op     = "multiply"
inputs = ["hdrc:24", [1.0, 1.0, 1.0, 1.0]]

[[node]]
name   = "cols"
op     = "multiply"
inputs = ["hdrc:25", [1.0, 1.0, 1.0, 1.0]]

[[node]]
name   = "colax"
op     = "multiply"
inputs = ["hdrc:29", [1.0, 1.0, 1.0, 1.0]]

[[node]]
name   = "colaz"
op     = "multiply"
inputs = ["hdrc:30", [1.0, 1.0, 1.0, 1.0]]
)";

inline constexpr const char* kDrawLineMat = R"(schema = 1
name   = "crd://material/draw_line"

[[node]]
name   = "col"
op     = "geomcolor"
inputs = [0]

[[node]]
name   = "q"
op     = "texcoord"
inputs = [1]

[[node]]
name   = "d0"
op     = "extract"
inputs = ["q", 0]

[[node]]
name   = "d"
op     = "absval"
inputs = ["d0"]

[[node]]
name   = "fade"
op     = "extract"
inputs = ["q", 1]

[[node]]
name   = "edge0"
op     = "subtract"
inputs = [1.0, "fade"]

# NOTE: MaterialX operand order -- smoothstep(in, low, high), the VALUE first. Writing it GLSL-style
# cooked to `smoothstep(1.0, d, edge0)`: a reversed range that rendered every debug line narrow and
# translucent, with a full-alpha 1px sliver hugging one edge (the "dotted line beside the gizmo").
# The full scar is written up in assets/material/draw_line.crdm.
[[node]]
name   = "aa"
op     = "smoothstep"
inputs = ["d", "edge0", 1.0]

[[node]]
name   = "alpha"
op     = "subtract"
inputs = [1.0, "aa"]

[[node]]
name   = "cr"
op     = "extract"
inputs = ["col", 0]

[[node]]
name   = "cg"
op     = "extract"
inputs = ["col", 1]

[[node]]
name   = "cb"
op     = "extract"
inputs = ["col", 2]

[[node]]
name   = "ca"
op     = "extract"
inputs = ["col", 3]

[[node]]
name   = "base"
op     = "combine3"
inputs = ["cr", "cg", "cb"]

[[node]]
name   = "aout"
op     = "multiply"
inputs = ["ca", "alpha"]

[surface]
base_color = "base"
opacity    = "aout"
)";

inline constexpr const char* kDrawTriMat = R"(schema = 1
name   = "crd://material/draw_tri"

[[node]]
name   = "col"
op     = "geomcolor"
inputs = [0]

[[node]]
name   = "cr"
op     = "extract"
inputs = ["col", 0]

[[node]]
name   = "cg"
op     = "extract"
inputs = ["col", 1]

[[node]]
name   = "cb"
op     = "extract"
inputs = ["col", 2]

[[node]]
name   = "ca"
op     = "extract"
inputs = ["col", 3]

[[node]]
name   = "base"
op     = "combine3"
inputs = ["cr", "cg", "cb"]

[surface]
base_color = "base"
opacity    = "ca"
)";

inline constexpr const char* kDrawGridMat = R"(schema = 1
name   = "crd://material/draw_grid"

[[node]]
name   = "wp"
op     = "position"
inputs = [0]

[[node]]
name   = "cr"
op     = "normal"
inputs = [1]

[[node]]
name   = "cells"
op     = "texcoord"
inputs = [2]

[[node]]
name   = "colp"
op     = "geomcolor"
inputs = [3]

[[node]]
name   = "cols"
op     = "geomcolor"
inputs = [4]

[[node]]
name   = "colax"
op     = "geomcolor"
inputs = [5]

[[node]]
name   = "colaz"
op     = "geomcolor"
inputs = [6]

[[node]]
name   = "wx"
op     = "extract"
inputs = ["wp", 0]

[[node]]
name   = "wz"
op     = "extract"
inputs = ["wp", 2]

[[node]]
name   = "cellp"
op     = "extract"
inputs = ["cells", 0]

[[node]]
name   = "cellsz"
op     = "extract"
inputs = ["cells", 1]

[[node]]
name   = "upx"
op     = "divide"
inputs = ["wx", "cellp"]

[[node]]
name   = "upz"
op     = "divide"
inputs = ["wz", "cellp"]

[[node]]
name   = "pfx0"
op     = "subtract"
inputs = ["upx", 0.5]

[[node]]
name   = "pfx1"
op     = "modulo"
inputs = ["pfx0", 1.0]

[[node]]
name   = "pfx2"
op     = "subtract"
inputs = ["pfx1", 0.5]

[[node]]
name   = "pfx3"
op     = "absval"
inputs = ["pfx2"]

[[node]]
name   = "pfxw"
op     = "fwidth"
inputs = ["upx"]

[[node]]
name   = "plfx"
op     = "divide"
inputs = ["pfx3", "pfxw"]

[[node]]
name   = "pfz0"
op     = "subtract"
inputs = ["upz", 0.5]

[[node]]
name   = "pfz1"
op     = "modulo"
inputs = ["pfz0", 1.0]

[[node]]
name   = "pfz2"
op     = "subtract"
inputs = ["pfz1", 0.5]

[[node]]
name   = "pfz3"
op     = "absval"
inputs = ["pfz2"]

[[node]]
name   = "pfzw"
op     = "fwidth"
inputs = ["upz"]

[[node]]
name   = "plfz"
op     = "divide"
inputs = ["pfz3", "pfzw"]

[[node]]
name   = "pmin"
op     = "minimum"
inputs = ["plfx", "plfz"]

[[node]]
name   = "pminc"
op     = "clamp01"
inputs = ["pmin"]

[[node]]
name   = "pri"
op     = "subtract"
inputs = [1.0, "pminc"]

[[node]]
name   = "usx"
op     = "divide"
inputs = ["wx", "cellsz"]

[[node]]
name   = "usz"
op     = "divide"
inputs = ["wz", "cellsz"]

[[node]]
name   = "sfx0"
op     = "subtract"
inputs = ["usx", 0.5]

[[node]]
name   = "sfx1"
op     = "modulo"
inputs = ["sfx0", 1.0]

[[node]]
name   = "sfx2"
op     = "subtract"
inputs = ["sfx1", 0.5]

[[node]]
name   = "sfx3"
op     = "absval"
inputs = ["sfx2"]

[[node]]
name   = "sfxw"
op     = "fwidth"
inputs = ["usx"]

[[node]]
name   = "slfx"
op     = "divide"
inputs = ["sfx3", "sfxw"]

[[node]]
name   = "sfz0"
op     = "subtract"
inputs = ["usz", 0.5]

[[node]]
name   = "sfz1"
op     = "modulo"
inputs = ["sfz0", 1.0]

[[node]]
name   = "sfz2"
op     = "subtract"
inputs = ["sfz1", 0.5]

[[node]]
name   = "sfz3"
op     = "absval"
inputs = ["sfz2"]

[[node]]
name   = "sfzw"
op     = "fwidth"
inputs = ["usz"]

[[node]]
name   = "slfz"
op     = "divide"
inputs = ["sfz3", "sfzw"]

[[node]]
name   = "smin"
op     = "minimum"
inputs = ["slfx", "slfz"]

[[node]]
name   = "sminc"
op     = "clamp01"
inputs = ["smin"]

[[node]]
name   = "sec"
op     = "subtract"
inputs = [1.0, "sminc"]

[[node]]
name   = "crx"
op     = "extract"
inputs = ["cr", 0]

[[node]]
name   = "crz"
op     = "extract"
inputs = ["cr", 1]

[[node]]
name   = "halfd"
op     = "extract"
inputs = ["cr", 2]

[[node]]
name   = "d2a"
op     = "multiply"
inputs = ["crx", "crx"]

[[node]]
name   = "d2b"
op     = "multiply"
inputs = ["crz", "crz"]

[[node]]
name   = "d2"
op     = "add"
inputs = ["d2a", "d2b"]

[[node]]
name   = "dist"
op     = "sqrt"
inputs = ["d2"]

[[node]]
name   = "dr"
op     = "divide"
inputs = ["dist", "halfd"]

[[node]]
name   = "drc"
op     = "clamp01"
inputs = ["dr"]

[[node]]
name   = "f0"
op     = "subtract"
inputs = [1.0, "drc"]

[[node]]
name   = "fadef"
op     = "multiply"
inputs = ["f0", "f0"]

[[node]]
name   = "wxa"
op     = "absval"
inputs = ["wx"]

[[node]]
name   = "wxf"
op     = "fwidth"
inputs = ["wx"]

[[node]]
name   = "axr"
op     = "divide"
inputs = ["wxa", "wxf"]

[[node]]
name   = "axc"
op     = "clamp01"
inputs = ["axr"]

[[node]]
name   = "axf"
op     = "subtract"
inputs = [1.0, "axc"]

[[node]]
name   = "wza"
op     = "absval"
inputs = ["wz"]

[[node]]
name   = "wzf"
op     = "fwidth"
inputs = ["wz"]

[[node]]
name   = "azr"
op     = "divide"
inputs = ["wza", "wzf"]

[[node]]
name   = "azc"
op     = "clamp01"
inputs = ["azr"]

[[node]]
name   = "azf"
op     = "subtract"
inputs = [1.0, "azc"]

[[node]]
name   = "s0"
op     = "extract"
inputs = ["cols", 0]

[[node]]
name   = "p0"
op     = "extract"
inputs = ["colp", 0]

[[node]]
name   = "ss0"
op     = "multiply"
inputs = ["s0", "sec"]

[[node]]
name   = "dd0"
op     = "subtract"
inputs = ["p0", "ss0"]

[[node]]
name   = "dm0"
op     = "multiply"
inputs = ["dd0", "pri"]

[[node]]
name   = "v10"
op     = "add"
inputs = ["ss0", "dm0"]

[[node]]
name   = "ax0"
op     = "extract"
inputs = ["colax", 0]

[[node]]
name   = "aa0"
op     = "subtract"
inputs = ["ax0", "v10"]

[[node]]
name   = "ab0"
op     = "multiply"
inputs = ["aa0", "azf"]

[[node]]
name   = "v20"
op     = "add"
inputs = ["v10", "ab0"]

[[node]]
name   = "az0"
op     = "extract"
inputs = ["colaz", 0]

[[node]]
name   = "ba0"
op     = "subtract"
inputs = ["az0", "v20"]

[[node]]
name   = "bb0"
op     = "multiply"
inputs = ["ba0", "axf"]

[[node]]
name   = "v30"
op     = "add"
inputs = ["v20", "bb0"]

[[node]]
name   = "s1"
op     = "extract"
inputs = ["cols", 1]

[[node]]
name   = "p1"
op     = "extract"
inputs = ["colp", 1]

[[node]]
name   = "ss1"
op     = "multiply"
inputs = ["s1", "sec"]

[[node]]
name   = "dd1"
op     = "subtract"
inputs = ["p1", "ss1"]

[[node]]
name   = "dm1"
op     = "multiply"
inputs = ["dd1", "pri"]

[[node]]
name   = "v11"
op     = "add"
inputs = ["ss1", "dm1"]

[[node]]
name   = "ax1"
op     = "extract"
inputs = ["colax", 1]

[[node]]
name   = "aa1"
op     = "subtract"
inputs = ["ax1", "v11"]

[[node]]
name   = "ab1"
op     = "multiply"
inputs = ["aa1", "azf"]

[[node]]
name   = "v21"
op     = "add"
inputs = ["v11", "ab1"]

[[node]]
name   = "az1"
op     = "extract"
inputs = ["colaz", 1]

[[node]]
name   = "ba1"
op     = "subtract"
inputs = ["az1", "v21"]

[[node]]
name   = "bb1"
op     = "multiply"
inputs = ["ba1", "axf"]

[[node]]
name   = "v31"
op     = "add"
inputs = ["v21", "bb1"]

[[node]]
name   = "s2"
op     = "extract"
inputs = ["cols", 2]

[[node]]
name   = "p2"
op     = "extract"
inputs = ["colp", 2]

[[node]]
name   = "ss2"
op     = "multiply"
inputs = ["s2", "sec"]

[[node]]
name   = "dd2"
op     = "subtract"
inputs = ["p2", "ss2"]

[[node]]
name   = "dm2"
op     = "multiply"
inputs = ["dd2", "pri"]

[[node]]
name   = "v12"
op     = "add"
inputs = ["ss2", "dm2"]

[[node]]
name   = "ax2"
op     = "extract"
inputs = ["colax", 2]

[[node]]
name   = "aa2"
op     = "subtract"
inputs = ["ax2", "v12"]

[[node]]
name   = "ab2"
op     = "multiply"
inputs = ["aa2", "azf"]

[[node]]
name   = "v22"
op     = "add"
inputs = ["v12", "ab2"]

[[node]]
name   = "az2"
op     = "extract"
inputs = ["colaz", 2]

[[node]]
name   = "ba2"
op     = "subtract"
inputs = ["az2", "v22"]

[[node]]
name   = "bb2"
op     = "multiply"
inputs = ["ba2", "axf"]

[[node]]
name   = "v32"
op     = "add"
inputs = ["v22", "bb2"]

[[node]]
name   = "s3"
op     = "extract"
inputs = ["cols", 3]

[[node]]
name   = "p3"
op     = "extract"
inputs = ["colp", 3]

[[node]]
name   = "ss3"
op     = "multiply"
inputs = ["s3", "sec"]

[[node]]
name   = "dd3"
op     = "subtract"
inputs = ["p3", "ss3"]

[[node]]
name   = "dm3"
op     = "multiply"
inputs = ["dd3", "pri"]

[[node]]
name   = "v13"
op     = "add"
inputs = ["ss3", "dm3"]

[[node]]
name   = "ax3"
op     = "extract"
inputs = ["colax", 3]

[[node]]
name   = "aa3"
op     = "subtract"
inputs = ["ax3", "v13"]

[[node]]
name   = "ab3"
op     = "multiply"
inputs = ["aa3", "azf"]

[[node]]
name   = "v23"
op     = "add"
inputs = ["v13", "ab3"]

[[node]]
name   = "az3"
op     = "extract"
inputs = ["colaz", 3]

[[node]]
name   = "ba3"
op     = "subtract"
inputs = ["az3", "v23"]

[[node]]
name   = "bb3"
op     = "multiply"
inputs = ["ba3", "axf"]

[[node]]
name   = "v33"
op     = "add"
inputs = ["v23", "bb3"]

[[node]]
name   = "base"
op     = "combine3"
inputs = ["v30", "v31", "v32"]

[[node]]
name   = "alpha"
op     = "multiply"
inputs = ["v33", "fadef"]

[surface]
base_color = "base"
opacity    = "alpha"
)";

// The drift-gate seam: the SAME accessor shape the scene renderer's pack exposes. Names mirror the shipped
// paths ("vertex/draw_line.crdv", "material/draw_grid.crdm", ...). Returns false for a name the pack does not hold.
[[nodiscard]] inline bool builtin_draw_asset_text(const char* name, crd::containers::String& out)
{
    const crd::containers::StringView n(name);
    out.clear();
    const auto is = [&](const char* k) { return n == crd::containers::StringView(k); };
    if (is("vertex/draw_line.crdv")) { out.append(kDrawLineVs); return true; }
    if (is("vertex/draw_tri.crdv")) { out.append(kDrawTriVs); return true; }
    if (is("vertex/draw_grid.crdv")) { out.append(kDrawGridVs); return true; }
    if (is("material/draw_line.crdm")) { out.append(kDrawLineMat); return true; }
    if (is("material/draw_tri.crdm")) { out.append(kDrawTriMat); return true; }
    if (is("material/draw_grid.crdm")) { out.append(kDrawGridMat); return true; }
    return false;
}

} // namespace crd::draw
