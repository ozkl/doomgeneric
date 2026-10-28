// ipxsetup's screens, in the terminal: a blue setup menu (the game, map,
// skill, how to connect...) and the box shown while it looks for the other
// players, as IPXSETUP.EXE's. See setup_ui.cpp.
#pragma once
#include <string>
#include <vector>

// A terminal to draw on (stdin and stdout are one, and no -text).
bool ui_available(int argc, char** argv);

// The menu. True with the command line to run (args[0] the program's
// name); false: the player quit. `note`: shown at the bottom (what went
// wrong last time), or empty.
bool ui_menu(std::vector<std::string>& args, const std::string& note);

// The box, around the search: until ui_search_end, what the driver prints
// is kept (its last line shown in the box) and Esc gives up. wanted: the
// number of players.
void ui_search_begin(int wanted, const std::string& how);
// Back to the plain terminal. ok: the players were found (the box says so
// for a moment). Returns what the driver printed; the last lines of it
// on failure, for the menu.
std::string ui_search_end(bool ok, int console, int players);
