#pragma once
// The two files' versions, each its own: the DLL's goes up with every update, the exe's only when it has to change.
// Read by the .rc files (their version resource, which signing and Explorer show) and by the code.

#define VCAPP_EXE_VERSION_NUM 0, 1, 0, 0
#define VCAPP_EXE_VERSION_STR "0.1.0"
#define VCAPP_DLL_VERSION_NUM 0, 1, 0, 0
#define VCAPP_DLL_VERSION_STR "0.1.0"

// Where the App's releases are (app-v<version>, .github/workflows/app.yaml): the splash's update check asks it
#define VCAPP_GITHUB_REPO L"CatsAndBoats/VanaCore"
