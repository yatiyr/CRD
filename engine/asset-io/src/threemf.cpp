// threemf.cpp — GEO-5 pt 3 (D-007): the 3MF core-spec parser + model writer. See threemf.hpp for the contract.

#include <crd/assetio/threemf.hpp>

#include <crd/assetio/xml.hpp>
#include <crd/geometry/mesh/mesh_validate.hpp>

#include <crd/math/cmath.hpp> // crd::math::pow — the math mandate

#include <cmath> // std::isfinite — classification only (the gltf.cpp idiom)
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace crd::assetio
{

// the shared TRS extraction (gltf.cpp owns the definition — promoted for exactly this reuse)
[[nodiscard]] bool decompose_matrix_trs(const crd::f64 m[16], crd::math::Vec3<crd::f32>& t,
                                        crd::math::Vec4<crd::f32>& q, crd::math::Vec3<crd::f32>& s);

namespace
{

[[nodiscard]] crd::f64 unit_scale(const char* unit) noexcept
{
    if (unit == nullptr || std::strcmp(unit, "millimeter") == 0) { return 1.0e-3; } // the spec default
    if (std::strcmp(unit, "micron") == 0) { return 1.0e-6; }
    if (std::strcmp(unit, "centimeter") == 0) { return 1.0e-2; }
    if (std::strcmp(unit, "inch") == 0) { return 0.0254; }
    if (std::strcmp(unit, "foot") == 0) { return 0.3048; }
    if (std::strcmp(unit, "meter") == 0) { return 1.0; }
    return 0.0; // unknown unit — Malformed
}

// IEC 61966-2-1 sRGB byte → linear f32 (3MF displaycolor is sRGB; our material factors are linear — the same
// color-space contract the texture cook enforces)
[[nodiscard]] crd::f32 srgb_to_linear(crd::u8 v) noexcept
{
    const crd::f64 c = static_cast<crd::f64>(v) / 255.0;
    return static_cast<crd::f32>(c <= 0.04045 ? c / 12.92 : crd::math::pow((c + 0.055) / 1.055, 2.4));
}

[[nodiscard]] bool parse_f64(const char* s, crd::f64& out) noexcept
{
    if (s == nullptr) { return false; }
    char*          end = nullptr;
    const crd::f64 v   = std::strtod(s, &end);
    if (end == s || !std::isfinite(v)) { return false; }
    out = v;
    return true;
}
[[nodiscard]] bool parse_u32(const char* s, crd::u32& out) noexcept
{
    if (s == nullptr) { return false; }
    char*                   end = nullptr;
    const unsigned long long v  = std::strtoull(s, &end, 10);
    if (end == s || v > 0xFFFFFFFFULL) { return false; }
    out = static_cast<crd::u32>(v);
    return true;
}

[[nodiscard]] int hex_nibble(char c) noexcept
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return 10 + c - 'a'; }
    if (c >= 'A' && c <= 'F') { return 10 + c - 'A'; }
    return -1;
}

// qualified-name match ignoring any namespace prefix — extension elements arrive as "b:beamlattice"/"s:slicestack"
// under producer-chosen prefixes, and the XML layer keeps prefixes verbatim by design
[[nodiscard]] bool local_name_is(const char* qualified, const char* local) noexcept
{
    const char* colon = std::strchr(qualified, ':');
    return std::strcmp(colon != nullptr ? colon + 1 : qualified, local) == 0;
}

} // namespace

