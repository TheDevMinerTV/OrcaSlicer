#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"

#include "test_helpers.hpp"

#include <cctype>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinRel;

// Total area of a LayerRegion's slices in mm^2.
static double slices_area_mm2(const LayerRegion &layerm)
{
    double area = 0.;
    for (const Surface &surface : layerm.slices.surfaces)
        area += surface.expolygon.area();
    return area * SCALING_FACTOR * SCALING_FACTOR;
}

// The layer's regions split by whether they print with the shell material filament (id 2 here).
static std::pair<const LayerRegion *, const LayerRegion *> core_and_shell(const Layer &layer)
{
    const LayerRegion *core = nullptr, *shell = nullptr;
    for (const LayerRegion *layerm : layer.regions()) {
        if (layerm->region().config().outer_wall_filament_id == 2)
            shell = layerm;
        else
            core = layerm;
    }
    return { core, shell };
}

// 0-based tool indices used by extrusions whose role comment contains `role` (needs gcode_comments).
static std::set<int> tools_for_role(const std::string &gcode, const std::string &role)
{
    std::set<int> tools;
    int current_tool = 0;
    GCodeReader reader;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string cmd(line.cmd());
        if (cmd.size() >= 2 && cmd[0] == 'T' && std::isdigit((unsigned char) cmd[1]))
            current_tool = std::stoi(cmd.substr(1));
        else if (line.extruding(self) && std::string(line.comment()).find(role) != std::string::npos)
            tools.insert(current_tool);
    });
    return tools;
}

// A 20mm cube with a 2mm shell: the bottom/top 2mm print entirely as shell, mid layers
// split into a 2mm outer band (shell region) around a 16x16mm core region.
TEST_CASE("Shell material band splits layers into shell and core regions", "[ShellMaterial]")
{
    Print print;
    init_and_process_print({ cube(20) }, print,
        multifilament_config(2, {
            { "shell_material_filament_id",           2 },
            { "shell_material_thickness",             2. },
            { "shell_material_sparse_infill_density", 10 },
            { "layer_height",                         0.2 },
            { "initial_layer_print_height",           0.2 },
            { "elefant_foot_compensation",            0. },
            { "skirt_loops",                          0 },
            { "brim_type",                            "no_brim" },
        }));

    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.layers().empty());

    for (const Layer *layer : object.layers()) {
        const auto [core, shell] = core_and_shell(*layer);
        REQUIRE(core != nullptr);
        REQUIRE(shell != nullptr);

        const double core_area  = slices_area_mm2(*core);
        const double shell_area = slices_area_mm2(*shell);
        INFO("print_z " << layer->print_z);
        if (layer->bottom_z() < 2. - EPSILON || layer->print_z > 18. + EPSILON) {
            // Within 2mm of the object bottom/top: the whole cross section is shell.
            CHECK(core_area == 0.);
            CHECK_THAT(shell_area, WithinRel(400., 0.02));
        } else {
            // Mid height: 16x16 core, 2mm band around it.
            CHECK_THAT(core_area, WithinRel(256., 0.02));
            CHECK_THAT(shell_area, WithinRel(144., 0.02));
        }
    }

    // The infill overrides reached the shell region's config.
    const Layer &mid_layer = *object.layers()[object.layers().size() / 2];
    const auto [core, shell] = core_and_shell(mid_layer);
    CHECK(shell->region().config().sparse_infill_density == 10);
    CHECK(shell->region().config().sparse_infill_filament_id == 2);
    // The core keeps the object's filament (the "default" 0 resolves to the base filament 1).
    CHECK(core->region().config().sparse_infill_filament_id == 1);
}

TEST_CASE("Shell material is off by default", "[ShellMaterial]")
{
    Print print;
    init_and_process_print({ cube(20) }, print,
        multifilament_config(2, {
            { "layer_height",  0.2 },
            { "skirt_loops",   0 },
            { "brim_type",     "no_brim" },
        }));

    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.layers().empty());
    for (const Layer *layer : object.layers()) {
        REQUIRE(layer->regions().size() == 1);
        CHECK(slices_area_mm2(*layer->regions().front()) > 0.);
    }
}

// Tool index = filament id - 1: shell walls print on T1 while core walls stay on T0.
TEST_CASE("Shell and core print with their assigned filaments", "[ShellMaterial]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(2, {
            { "shell_material_filament_id", 2 },
            { "shell_material_thickness",   2. },
            { "layer_height",               0.2 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 0, 1 });
}

