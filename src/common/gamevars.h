/*
 * gamevars.h — the GAME VARIABLES word, which two layers need.
 *
 * The pause menu's GAME VARIABLES page writes seven settings into the shared
 * interface block at 0x800B335A…0x800B336C, and 0x8001C698 folds four of the
 * toggles into a single halfword of cheat bits at 0x800B29EC. The MENU owns
 * writing that word; the GAME reads it — the item dispatch tests bit 0 before
 * deciding how much ammo a weapon pickup grants (0x800363C8), and the movement
 * and damage paths test the others.
 *
 * So the bits belong to neither layer, and they live here rather than being
 * declared twice with a comment hoping the two stay in step.
 *
 * WEAPON STAY (0x800B3360) is deliberately not here: it is read directly as its
 * own halfword rather than folded into this mask (0x80037E60, 0x8005988C).
 */
#ifndef Q2PSX_GAMEVARS_H
#define Q2PSX_GAMEVARS_H

#define Q2_CHEAT_INFINITE_AMMO   0x01u  /* 0x8001C724                         */
#define Q2_CHEAT_ALL_WEAPONS     0x20u  /* 0x8001C74C                         */
#define Q2_CHEAT_NO_FALL_DAMAGE  0x40u  /* 0x8001C704, set when FALLING
                                         * DAMAGE is *off*                    */
#define Q2_CHEAT_ONE_SHOT_KILL   0x80u  /* 0x8001C774                         */

#endif /* Q2PSX_GAMEVARS_H */
