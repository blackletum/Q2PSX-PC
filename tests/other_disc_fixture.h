/*
 * other_disc_fixture.h -- a PlayStation disc that is not Quake II.
 *
 * A 2048-byte-sector ISO 9660 volume holding one file, SYSTEM.CNF, that boots
 * SLUS_999.99. q2_identify accepts it -- it has a serial -- but it has no
 * Q2DATA/LEVELS tree, so neither the client nor the selector may treat it as
 * a playable "uncatalogued build". Include after disc.h and q2psx.h.
 */
#ifndef Q2_TEST_OTHER_DISC_FIXTURE_H
#define Q2_TEST_OTHER_DISC_FIXTURE_H

#include <stdio.h>
#include <string.h>

static bool write_other_psx_disc(const char *path)
{
    static const char cnf[] = "BOOT = cdrom:\\SLUS_999.99;1\r\nTCB = 4\r\n";
    u8 sector[CD_SECTOR_FORM1];
    FILE *fp = fopen(path, "wb");
    u32 lba;
    bool ok = true;

    if (!fp)
        return false;
    for (lba = 0; lba < 20; lba++) {
        memset(sector, 0, sizeof(sector));
        if (lba == 16) {                                   /* the PVD       */
            sector[0] = 1;
            memcpy(sector + 1, "CD001", 5);
            sector[6] = 1;
            q2_wr_u32(sector + 80, 20);                    /* volume size   */
            sector[156] = 34;                              /* root record   */
            q2_wr_u32(sector + 156 + 2, 17);
            q2_wr_u32(sector + 156 + 10, CD_SECTOR_FORM1);
            sector[156 + 25] = 2;
            sector[156 + 32] = 1;
        } else if (lba == 17) {                            /* the root      */
            sector[0] = 33 + 12 + 1;
            q2_wr_u32(sector + 2, 18);
            q2_wr_u32(sector + 10, (u32)(sizeof(cnf) - 1));
            sector[32] = 12;
            memcpy(sector + 33, "SYSTEM.CNF;1", 12);
        } else if (lba == 18) {                            /* SYSTEM.CNF    */
            memcpy(sector, cnf, sizeof(cnf) - 1);
        }
        ok &= fwrite(sector, 1, sizeof(sector), fp) == sizeof(sector);
    }
    return (fclose(fp) == 0) && ok;
}

#endif
