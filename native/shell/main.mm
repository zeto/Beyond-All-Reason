#include "anim/unit_anim.h"
#include "archive.h"
#include "bookmarks.h"
#include "build/nanoframe.h"
#include "build/reclaim.h"
#include "build/death_wreck.h"
#include "weapon/beam.h"
#include "weapon/cannon.h"
#include "weapon/flame.h"
#include "weapon/torpedo.h"
#include "weapon/bomb.h"
#include "weapon/dgun.h"
#include "los/los.h"
#include "weapon/laser.h"
#include "weapon/lightning.h"
#include "weapon/missile.h"
#include "camera.h"
#include "autocolors.h"
#include "grass.h"
#include "command/orders.h"
#include "economy/energy_yield.h"
#include "economy/metal_extract.h"
#include "economy/team_resources.h"
#include "game_name.h"
#include "gadget_host.h"
#include "widget_host.h"
#include "selection.h"
#include "checklist.h"
#include "build_cmd.h"
#include "minimap.h"
#include "hud_status.h"
#include "unitdef.h"
#include "los/los_map.h"
#include "move/unit_sim.h"
#include "smf.h"
#include "unit_spawn.h"
#include "startscript.h"
#include "terrain_renderer.h"
#include "triangle_renderer.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>

extern "C" {
void* objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void* pool);
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <algorithm>
#include <thread>
#include <fstream>
#include <sstream>
#include <iostream>
#include <string>

@interface BarApp : NSObject <NSApplicationDelegate, NSWindowDelegate>
@property(nonatomic, assign) BOOL running;
@end

namespace {

struct Options {
    std::string game_root = BAR_GAME_ROOT;
    std::string shader_path = BAR_SHADER_PATH;
    std::string terrain_shader = BAR_TERRAIN_SHADER;
    std::string screenshot;
    std::string screenshot_dir;
    std::string start_script;
    std::string map_dir;
    std::string bookmarks;
    std::string trace;
    std::string orders;
    std::string input;
    // Scripted mouse clicks routed like live input (widgets first, then placement / selection / orders).
    std::string ui_input;
    std::string pieces;
    bool lock_camera = false;
    // Skip Map Grass GL4 (A/B timing of the grass pass).
    bool no_grass = false;
    // --grass-path=mesh|vertex: object/mesh shader path (default) or the CPU-culled instanced draws.
    bool grass_mesh = true;
    // --gpu-timing FILE.csv: per-frame GPU ms (commit feedback) and CPU encode ms (waits excluded).
    std::string gpu_timing;
    // The LOS view (gfx_los_view / gfx_los_colors) is on by default, as in BAR.
    bool no_los_view = false;
    // FILE#ID: with --lock-camera, hold that bookmark camera instead of following unit 0.
    std::string fixed_camera;
    bool check_hud_text = false;
    int frames = 0;
};

void usage() {
    std::cerr << "bar-metal [--gamedata DIR] [--frames N] [--screenshot FILE.ppm]\n"
              << "          [--startscript FILE] [--mapdir DIR]\n"
              << "          [--bookmarks FILE] [--screenshot-dir DIR] [--trace FILE]\n"
              << "          [--orders FILE] [--input FILE] [--pieces FILE] [--lock-camera]\n"
              << "          [--check-hud-text]\n";
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need = [&](const char* flag) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << flag << "\n";
                return nullptr;
            }
            return argv[++i];
        };
        if (arg == "--gamedata") {
            const char* value = need("--gamedata");
            if (value == nullptr) {
                return false;
            }
            options.game_root = value;
        } else if (arg == "--frames") {
            const char* value = need("--frames");
            if (value == nullptr) {
                return false;
            }
            options.frames = std::stoi(value);
        } else if (arg == "--screenshot") {
            const char* value = need("--screenshot");
            if (value == nullptr) {
                return false;
            }
            options.screenshot = value;
        } else if (arg == "--screenshot-dir") {
            const char* value = need("--screenshot-dir");
            if (value == nullptr) {
                return false;
            }
            options.screenshot_dir = value;
        } else if (arg == "--startscript") {
            const char* value = need("--startscript");
            if (value == nullptr) {
                return false;
            }
            options.start_script = value;
        } else if (arg == "--mapdir") {
            const char* value = need("--mapdir");
            if (value == nullptr) {
                return false;
            }
            options.map_dir = value;
        } else if (arg == "--bookmarks") {
            const char* value = need("--bookmarks");
            if (value == nullptr) {
                return false;
            }
            options.bookmarks = value;
        } else if (arg == "--trace") {
            const char* value = need("--trace");
            if (value == nullptr) {
                return false;
            }
            options.trace = value;
        } else if (arg == "--orders") {
            const char* value = need("--orders");
            if (value == nullptr) {
                return false;
            }
            options.orders = value;
        } else if (arg == "--input") {
            const char* value = need("--input");
            if (value == nullptr) {
                return false;
            }
            options.input = value;
        } else if (arg == "--ui-input") {
            const char* value = need("--ui-input");
            if (value == nullptr) {
                return false;
            }
            options.ui_input = value;
        } else if (arg == "--pieces") {
            const char* value = need("--pieces");
            if (value == nullptr) {
                return false;
            }
            options.pieces = value;
        } else if (arg == "--fixed-camera") {
            const char* value = need("--fixed-camera");
            if (value == nullptr) {
                return false;
            }
            options.fixed_camera = value;
        } else if (arg == "--grass-path=vertex") {
            options.grass_mesh = false;
        } else if (arg == "--grass-path=mesh") {
            options.grass_mesh = true;
        } else if (arg == "--gpu-timing") {
            const char* value = need("--gpu-timing");
            if (value == nullptr) {
                return false;
            }
            options.gpu_timing = value;
        } else if (arg == "--no-grass") {
            options.no_grass = true;
        } else if (arg == "--no-los-view") {
            options.no_los_view = true;
        } else if (arg == "--lock-camera") {
            options.lock_camera = true;
        } else if (arg == "--check-hud-text") {
            options.check_hud_text = true;
        } else if (arg == "--help") {
            usage();
            return false;
        } else {
            std::cerr << "unknown argument " << arg << "\n";
            usage();
            return false;
        }
    }
    return true;
}

std::string resolve_map_dir(const Options& options) {
    if (!options.map_dir.empty()) {
        return options.map_dir;
    }
    if (const char* data = std::getenv("BAR_DATA")) {
        return std::string(data) + "/maps";
    }
    const char* home = std::getenv("HOME");
    std::string fallback = std::string(home != nullptr ? home : "") +
                           "/Library/Application Support/Beyond-All-Reason-mac/maps";
    return fallback;
}

void pump_events() {
    NSEvent* event = nil;
    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate date]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
        [NSApp sendEvent:event];
    }
}

struct FrameInput {
    float mouse_x = 0;
    float mouse_y = 0;
    float delta_x = 0;
    float delta_y = 0;
    float wheel = 0;
    bool order_click = false;
    float order_x = 0;
    float order_y = 0;
    bool left_down = false;
    bool left_up = false;
    float left_x = 0;
    float left_y = 0;
};

FrameInput collect_input(NSWindow* window, NSView* view) {
    FrameInput input;
    static bool right_down = false;
    static NSPoint right_at = NSZeroPoint;
    static bool left_down = false;
    NSPoint screen = [NSEvent mouseLocation];
    NSPoint local = [view convertPoint:[window convertPointFromScreen:screen] fromView:nil];
    input.mouse_x = static_cast<float>(local.x);
    input.mouse_y = static_cast<float>(view.bounds.size.height - local.y);
    NSEvent* event = nil;
    while ((event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate date]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
        if (event.type == NSEventTypeLeftMouseDown) {
            left_down = true;
            const NSPoint at = [view convertPoint:event.locationInWindow fromView:nil];
            input.left_down = true;
            input.left_x = static_cast<float>(at.x);
            input.left_y = static_cast<float>(view.bounds.size.height - at.y);
        } else if (event.type == NSEventTypeLeftMouseUp && left_down) {
            left_down = false;
            const NSPoint at = [view convertPoint:event.locationInWindow fromView:nil];
            input.left_up = true;
            input.left_x = static_cast<float>(at.x);
            input.left_y = static_cast<float>(view.bounds.size.height - at.y);
        } else if (event.type == NSEventTypeScrollWheel) {
            float wheel = static_cast<float>(event.scrollingDeltaY);
            if (event.hasPreciseScrollingDeltas) {
                wheel *= 0.02f;
            }
            input.wheel += wheel;
        } else if (event.type == NSEventTypeMouseMoved || event.type == NSEventTypeLeftMouseDragged ||
                   event.type == NSEventTypeRightMouseDragged || event.type == NSEventTypeOtherMouseDragged) {
            input.delta_x += static_cast<float>(event.deltaX);
            input.delta_y += static_cast<float>(event.deltaY);
        } else if (event.type == NSEventTypeRightMouseDown) {
            right_down = true;
            right_at = event.locationInWindow;
        } else if (event.type == NSEventTypeKeyDown && event.keyCode == 53) {
            ((BarApp*)NSApp.delegate).running = NO;
        } else if (event.type == NSEventTypeRightMouseUp && right_down) {
            right_down = false;
            const NSPoint up = event.locationInWindow;
            const float dx = static_cast<float>(up.x - right_at.x);
            const float dy = static_cast<float>(up.y - right_at.y);
            if (dx * dx + dy * dy < 36.f) {
                const NSPoint local = [view convertPoint:up fromView:nil];
                input.order_click = true;
                input.order_x = static_cast<float>(local.x);
                input.order_y = static_cast<float>(view.bounds.size.height - local.y);
            }
        }
        [NSApp sendEvent:event];
    }
    return input;
}

// --ui-input line: FRAME press|release|rclick X Y [shift]. X/Y are drawable pixels, y up (Spring).
struct UiEvent {
    int frame = 0;
    std::string kind;
    float x = 0;
    float y = 0;
    bool shift = false;
};

