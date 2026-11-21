// nameplates.cpp's, inside its namespace: nameplates.hideparty and .hidealliance. Who is in your party (its other members,
// slots 1-5, trusts with them) and in the alliance's other two parties (slots 6-17), by server id, read from the game's
// party list every kMembersEvery ms while either is set; a name is left out by its entity's server id, so a member in
// another zone (their target index is stale) never hides someone else's name. Asked for 2026-11-10 (Yunas): YunaUI draws
// party and alliance members' names in its own bars, so they showed twice.

struct Member {
    std::uint32_t id = 0;
    bool party = false; // your own party's, else the alliance's
};
constexpr ULONGLONG kMembersEvery = 250;
Member g_members[17];
unsigned g_member_count = 0;
ULONGLONG g_members_at = 0;

// Read again now and then while either setting is on; let go once neither is
void find_members(const Names& names)
{
    if (!names.hide_party && !names.hide_alliance) {
        g_member_count = 0;
        return;
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_members_at < kMembersEvery)
        return;
    g_members_at = now;
    g_member_count = 0;
    game::PartyMember members[18];
    game::Alliance alliance;
    if (!game::party(members, alliance))
        return;
    for (unsigned slot = 1; slot < 18; ++slot) // slot 0 is you: nameplates.hideown
        if (members[slot].active && members[slot].id != 0)
            g_members[g_member_count++] = Member{members[slot].id, slot < 6};
}

// Whether nameplates.hideparty or .hidealliance leaves out the name of the character with server id `id`
bool member_left_out(std::uint32_t id)
{
    for (unsigned i = 0; i < g_member_count; ++i)
        if (g_members[i].id == id)
            return g_members[i].party ? g_names.hide_party : g_names.hide_alliance;
    return false;
}
