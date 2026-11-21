#include "grass_floors.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "../gfx/vanacore_gfx.h"
#include "dats.hpp"
#include "log.hpp"
#include "zonefloor.hpp"

namespace vanacore::grass_floors {
namespace {

// A zone's floors, read on a thread.
struct Job {
    int zone = 0;
    bool walls = false; // the whole collision (update_collision)
    zonefloor::Floors floors;
    bool ok = false;
    std::atomic<bool> done{false};
};

std::shared_ptr<Job> g_job; // the read under way, or done and not yet handed over
int g_wanted = 0;           // the zone whose floors were last asked for (0: none)
std::shared_ptr<Job> g_collision_job; // ... the same for the collision view's whole collision
int g_collision_wanted = 0;

// The zone's model DAT, as zonelines.cpp finds it: file id 100 + the zone below 0x100, 0x147B3 + (zone - 0x100) from there.
std::uint32_t model_file(int zone)
{
    return zone < 0x100 ? 100u + static_cast<std::uint32_t>(zone) : 0x147B3u + static_cast<std::uint32_t>(zone - 0x100);
}

// (an exception left out of a thread ends the game at once: short of memory, no floors, as zone_casters.cpp's read)
void read(std::shared_ptr<Job> job)
{
    try {
        std::vector<std::uint8_t> dat;
        if (dats::read(model_file(job->zone), dat))
            job->ok = zonefloor::read_floors(dat, zonefloor::game_key_table(), job->floors, job->walls); // the key table read once a session
    } catch (const std::exception&) {
        job->floors = zonefloor::Floors{}, job->ok = false;
    }
    job->done = true;
}

// The zone's floors as the graphics take them (none: f null)
VanaFloors handed(int zone, const zonefloor::Floors* f)
{
    VanaFloors v{};
    v.zone = zone;
    if (f == nullptr)
        return v;
    v.tris = f->tris.data(), v.terrain = f->terrain.data(), v.ntris = static_cast<unsigned>(f->terrain.size());
    v.mesh_first = f->mesh_first.data();
    v.nmesh = f->mesh_first.empty() ? 0u : static_cast<unsigned>(f->mesh_first.size() - 1);
    v.matrices = f->matrices.data(), v.nmatrices = static_cast<unsigned>(f->matrices.size() / 12);
    v.pairs = f->pairs.data(), v.npairs = static_cast<unsigned>(f->pairs.size() / 2);
    return v;
}

void hand(int zone, const zonefloor::Floors* f)
{
    const VanaFloors v = handed(zone, f);
    vanacore_gfx_set_grass_floors(&v);
}

void hand_collision(int zone, const zonefloor::Floors* f)
{
    const VanaFloors v = handed(zone, f);
    vanacore_gfx_set_collision(&v);
}

} // namespace

void update(int zone, bool wanted)
{
    if (!wanted)
        zone = 0;
    if (zone != g_wanted) {
        g_wanted = zone;
        g_job.reset(); // another zone's read finishes on its own, unheard
        if (zone == 0) {
            hand(0, nullptr);
            return;
        }
        auto job = std::make_shared<Job>();
        job->zone = zone;
        g_job = job;
        std::thread(read, std::move(job)).detach();
        return;
    }
    if (g_job && g_job->done) {
        const std::shared_ptr<Job> job = std::move(g_job);
        const zonefloor::Floors& f = job->floors;
        if (job->ok) {
            log::write(L"Graphics: zone %d's floors from its collision: %u placed, %u meshes, %u triangles (grass.walkable, refl_floors)", zone,
                       static_cast<unsigned>(f.pairs.size() / 2), static_cast<unsigned>(f.mesh_first.empty() ? 0 : f.mesh_first.size() - 1),
                       static_cast<unsigned>(f.terrain.size()));
            hand(zone, &f);
        } else {
            log::write(L"Graphics: no floors read from zone %d's collision (file %u): the grass grows on all its grass ground and the "
                       L"reflections show on any ground (grass.walkable, refl_floors)",
                       zone, model_file(zone));
            hand(zone, nullptr);
        }
    }
}

void update_collision(int zone, bool wanted)
{
    if (!wanted)
        zone = 0;
    if (zone != g_collision_wanted) {
        g_collision_wanted = zone;
        g_collision_job.reset(); // another zone's read finishes on its own, unheard
        hand_collision(0, nullptr); // the last zone's not drawn meanwhile
        if (zone == 0)
            return;
        auto job = std::make_shared<Job>();
        job->zone = zone;
        job->walls = true;
        g_collision_job = job;
        std::thread(read, std::move(job)).detach();
        return;
    }
    if (g_collision_job && g_collision_job->done) {
        const std::shared_ptr<Job> job = std::move(g_collision_job);
        const zonefloor::Floors& f = job->floors;
        if (job->ok) {
            log::write(L"Graphics: zone %d's collision for the collision view: %u placed, %u meshes, %u triangles (collision.show)", zone,
                       static_cast<unsigned>(f.pairs.size() / 2), static_cast<unsigned>(f.mesh_first.empty() ? 0 : f.mesh_first.size() - 1),
                       static_cast<unsigned>(f.terrain.size()));
            hand_collision(zone, &f);
        } else {
            log::write(L"Graphics: no collision read from zone %d (file %u): the collision view has nothing to show (collision.show)", zone,
                       model_file(zone));
            hand_collision(zone, nullptr);
        }
    }
}

void forget()
{
    g_wanted = 0;
    g_job.reset();
    g_collision_wanted = 0;
    g_collision_job.reset();
}

} // namespace vanacore::grass_floors