bool load_ui_events(const std::string& path, std::vector<UiEvent>& events, std::string& error) {
    std::ifstream in(path);
    if (!in) {
        error = "could not read ui input " + path;
        return false;
    }
    std::string line;
    int lineno = 0;
    while (std::getline(in, line)) {
        ++lineno;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        std::istringstream row(line);
        UiEvent event;
        if (!(row >> event.frame >> event.kind >> event.x >> event.y) ||
            (event.kind != "press" && event.kind != "release" && event.kind != "rclick")) {
            error = "bad ui input line " + std::to_string(lineno);
            return false;
        }
        std::string mod;
        event.shift = (row >> mod) && mod == "shift";
        events.push_back(event);
    }
    return true;
}

struct CocoaWindow {
    NSWindow* window = nil;
    NSView* view = nil;
    CAMetalLayer* layer = nil;
};

// Pops before the renderer and window, which are declared first. Convenience constructors
// autorelease into this pool; those objects must still be owned when the pool drains.
struct AutoreleaseDrain {
    AutoreleaseDrain() { token = objc_autoreleasePoolPush(); }
    ~AutoreleaseDrain() { objc_autoreleasePoolPop(token); }
    AutoreleaseDrain(const AutoreleaseDrain&) = delete;
    AutoreleaseDrain& operator=(const AutoreleaseDrain&) = delete;
    void* token = nullptr;
};

CocoaWindow make_window(const std::string& title, int width, int height) {
    CocoaWindow pack;
    NSRect frame = NSMakeRect(0, 0, width, height);
    pack.window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    pack.window.title = [NSString stringWithUTF8String:title.c_str()];
    // Default is YES, which frees the window on close. We own it until run_terrain returns.
    pack.window.releasedWhenClosed = NO;
    pack.view = pack.window.contentView;
    pack.view.wantsLayer = YES;
    pack.layer = [CAMetalLayer layer];
    pack.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    pack.layer.opaque = YES;
    // Checks present without waiting for the refresh. The watched window turns this back on.
    pack.layer.displaySyncEnabled = NO;
    pack.view.layer = pack.layer;
    pack.layer.frame = pack.view.bounds;
    pack.window.delegate = (id<NSWindowDelegate>)NSApp.delegate;
    [pack.window center];
    [pack.window makeKeyAndOrderFront:nil];
    return pack;
}

int run_triangle(const Options& options, const std::string& game_name) {
    std::string error;
    bar::TriangleRenderer renderer;
    CocoaWindow cocoa;
    AutoreleaseDrain drain;
    if (!renderer.init(options.shader_path, error)) {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }
    cocoa = make_window(game_name, 960, 540);
    NSWindow* window = cocoa.window;
    NSView* view = cocoa.view;
    CAMetalLayer* layer = cocoa.layer;
    const int frame_limit = options.frames > 0 ? options.frames : 1000000;
    int presented = 0;
    while (presented < frame_limit && ((BarApp*)NSApp.delegate).running) {
        @autoreleasepool {
            pump_events();
            if (!((BarApp*)NSApp.delegate).running) {
                break;
            }
            const CGFloat scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1;
            layer.contentsScale = scale;
            layer.drawableSize = CGSizeMake(view.bounds.size.width * scale, view.bounds.size.height * scale);
            if (layer.drawableSize.width < 1 || layer.drawableSize.height < 1) {
                continue;
            }
            bar::FrameRequest request;
            request.metal_layer = (__bridge void*)layer;
            if (!options.screenshot.empty() && presented == 0) {
                request.screenshot_ppm = options.screenshot.c_str();
            }
            if (!renderer.draw(request, error)) {
                std::cerr << "draw: " << error << "\n";
                return 1;
            }
            ++presented;
            std::cout << "frame " << presented << " presented\n";
        }
    }
    std::cout << "presented " << presented << "\n";
    if (options.frames > 0 && presented < options.frames) {
        std::cerr << "stopped after " << presented << " of " << options.frames << " frames\n";
        return 1;
    }
    return 0;
}

