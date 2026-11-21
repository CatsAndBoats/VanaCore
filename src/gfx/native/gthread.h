/* Upstream's guest threads and guest lock: a native process has neither (the game's own threads call
 * Direct3D on the device's thread, as with the real d3d8.dll). Kept so d3d8.c's includes resolve. */
#pragma once
