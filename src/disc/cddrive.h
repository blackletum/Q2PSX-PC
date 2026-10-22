/*
 * cddrive.h — a physical optical drive as a source of sectors.
 *
 * The image backends in disc.c read a file. This reads the drive itself, so
 * the rest of the port cannot tell a .cue from a disc in the tray: it gets the
 * same track table and the same 2352-byte sectors.
 *
 * Three read strategies, tried once at open in this order, the first that
 * works being kept for the session:
 *
 *   raw       READ CD (MMC 0xBE) for whole 2352-byte sectors of any type —
 *             Windows through SCSI pass-through, Linux through CDROMREADRAW.
 *             This is the one the game wants: XA audio and STR films are
 *             Form 2 and need the subheader.
 *   raw+sub   Windows' own IOCTL_CDROM_RAW_READ in RawWithSubCode mode, for a
 *             handle the pass-through is refused on (2448 bytes; the trailing
 *             96 of subchannel are dropped).
 *   cooked    plain 2048-byte reads, as the OS mounts a data disc. Every Form 1
 *             file reads correctly; Form 2 streams do not, so the music and the
 *             films are lost. Reported, never silent.
 *
 * Supported on Windows and Linux. Elsewhere `cd_drive_list` finds nothing and
 * `cd_drive_open` returns Q2_ERR_UNSUPPORTED.
 */
#ifndef Q2PSX_CDDRIVE_H
#define Q2PSX_CDDRIVE_H

#include "disc.h"

typedef struct cd_drive cd_drive;

typedef enum cd_drive_mode {
    CD_DRIVE_RAW = 0,
    CD_DRIVE_RAW_SUBCODE,
    CD_DRIVE_COOKED
} cd_drive_mode;

/*
 * Whether `path` names a drive rather than a file — "E:", "E:\", "\\.\E:" on
 * Windows, a block device such as "/dev/sr0" on Linux — and if so, the device
 * path to open and a short display name ("E:", "sr0").
 */
bool cd_drive_parse(const char *path, char *device, size_t device_cap,
                    char *name, size_t name_cap);

q2_result cd_drive_open(cd_drive **out, const char *path);
void      cd_drive_close(cd_drive *dr);

cd_drive_mode cd_drive_read_mode(const cd_drive *dr);
const char   *cd_drive_mode_name(cd_drive_mode mode);

/*
 * The disc's table of contents as cd_tracks: absolute LBAs, every track
 * 2352 bytes a sector, file_index -1. Fails on an empty tray.
 */
q2_result cd_drive_read_toc(cd_drive *dr, cd_track *tracks, int max,
                            int *count);

/* One sector as 2352 raw bytes. A cooked drive synthesises the Mode 2 Form 1
 * header and subheader around the 2048 it can read, as a cooked image does. */
q2_result cd_drive_read_raw(cd_drive *dr, u32 lba, u8 *out);

/* ------------------------------------------------------------------------- */
/* Enumeration, for the launcher                                              */
/* ------------------------------------------------------------------------- */
#define CD_DRIVE_LIST_MAX 16

typedef struct cd_drive_info {
    char path[64];        /* what disc_open takes: "E:" or "/dev/sr0"      */
    char name[32];        /* "E:" or "sr0"                                  */
    char label[64];       /* the volume label, "" when unknown              */
    bool has_media;       /* a readable disc is in the tray                 */
} cd_drive_info;

/* Fill `out` with up to `max` optical drives; returns how many. Cheap enough
 * to call when the player asks for a refresh, not every frame. */
int cd_drive_list(cd_drive_info *out, int max);

#endif /* Q2PSX_CDDRIVE_H */
