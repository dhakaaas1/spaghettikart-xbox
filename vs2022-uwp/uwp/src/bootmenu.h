#ifndef BOOTMENU_H
#define BOOTMENU_H

namespace bootmenu {
// Boot menu that handles ROM selection and asset extraction before game
// initialization. Returns true if boot should continue, false if the user
// cancelled or extraction failed and they chose to exit.
bool BootSelect(void* wnd, int w, int h);
} // namespace bootmenu

#endif // BOOTMENU_H
