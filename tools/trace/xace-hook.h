/* xace-hook.h — force-included into xAce's z80.c by build-xace.sh, which
 * puts XACE_TRACE() at the top of mainloop's while loop, where the
 * registers are its locals (design.md §13.4).
 *
 * A DD or FD prefix is a pass of xAce's loop of its own, which leaves
 * new_ixoriy set for the next; only a pass that starts an instruction
 * prints. */
#ifndef PICO_ACE_XACE_HOOK_H
#define PICO_ACE_XACE_HOOK_H

void xace_trace(unsigned r_pc, unsigned r_af, unsigned r_bc, unsigned r_de, unsigned r_hl,
                unsigned r_ix, unsigned r_iy, unsigned r_sp);

#define XACE_TRACE()                                                         \
    do {                                                                     \
        if (!new_ixoriy)                                                     \
            xace_trace(pc, (a << 8) | f, (b << 8) | c, (d << 8) | e,         \
                       (h << 8) | l, ix, iy, sp);                            \
    } while (0)

#endif /* PICO_ACE_XACE_HOOK_H */