int run_terrain(const Options& options, const std::string& game_name) {
    std::string error;
    if (!bar::camera_frame_ok(error)) {
        std::cerr << "camera: " << error << "\n";
        return 1;
    }
    if (!bar::move_frame_ok(error)) {
        std::cerr << "move: " << error << "\n";
        return 1;
    }

    bar::StartScript script;
    if (!bar::parse_start_script(options.start_script, script, error)) {
        std::cerr << "startscript: " << error << "\n";
        return 1;
    }
    {
        std::string note;
        bar::apply_bar_autocolors(script, note);
        std::cout << note << "\n";
        if (!script.teams.empty()) {
            bar::set_ui_team_color(script.teams[0].color_r, script.teams[0].color_g, script.teams[0].color_b);
        }
    }
    const std::string map_dir = resolve_map_dir(options);
    std::cout << "mapdir: " << map_dir << "\n";
    std::string archive;
    if (!bar::find_map_archive(map_dir, script.map_name, archive, error)) {
        std::cerr << "map: " << error << "\n";
        return 1;
    }
    std::cout << "map: " << script.map_name << "\n";
    std::cout << "archive: " << archive << "\n";

    bar::TerrainMesh mesh;
    if (!bar::load_terrain(archive, mesh, error)) {
        std::cerr << "terrain: " << error << "\n";
        return 1;
    }
    mesh.game_root = options.game_root;
    std::cout << "world: " << mesh.world_x << " x " << mesh.world_z << "\n";
    std::cout << "heights: " << mesh.min_height << " .. " << mesh.max_height
              << (mesh.height_from_mapinfo ? " (mapinfo)" : " (smf header)") << "\n";
    std::cout << "center height: " << mesh.center_height << "\n";
    std::cout << "corner height: " << mesh.corner_height << "\n";
    std::cout << "mesh: " << mesh.vertices.size() << " verts, " << mesh.indices.size() << " indices\n";
    if (mesh.metal_map) {
        std::cout << "metal spots: metal map, rings off\n";
    } else {
        std::cout << "metal spots: " << mesh.metal_spot_count << "\n";
    }
    std::cout << "max metal: " << mesh.max_metal << " radius " << mesh.extractor_radius << "\n";
    std::cout << "wind: " << mesh.min_wind << " " << mesh.max_wind << "\n";

    bar::TerrainRenderer renderer;
    CocoaWindow cocoa;
    AutoreleaseDrain drain;
    // The interactive window is the one a person watches. Checks pass bookmarks,
    // screenshots, a locked camera, or a frame count, and those stay free of overlays.
    const bool user_window = options.bookmarks.empty() && options.screenshot.empty() &&
                             options.screenshot_dir.empty() && !options.lock_camera && options.frames <= 0;
    if (user_window) {
        // Apple reads this when Metal starts. It is the performance overlay: FPS, GPU time, frame pacing.
        setenv("MTL_HUD_ENABLED", "1", 1);
    }
    if (!renderer.init(options.terrain_shader, mesh, error)) {
        std::cerr << "renderer: " << error << "\n";
        return 1;
    }
    renderer.set_metal_hud(user_window && !mesh.metal_map);
    renderer.set_grass_path(options.grass_mesh ? bar::GrassPath::Mesh : bar::GrassPath::Vertex);
    std::cout << "grass path: " << (options.grass_mesh ? "mesh" : "vertex") << "\n";
    renderer.set_gpu_timing(!options.gpu_timing.empty());
    std::vector<bar::GpuFrameTiming> gpu_timings;
    std::cout << "spot rings: " << ((user_window && !mesh.metal_map) ? "on" : "off") << "\n";

    std::vector<bar::UnitSpawn> units;
    bar::ModelMesh model;
    float def_speed = 0.f;
    if (!script.teams.empty()) {
        const std::filesystem::path data_dir = std::filesystem::path(map_dir).parent_path();
        std::cout << "content: " << data_dir.string() << "\n";
        if (!bar::spawn_commanders(options.game_root, data_dir.string(), script, mesh, units, model, error)) {
            std::cerr << "spawn: " << error << "\n";
            return 1;
        }
        {
            bar::GrassData grass;
            std::string grass_error;
            if (options.no_grass) {
                std::cerr << "grass: disabled by --no-grass\n";
            } else if (bar::load_grass(mesh, grass, grass_error)) {
                if (!renderer.set_grass(grass, grass_error)) {
                    std::cerr << grass_error << "\n";
                }
            } else {
                std::cerr << grass_error << "\n";
            }
        }
        const bar::UnitSpawn& lead = units.front();
        if (!renderer.set_model(model, lead.color_r, lead.color_g, lead.color_b, error)) {
            std::cerr << "model: " << error << "\n";
            return 1;
        }
        std::cout << "model: " << model.vertices.size() << " verts, " << model.indices.size() << " indices, tex "
                  << model.tex_size << "\n";
        {
            bar::UnitBuild cost;
            std::string cost_error;
            if (bar::load_unit_build(options.game_root, lead.def_name, cost, cost_error)) {
                // UnitDef power: metal cost plus energy cost / 60.
                bar::hud_status_set_unit(model.s3o_height, model.s3o_radius,
                                         cost.metal_cost + cost.energy_cost / 60.f, cost.health);
            }
        }
        for (const bar::UnitSpawn& unit : units) {
            std::cout << "spawn team " << unit.team << " " << unit.def_name << " at " << unit.x << " " << unit.y
                      << " " << unit.z << "\n";
        }
    }
    bar::UnitSim sim;
    for (const bar::UnitSpawn& unit : units) {
        bar::UnitMotion motion;
        if (!bar::load_unit_motion(options.game_root, unit.def_name, motion, error)) {
            std::cerr << "motion: " << error << "\n";
            return 1;
        }
        float slope = 20.f;
        float water = 35.f;
        float min_water = -1.0e6f;
        bool hover = false;
        if (!motion.move_class.empty()) {
            bar::MoveClass move_class;
            if (!bar::load_move_class(options.game_root, motion.move_class, move_class, error)) {
                std::cerr << "movedefs: " << error << "\n";
                return 1;
            }
            slope = move_class.max_slope_deg;
            water = move_class.max_water_depth;
            min_water = move_class.min_water_depth;
            hover = move_class.hover;
        }
        const float cruise = motion.can_fly ? motion.cruise_altitude : 0.f;
        const float ground = bar::terrain_height_at(mesh, unit.x, unit.z);
        if (!bar::sim_add_unit(sim, unit, motion, slope, water, min_water, ground, hover, cruise, error)) {
            std::cerr << "sim: " << error << "\n";
            return 1;
        }
        {
            float hp = 0.f;
            std::string hp_error;
            if (bar::load_unit_health(options.game_root, unit.def_name, hp, hp_error) && hp > 0.f) {
                const size_t at = sim.id.size() - 1;
                sim.health[at] = hp;
                sim.max_health[at] = hp;
            }
        }
        // The lead unit owns the script host. A later building with speed 0 must not replace it.
        if (sim.id.size() == 1) {
            def_speed = motion.speed_per_sec;
        }
        std::cout << "motion " << unit.def_name << " " << motion.speed_per_sec << " elmos/s slope " << slope
                  << " water " << water << " min " << min_water << " hover " << (hover ? 1 : 0) << " cruise "
                  << cruise << " plane " << ((motion.can_fly && !motion.hover_attack) ? 1 : 0) << "\n";
    }
    std::vector<bar::TeamResources> team_rows;
    bar::ResourceRules rules;
    bar::MetalClaims claims;
    bool have_claims = false;
    float wind_strength = 0.f;
    bar::GadgetReport gadget_report;
    std::vector<bar::MexTrace> mex_claims;
    std::vector<bar::GenTrace> generators;
    bool have_resources = false;
    if (!units.empty()) {
        if (!bar::load_resource_rules(options.game_root, script, rules, error)) {
            std::cerr << "resources: " << error << "\n";
            return 1;
        }
        auto row_for = [&](int team) -> bar::TeamResources* {
            for (bar::TeamResources& row : team_rows) {
                if (row.team == team) {
                    return &row;
                }
            }
            return nullptr;
        };
        for (const bar::TeamStart& team : script.teams) {
            const bar::UnitSpawn* lead = nullptr;
            for (const bar::UnitSpawn& unit : units) {
                if (unit.team == team.team) {
                    lead = &unit;
                    break;
                }
            }
            if (lead == nullptr) {
                continue;
            }
            bar::UnitYield yield;
            if (!bar::load_unit_yield(options.game_root, lead->def_name, yield, error)) {
                std::cerr << "yield: " << error << "\n";
                return 1;
            }
            bar::TeamResources row;
            if (!bar::team_resources_open(row, team.team, rules, yield, error)) {
                std::cerr << "resources: " << error << "\n";
                return 1;
            }
            team_rows.push_back(row);
        }
        have_resources = !team_rows.empty();
        if (!bar::metal_claims_open(claims, mesh, error)) {
            std::cerr << "mex: " << error << "\n";
            return 1;
        }
        have_claims = true;
        for (const bar::UnitSpawn& unit : units) {
            bar::TeamResources* row = row_for(unit.team);
            if (row == nullptr) {
                continue;
            }
            bar::UnitExtractor extractor;
            if (!bar::load_unit_extractor(options.game_root, unit.def_name, extractor, error)) {
                std::cerr << "mex: " << error << "\n";
                return 1;
            }
            if (!extractor.active || !(extractor.depth > 0.f)) {
                continue;
            }
            float depth = extractor.depth;
            if (extractor.metal_extractor > 0.f) {
                depth *= rules.metal_scale;
            }
            const bar::MexTrace claim = bar::metal_claim(claims, mesh, unit.def_name, unit.x, unit.z, depth);
            mex_claims.push_back(claim);
            row->mex_income += claim.rate;
            row->energy_upkeep += extractor.energy_upkeep;
            std::cout << "mex " << claim.def_name << " at " << claim.x << " " << claim.z << " depth " << claim.depth
                      << " raw " << claim.raw << " rate " << claim.rate << " upkeep " << extractor.energy_upkeep
                      << "\n";
        }
        wind_strength = bar::wind_cycle_strength(mesh.min_wind, mesh.max_wind);
        for (const bar::UnitSpawn& unit : units) {
            bar::TeamResources* row = row_for(unit.team);
            if (row == nullptr) {
                continue;
            }
            bar::UnitPower power;
            if (!bar::load_unit_power(options.game_root, unit.def_name, power, error)) {
                std::cerr << "power: " << error << "\n";
                return 1;
            }
            if (!power.active) {
                continue;
            }
            bar::GenTrace gen;
            gen.def_name = unit.def_name;
            gen.x = unit.x;
            gen.z = unit.z;
            gen.upkeep = power.energy_upkeep;
            gen.wind = wind_strength;
            bool produces = false;
            if (power.energy_upkeep < 0.f) {
                gen.production = -power.energy_upkeep * rules.energy_scale;
                row->solar_income += gen.production;
                produces = true;
                std::cout << "solar " << unit.def_name << " at " << unit.x << " " << unit.z << " upkeep "
                          << power.energy_upkeep << " make " << gen.production << "\n";
            }
            if (power.wind_generator > 0.f) {
                gen.wind_cap = power.wind_generator * rules.energy_scale;
                gen.rate = bar::capped_wind_energy(wind_strength, gen.wind_cap);
                row->wind_income += gen.rate;
                produces = true;
                std::cout << "wind " << unit.def_name << " at " << unit.x << " " << unit.z << " cap " << gen.wind_cap
                          << " strength " << wind_strength << " rate " << gen.rate << "\n";
            }
            if (produces) {
                generators.push_back(gen);
            }
        }
        for (const bar::TeamResources& row : team_rows) {
            std::cout << "resources team " << row.team << " metal " << row.metal << " energy " << row.energy
                      << " store " << row.metal_storage << " " << row.energy_storage << " income " << row.metal_income
                      << " " << row.energy_income << " mex " << row.mex_income << " upkeep " << row.energy_upkeep
                      << " solar " << row.solar_income << " wind " << row.wind_income << "\n";
        }
        if (!have_resources ||
            !bar::load_spawn_gadgets(options.game_root, script, rules, team_rows, sim, mesh, gadget_report, error)) {
            std::cerr << "gadget: " << error << "\n";
            return 1;
        }
        std::cout << "gadget spawn " << gadget_report.def_name << " team " << gadget_report.team << " at "
                  << gadget_report.x << " " << gadget_report.y << " " << gadget_report.z << " unit " << gadget_report.unit
                  << " hide " << gadget_report.hide << " paralyze " << gadget_report.paralyze << " movectrl "
                  << gadget_report.movectrl << "\n";
        std::cout << "gadget resources m " << gadget_report.metal << " e " << gadget_report.energy << " ms "
                  << gadget_report.metal_store << " es " << gadget_report.energy_store << "\n";
    }
    struct SessionClose {
        bool armed = false;
        ~SessionClose() {
            if (armed) {
                bar::gadget_session_close();
            }
        }
    } session_close;
    session_close.armed = gadget_report.loaded;
    bar::ConstructionRules construction;
    bool have_construction = false;
    if (have_resources) {
        if (!bar::load_construction_rules(options.game_root, construction, error)) {
            std::cerr << "build: " << error << "\n";
            return 1;
        }
        have_construction = true;
        std::cout << "build rules decay " << (construction.decay ? 1 : 0) << " time " << construction.decay_time
                  << " speed " << construction.decay_speed << " power " << construction.build_power_scale << " reach "
                  << construction.build_distance_scale << " reclaim " << construction.reclaim_method << " "
                  << construction.unit_method << " repair " << construction.repair_energy_cost_factor << " resurrect "
                  << construction.resurrect_energy_cost_factor << " capture " << construction.capture_energy_cost_factor
                  << " wreck " << construction.wreck_metal_ratio << "\n";
    }
    bar::Nanoframe nano;
    bar::Wreck wreck;
    bar::LaserDuel laser;
    bar::CannonDuel cannon;
    bar::BeamDuel beam;
    bar::MissileDuel missile;
    bar::LightningDuel lightning;
    bar::FlameDuel flame;
    bar::TorpedoDuel torpedo;
    bar::BombDuel bomb;
    bar::DgunDuel dgun;
    bar::LosDuel los;
    bar::DeathWrecks deaths;
    bar::UnitAnim anim;
    if (!model.pieces.empty()) {
        const std::string def_name = units.empty() ? std::string() : units.front().def_name;
        if (def_name == "armcom") {
            const std::string script_path = options.game_root + "/scripts/Units/armcom_lus.lua";
            if (!bar::anim_open(anim, model, script_path, def_speed, error)) {
                std::cerr << "anim: " << error << "\n";
                return 1;
            }
        } else if (def_name == "armbull") {
            const std::string script_path = options.game_root + "/scripts/Units/armbull.cob";
            if (!bar::anim_open_cob(anim, model, script_path, def_speed, error)) {
                std::cerr << "anim: " << error << "\n";
                return 1;
            }
            std::cout << "cob " << script_path << "\n";
        } else if (!bar::anim_bind_pose(anim, model, error)) {
            std::cerr << "anim: " << error << "\n";
            return 1;
        }
        std::cout << "anim: " << anim.piece_count << " pieces\n";
    }
    auto upload_pieces = [&]() {
        if (!anim.ready) {
            return;
        }
        renderer.set_pieces(anim.matrices.data(), static_cast<int>(anim.matrices.size() / 16));
    };
    upload_pieces();
    std::vector<bar::ScriptOrder> orders;
    std::vector<bar::SelectEvent> select_events;
    std::vector<bar::BuildEvent> build_events;
    std::vector<bar::MinimapEvent> minimap_events;
    std::vector<bar::HudStatusEvent> hud_status_events;
    if (!options.orders.empty() && !bar::load_orders(options.orders, orders, error)) {
        std::cerr << "orders: " << error << "\n";
        return 1;
    }
    if (!options.input.empty()) {
        if (!bar::load_select_events(options.input, select_events, error)) {
            std::cerr << "input: " << error << "\n";
            return 1;
        }
        if (!bar::load_build_events(options.input, build_events, error)) {
            std::cerr << "input: " << error << "\n";
            return 1;
        }
        if (!bar::load_minimap_events(options.input, minimap_events, error)) {
            std::cerr << "input: " << error << "\n";
            return 1;
        }
        if (!bar::load_hud_status_events(options.input, hud_status_events, error)) {
            std::cerr << "input: " << error << "\n";
            return 1;
        }
    }
    bar::selection_reset();
    bar::checklist_reset();
    bar::build_cmd_reset();
    // gui_selectedunits_gl4 unitScale: 7.5 * sqrt(xsize^2 + zsize^2) + 8 (xsize = 2 * footprintx),
    // x0.7 for aircraft; the circle platter is that wide, so its radius is half of it.
    float platter_radius = 0.f;
    if (!sim.def_name.empty()) {
        bar::UnitRole role;
        std::string role_error;
        if (bar::load_unit_role(options.game_root, sim.def_name[0], role, role_error) && role.footprint_x > 0) {
            const float xs = 2.f * static_cast<float>(role.footprint_x);
            const float zs = 2.f * static_cast<float>(role.footprint_z);
            float scale = 7.5f * std::sqrt(xs * xs + zs * zs) + 8.f;
            if (role.can_fly) {
                scale *= 0.7f;
            }
            platter_radius = scale * 0.5f;
        }
    }
    bar::LosMap los_map;
    bool los_view = false;
    uint32_t los_uploaded = 0;
    if (!options.no_los_view) {
        std::string los_error;
        los_view = bar::los_map_init(options.game_root, mesh, los_map, los_error);
        if (!los_view) {
            std::cerr << "los: " << los_error << "\n";
        }
    }
    auto update_los = [&]() {
        if (!los_view || sim.x.empty()) {
            return;
        }
        std::string los_error;
        const int team = sim.team.empty() ? 0 : sim.team[0];
        if (!bar::los_map_update(options.game_root, sim, mesh, team, los_map, los_error)) {
            std::cerr << "los: " << los_error << "\n";
            los_view = false;
            renderer.set_los({}, 0, 0, 0, 0);
            return;
        }
        if (los_map.version == los_uploaded && los_uploaded != 0) {
            return;
        }
        // One texel per radar square (the finer grid): R in LOS, G in radar, sampled at texel centres.
        std::vector<uint8_t> rg(static_cast<size_t>(los_map.radar_width) * los_map.radar_height * 2, 0);
        for (int z = 0; z < los_map.radar_height; ++z) {
            for (int x = 0; x < los_map.radar_width; ++x) {
                const float wx = (static_cast<float>(x) + 0.5f) * los_map.radar_square;
                const float wz = (static_cast<float>(z) + 0.5f) * los_map.radar_square;
                const size_t at = (static_cast<size_t>(z) * los_map.radar_width + x) * 2;
                rg[at] = bar::los_map_in_los(los_map, wx, wz) ? 255 : 0;
                rg[at + 1] = los_map.radar[static_cast<size_t>(z) * los_map.radar_width + x] ? 255 : 0;
            }
        }
        los_uploaded = los_map.version == 0 ? 1 : los_map.version;
        renderer.set_los(rg, los_map.radar_width, los_map.radar_height, los_map.radar_square, los_uploaded);
    };
    auto place_model = [&](float distance) {
        (void)distance;
        renderer.set_marker(0);
        if (sim.x.empty()) {
            renderer.set_platter(0, false);
            return;
        }
        const float bank = sim.bank.empty() ? 0.f : sim.bank[0];
        renderer.set_pose(sim.x[0], sim.y[0], sim.z[0], sim.heading[0], bank);
        upload_pieces();
        update_los();
        const bar::SelectionSnapshot& snap = bar::selection_snapshot();
        const bool selected =
            !snap.selected.empty() &&
            (sim.id.empty() || std::find(snap.selected.begin(), snap.selected.end(), sim.id[0]) != snap.selected.end());
        renderer.set_platter(selected ? platter_radius : 0.f, selected);
    };
    place_model(720.f);
    if (!options.trace.empty()) {
        const std::filesystem::path trace_path(options.trace);
        std::error_code mkdir_error;
        if (trace_path.has_parent_path()) {
            std::filesystem::create_directories(trace_path.parent_path(), mkdir_error);
        }
        if (mkdir_error || !bar::write_spawn_trace(options.trace, units, error)) {
            std::cerr << "trace: " << (error.empty() ? mkdir_error.message() : error) << "\n";
            return 1;
        }
        std::cout << "trace: " << options.trace << "\n";
        if (gadget_report.loaded && trace_path.has_parent_path()) {
            const std::filesystem::path gadget_path = trace_path.parent_path() / "gadget.json";
            if (!bar::write_gadget_report(gadget_path.string(), gadget_report, error)) {
                std::cerr << "gadget: " << error << "\n";
                return 1;
            }
            std::cout << "gadget: " << gadget_path.string() << "\n";
        }
    }

    std::vector<bar::Bookmark> shots;
    bool have_fixed_camera = false;
    bar::Camera fixed_camera;
    if (!options.fixed_camera.empty()) {
        const size_t hash = options.fixed_camera.find('#');
        std::vector<bar::Bookmark> fixed_shots;
        if (hash == std::string::npos ||
            !bar::load_bookmarks(options.fixed_camera.substr(0, hash), script.map_name, fixed_shots, error)) {
            std::cerr << "fixed camera: " << (hash == std::string::npos ? "expected FILE#ID" : error) << "\n";
            return 1;
        }
        const std::string want = options.fixed_camera.substr(hash + 1);
        for (const bar::Bookmark& shot : fixed_shots) {
            if (shot.id == want) {
                fixed_camera = shot.camera;
                have_fixed_camera = true;
            }
        }
        if (!have_fixed_camera) {
            std::cerr << "fixed camera: no bookmark " << want << "\n";
            return 1;
        }
    }
    if (!options.bookmarks.empty()) {
        if (!bar::load_bookmarks(options.bookmarks, script.map_name, shots, error)) {
            std::cerr << "bookmarks: " << error << "\n";
            return 1;
        }
    }

    const bool bookmark_mode = !shots.empty();
    const bool locked = options.lock_camera;
    const int window_w = (bookmark_mode || locked) ? 1280 : 960;
    const int window_h = (bookmark_mode || locked) ? 720 : 540;
    cocoa = make_window(game_name + " - " + script.map_name, window_w, window_h);
    NSWindow* window = cocoa.window;
    NSView* view = cocoa.view;
    CAMetalLayer* layer = cocoa.layer;
    renderer.attach((__bridge void*)layer);
    if (user_window) {
        // A finished frame per refresh. Uncapped presents were scanning tiles that were
        // still clear, which is the pink on screen. The display is 60 Hz.
        layer.displaySyncEnabled = YES;
        window.collectionBehavior =
            NSWindowCollectionBehaviorFullScreenPrimary | NSWindowCollectionBehaviorManaged;
        pump_events();
        [window toggleFullScreen:nil];
        // Same keys as MTL_HUD_ENABLED. This is Apple's overlay, not the mex rings.
        layer.developerHUDProperties = @{@"MTL_HUD_ENABLED" : @"1"};
        std::cout << "apple metal hud: on\n";
        std::cout << "fullscreen: on\n";
        const NSDictionary* info = NSBundle.mainBundle.infoDictionary;
        const bool game_mode = [info[@"LSApplicationCategoryType"] isEqualToString:@"public.app-category.games"] &&
                                [info[@"LSSupportsGameMode"] boolValue];
        std::cout << "game mode: " << (game_mode ? "eligible" : "plist missing") << "\n";
        std::cout << "esc closes the window\n";
        std::cout.flush();
    }
    std::cout << "vsync: " << (layer.displaySyncEnabled ? "on" : "off") << "\n";
    std::cout.flush();

    // Apply the live drawable size to Spring.GetViewGeometry / widget ViewResize.
    // Widget bootstrap may have used a placeholder; interactive fullscreen (and any
    // unlocked window) must see the real backing size, not a constant.
    {
        pump_events();
        const CGFloat boot_scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1;
        if (locked) {
            layer.contentsScale = 1;
            layer.drawableSize = CGSizeMake(1280, 720);
        } else if (options.frames > 0) {
            layer.contentsScale = 2;
            layer.drawableSize = CGSizeMake(2940, 1846);
        } else {
            layer.contentsScale = boot_scale;
            layer.drawableSize =
                CGSizeMake(view.bounds.size.width * boot_scale, view.bounds.size.height * boot_scale);
        }
        const int boot_w = static_cast<int>(layer.drawableSize.width);
        const int boot_h = static_cast<int>(layer.drawableSize.height);
        if (boot_w > 0 && boot_h > 0) {
            bar::widgets_set_view(boot_w, boot_h);
            std::cout << "drawable: " << boot_w << "x" << boot_h << "\n";
            std::cout.flush();
        }
    }

    if (bookmark_mode) {
        const std::string shot_dir = options.screenshot_dir.empty() ? "native/oracle/out/m1" : options.screenshot_dir;
        std::error_code mkdir_error;
        std::filesystem::create_directories(shot_dir, mkdir_error);
        if (mkdir_error) {
            std::cerr << "screenshot dir: " << mkdir_error.message() << "\n";
            return 1;
        }
        int presented = 0;
        for (const bar::Bookmark& shot : shots) {
            @autoreleasepool {
                while (((BarApp*)NSApp.delegate).running) {
                    pump_events();
                    layer.contentsScale = 1;
                    layer.drawableSize = CGSizeMake(1280, 720);
                    if (layer.drawableSize.width < 1 || layer.drawableSize.height < 1) {
                        continue;
                    }
                    bar::Camera camera = shot.camera;
                    camera.view_width = 1280;
                    camera.view_height = 720;
                    const bar::CameraPose pose = bar::camera_pose(camera);
                    const std::string path = shot_dir + "/" + shot.id + ".ppm";
                    bar::TerrainFrame frame;
                    frame.metal_layer = (__bridge void*)layer;
                    frame.view_proj = pose.view_proj;
                    frame.eye = pose.eye;
                    frame.screenshot_ppm = path.c_str();
                    place_model(shot.camera.distance);
                    if (!renderer.draw(frame, error)) {
                        std::cerr << "draw: " << error << "\n";
                        return 1;
                    }
                    ++presented;
                    std::cout << "shot " << shot.id << " -> " << path << "\n";
                    break;
                }
                if (!((BarApp*)NSApp.delegate).running) {
                    std::cerr << "window closed before shot " << shot.id << "\n";
                    return 1;
                }
            }
        }
        std::cout << "presented " << presented << "\n";
        return 0;
    }

    if (anim.ready && !bar::anim_start(anim, error)) {
        std::cerr << "anim: " << error << "\n";
        return 1;
    }
    upload_pieces();
    if (!options.screenshot_dir.empty()) {
        std::error_code mkdir_error;
        std::filesystem::create_directories(options.screenshot_dir, mkdir_error);
        if (mkdir_error) {
            std::cerr << "screenshot dir: " << mkdir_error.message() << "\n";
            return 1;
        }
    }
    if (!options.pieces.empty()) {
        const std::filesystem::path piece_path(options.pieces);
        std::error_code mkdir_error;
        if (piece_path.has_parent_path()) {
            std::filesystem::create_directories(piece_path.parent_path(), mkdir_error);
        }
        if (mkdir_error) {
            std::cerr << "pieces: " << mkdir_error.message() << "\n";
            return 1;
        }
    }
    int sim_frame = 0;
    bool ai_failed = false;
    auto step_unit = [&]() {
        if (!bar::checklist_state().paused) {
                    for (int _gs = 0; _gs < std::max(1, bar::checklist_state().gamespeed); ++_gs) {
                        bar::sim_step(sim, mesh);
                    }
                }
        if (have_resources) {
            for (bar::TeamResources& row : team_rows) {
                bar::team_resources_step(row);
            }
        }
        ++sim_frame;
        if (nano.active && have_resources && nano.builder >= 0 &&
            nano.builder < static_cast<int>(sim.x.size())) {
            bar::nano_step(nano, team_rows[0], sim.x[static_cast<size_t>(nano.builder)],
                           sim.y[static_cast<size_t>(nano.builder)], sim.z[static_cast<size_t>(nano.builder)],
                           sim_frame);
        }
        if (wreck.active && have_resources && wreck.builder >= 0 &&
            wreck.builder < static_cast<int>(sim.x.size())) {
            bar::reclaim_step(wreck, team_rows[0], sim.x[static_cast<size_t>(wreck.builder)],
                              sim.y[static_cast<size_t>(wreck.builder)], sim.z[static_cast<size_t>(wreck.builder)]);
        }
        if (laser.active) {
            bar::laser_step(laser, sim, mesh, sim_frame);
        }
        if (cannon.active) {
            bar::cannon_step(cannon, sim, mesh, sim_frame);
        }
        if (beam.active) {
            bar::beam_step(beam, sim, sim_frame);
        }
        if (missile.active) {
            bar::missile_step(missile, sim, mesh, sim_frame);
        }
        if (lightning.active) {
            bar::lightning_step(lightning, sim, have_resources ? &team_rows[0] : nullptr, sim_frame);
        }
        if (flame.active) {
            bar::flame_step(flame, sim, sim_frame);
        }
        if (torpedo.active) {
            bar::torpedo_step(torpedo, sim, mesh, sim_frame);
        }
        if (bomb.active) {
            bar::bomb_step(bomb, sim, mesh, sim_frame);
        }
        if (dgun.active) {
            bar::dgun_step(dgun, sim, mesh, sim_frame);
        }
        if (have_construction) {
            const float wreck_ratio = construction.wreck_metal_ratio;
            const size_t wrecks_before = deaths.items.size();
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, laser.active, laser.die, laser.target,
                            sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, cannon.active, cannon.die, cannon.target,
                            sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, beam.active, beam.die, beam.target, sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, missile.active, missile.die, missile.target,
                            sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, lightning.active, lightning.die,
                            lightning.target, sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, flame.active, flame.die, flame.target,
                            sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, torpedo.active, torpedo.die, torpedo.target,
                            sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, bomb.active, bomb.die, bomb.target, sim_frame);
            bar::death_note(deaths, options.game_root, wreck_ratio, sim, dgun.active, dgun.die, dgun.target, sim_frame);
            if (deaths.items.size() != wrecks_before) {
                const bar::DeathWreck& made = deaths.items.back();
                std::cout << "wreck " << made.def_name << " id " << made.unit << " born " << made.born << " metal "
                          << made.metal << " cost " << made.metal_cost << " ratio " << made.ratio << " at " << made.x
                          << " " << made.y << " " << made.z << "\n";
            }
        }
        if (!bar::simpleai_game_frame(sim_frame, error)) {
            ai_failed = true;
            return;
        }
        if (have_resources && have_construction && have_claims) {
            bar::AiEconomy economy;
            economy.sim = &sim;
            economy.teams = &team_rows;
            economy.claims = &claims;
            economy.mex = &mex_claims;
            economy.generators = &generators;
            economy.construction = &construction;
            economy.terrain = &mesh;
            economy.game_root = options.game_root;
            economy.energy_scale = rules.energy_scale;
            economy.metal_scale = rules.metal_scale;
            economy.wind_strength = wind_strength;
            if (!bar::simpleai_drive(economy, sim_frame, error)) {
                ai_failed = true;
                return;
            }
        }
        if (!bar::mode_game_frame(sim_frame, error)) {
            ai_failed = true;
            return;
        }
        if (!bar::widgets_frame(sim_frame, error)) {
            ai_failed = true;
            return;
        }
        if (!anim.ready || sim.speed.empty()) {
            return;
        }
        bar::anim_set_speed(anim, sim.speed[0]);
        bar::anim_set_query(anim, sim.heading[0], bar::terrain_height_at(mesh, sim.x[0], sim.z[0]));
        bar::anim_set_moving(anim, sim.speed[0] > 0.02f);
        bar::anim_step(anim);
    };

    bar::Camera camera;
    if (!sim.x.empty()) {
        // The map-center view left the commander a few pixels tall. Open on the body,
        // from the south, so the eye is not inside the cliff north of the start.
        camera.target_x = sim.x[0];
        camera.target_y = sim.y[0] + 30.f;
        camera.target_z = sim.z[0];
        camera.distance = 720.f;
        camera.tilt = 1.0f;
        camera.yaw = 3.14159265f;
    } else {
        camera.target_x = mesh.world_x * 0.5f;
        camera.target_z = mesh.world_z * 0.5f;
        camera.distance = 9000.f;
    }
    camera.view_width = window_w;
    camera.view_height = window_h;
    const bool scripted = options.frames > 0;
    std::vector<UiEvent> ui_events;
    if (!options.ui_input.empty() && !load_ui_events(options.ui_input, ui_events, error)) {
        std::cerr << "ui input: " << error << "\n";
        return 1;
    }
    bool ui_left_owned = false;
    const int frame_limit = options.frames > 0 ? options.frames : 1000000;
    int presented = 0;
    float sim_accum = 0;
    auto last_tick = std::chrono::steady_clock::now();
    while (presented < frame_limit && ((BarApp*)NSApp.delegate).running) {
        @autoreleasepool {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::chrono::duration<float>(now - last_tick).count();
            last_tick = now;
            const FrameInput pointer = collect_input(window, view);
            if (!((BarApp*)NSApp.delegate).running) {
                break;
            }
            const NSUInteger buttons = [NSEvent pressedMouseButtons];
            const CGFloat scale = window.backingScaleFactor > 0 ? window.backingScaleFactor : 1;
            if (locked) {
                layer.contentsScale = 1;
                layer.drawableSize = CGSizeMake(1280, 720);
            } else if (scripted) {
                // Match interactive Retina fullscreen (~2940x1846) so HUD scale,
                // minimap marker, and top-bar geometry are exercised at that size.
                layer.contentsScale = 2;
                layer.drawableSize = CGSizeMake(2940, 1846);
            } else {
                layer.contentsScale = scale;
                layer.drawableSize = CGSizeMake(view.bounds.size.width * scale, view.bounds.size.height * scale);
            }
            if (layer.drawableSize.width < 1 || layer.drawableSize.height < 1) {
                continue;
            }
            bar::widgets_set_view(static_cast<int>(layer.drawableSize.width),
                                  static_cast<int>(layer.drawableSize.height));
            bar::CameraInput input;
            input.mouse_x = pointer.mouse_x;
            input.mouse_y = pointer.mouse_y;
            input.delta_x = pointer.delta_x;
            input.delta_y = pointer.delta_y;
            input.wheel = pointer.wheel;
            input.dt = dt;
            input.middle = (buttons & (1u << 2)) != 0;
            input.right = (buttons & (1u << 1)) != 0;
            input.view_width = static_cast<int>(view.bounds.size.width);
            input.view_height = static_cast<int>(view.bounds.size.height);
            if (!locked) {
                bar::camera_apply_input(camera, input);
                camera.eye_lift = 0;
            } else if (have_fixed_camera) {
                camera.target_x = fixed_camera.target_x;
                camera.target_y = fixed_camera.target_y;
                camera.target_z = fixed_camera.target_z;
                camera.distance = fixed_camera.distance;
                camera.tilt = fixed_camera.tilt;
                camera.yaw = fixed_camera.yaw;
                camera.eye_lift = 0.f;
            } else if (!sim.x.empty()) {
                camera.target_x = sim.x[0];
                camera.target_y = sim.y[0] + 30.f;
                camera.target_z = sim.z[0];
                camera.distance = 400.f;
                camera.tilt = 1.f;
                camera.yaw = 3.14159265f;
                // Eye height is cos(tilt)*distance above world y=0, about 216 here.
                // A craft above 66 elmos would put that eye under the plateau.
                // Lifting by y-66 holds the same look-down margin. The commander
                // walk and the ship stay under 66, so their shots do not move.
                camera.eye_lift = sim.y[0] > 66.f ? sim.y[0] - 66.f : 0.f;
            }
            if (camera.target_x < 0) {
                camera.target_x = 0;
            } else if (camera.target_x > mesh.world_x) {
                camera.target_x = mesh.world_x;
            }
            if (camera.target_z < 0) {
                camera.target_z = 0;
            } else if (camera.target_z > mesh.world_z) {
                camera.target_z = mesh.world_z;
            }
            camera.view_width = static_cast<int>(layer.drawableSize.width);
            camera.view_height = static_cast<int>(layer.drawableSize.height);
            const bar::CameraPose pose = bar::camera_pose(camera);
            {
                const float pixel_scale = static_cast<float>(scale);
                const float view_h = static_cast<float>(view.bounds.size.height);
                // Spring / selection y grows up from the bottom. collect_input stores top-down y.
                const float spring_x = pointer.mouse_x * pixel_scale;
                const float spring_y = (view_h - pointer.mouse_y) * pixel_scale;
                const bool left = (buttons & (1u << 0)) != 0;
                const bool right = (buttons & (1u << 1)) != 0;
                const bool middle = (buttons & (1u << 2)) != 0;
                bar::widgets_set_pointer(spring_x, spring_y, left, middle, right, false, false, false, false);
                bar::selection_set_view(static_cast<int>(layer.drawableSize.width),
                                        static_cast<int>(layer.drawableSize.height));

                std::vector<bar::SelectionMark> marks;
                for (size_t i = 0; i < sim.x.size(); ++i) {
                    if (i < sim.alive.size() && sim.alive[i] == 0) {
                        continue;
                    }
                    float sx = 0;
                    float sy = 0;
                    if (!bar::world_to_screen(camera, pose, sim.x[i], sim.y[i], sim.z[i], sx, sy)) {
                        continue;
                    }
                    bar::SelectionMark mark;
                    mark.unit_id = sim.id[i];
                    mark.sim_index = static_cast<int>(i);
                    mark.screen_x = sx;
                    mark.screen_y = sy;
                    mark.radius = 36.f * pixel_scale;
                    marks.push_back(mark);
                }
                bar::selection_set_marks(marks);

                // One path for live clicks and --ui-input clicks: the hosted widgets see the press first
                // (order menu, grid menu, info, minimap); an untaken left press places the active build or
                // starts selection; an untaken right click cancels the active command or orders a move.
                std::vector<UiEvent> clicks;
                const bool shift_down = ([NSEvent modifierFlags] & NSEventModifierFlagShift) != 0;
                if (!scripted) {
                    if (pointer.left_down) {
                        clicks.push_back({0, "press", pointer.left_x * pixel_scale,
                                          (view_h - pointer.left_y) * pixel_scale, shift_down});
                    }
                    if (pointer.left_up) {
                        clicks.push_back({0, "release", pointer.left_x * pixel_scale,
                                          (view_h - pointer.left_y) * pixel_scale, shift_down});
                    }
                    if (pointer.order_click) {
                        clicks.push_back({0, "rclick", pointer.order_x * pixel_scale,
                                          (view_h - pointer.order_y) * pixel_scale, shift_down});
                    }
                }
                for (const UiEvent& event : ui_events) {
                    if (event.frame == presented) {
                        clicks.push_back(event);
                    }
                }
                const float drawable_h = static_cast<float>(layer.drawableSize.height);
                auto ground_at = [&](float sx, float sy, float& gx, float& gz) {
                    float origin[3] = {};
                    float direction[3] = {};
                    bar::camera_pointer_ray(camera, pose, sx, drawable_h - sy, origin, direction);
                    return bar::terrain_raycast(mesh, origin, direction, gx, gz);
                };
                for (const UiEvent& click : clicks) {
                    const int lead = bar::selection_snapshot().selected.empty()
                                         ? 0
                                         : bar::selection_snapshot().selected.front();
                    if (click.kind == "press") {
                        bool consumed = false;
                        if (!bar::widgets_mouse_press(click.x, click.y, 1, consumed, error)) {
                            std::cerr << "ui: " << error << "\n";
                            error.clear();
                        }
                        ui_left_owned = consumed;
                        if (consumed) {
                            continue;
                        }
                        float gx = 0;
                        float gz = 0;
                        if (bar::build_cmd_state().placing && ground_at(click.x, click.y, gx, gz) &&
                            bar::build_live_left_click(sim, mesh, lead, gx, gz, click.shift, error)) {
                            ui_left_owned = true;
                            continue;
                        }
                        if (!scripted) {
                            bar::selection_left_down(click.x, click.y);
                        }
                    } else if (click.kind == "release") {
                        if (ui_left_owned) {
                            ui_left_owned = false;
                            if (!bar::widgets_mouse_release(click.x, click.y, 1, error)) {
                                std::cerr << "ui: " << error << "\n";
                                error.clear();
                            }
                            continue;
                        }
                        if (!scripted) {
                            bar::selection_left_up(click.x, click.y);
                            const bar::SelectionSnapshot& snap = bar::selection_snapshot();
                            std::cout << "select ids";
                            for (int id : snap.selected) {
                                std::cout << " " << id;
                            }
                            std::cout << " cursor " << snap.cursor << "\n";
                        }
                    } else {
                        bool consumed = false;
                        if (!bar::widgets_mouse_press(click.x, click.y, 3, consumed, error)) {
                            std::cerr << "ui: " << error << "\n";
                            error.clear();
                        }
                        if (consumed) {
                            if (!bar::widgets_mouse_release(click.x, click.y, 3, error)) {
                                error.clear();
                            }
                            continue;
                        }
                        if (bar::build_cmd_state().active_cmd != 0) {
                            // Right click drops the active command (build placement or order cursor).
                            std::cout << "ui cancel active " << bar::build_cmd_state().active_cmd << "\n";
                            bar::build_clear_active_command();
                            continue;
                        }
                        float gx = 0;
                        float gz = 0;
                        if (!sim.x.empty() && ground_at(click.x, click.y, gx, gz)) {
                            if (!bar::selection_issue_move(sim, gx, gz, mesh, error)) {
                                if (error != "no selection") {
                                    std::cerr << "order: " << error << "\n";
                                }
                                error.clear();
                            }
                        }
                    }
                }
                if (!scripted && left && bar::selection_snapshot().boxing) {
                    bar::selection_left_drag(spring_x, spring_y);
                }
                {
                    if (!bar::widgets_selection_changed_live(bar::selection_snapshot().selected, error)) {
                        std::cerr << "ui: " << error << "\n";
                        error.clear();
                    }
                }
                // Footprint follows the pointer while a build is active.
                if ((!scripted || !ui_events.empty()) && bar::build_cmd_state().placing) {
                    float gx = 0;
                    float gz = 0;
                    if (ground_at(spring_x, spring_y, gx, gz)) {
                        bar::build_update_preview(mesh, gx, gz);
                    }
                }
            }
            bar::widgets_set_sim(&sim, &mesh);
            bar::widgets_set_camera(&camera);
            bar::minimap_set_world(mesh.world_x, mesh.world_z, camera.view_width, camera.view_height);
            bar::widgets_set_view_proj(pose.view_proj);
            if (scripted) {
                if (!bar::apply_select_events(sim, select_events, presented, mesh, options.game_root, error)) {
                    std::cerr << "input: " << error << "\n";
                    return 1;
                }
                // Tell the stock grid menu about the current selection.
                if (!bar::widgets_selection_changed(bar::selection_snapshot().selected, error)) {
                    std::cerr << "gridmenu: " << error << "\n";
                    return 1;
                }
                const int selected = bar::selection_snapshot().selected.empty()
                                         ? 0
                                         : bar::selection_snapshot().selected.front();
                if (!bar::apply_build_events(sim, build_events, presented, mesh, selected, error)) {
                    std::cerr << "input: " << error << "\n";
                    return 1;
                }
                if (!bar::apply_minimap_events(sim, minimap_events, presented, mesh, camera, error)) {
                    std::cerr << "input: " << error << "\n";
                    return 1;
                }
                bar::hud_status_set_camera(&camera, &pose);
                if (!bar::apply_hud_status_events(sim, hud_status_events, presented, mesh, error)) {
                    std::cerr << "input: " << error << "\n";
                    return 1;
                }
                if (!bar::apply_orders(sim, orders, presented, mesh, error)) {
                    // A ship onto land, or a hover onto a cliff, has no legal cell. The unit stays put.
                    if (error == "no ground path") {
                        std::cout << "order refused: no ground path\n";
                        error.clear();
                    } else {
                        std::cerr << "order: " << error << "\n";
                        return 1;
                    }
                }
                if (presented == 0 && !orders.empty()) {
                    std::cout << "path waypoints " << sim.path_x[0].size() << "\n";
                }
                for (const bar::ScriptOrder& order : orders) {
                    if (order.frame != presented) {
                        continue;
                    }
                    if (order.kind == bar::OrderKind::Attack) {
                        if (order.unit < 0 || order.unit >= static_cast<int>(sim.x.size())) {
                            std::cerr << "order: no unit for attack\n";
                            return 1;
                        }
                        bar::LosCheck los_check;
                        bool los_applied = false;
                        if (!bar::los_consider(options.game_root, order.unit, order.target, sim, mesh, los_applied,
                                               los_check, error)) {
                            std::cerr << "order: " << error << "\n";
                            return 1;
                        }
                        if (los_applied) {
                            if (los_check.see == 0) {
                                std::cout << "los " << los_check.tag << " " << los_check.attacker_def << " -> "
                                          << los_check.target_def << " dist " << los_check.dist << " sight "
                                          << los_check.sight << " see 0\n";
                                los.checks.push_back(std::move(los_check));
                                los.active = true;
                                continue;
                            }
                            los_check.weapon = 1;
                            std::cout << "los " << los_check.tag << " " << los_check.attacker_def << " -> "
                                      << los_check.target_def << " dist " << los_check.dist << " sight "
                                      << los_check.sight << " see 1\n";
                            los.checks.push_back(std::move(los_check));
                            los.active = true;
                        }
                        if (!bar::laser_begin(laser, options.game_root, order.unit, order.target, sim, error)) {
                            if (error.find("not a LaserCannon") == std::string::npos) {
                                std::cerr << "order: " << error << "\n";
                                return 1;
                            }
                            error.clear();
                            if (!bar::cannon_begin(cannon, options.game_root, order.unit, order.target, sim, mesh,
                                                   error)) {
                                if (error.find("not a Cannon") == std::string::npos) {
                                    std::cerr << "order: " << error << "\n";
                                    return 1;
                                }
                                error.clear();
                                if (!bar::beam_begin(beam, options.game_root, order.unit, order.target, sim, error)) {
                                    if (error.find("not a BeamLaser") == std::string::npos) {
                                        std::cerr << "order: " << error << "\n";
                                        return 1;
                                    }
                                    error.clear();
                                    if (!bar::missile_begin(missile, options.game_root, order.unit, order.target, sim,
                                                            error)) {
                                        if (error.find("not a MissileLauncher") == std::string::npos) {
                                            std::cerr << "order: " << error << "\n";
                                            return 1;
                                        }
                                        error.clear();
                                        if (!bar::lightning_begin(lightning, options.game_root, order.unit, order.target,
                                                                  sim, error)) {
                                            if (error.find("not a LightningCannon") == std::string::npos) {
                                                std::cerr << "order: " << error << "\n";
                                                return 1;
                                            }
                                            error.clear();
                                            if (!bar::flame_begin(flame, options.game_root, order.unit, order.target,
                                                                  sim, error)) {
                                                if (error.find("not a Flame") == std::string::npos) {
                                                    std::cerr << "order: " << error << "\n";
                                                    return 1;
                                                }
                                                error.clear();
                                                if (!bar::torpedo_begin(torpedo, options.game_root, order.unit,
                                                                        order.target, sim, mesh, error)) {
                                                    if (error.find("not a TorpedoLauncher") == std::string::npos) {
                                                        std::cerr << "order: " << error << "\n";
                                                        return 1;
                                                    }
                                                    error.clear();
                                                    if (!bar::bomb_begin(bomb, options.game_root, order.unit,
                                                                         order.target, sim, mesh, error)) {
                                                        if (error.find("not an AircraftBomb") == std::string::npos) {
                                                            std::cerr << "order: " << error << "\n";
                                                            return 1;
                                                        }
                                                        error.clear();
                                                        if (!bar::dgun_begin(dgun, options.game_root, order.unit,
                                                                             order.target, sim, mesh, error)) {
                                                            std::cerr << "order: " << error << "\n";
                                                            return 1;
                                                        }
                                                        std::cout << "dgun " << dgun.attacker_def << " at "
                                                                  << dgun.muzzle_x << " " << dgun.muzzle_y << " "
                                                                  << dgun.muzzle_z << " -> " << dgun.target_def
                                                                  << " dmg " << dgun.damage << " speed " << dgun.speed
                                                                  << " range " << dgun.range << " reload "
                                                                  << dgun.reload_frames << "\n";
                                                        continue;
                                                    }
                                                    std::cout << "bomb " << bomb.attacker_def << " at " << bomb.muzzle_x
                                                              << " " << bomb.muzzle_y << " " << bomb.muzzle_z
                                                              << " -> " << bomb.target_def << " dmg " << bomb.damage
                                                              << " range " << bomb.range << " reload "
                                                              << bomb.reload_frames << " burst " << bomb.burst
                                                              << " delay " << bomb.salvo_delay << "\n";
                                                    continue;
                                                }
                                                std::cout << "torpedo " << torpedo.attacker_def << " at "
                                                          << torpedo.muzzle_x << " " << torpedo.muzzle_y << " "
                                                          << torpedo.muzzle_z << " -> " << torpedo.target_def
                                                          << " dmg " << torpedo.damage << " speed " << torpedo.speed
                                                          << " range " << torpedo.range << " reload "
                                                          << torpedo.reload_frames << " ttl " << torpedo.ttl << "\n";
                                                continue;
                                            }
                                            std::cout << "flame " << flame.attacker_def << " at " << flame.muzzle_x
                                                      << " " << flame.muzzle_y << " " << flame.muzzle_z << " -> "
                                                      << flame.target_def << " dmg " << flame.damage << " speed "
                                                      << flame.speed << " range " << flame.range << " reload "
                                                      << flame.reload_frames << " burst " << flame.burst << " delay "
                                                      << flame.salvo_delay << "\n";
                                            continue;
                                        }
                                        std::cout << "lightning " << lightning.attacker_def << " at "
                                                  << lightning.muzzle_x << " " << lightning.muzzle_y << " "
                                                  << lightning.muzzle_z << " -> " << lightning.target_def << " dmg "
                                                  << lightning.damage << " range " << lightning.range << " reload "
                                                  << lightning.reload_frames << " burst " << lightning.burst
                                                  << " delay " << lightning.salvo_delay << " cost "
                                                  << lightning.energy_cost << "\n";
                                        continue;
                                    }
                                    std::cout << "missile " << missile.attacker_def << " at " << missile.muzzle_x << " "
                                              << missile.muzzle_y << " " << missile.muzzle_z << " -> "
                                              << missile.target_def << " dmg " << missile.damage << " speed "
                                              << missile.speed << " range " << missile.range << " reload "
                                              << missile.reload_frames << " hmod " << missile.height_mod << "\n";
                                    continue;
                                }
                                std::cout << "beam " << beam.attacker_def << " at " << beam.muzzle_x << " "
                                          << beam.muzzle_y << " " << beam.muzzle_z << " -> " << beam.target_def
                                          << " dmg " << beam.damage << " range " << beam.range << " reload "
                                          << beam.reload_frames << " salvo " << beam.salvo_size << " beamtime "
                                          << beam.beamtime << "\n";
                                continue;
                            }
                            std::cout << "cannon " << cannon.attacker_def << " at " << cannon.muzzle_x << " "
                                      << cannon.muzzle_y << " " << cannon.muzzle_z << " -> " << cannon.target_def
                                      << " dmg " << cannon.damage << " speed " << cannon.speed << " range "
                                      << cannon.range << " reload " << cannon.reload_frames << " grav "
                                      << cannon.gravity << "\n";
                            continue;
                        }
                        std::cout << "laser " << laser.attacker_def << " at " << laser.muzzle_x << " " << laser.muzzle_y
                                  << " " << laser.muzzle_z << " -> " << laser.target_def << " dmg " << laser.damage
                                  << " speed " << laser.speed << " range " << laser.range << " reload "
                                  << laser.reload_frames << "\n";
                        continue;
                    }
                    if (order.kind != bar::OrderKind::Build && order.kind != bar::OrderKind::Make &&
                        order.kind != bar::OrderKind::Reclaim) {
                        continue;
                    }
                    if (!have_construction || sim.x.empty() || order.unit < 0 ||
                        order.unit >= static_cast<int>(sim.x.size())) {
                        std::cerr << "order: no builder for " << order.build_def << "\n";
                        return 1;
                    }
                    const size_t at = static_cast<size_t>(order.unit);
                    if (order.kind == bar::OrderKind::Build) {
                        if (!bar::nano_begin(nano, options.game_root, construction, order.unit, sim.def_name[at],
                                             sim.x[at], sim.y[at], sim.z[at], order.build_def, order.x, order.z, mesh,
                                             error)) {
                            std::cerr << "order: " << error << "\n";
                            return 1;
                        }
                        std::cout << "nano " << nano.def_name << " at " << nano.x << " " << nano.y << " " << nano.z
                                  << " click " << order.x << " " << order.z << " power " << nano.power_per_frame
                                  << " time " << nano.build_time << " cost " << nano.metal_cost << " "
                                  << nano.energy_cost << " hp " << nano.health << "\n";
                    } else if (order.kind == bar::OrderKind::Make) {
                        if (!bar::factory_begin(nano, options.game_root, construction, order.unit, sim.def_name[at],
                                                sim.x[at], sim.y[at], sim.z[at], order.build_def, error)) {
                            std::cerr << "order: " << error << "\n";
                            return 1;
                        }
                        std::cout << "make " << nano.def_name << " at " << nano.x << " " << nano.y << " " << nano.z
                                  << " power " << nano.power_per_frame << " time " << nano.build_time << " cost "
                                  << nano.metal_cost << " " << nano.energy_cost << " hp " << nano.health << "\n";
                    } else if (!bar::reclaim_begin(wreck, options.game_root, construction, order.unit, sim.def_name[at],
                                                   sim.x[at], sim.y[at], sim.z[at], order.build_def, order.x, order.z,
                                                   mesh, error)) {
                        std::cerr << "order: " << error << "\n";
                        return 1;
                    } else {
                        std::cout << "reclaim " << wreck.def_name << " at " << wreck.x << " " << wreck.y << " "
                                  << wreck.z << " metal " << wreck.metal << " time " << wreck.reclaim_time << " power "
                                  << wreck.power_per_frame << "\n";
                    }
                }
                step_unit();
                if (ai_failed) {
                    std::cerr << "sim: " << error << "\n";
                    return 1;
                }
            } else {
                sim_accum += dt;
                int steps = 0;
                while (sim_accum >= 1.f / 30.f && steps < 4) {
                    step_unit();
                    if (ai_failed) {
                        std::cerr << "sim: " << error << "\n";
                        return 1;
                    }
                    sim_accum -= 1.f / 30.f;
                    ++steps;
                }
            }
            place_model(camera.distance);
            bar::TerrainFrame frame;
            frame.metal_layer = (__bridge void*)layer;
            frame.view_proj = pose.view_proj;
            frame.eye = pose.eye;
            frame.sim_frame = sim_frame;
            frame.hud = bar::hud_list();
            std::string shot_path;
            if (scripted && !options.screenshot_dir.empty()) {
                const int label = presented + 1;
                if (label % 15 == 0 && label >= 60 && label <= 150) {
                    char name[16];
                    std::snprintf(name, sizeof(name), "f%03d.ppm", label);
                    shot_path = options.screenshot_dir + "/" + name;
                    frame.screenshot_ppm = shot_path.c_str();
                }
            }
            if (shot_path.empty() && !options.screenshot.empty() &&
                (presented == 0 || (scripted && presented + 1 == frame_limit))) {
                shot_path = options.screenshot;
                if (presented != 0) {
                    const auto dot = shot_path.rfind('.');
                    const auto at = dot == std::string::npos ? shot_path.size() : dot;
                    shot_path.insert(at, "-end");
                }
                frame.screenshot_ppm = shot_path.c_str();
            }
            if (!renderer.draw(frame, error)) {
                std::cerr << "draw: " << error << "\n";
                return 1;
            }
            if (!options.gpu_timing.empty()) {
                for (const bar::GpuFrameTiming& t : renderer.take_gpu_timings()) {
                    gpu_timings.push_back(t);
                }
            }
            ++presented;
            if (scripted && anim.ready && !options.pieces.empty() && presented % 15 == 0) {
                const bool append = presented != 15;
                const bool wrote = bar::anim_is_cob(anim)
                                       ? bar::anim_write_cob_sample(anim, options.pieces, presented, append, error)
                                       : bar::anim_write_sample(anim, options.pieces, presented, append, error);
                if (!wrote) {
                    std::cerr << "pieces: " << error << "\n";
                    return 1;
                }
            }
            if (scripted && !options.trace.empty() && !sim.x.empty() &&
                (presented % 30 == 0 || presented == frame_limit)) {
                const bool append = presented != 30 && !(presented < 30 && presented == frame_limit);
                std::vector<bar::TeamTrace> team_traces;
                if (have_resources) {
                    for (const bar::TeamResources& row : team_rows) {
                        bar::TeamTrace team_trace;
                        team_trace.id = row.team;
                        team_trace.metal = row.metal;
                        team_trace.energy = row.energy;
                        team_trace.metal_storage = row.metal_storage;
                        team_trace.energy_storage = row.energy_storage;
                        team_trace.metal_income = row.metal_income;
                        team_trace.energy_income = row.energy_income;
                        team_trace.mex_income = row.mex_income;
                        team_trace.energy_upkeep = row.energy_upkeep;
                        team_trace.solar_income = row.solar_income;
                        team_trace.wind_income = row.wind_income;
                        team_traces.push_back(team_trace);
                    }
                }
                const bar::NanoTrace nano_out = bar::nano_trace(nano);
                const bar::WreckTrace wreck_out = bar::wreck_trace(wreck);
                const bar::LaserTrace laser_out = bar::laser_trace(laser);
                const bar::CannonTrace cannon_out = bar::cannon_trace(cannon);
                const bar::BeamTrace beam_out = bar::beam_trace(beam);
                const bar::MissileTrace missile_out = bar::missile_trace(missile);
                const bar::LightningTrace lightning_out = bar::lightning_trace(lightning);
                const bar::FlameTrace flame_out = bar::flame_trace(flame);
                const bar::TorpedoTrace torpedo_out = bar::torpedo_trace(torpedo);
                const bar::BombTrace bomb_out = bar::bomb_trace(bomb);
                const bar::DgunTrace dgun_out = bar::dgun_trace(dgun);
                if (!bar::write_sim_trace(options.trace, presented, sim, mesh, append, error,
                                          team_traces.empty() ? nullptr : &team_traces,
                                          mex_claims.empty() ? nullptr : &mex_claims,
                                          generators.empty() ? nullptr : &generators,
                                          nano.active ? &nano_out : nullptr, wreck.active ? &wreck_out : nullptr,
                                          laser.active ? &laser_out : nullptr,
                                          cannon.active ? &cannon_out : nullptr,
                                          beam.active ? &beam_out : nullptr,
                                          missile.active ? &missile_out : nullptr,
                                          lightning.active ? &lightning_out : nullptr,
                                          flame.active ? &flame_out : nullptr,
                                          torpedo.active ? &torpedo_out : nullptr,
                                          bomb.active ? &bomb_out : nullptr,
                                          dgun.active ? &dgun_out : nullptr,
                                          los.active ? &los : nullptr,
                                          deaths.items.empty() ? nullptr : &deaths)) {
                    std::cerr << "trace: " << error << "\n";
                    return 1;
                }
            }
            if (presented % 30 == 0 || (scripted && presented == frame_limit)) {
                std::cout << "frame " << presented << " presented\n";
            }
        }
    }
    if (!options.gpu_timing.empty()) {
        // Frames still in flight report shortly after their slot's work completes.

        for (int wait = 0; wait < 20; ++wait) {
            for (const bar::GpuFrameTiming& t : renderer.take_gpu_timings()) {
                gpu_timings.push_back(t);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::sort(gpu_timings.begin(), gpu_timings.end(),
                  [](const bar::GpuFrameTiming& x, const bar::GpuFrameTiming& y) { return x.frame < y.frame; });
        std::ofstream csv(options.gpu_timing, std::ios::trunc);
        csv << "frame,gpu_ms,encode_ms,grass_gpu_ms,grass_cpu_ms\n";
        for (const bar::GpuFrameTiming& t : gpu_timings) {
            csv << t.frame << "," << t.gpu_ms << "," << t.encode_ms << "," << t.grass_gpu_ms << ","
                << t.grass_cpu_ms << "\n";
        }
        std::cout << "gpu timing: " << gpu_timings.size() << " frames -> " << options.gpu_timing << "\n";
    }

    if (!options.trace.empty()) {
        const std::filesystem::path behavior_path =
            std::filesystem::path(options.trace).parent_path() / "behavior.json";
        if (!behavior_path.empty() && !bar::write_behavior_report(behavior_path.string(), error)) {
            std::cerr << "behavior: " << error << "\n";
            return 1;
        }
        if (!behavior_path.empty()) {
            std::cout << "behavior: " << behavior_path.string() << " calls\n";
        }
        const std::filesystem::path ai_path = std::filesystem::path(options.trace).parent_path() / "ai.json";
        if (!ai_path.empty() && !bar::write_ai_report(ai_path.string(), error)) {
            std::cerr << "simpleai: " << error << "\n";
            return 1;
        }
        if (!ai_path.empty()) {
            std::cout << "simpleai: " << ai_path.string() << "\n";
        }
    }
    if (!options.input.empty()) {
        const std::filesystem::path report_path =
            options.trace.empty() ? std::filesystem::path("native/oracle/out/m7select/select.json")
                                  : std::filesystem::path(options.trace).parent_path() / "select.json";
        std::error_code mkdir_error;
        if (report_path.has_parent_path()) {
            std::filesystem::create_directories(report_path.parent_path(), mkdir_error);
        }
        if (mkdir_error) {
            std::cerr << "select: " << mkdir_error.message() << "\n";
            return 1;
        }
        if (!bar::write_selection_report(report_path.string(), presented, error)) {
            std::cerr << "select: " << error << "\n";
            return 1;
        }
        std::cout << "select: " << report_path.string() << "\n";
        const bar::SelectionSnapshot& snap = bar::selection_snapshot();
        std::cout << "select final ids";
        for (int id : snap.selected) {
            std::cout << " " << id;
        }
        std::cout << " cursor " << snap.cursor << " cmd " << snap.active_cmd << "\n";
        const std::filesystem::path build_path =
            options.trace.empty() ? std::filesystem::path("native/oracle/out/m7select/build.json")
                                  : std::filesystem::path(options.trace).parent_path() / "build.json";
                {
            const auto check_path = report_path.parent_path() / "checklist.json";
            if (!bar::write_checklist_report(check_path.string(), error)) {
                std::cerr << "checklist: " << error << "\n";
                return 1;
            }
        }
if (!bar::write_build_report(build_path.string(), presented, error)) {
            std::cerr << "build: " << error << "\n";
            return 1;
        }
        std::cout << "build: " << build_path.string() << "\n";
    }
    {
        const std::filesystem::path mmap_path =
            options.trace.empty() ? std::filesystem::path("native/oracle/out/m7minimap/minimap.json")
                                  : std::filesystem::path(options.trace).parent_path() / "minimap.json";
        std::error_code ec;
        std::filesystem::create_directories(mmap_path.parent_path(), ec);
        if (!bar::write_minimap_report(mmap_path.string(), presented, error)) {
            std::cerr << "minimap: " << error << "\n";
            return 1;
        }
        std::cout << "minimap: " << mmap_path.string() << "\n";
    }
    {
        const std::filesystem::path hud_path =
            options.trace.empty() ? std::filesystem::path("native/oracle/out/m7hud/hud.json")
                                  : std::filesystem::path(options.trace).parent_path() / "hud.json";
        std::error_code ec;
        std::filesystem::create_directories(hud_path.parent_path(), ec);
        if (!bar::write_hud_status_report(hud_path.string(), presented, error)) {
            std::cerr << "hud: " << error << "\n";
            return 1;
        }
        std::cout << "hud: " << hud_path.string() << "\n";
    }
    std::cout << "presented " << presented << "\n";
    if (scripted && !sim.x.empty()) {
        std::cout << "sim frame " << presented << " at " << sim.x[0] << " " << sim.y[0] << " " << sim.z[0] << "\n";
    }
    if (options.frames > 0 && presented < options.frames) {
        std::cerr << "stopped after " << presented << " of " << options.frames << " frames\n";
        return 1;
    }
    int ai_teams = 0;
    for (const bar::TeamStart& team : script.teams) {
        if (!team.lua_ai.empty()) {
            ++ai_teams;
        }
    }
    if (options.frames > 0 && ai_teams >= 2) {
        int alive = 0;
        for (const bar::TeamStart& team : script.teams) {
            if (team.lua_ai.empty()) {
                continue;
            }
            for (size_t i = 0; i < sim.def_name.size(); ++i) {
                if (sim.team[i] == team.team && sim.def_name[i] == "armcom") {
                    ++alive;
                    break;
                }
            }
        }
        std::cout << (alive == ai_teams ? "winner none\n" : "winner incomplete\n");
    }
    return 0;
}

}  // namespace

