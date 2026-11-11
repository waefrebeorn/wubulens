/*
 * tui.h -- the WuBu lens interface.
 *
 * A terminal interface in the WuBu style: one screen, a story line at the
 * top, the numbers below it, and tabs across the middle. It owns no data --
 * it reads a Snapshot and draws it.
 *
 *   wubulens            the interface
 *   wubulens --demo     the interface against a scripted card
 *
 * Keys: Tab / Shift-Tab or h/l change tab, j/k or arrows move, q quits,
 *       ? shows the key list.
 *
 * C11, minimal includes.
 */
#ifndef WUBULENS_TUI_H
#define WUBULENS_TUI_H

/* Run the interface. Returns the process exit status. */
int wubulens_tui(int demo);

#endif /* WUBULENS_TUI_H */
