#include "zone_casters.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "dats.hpp"
#include "log.hpp"
#include "zonefloor.hpp"
#include "zonemesh.hpp"

namespace vanacore::zone_casters {
namespace {

struct Job {
    int zone = 0;
    bool shade = false; // the ray-traced reflections want the meshes' textures and colours too (refl_rt 2)
    zonemesh::Mesh mesh;
    bool ok = false;
    std::string why;
    std::atomic<bool> done{false};
};

std::shared_ptr<Job> g_job; // the read under way, or done and not yet handed over
int g_wanted = 0;           // the zone whose meshes were last asked for (0: none)

// The zone's model DAT, as grass_floors.cpp finds it
std::uint32_t model_file(int zone)
{
    return zone < 0x100 ? 100u + static_cast<std::uint32_t>(zone) : 0x147B3u + static_cast<std::uint32_t>(zone - 0x100);
}

// FFXiMain.dll's second key table (the meshes' block swaps; zonefloor::game_key_table has the first), read once a session
const std::uint8_t* game_key_table2()
{
    static std::mutex lock;
    static bool tried = false, have = false;
    static std::uint8_t table[256];
    const std::lock_guard guard(lock);
    if (!tried) {
        const HMODULE game = GetModuleHandleW(L"FFXiMain.dll");
        if (game == nullptr)
            return nullptr;
        tried = true;
        wchar_t path[MAX_PATH] = L"";
        std::vector<std::uint8_t> dll;
        have = GetModuleFileNameW(game, path, MAX_PATH) != 0 && dats::read_file(path, dll) && zonemesh::key_table2(dll, table);
    }
    return have ? table : nullptr;
}

// Why: a thread's uncaught exception ends the game at once (std::terminate, no crash report). The Sanctuary of Zi'Tah's meshes
// laid in the world are 108 MB (2.07 million triangles, 2.4x the next zone), grown in vectors that each want one free block that
// size in the 32-bit game; a player whose address space was fuller than ours lost the game the moment the zone came in, and again
// at every login there (2026-11-17, the megathread). Short of memory, the shadows come from the game's draws, as with it off.
void read(std::shared_ptr<Job> job)
{
    try {
        std::vector<std::uint8_t> dat;
        if (dats::read(model_file(job->zone), dat))
            job->ok = zonemesh::read(dat, zonefloor::game_key_table(), game_key_table2(), job->mesh, job->why, job->shade);
        else
            job->why = "its model DAT couldn't be read";
    } catch (const std::bad_alloc&) {
        job->mesh = zonemesh::Mesh{}, job->ok = false, job->why = "not enough free memory in the game for them";
    } catch (const std::exception& e) {
        job->mesh = zonemesh::Mesh{}, job->ok = false, job->why = e.what();
    }
    job->done = true;
}

void hand(int zone, const zonemesh::Mesh* m)
{
    VanaZoneMesh v{};
    v.zone = zone;
    std::vector<VanaZoneMeshRange> ranges;
    std::vector<VanaZoneMeshTexture> textures;
    if (m != nullptr) {
        for (const zonemesh::Range& r : m->ranges)
            ranges.push_back({r.first, r.count, r.texture, {r.lo[0], r.lo[1], r.lo[2]}, {r.hi[0], r.hi[1], r.hi[2]}});
        for (const zonemesh::Texture& t : m->textures)
            textures.push_back({t.argb.data(), t.width, t.height});
        v.pos = m->pos.data(), v.npos = static_cast<unsigned>(m->pos.size() / 3);
        v.nrm = m->nrm.size() == m->pos.size() / 3 * 4 ? m->nrm.data() : nullptr;
        v.cut = m->cut.data(), v.ncut = static_cast<unsigned>(m->cut.size() / 5);
        v.ranges = ranges.data(), v.nranges = static_cast<unsigned>(ranges.size());
        if (m->uv.size() == m->pos.size() / 3 * 2 && m->col.size() == m->pos.size() / 3 && m->tex.size() == m->pos.size() / 9 &&
            m->cut_col.size() == m->cut.size() / 5)
            v.uv = m->uv.data(), v.col = m->col.data(), v.tex = m->tex.data(), v.cut_col = m->cut_col.data();
        v.textures = textures.data(), v.ntextures = static_cast<unsigned>(textures.size());
    }
    vanacore_gfx_set_zone_mesh(&v);
}

} // namespace

void update(int zone, bool wanted)
{
    if (!wanted)
        zone = 0;
    if (zone != g_wanted) {
        g_wanted = zone;
        g_job.reset(); // another zone's read finishes on its own, unheard
        hand(0, nullptr);
        if (zone == 0)
            return;
        auto job = std::make_shared<Job>();
        job->zone = zone;
        job->shade = (vanacore_gfx_floors_wanted() & 4) != 0;
        g_job = job;
        std::thread(read, std::move(job)).detach();
        return;
    }
    if (!g_job || !g_job->done)
        return;
    const std::shared_ptr<Job> job = std::move(g_job);
    const zonemesh::Mesh& m = job->mesh;
    if (!job->ok) {
        log::write(L"Graphics: zone %d's own meshes weren't read for the sun's shadows (file %u: %hs); they come from what the game "
                   L"draws ([graphics.effects] sun_zone_mesh)",
                   zone, model_file(zone), job->why.c_str());
        return;
    }
    log::write(L"Graphics: zone %d's own meshes for the sun's shadows: %u of its %u placements (%u collision stand-ins and %u that "
               L"move left out, %u with no mesh), %u meshes, %llu triangles, %u blended parts left out, %u cut-out textures "
               L"([graphics.effects] sun_zone_mesh)",
               zone, m.placed, m.placements, m.stand_ins, m.moving, m.unfound, m.meshes, static_cast<unsigned long long>(m.triangles),
               m.blended, static_cast<unsigned>(m.textures.size()));
    hand(zone, &m);
}

void forget()
{
    g_wanted = 0;
    g_job.reset();
}

} // namespace vanacore::zone_casters