@implementation BarApp
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    self.running = YES;
}
- (void)windowWillClose:(NSNotification*)notification {
    self.running = NO;
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    self.running = NO;
    return YES;
}
@end

int main(int argc, char** argv) {
    @autoreleasepool {
        Options options;
        try {
            if (!parse_args(argc, argv, options)) {
                return argc > 1 && std::string(argv[1]) == "--help" ? 0 : 1;
            }
        } catch (const std::exception& ex) {
            std::cerr << ex.what() << "\n";
            return 1;
        }

        if (options.check_hud_text) {
            std::string hud_error;
            if (!bar::check_hud_text(hud_error)) {
                std::cerr << "check-hud-text: " << hud_error << "\n";
                return 1;
            }
            std::cout << "check-hud-text ok\n";
            return 0;
        }

        std::string game_name;
        std::string error;
        if (!bar::load_game_name(options.game_root, game_name, error)) {
            std::cerr << "modinfo: " << error << "\n";
            return 1;
        }
        std::cout << "game: " << game_name << "\n";

        [NSApplication sharedApplication];
        BarApp* delegate = [BarApp new];
        [NSApp setDelegate:delegate];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        [NSApp activateIgnoringOtherApps:YES];
        [NSApp finishLaunching];
        delegate.running = YES;

        if (options.start_script.empty()) {
            return run_triangle(options, game_name);
        }
        return run_terrain(options, game_name);
    }
}