// A thickness larger than the part turns the entire object into shell material.
TEST_CASE("Shell material claims the whole object when thickness exceeds the part size", "[ShellMaterial]")
{
    const std::string gcode = slice({ cube(20) },
        multifilament_config(2, {
            { "shell_material_filament_id", 2 },
            { "shell_material_thickness",   12. },
            { "layer_height",               0.2 },
            { "skirt_loops",                0 },
            { "brim_type",                  "no_brim" },
        }));
    CHECK(tools_for_role(gcode, "perimeter") == std::set<int>{ 1 });
    CHECK(tools_for_role(gcode, "infill")    == std::set<int>{ 1 });
}

// Color painting with the shell filament scopes the shell to the painted area ("paint-on shell"):
// the painted band becomes shell material, the painted interior returns to the core material,
// and unpainted areas are not shelled at all.
TEST_CASE("Painting with the shell filament scopes the shell", "[ShellMaterial]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "shell_material_filament_id",           2 },
        { "shell_material_thickness",             2. },
        { "shell_material_sparse_infill_density", 10 },
        // In the paint-masked mode the painted core must merge into the parent region instead of a
        // core-side region: assert below that no region with this wall count holds any geometry.
        { "shell_material_interface_wall_loops",  5 },
        { "sparse_infill_density",                30 },
        { "layer_height",                         0.2 },
        { "initial_layer_print_height",           0.2 },
        { "elefant_foot_compensation",            0. },
        { "skirt_loops",                          0 },
        { "brim_type",                            "no_brim" },
        // The MMU segmentation reads the outer wall width; the config default of 0 ("auto") makes it throw.
        { "outer_wall_line_width",                0.42 },
    });
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Model        model;
    ModelObject *object = model.add_object();
    object->name = "cube";
    ModelVolume *volume = object->add_volume(cube(20));
    // Paint the +X face with filament 2 ("8" is the facet code of the 2nd filament):
    // the shell must appear only near that face.
    const indexed_triangle_set &its = volume->mesh().its;
    for (int facet_idx = 0; facet_idx < int(its.indices.size()); ++facet_idx) {
        const stl_triangle_vertex_indices &face = its.indices[facet_idx];
        const Vec3f normal = (its.vertices[face(1)] - its.vertices[face(0)]).cross(its.vertices[face(2)] - its.vertices[face(0)]).normalized();
        if (normal.x() > 0.9f)
            volume->mmu_segmentation_facets.set_triangle_from_string(facet_idx, "8");
    }
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    REQUIRE_FALSE(print_object.layers().empty());

    for (const Layer *layer : print_object.layers()) {
        if (layer->print_z < 4. || layer->print_z > 16.)
            continue; // Stay away from the top/bottom band and first-layer effects.
        double base_area = 0., painted_area = 0., shell_area = 0., core_region_area = 0.;
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &cfg = layerm->region().config();
            if (cfg.outer_wall_filament_id == 2 && cfg.sparse_infill_density == 10)
                shell_area += slices_area_mm2(*layerm);
            else if (cfg.outer_wall_filament_id == 2)
                painted_area += slices_area_mm2(*layerm);
            else if (cfg.wall_loops == 5)
                core_region_area += slices_area_mm2(*layerm);
            else
                base_area += slices_area_mm2(*layerm);
        }

        INFO("print_z " << layer->print_z);
        // All painted content is redistributed: its band into the shell region, its interior back to the core.
        CHECK(painted_area == 0.);
        // The painted core merges into the parent region; the interface-walls region must stay empty,
        // otherwise a needless double wall appears between the painted core and the untouched core.
        CHECK(core_region_area == 0.);
        // The shell exists, but covers only the band near the painted face — far less than the full
        // 144mm2 perimeter ring an unmasked shell would claim.
        CHECK(shell_area > 10.);
        CHECK(shell_area < 120.);
        CHECK_THAT(base_area + shell_area, WithinRel(400., 0.02));
    }
}