ImportStatus parse_3mf_model(crd::containers::ConstSpan<crd::u8> xml_bytes, crd::memory::IAllocator* alloc,
                             ImportedAsset& out)
{
    XmlDoc doc(alloc);
    const XmlError xe = doc.parse(xml_bytes);
    if (xe == XmlError::Truncated) { return ImportStatus::Truncated; }
    if (xe != XmlError::Ok) { return ImportStatus::Malformed; }

    const crd::i32 model = doc.root();
    if (std::strcmp(doc.name(model), "model") != 0) { return ImportStatus::NotRecognized; }

    // requiredextensions we cannot honor ⇒ Unsupported BY NAME (a print with ignored REQUIRED semantics is wrong)
    const char* required = doc.attr(model, "requiredextensions");
    if (required != nullptr && required[0] != '\0') { return ImportStatus::Unsupported; }

    const crd::f64 scale = unit_scale(doc.attr(model, "unit"));
    if (scale == 0.0) { return ImportStatus::Malformed; }

    const crd::i32 resources = doc.child(model, "resources");
    if (resources == kXmlInvalid) { return ImportStatus::Malformed; }

    // slice-extension AWARENESS: slice stacks are auxiliary print data riding beside real triangle geometry — the
    // meshes import complete, the slices are dropped, and the drop is WARNED (never silent, never a hard refusal;
    // a producer that truly requires them says so in requiredextensions, refused above)
    for (crd::i32 r = doc.first_child(resources); r != kXmlInvalid; r = doc.next(r))
    {
        if (!doc.is_text(r) && local_name_is(doc.name(r), "slicestack")) { ++out.warning_count; }
    }

    // ── basematerials → ImportedMaterials (a flat list across groups; (pid,pindex) resolves into it) ─────────────
    crd::containers::Array<crd::u32> mat_group_id(alloc);    // the <basematerials id>
    crd::containers::Array<crd::u32> mat_group_start(alloc); // first material index of that group
    for (crd::i32 bm = doc.child(resources, "basematerials"); bm != kXmlInvalid; bm = doc.sibling(bm, "basematerials"))
    {
        crd::u32 gid = 0;
        if (!parse_u32(doc.attr(bm, "id"), gid)) { return ImportStatus::Malformed; }
        mat_group_id.push_back(gid);
        mat_group_start.push_back(static_cast<crd::u32>(out.materials.size()));
        for (crd::i32 base = doc.child(bm, "base"); base != kXmlInvalid; base = doc.sibling(base, "base"))
        {
            ImportedMaterial m(alloc);
            const char*      nm = doc.attr(base, "name");
            if (nm != nullptr) { m.name.append(nm); }
            const char* dc = doc.attr(base, "displaycolor");
            if (dc != nullptr && dc[0] == '#')
            {
                const crd::usize len = std::strlen(dc);
                if (len != 7U && len != 9U) { return ImportStatus::Malformed; }
                crd::u8 ch[4] = {255U, 255U, 255U, 255U};
                for (crd::usize c = 0; c * 2U + 2U < len + 1U; ++c)
                {
                    const int hi = hex_nibble(dc[1U + c * 2U]);
                    const int lo = hex_nibble(dc[2U + c * 2U]);
                    if (hi < 0 || lo < 0) { return ImportStatus::Malformed; }
                    ch[c] = static_cast<crd::u8>(hi * 16 + lo);
                }
                m.base_color = {srgb_to_linear(ch[0]), srgb_to_linear(ch[1]), srgb_to_linear(ch[2])};
                m.base_alpha = static_cast<crd::f32>(ch[3]) / 255.0F; // alpha is linear coverage
            }
            m.roughness = 1.0F; // 3MF base materials are diffuse prints — honest defaults
            m.metallic  = 0.0F;
            out.materials.push_back(std::move(m));
        }
    }
    const auto resolve_material = [&](crd::u32 pid, crd::u32 pindex) -> crd::i32 {
        for (crd::usize g = 0; g < mat_group_id.size(); ++g)
        {
            if (mat_group_id[g] == pid)
            {
                const crd::u32 idx = mat_group_start[g] + pindex;
                return idx < out.materials.size() ? static_cast<crd::i32>(idx) : kXmlInvalid;
            }
        }
        return kXmlInvalid;
    };

    // ── objects → meshes (object id → mesh index, for build items + components) ──────────────────────────────────
    crd::containers::Array<crd::u32> object_id(alloc);
    crd::containers::Array<crd::i32> object_mesh(alloc); // -1 for non-mesh objects
    for (crd::i32 obj = doc.child(resources, "object"); obj != kXmlInvalid; obj = doc.sibling(obj, "object"))
    {
        crd::u32 oid = 0;
        if (!parse_u32(doc.attr(obj, "id"), oid)) { return ImportStatus::Malformed; }
        const char* type = doc.attr(obj, "type");
        if (type != nullptr && std::strcmp(type, "model") != 0 && std::strcmp(type, "solidsupport") != 0)
        {
            object_id.push_back(oid); // support/other objects exist but carry no imported mesh
            object_mesh.push_back(kXmlInvalid);
            continue;
        }
        const crd::i32 mesh = doc.child(obj, "mesh");
        if (mesh == kXmlInvalid)
        {
            object_id.push_back(oid);
            object_mesh.push_back(kXmlInvalid);
            continue; // components-only objects handled at build time
        }
        // beam-lattice AWARENESS: a <b:beamlattice> mesh is NOT triangle geometry — importing the bare vertex cloud
        // would silently drop the printed structure. Refused BY NAME, never misclassified as Malformed.
        for (crd::i32 mc = doc.first_child(mesh); mc != kXmlInvalid; mc = doc.next(mc))
        {
            if (!doc.is_text(mc) && local_name_is(doc.name(mc), "beamlattice")) { return ImportStatus::Unsupported; }
        }

        ImportedMesh im(alloc);
        const char*  oname = doc.attr(obj, "name");
        if (oname != nullptr) { im.name.append(oname); }

        const crd::i32 vertices = doc.child(mesh, "vertices");
        for (crd::i32 v = doc.child(vertices, "vertex"); v != kXmlInvalid; v = doc.sibling(v, "vertex"))
        {
            crd::f64 x = 0.0;
            crd::f64 y = 0.0;
            crd::f64 z = 0.0;
            if (!parse_f64(doc.attr(v, "x"), x) || !parse_f64(doc.attr(v, "y"), y) || !parse_f64(doc.attr(v, "z"), z))
            {
                return ImportStatus::Malformed;
            }
            im.positions.push_back({static_cast<crd::f32>(x * scale), static_cast<crd::f32>(y * scale),
                                    static_cast<crd::f32>(z * scale)});
        }
        // object-level material binding: pid + pindex (per-triangle pids ride the materials extension — core keeps
        // the object-level binding)
        crd::u32 pid    = 0;
        crd::u32 pindex = 0;
        if (parse_u32(doc.attr(obj, "pid"), pid))
        {
            (void)parse_u32(doc.attr(obj, "pindex"), pindex);
            im.material = resolve_material(pid, pindex);
        }
        const crd::i32 triangles = doc.child(mesh, "triangles");
        for (crd::i32 t = doc.child(triangles, "triangle"); t != kXmlInvalid; t = doc.sibling(t, "triangle"))
        {
            crd::u32 v1 = 0;
            crd::u32 v2 = 0;
            crd::u32 v3 = 0;
            if (!parse_u32(doc.attr(t, "v1"), v1) || !parse_u32(doc.attr(t, "v2"), v2)
                || !parse_u32(doc.attr(t, "v3"), v3))
            {
                return ImportStatus::Malformed;
            }
            const crd::u32 n = static_cast<crd::u32>(im.positions.size());
            if (v1 >= n || v2 >= n || v3 >= n) { return ImportStatus::Malformed; }
            im.indices.push_back(v1);
            im.indices.push_back(v2);
            im.indices.push_back(v3);
        }
        if (im.indices.size() == 0U) { return ImportStatus::Malformed; } // a mesh object must have triangles

        // the glTF fan-out convention: node.mesh names a LIBRARY mesh and each ImportedMesh points back at it via
        // source_mesh. 3MF objects are 1:1 with meshes, so the library index is the mesh's own index — without this
        // the scene decompose finds no primitives for the build item and the SCEN ships with no drawable.
        im.source_mesh = static_cast<crd::i32>(out.meshes.size());
        object_id.push_back(oid);
        object_mesh.push_back(static_cast<crd::i32>(out.meshes.size()));
        out.meshes.push_back(std::move(im));
    }

    const auto mesh_of = [&](crd::u32 oid) -> crd::i32 {
        for (crd::usize i = 0; i < object_id.size(); ++i)
        {
            if (object_id[i] == oid) { return object_mesh[i]; }
        }
        return kXmlInvalid - 1; // distinguish "unknown object id" (error) from "object with no mesh" (kXmlInvalid)
    };

    // ── build items → root nodes (transform = row-major 4×3 affine → the SHARED TRS decompose) ───────────────────
    const crd::i32 build = doc.child(model, "build");
    for (crd::i32 item = build == kXmlInvalid ? kXmlInvalid : doc.child(build, "item"); item != kXmlInvalid;
         item = doc.sibling(item, "item"))
    {
        crd::u32 oid = 0;
        if (!parse_u32(doc.attr(item, "objectid"), oid)) { return ImportStatus::Malformed; }
        const crd::i32 mesh_idx = mesh_of(oid);
        if (mesh_idx == kXmlInvalid - 1) { return ImportStatus::Malformed; } // an item must reference a real object

        ImportedNode node(alloc);
        node.mesh = mesh_idx; // kXmlInvalid (-1) for a meshless object — a grouping node
        const char* tf = doc.attr(item, "transform");
        if (tf != nullptr)
        {
            // 3MF: 12 values, ROW-major 4×3 (rows of the affine; the 4th column is implicit 0 0 0 1)
            crd::f64    v[12];
            const char* s = tf;
            for (int k = 0; k < 12; ++k)
            {
                char*          e  = nullptr;
                const crd::f64 pv = std::strtod(s, &e);
                if (e == s || !std::isfinite(pv)) { return ImportStatus::Malformed; }
                v[k] = pv;
                s    = e;
            }
            // → column-major 4×4 for the shared decompose: columns are the 3MF rows' transposition
            const crd::f64 m[16] = {v[0], v[1],  v[2],  0.0, v[3], v[4],  v[5],  0.0,
                                    v[6], v[7],  v[8],  0.0, v[9] * scale, v[10] * scale, v[11] * scale, 1.0};
            if (!decompose_matrix_trs(m, node.translation, node.rotation, node.scale)) { ++out.warning_count; }
        }
        out.nodes.push_back(std::move(node));
    }

    return ImportStatus::Ok;
}

