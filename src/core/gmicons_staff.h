// GM_LIST: each server's staff and the picture before their names while they're in GM mode (gmicons.cpp's include, inside
// its namespace, after Icon). A server by its host as VanaCore.exe was given it (--server, else [server] server=; any case),
// a name as the game shows it (any case). Anyone else in GM mode on a listed server gets gm.png; on a server that isn't
// listed, every GM icon stays the game's. A change here takes a rebuild.

struct Staff {
    const char* name;
    Icon icon;
};

constexpr Staff kCatsEyeXI[] = {
    {"Vekien", kDev},
    {"Carver", kAdmin},
    {"Box", kSgm},
    {"Rumham", kSgm},
};

struct ServerStaff {
    const char* host;
    const Staff* staff;
    std::size_t count;
};

constexpr ServerStaff kGmList[] = {
    {"server.catseyexi.com", kCatsEyeXI, std::size(kCatsEyeXI)},
};