// With scope "whole_object", paint with the shell filament does not mask the shell: the band wraps
// the entire outline and the painted interior merges into the single continuous core.
TEST_CASE("Whole-object scope shells the entire outline despite paint", "[ShellMaterial]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "shell_material_filament_id",           2 },
        { "shell_material_thickness",             2. },
        { "shell_material_scope",                 "whole_object" },
        { "shell_material_sparse_infill_density", 10 },
        { "sparse_infill_density",                30 },
        { "layer_height",                         0.2 },
        { "initial_layer_print_height",           0.2 },
        { "elefant_foot_compensation",            0. },
        { "skirt_loops",                          0 },
        { "brim_type",                            "no_brim" },
        // The MMU segmentation reads the outer wall width; the config default of 0 ("auto") makes it throw.
        { "outer_wall_line_width",                0.42 },
    });
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Model        model;
    ModelObject *object = model.add_object();
    object->name = "cube";
    ModelVolume *volume = object->add_volume(cube(20));
    // Paint the +X face with filament 2; with whole-object scope it must NOT limit the shell.
    const indexed_triangle_set &its = volume->mesh().its;
    for (int facet_idx = 0; facet_idx < int(its.indices.size()); ++facet_idx) {
        const stl_triangle_vertex_indices &face = its.indices[facet_idx];
        const Vec3f normal = (its.vertices[face(1)] - its.vertices[face(0)]).cross(its.vertices[face(2)] - its.vertices[face(0)]).normalized();
        if (normal.x() > 0.9f)
            volume->mmu_segmentation_facets.set_triangle_from_string(facet_idx, "8");
    }
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    REQUIRE_FALSE(print_object.layers().empty());

    for (const Layer *layer : print_object.layers()) {
        if (layer->print_z < 4. || layer->print_z > 16.)
            continue;
        double      base_area = 0., painted_area = 0., shell_area = 0.;
        std::string breakdown;
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &cfg = layerm->region().config();
            breakdown += " region " + std::to_string(layerm->region().print_object_region_id()) +
                         " wall_filament " + std::to_string(cfg.outer_wall_filament_id.value) +
                         " density " + std::to_string(int(cfg.sparse_infill_density.value)) +
                         " area " + std::to_string(slices_area_mm2(*layerm)) + ";";
            if (cfg.outer_wall_filament_id == 2 && cfg.sparse_infill_density == 10)
                shell_area += slices_area_mm2(*layerm);
            else if (cfg.outer_wall_filament_id == 2)
                painted_area += slices_area_mm2(*layerm);
            else
                base_area += slices_area_mm2(*layerm);
        }

        INFO("print_z " << layer->print_z << breakdown);
        CHECK(painted_area == 0.);
        // Full 2mm perimeter ring in shell material, single 16x16 core in the base material.
        CHECK_THAT(shell_area, WithinRel(144., 0.02));
        CHECK_THAT(base_area, WithinRel(256., 0.02));
    }
}

// A modifier mesh carrying the shell settings scopes the shell material to part of the object
// (e.g. a sword whose blade gets a soft TPU skin while an attached piece stays plain).
TEST_CASE("Shell material can be scoped by a modifier mesh", "[ShellMaterial]")
{
    DynamicPrintConfig config = multifilament_config(2, {
        { "layer_height",               0.2 },
        { "initial_layer_print_height", 0.2 },
        { "elefant_foot_compensation",  0. },
        { "skirt_loops",                0 },
        { "brim_type",                  "no_brim" },
    });
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Model        model;
    ModelObject *object = model.add_object();
    object->name = "cube";
    object->add_volume(cube(20));
    // Modifier box covering the lower 10mm of the cube (with XY margin), carrying the shell settings.
    ModelVolume *modifier = object->add_volume(make_cube(24., 24., 10.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->translate(-2., -2., 0.);
    modifier->config.set_key_value("shell_material_filament_id", new ConfigOptionInt(2));
    modifier->config.set_key_value("shell_material_thickness", new ConfigOptionFloat(2.));
    object->add_instance();
    object->ensure_on_bed();

    Print print;
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    const PrintObject &print_object = *print.objects().front();
    REQUIRE_FALSE(print_object.layers().empty());

    for (const Layer *layer : print_object.layers()) {
        double base_area = 0., modifier_area = 0., shell_area = 0.;
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &cfg = layerm->region().config();
            if (cfg.outer_wall_filament_id == 2)
                shell_area += slices_area_mm2(*layerm);
            else if (cfg.shell_material_filament_id == 2)
                modifier_area += slices_area_mm2(*layerm);
            else
                base_area += slices_area_mm2(*layerm);
        }

        INFO("print_z " << layer->print_z);
        if (layer->print_z > 10.4) {
            // Above the modifier: no shell material at all, even in the top 2mm of the object.
            CHECK(shell_area == 0.);
            CHECK_THAT(base_area, WithinRel(400., 0.02));
        } else if (layer->bottom_z() < 2. - EPSILON) {
            // Bottom 2mm, inside the modifier: full shell.
            CHECK_THAT(shell_area, WithinRel(400., 0.02));
        } else if (layer->print_z < 9.6) {
            // Inside the modifier, away from the bottom: band + core.
            CHECK_THAT(shell_area, WithinRel(144., 0.02));
            CHECK_THAT(modifier_area, WithinRel(256., 0.02));
        }
    }
}

// With "shell on top and bottom" disabled, the band is a pure XY erosion: every layer keeps a core,
// including the ones within the shell thickness of the object's top and bottom faces.
TEST_CASE("Shell can be limited to the sides", "[ShellMaterial]")
{
    Print print;
    init_and_process_print({ cube(20) }, print,
        multifilament_config(2, {
            { "shell_material_filament_id",   2 },
            { "shell_material_thickness",     2. },
            { "shell_material_top_and_bottom", false },
            { "layer_height",                 0.2 },
            { "initial_layer_print_height",   0.2 },
            { "elefant_foot_compensation",    0. },
            { "skirt_loops",                  0 },
            { "brim_type",                    "no_brim" },
        }));

    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.layers().empty());
    for (const Layer *layer : object.layers()) {
        const auto [core, shell] = core_and_shell(*layer);
        REQUIRE(core != nullptr);
        REQUIRE(shell != nullptr);
        INFO("print_z " << layer->print_z);
        CHECK_THAT(slices_area_mm2(*core), WithinRel(256., 0.02));
        CHECK_THAT(slices_area_mm2(*shell), WithinRel(144., 0.02));
    }
}

