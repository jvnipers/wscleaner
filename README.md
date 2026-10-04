# CS2 Workshop Cleaner

Metamod plugin that deletes workshop items that are not currently loaded by the server.

### Optional: Workshop Host Map (`wshostmap`)
A second, independent plugin built from the same repository. It lets any player type `!hostmap <workshop id>` (or paste a workshop URL) in chat to host a workshop map, like the server console's `host_workshop_map`. The server answers in chat with the item's last update date and size, reports download progress every `wshostmap_progress_interval` seconds, and changes map once the download finishes.

It is released as its own archive (`wshostmap-windows.zip` / `wshostmap-linux.tar.gz`), separate from `wscleaner`.

### Prerequisites
 * This repository is cloned recursively (ie. has submodules)
 * [python3](https://www.python.org/)
 * [ambuild](https://github.com/alliedmodders/ambuild), make sure ``ambuild`` command is available via the ``PATH`` environment variable;
 * MSVC (VS build tools)/Clang installed for Windows/Linux.

### Setting up
 * ``mkdir build`` & ``cd build`` in the root of the plugin folder.
 * Run ``python3 ../configure.py``.
 * If the process of configuring was successful, you should be able to run ``ambuild`` in the ``\build`` folder to compile the plugin.
 * Once the plugin is compiled the files would be packaged and placed in ``\build\package``, one folder per plugin (``wscleaner``, ``wshostmap``).
 * To run the plugin on the server, copy the contents of a plugin's folder into ``game/csgo``, preserving the layout. Be aware that plugins get loaded either by corresponding ``.vdf`` files (automatic step) in the metamod folder, or by listing them in ``addons/metamod/metaplugins.ini`` file (manual step).