const char* threemf_content_types_xml() noexcept
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
           "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
           "<Default Extension=\"model\" ContentType=\"application/vnd.ms-package.3dmanufacturing-3dmodel+xml\"/>"
           "</Types>";
}

const char* threemf_rels_xml() noexcept
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
           "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
           "<Relationship Target=\"/3D/3dmodel.model\" Id=\"rel0\" "
           "Type=\"http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel\"/>"
           "</Relationships>";
}

bool threemf_write_model_xml(crd::containers::ConstSpan<crd::u8> vertices48,
                             crd::containers::ConstSpan<crd::u8> indices_u32, const char* name,
                             crd::memory::IAllocator* alloc, crd::containers::String& out)
{
    out.clear();
    if (vertices48.size() == 0U || (vertices48.size() % 48U) != 0U) { return false; }
    if (indices_u32.size() == 0U || (indices_u32.size() % 12U) != 0U) { return false; } // whole triangles (3×u32)

    const crd::u32 vcount = static_cast<crd::u32>(vertices48.size() / 48U);
    const crd::u32 icount = static_cast<crd::u32>(indices_u32.size() / 4U);

    // rebuild the position/index views for the WATERTIGHT gate (slicers assume closed positive-volume solids)
    crd::containers::Array<crd::math::Vec3<crd::f32>> pos(alloc);
    for (crd::u32 v = 0; v < vcount; ++v)
    {
        crd::f32 f[3];
        std::memcpy(f, vertices48.data() + static_cast<crd::usize>(v) * 48U, 12U);
        pos.push_back({f[0], f[1], f[2]});
    }
    crd::containers::Array<crd::u32> idx(alloc);
    for (crd::u32 i = 0; i < icount; ++i)
    {
        crd::u32 iv = 0;
        std::memcpy(&iv, indices_u32.data() + static_cast<crd::usize>(i) * 4U, 4U);
        if (iv >= vcount) { return false; }
        idx.push_back(iv);
    }
    crd::geometry::mesh::TriangleMeshViewf view;
    view.vertices = crd::containers::ConstSpan<crd::math::Vec3<crd::f32>>(pos.data(), pos.size());
    view.indices  = crd::containers::ConstSpan<crd::u32>(idx.data(), idx.size());
    const auto report = crd::geometry::mesh::validate_triangle_mesh(view, alloc);
    if (!report.watertight) { return false; } // the gate: a holed/non-manifold solid never ships to a slicer

    // emit (SI metres → unit="meter"; %.9g round-trips every f32 exactly, the json_write discipline)
    out.append("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    out.append("<model unit=\"meter\" xml:lang=\"en-US\" "
               "xmlns=\"http://schemas.microsoft.com/3dmanufacturing/core/2015/02\">\n <resources>\n");
    out.append(R"(  <object id="1" type="model" name=")");
    out.append(name != nullptr ? name : "mesh");
    out.append("\">\n   <mesh>\n    <vertices>\n");
    char buf[128];
    for (crd::u32 v = 0; v < vcount; ++v)
    {
        std::snprintf(buf, sizeof(buf), "     <vertex x=\"%.9g\" y=\"%.9g\" z=\"%.9g\"/>\n",
                      static_cast<crd::f64>(pos[v].x), static_cast<crd::f64>(pos[v].y),
                      static_cast<crd::f64>(pos[v].z));
        out.append(buf);
    }
    out.append("    </vertices>\n    <triangles>\n");
    for (crd::u32 t = 0; t < icount / 3U; ++t)
    {
        std::snprintf(buf, sizeof(buf), "     <triangle v1=\"%u\" v2=\"%u\" v3=\"%u\"/>\n", idx[t * 3U],
                      idx[t * 3U + 1U], idx[t * 3U + 2U]);
        out.append(buf);
    }
    out.append("    </triangles>\n   </mesh>\n  </object>\n </resources>\n <build>\n  <item objectid=\"1\"/>\n"
               " </build>\n</model>\n");
    return true;
}

} // namespace crd::assetio
