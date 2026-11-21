# plugins/vanacore

Ashita plugins a distribution of VanaCore ships with it. VanaCore loads a plugin from `plugins\vanacore` before a
same-named one in `plugins\ashita`, and the launcher's Ashita mirror never touches this folder.

Nothing here is part of VanaCore's source, and the folder is empty in the repository: plugins are their makers', under
their makers' licences. To ship some, put their DLLs here before running `build-release.bat`, which packages whatever
is here besides this file.
