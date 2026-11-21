-- VanaCore game data: what's here (gen_gamedata.py writes it). A table with part_size is in files
-- <name>_<part>.lua, part = floor(id / part_size), each holding the ids of that range.
return {
  game = "D:\\cexi\\catseyexi-client\\Game\\FINAL FANTASY XI", server = "D:\\cexi-server\\catseyexi-stable", lang = "en",
  tables = {
    abilities = {rows=605,generated="2026-10-25"},
    augments = {rows=2046,generated="2026-10-25"},
    elements = {rows=9,generated="2026-10-25"},
    items = {rows=25258,generated="2026-10-25",part_size=4096,parts={[0]=3832,[1]=2493,[2]=3522,[3]=4080,[4]=3527,[5]=2808,[6]=2934,[7]=389,[14]=1569,[15]=104}},
    job_points = {rows=217,generated="2026-10-25"},
    jobs = {rows=24,generated="2026-10-25"},
    key_items = {rows=3224,generated="2026-10-25"},
    merits = {rows=296,generated="2026-10-25"},
    monster_families = {rows=401,generated="2026-10-25"},
    mounts = {rows=39,generated="2026-10-25"},
    skill_caps = {rows=100,generated="2026-10-25"},
    skill_ranks = {rows=33,generated="2026-10-25"},
    skills = {rows=49,generated="2026-10-25"},
    slots = {rows=16,generated="2026-10-25"},
    spells = {rows=890,generated="2026-10-25"},
    status_effects = {rows=635,generated="2026-10-25"},
    titles = {rows=1111,generated="2026-10-25"},
    traits = {rows=114,generated="2026-10-25"},
    weapon_skills = {rows=209,generated="2026-10-25"},
    zones = {rows=300,generated="2026-10-25"},
  },
}