// Wall count overrides: the shell's outer surface prints shell_material_wall_loops via a dedicated
// wall strip region, while both sides of the material interface print shell_material_interface_wall_loops
// (the shell's inner region and a dedicated core-side region).
TEST_CASE("Shell and interface wall counts are adjustable", "[ShellMaterial]")
{
    Print print;
    init_and_process_print({ cube(20) }, print,
        multifilament_config(2, {
            { "shell_material_filament_id",         2 },
            { "shell_material_thickness",           2. },
            { "shell_material_wall_loops",          3 },
            { "shell_material_interface_wall_loops", 0 },
            { "wall_loops",                         2 },
            { "layer_height",                       0.2 },
            { "initial_layer_print_height",         0.2 },
            { "elefant_foot_compensation",          0. },
            { "skirt_loops",                        0 },
            { "brim_type",                          "no_brim" },
        }));

    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.layers().empty());

    // The outer wall strip holds the 3 surface walls; with the default nozzle diameter of 0.4mm and
    // "auto" line widths it is 3 x 0.4 = 1.2mm wide: a 20x20 ring of 400 - 17.6^2 = 90.24mm2.
    const double strip_area = 400. - 17.6 * 17.6;

    for (const Layer *layer : object.layers()) {
        const LayerRegion *parent = nullptr, *core = nullptr, *shell_inner = nullptr, *shell_surface = nullptr;
        for (const LayerRegion *layerm : layer->regions()) {
            const PrintRegionConfig &cfg = layerm->region().config();
            if (cfg.outer_wall_filament_id == 2 && cfg.wall_loops == 3)
                shell_surface = layerm;
            else if (cfg.outer_wall_filament_id == 2)
                shell_inner = layerm;
            else if (cfg.wall_loops == 0)
                core = layerm;
            else
                parent = layerm;
        }
        REQUIRE(parent != nullptr);
        REQUIRE(core != nullptr);
        REQUIRE(shell_inner != nullptr);
        REQUIRE(shell_surface != nullptr);
        // The interface wall count applies to the shell's side of the interface as well.
        CHECK(shell_inner->region().config().wall_loops == 0);

        INFO("print_z " << layer->print_z);
        // The parent region is fully replaced by the shell band and the core-side region.
        CHECK(slices_area_mm2(*parent) == 0.);
        CHECK_THAT(slices_area_mm2(*shell_surface), WithinRel(strip_area, 0.02));
        if (layer->bottom_z() < 2. - EPSILON || layer->print_z > 18. + EPSILON) {
            CHECK(slices_area_mm2(*core) == 0.);
            CHECK_THAT(slices_area_mm2(*shell_inner), WithinRel(400. - strip_area, 0.02));
        } else {
            CHECK_THAT(slices_area_mm2(*core), WithinRel(256., 0.02));
            CHECK_THAT(slices_area_mm2(*shell_inner), WithinRel(144. - strip_area, 0.03));
        }
    }
}
