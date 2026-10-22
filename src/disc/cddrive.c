/*
 * cddrive.c — sectors straight off an optical drive. See cddrive.h.
 *
 * pread and the Linux CD-ROM ioctls are POSIX and kernel interfaces, not C11,
 * and this project compiles as strict C11; disc.c explains the same macro.
 */
#if defined(__linux__)
#  define _POSIX_C_SOURCE 200809L
#  define _DEFAULT_SOURCE 1
#endif

#include "cddrive.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <winioctl.h>
#  include <ntddcdrm.h>
#  include <ntddscsi.h>
#elif defined(__linux__)
#  include <errno.h>
#  include <fcntl.h>
#  include <limits.h>        /* CDSL_CURRENT is INT_MAX */
#  include <linux/cdrom.h>
#  include <sys/ioctl.h>
#  include <sys/stat.h>
#  include <unistd.h>
#endif

/* Sectors per device request. A level file is read front to back, so one
 * request serves the next fifteen sector reads from memory. */
#define CD_DRIVE_CHUNK      16u
/* RawWithSubCode's stride: the sector and 96 bytes of raw subchannel. */
#define CD_SECTOR_RAW_SUB   2448u
/* TRACK_MODE_TYPE's RawWithSubCode (ntddcdrm.h, Windows 7 and later), named
 * by value so an older SDK's header still builds. */
#define CD_TRACK_MODE_RAW_WITH_SUBCODE 5
#define CD_LBA_OFFSET       150   /* MSF 00:02:00 is LBA 0 */

struct cd_drive {
#if defined(_WIN32)
    HANDLE h;
#elif defined(__linux__)
    int    fd;
#endif
    cd_drive_mode mode;
    u8   *buf;          /* CD_DRIVE_CHUNK sectors of CD_SECTOR_RAW           */
    u8   *scratch;      /* the device's own stride, before it is repacked    */
    u32   buf_lba;
    u32   buf_count;
    u32   end_lba;      /* the lead-out once the TOC is read; 0 = not known  */
};

static const u8 k_sync[12] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
};

/* The two platforms with a drive backend share these; elsewhere nothing
 * would call them. */
#if defined(_WIN32) || defined(__linux__)
static void copy_str(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (!dst || cap == 0)
        return;
    n = src ? strlen(src) : 0;
    if (n >= cap)
        n = cap - 1;
    if (n)
        memcpy(dst, src, n);
    dst[n] = '\0';
}

/* The Mode 2 Form 1 frame a cooked read is wrapped in — the same synthesis
 * disc.c gives a 2048-byte image. */
static void cooked_to_raw(const u8 *cooked, u32 lba, u8 *out)
{
    u32 msf = lba + CD_LBA_OFFSET;
    u32 m = msf / (60u * 75u), s = (msf / 75u) % 60u, f = msf % 75u;

    memset(out, 0, CD_SECTOR_RAW);
    memcpy(out, k_sync, sizeof(k_sync));
    out[12] = (u8)(((m / 10u) << 4) | (m % 10u));
    out[13] = (u8)(((s / 10u) << 4) | (s % 10u));
    out[14] = (u8)(((f / 10u) << 4) | (f % 10u));
    out[15] = 0x02;
    out[18] = out[22] = (u8)CD_SUBMODE_DATA;
    memcpy(out + 24, cooked, CD_SECTOR_FORM1);
}
#endif

const char *cd_drive_mode_name(cd_drive_mode mode)
{
    switch (mode) {
    case CD_DRIVE_RAW:         return "raw";
    case CD_DRIVE_RAW_SUBCODE: return "raw with subchannel";
    case CD_DRIVE_COOKED:      return "cooked";
    }
    return "?";
}

cd_drive_mode cd_drive_read_mode(const cd_drive *dr)
{
    return dr ? dr->mode : CD_DRIVE_COOKED;
}

/* ========================================================================= */
#if defined(_WIN32)
/* ========================================================================= */

bool cd_drive_parse(const char *path, char *device, size_t device_cap,
                    char *name, size_t name_cap)
{
    char letter;
    const char *p = path;
    char buf[16];

    if (!path)
        return false;

    /* "\\.\E:" and "//./E:" */
    if ((p[0] == '\\' || p[0] == '/') && (p[1] == '\\' || p[1] == '/') &&
        p[2] == '.' && (p[3] == '\\' || p[3] == '/'))
        p += 4;

    letter = p[0];
    if (letter >= 'a' && letter <= 'z')
        letter = (char)(letter - 'a' + 'A');
    if (letter < 'A' || letter > 'Z' || p[1] != ':')
        return false;
    if (p[2] != '\0' && !((p[2] == '\\' || p[2] == '/') && p[3] == '\0'))
        return false;

    snprintf(buf, sizeof(buf), "\\\\.\\%c:", letter);
    copy_str(device, device_cap, buf);
    snprintf(buf, sizeof(buf), "%c:", letter);
    copy_str(name, name_cap, buf);
    return true;
}

static bool win_read_cd(HANDLE h, u32 lba, u32 count, u8 *out)
{
    struct {
        SCSI_PASS_THROUGH_DIRECT sptd;
        ULONG                    pad;
        UCHAR                    sense[32];
    } s;
    DWORD got = 0;

    memset(&s, 0, sizeof(s));
    s.sptd.Length             = sizeof(SCSI_PASS_THROUGH_DIRECT);
    s.sptd.CdbLength          = 12;
    s.sptd.SenseInfoLength    = sizeof(s.sense);
    s.sptd.DataIn             = SCSI_IOCTL_DATA_IN;
    s.sptd.DataTransferLength = count * CD_SECTOR_RAW;
    s.sptd.TimeOutValue       = 30;
    s.sptd.DataBuffer         = out;
    s.sptd.SenseInfoOffset    = (ULONG)((u8 *)s.sense - (u8 *)&s);
    s.sptd.Cdb[0] = 0xBE;                  /* READ CD                         */
    s.sptd.Cdb[1] = 0x00;                  /* any sector type                 */
    s.sptd.Cdb[2] = (UCHAR)(lba >> 24);
    s.sptd.Cdb[3] = (UCHAR)(lba >> 16);
    s.sptd.Cdb[4] = (UCHAR)(lba >> 8);
    s.sptd.Cdb[5] = (UCHAR)lba;
    s.sptd.Cdb[6] = (UCHAR)(count >> 16);
    s.sptd.Cdb[7] = (UCHAR)(count >> 8);
    s.sptd.Cdb[8] = (UCHAR)count;
    s.sptd.Cdb[9] = 0xF8;                  /* sync, headers, data, EDC/ECC   */

    if (!DeviceIoControl(h, IOCTL_SCSI_PASS_THROUGH_DIRECT, &s, sizeof(s),
                         &s, sizeof(s), &got, NULL))
        return false;
    return s.sptd.ScsiStatus == 0 &&
           s.sptd.DataTransferLength == count * CD_SECTOR_RAW;
}

static bool win_raw_read_sub(HANDLE h, u32 lba, u32 count, u8 *scratch,
                             u8 *out)
{
    RAW_READ_INFO rri;
    DWORD got = 0;
    u32 i;

    memset(&rri, 0, sizeof(rri));
    rri.DiskOffset.QuadPart = (LONGLONG)lba * CD_SECTOR_MODE1;
    rri.SectorCount         = count;
    rri.TrackMode           = (TRACK_MODE_TYPE)CD_TRACK_MODE_RAW_WITH_SUBCODE;

    if (!DeviceIoControl(h, IOCTL_CDROM_RAW_READ, &rri, sizeof(rri),
                         scratch, count * CD_SECTOR_RAW_SUB, &got, NULL) ||
        got < count * CD_SECTOR_RAW_SUB)
        return false;
    for (i = 0; i < count; i++)
        memcpy(out + (size_t)i * CD_SECTOR_RAW,
               scratch + (size_t)i * CD_SECTOR_RAW_SUB, CD_SECTOR_RAW);
    return true;
}

static bool win_read_cooked(HANDLE h, u32 lba, u32 count, u8 *out)
{
    LARGE_INTEGER at;
    DWORD got = 0;

    at.QuadPart = (LONGLONG)lba * CD_SECTOR_MODE1;
    if (!SetFilePointerEx(h, at, NULL, FILE_BEGIN))
        return false;
    return ReadFile(h, out, count * CD_SECTOR_MODE1, &got, NULL) &&
           got == count * CD_SECTOR_MODE1;
}

static bool dev_read(cd_drive *dr, cd_drive_mode mode, u32 lba, u32 count,
                     u8 *out)
{
    u32 i;

    switch (mode) {
    case CD_DRIVE_RAW:
        return win_read_cd(dr->h, lba, count, out);
    case CD_DRIVE_RAW_SUBCODE:
        return win_raw_read_sub(dr->h, lba, count, dr->scratch, out);
    case CD_DRIVE_COOKED:
        if (!win_read_cooked(dr->h, lba, count, dr->scratch))
            return false;
        for (i = 0; i < count; i++)
            cooked_to_raw(dr->scratch + (size_t)i * CD_SECTOR_MODE1, lba + i,
                          out + (size_t)i * CD_SECTOR_RAW);
        return true;
    }
    return false;
}

static void *page_alloc(size_t size)
{
    /* Pass-through and unbuffered device reads want an aligned buffer, and a
     * page satisfies every adapter's alignment mask. */
    return VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

static void page_free(void *p)
{
    if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

static bool dev_open(cd_drive *dr, const char *device, const char *name)
{
    char root[8];
    DWORD old_mode = 0;

    snprintf(root, sizeof(root), "%s\\", name);
    if (GetDriveTypeA(root) != DRIVE_CDROM) {
        Q2_ERROR("%s is not an optical drive", name);
        return false;
    }

    /* Read-write first: the pass-through needs it. Windows grants it to an
     * ordinary user on an optical drive, but fall back to read-only, which
     * still has the raw-read ioctl and plain reads. */
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    dr->h = CreateFileA(device, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                        OPEN_EXISTING, 0, NULL);
    if (dr->h == INVALID_HANDLE_VALUE)
        dr->h = CreateFileA(device, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    SetThreadErrorMode(old_mode, NULL);

    if (dr->h == INVALID_HANDLE_VALUE) {
        Q2_ERROR("cannot open %s (error %lu)", name,
                 (unsigned long)GetLastError());
        return false;
    }

    dr->buf     = (u8 *)page_alloc((size_t)CD_DRIVE_CHUNK * CD_SECTOR_RAW);
    dr->scratch = (u8 *)page_alloc((size_t)CD_DRIVE_CHUNK * CD_SECTOR_RAW_SUB);
    return dr->buf && dr->scratch;
}

static void dev_close(cd_drive *dr)
{
    if (dr->h && dr->h != INVALID_HANDLE_VALUE)
        CloseHandle(dr->h);
    page_free(dr->buf);
    page_free(dr->scratch);
}

q2_result cd_drive_read_toc(cd_drive *dr, cd_track *tracks, int max,
                            int *count)
{
    CDROM_TOC toc;
    DWORD got = 0;
    int first, last, i, n = 0;

    if (count)
        *count = 0;
    if (!dr || !tracks || max <= 0)
        return Q2_ERR_INVALID_ARG;

    memset(&toc, 0, sizeof(toc));
    if (!DeviceIoControl(dr->h, IOCTL_CDROM_READ_TOC, NULL, 0, &toc,
                         sizeof(toc), &got, NULL))
        return Q2_ERR_IO;

    first = toc.FirstTrack;
    last  = toc.LastTrack;
    if (first < 1 || last < first || last - first + 1 > MAXIMUM_NUMBER_TRACKS - 1)
        return Q2_ERR_BAD_FORMAT;

    /* TrackData holds the tracks in order and then the lead-out (0xAA). */
    for (i = 0; i <= last - first && n < max; i++) {
        const TRACK_DATA *td = &toc.TrackData[i];
        const TRACK_DATA *nx = &toc.TrackData[i + 1];
        u32 start = ((u32)td->Address[1] * 60u + td->Address[2]) * 75u +
                    td->Address[3] - CD_LBA_OFFSET;
        u32 next  = ((u32)nx->Address[1] * 60u + nx->Address[2]) * 75u +
                    nx->Address[3] - CD_LBA_OFFSET;
        cd_track *t = &tracks[n++];

        memset(t, 0, sizeof(*t));
        t->number         = td->TrackNumber;
        t->type           = (td->Control & 0x04) ? CD_TRACK_DATA
                                                 : CD_TRACK_AUDIO;
        t->start_lba      = start;
        t->pregap_lba     = UINT32_MAX;
        t->length_sectors = next > start ? next - start : 0;
        t->sector_size    = CD_SECTOR_RAW;
        t->file_index     = -1;
        dr->end_lba       = next;
    }

    if (count)
        *count = n;
    return n ? Q2_OK : Q2_ERR_NOT_FOUND;
}

int cd_drive_list(cd_drive_info *out, int max)
{
    DWORD mask = GetLogicalDrives();
    DWORD old_mode = 0;
    int n = 0, i;

    if (!out || max <= 0)
        return 0;

    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    for (i = 0; i < 26 && n < max; i++) {
        char root[8];
        char label[MAX_PATH + 1];
        cd_drive_info *info;

        snprintf(root, sizeof(root), "%c:\\", 'A' + i);
        if (!(mask & (1u << i)) || GetDriveTypeA(root) != DRIVE_CDROM)
            continue;

        info = &out[n++];
        memset(info, 0, sizeof(*info));
        snprintf(info->path, sizeof(info->path), "%c:", 'A' + i);
        snprintf(info->name, sizeof(info->name), "%c:", 'A' + i);
        label[0] = '\0';
        info->has_media = GetVolumeInformationA(root, label, sizeof(label),
                                                NULL, NULL, NULL, NULL, 0) != 0;
        copy_str(info->label, sizeof(info->label), label);
    }
    SetThreadErrorMode(old_mode, NULL);
    return n;
}

/* ========================================================================= */
#elif defined(__linux__)
/* ========================================================================= */

bool cd_drive_parse(const char *path, char *device, size_t device_cap,
                    char *name, size_t name_cap)
{
    struct stat st;
    const char *base;

    if (!path || strncmp(path, "/dev/", 5) != 0)
        return false;
    if (stat(path, &st) != 0 || !S_ISBLK(st.st_mode))
        return false;

    base = strrchr(path, '/');
    copy_str(device, device_cap, path);
    copy_str(name, name_cap, base ? base + 1 : path);
    return true;
}

static bool lx_read_raw(int fd, u32 lba, u32 count, u8 *out)
{
    u32 i;

    for (i = 0; i < count; i++) {
        u8 *sector = out + (size_t)i * CD_SECTOR_RAW;
        struct cdrom_msf msf;
        u32 a = lba + i + CD_LBA_OFFSET;

        memset(&msf, 0, sizeof(msf));
        msf.cdmsf_min0   = (u8)(a / (60u * 75u));
        msf.cdmsf_sec0   = (u8)((a / 75u) % 60u);
        msf.cdmsf_frame0 = (u8)(a % 75u);
        memcpy(sector, &msf, sizeof(msf));
        if (ioctl(fd, CDROMREADRAW, sector) != 0)
            return false;
    }
    return true;
}

static bool lx_read_cooked(int fd, u32 lba, u32 count, u8 *out)
{
    size_t want = (size_t)count * CD_SECTOR_MODE1;
    ssize_t got = pread(fd, out, want, (off_t)lba * CD_SECTOR_MODE1);

    return got == (ssize_t)want;
}

static bool dev_read(cd_drive *dr, cd_drive_mode mode, u32 lba, u32 count,
                     u8 *out)
{
    u32 i;

    switch (mode) {
    case CD_DRIVE_RAW:
        return lx_read_raw(dr->fd, lba, count, out);
    case CD_DRIVE_RAW_SUBCODE:
        return false;           /* a Windows ioctl; Linux has no such arm */
    case CD_DRIVE_COOKED:
        if (!lx_read_cooked(dr->fd, lba, count, dr->scratch))
            return false;
        for (i = 0; i < count; i++)
            cooked_to_raw(dr->scratch + (size_t)i * CD_SECTOR_MODE1, lba + i,
                          out + (size_t)i * CD_SECTOR_RAW);
        return true;
    }
    return false;
}

static bool dev_open(cd_drive *dr, const char *device, const char *name)
{
    dr->fd = open(device, O_RDONLY | O_NONBLOCK);
    if (dr->fd < 0) {
        Q2_ERROR("cannot open %s: %s", name, strerror(errno));
        return false;
    }
    dr->buf     = (u8 *)malloc((size_t)CD_DRIVE_CHUNK * CD_SECTOR_RAW);
    dr->scratch = (u8 *)malloc((size_t)CD_DRIVE_CHUNK * CD_SECTOR_RAW_SUB);
    return dr->buf && dr->scratch;
}

static void dev_close(cd_drive *dr)
{
    if (dr->fd >= 0)
        close(dr->fd);
    free(dr->buf);
    free(dr->scratch);
}

q2_result cd_drive_read_toc(cd_drive *dr, cd_track *tracks, int max,
                            int *count)
{
    struct cdrom_tochdr hdr;
    int t, n = 0;

    if (count)
        *count = 0;
    if (!dr || !tracks || max <= 0)
        return Q2_ERR_INVALID_ARG;
    if (ioctl(dr->fd, CDROMREADTOCHDR, &hdr) != 0)
        return Q2_ERR_IO;

    for (t = hdr.cdth_trk0; t <= hdr.cdth_trk1 && n < max; t++) {
        struct cdrom_tocentry e, nx;
        cd_track *tr;

        memset(&e, 0, sizeof(e));
        memset(&nx, 0, sizeof(nx));
        e.cdte_track   = (u8)t;
        e.cdte_format  = CDROM_LBA;
        nx.cdte_track  = (u8)(t == hdr.cdth_trk1 ? CDROM_LEADOUT : t + 1);
        nx.cdte_format = CDROM_LBA;
        if (ioctl(dr->fd, CDROMREADTOCENTRY, &e) != 0 ||
            ioctl(dr->fd, CDROMREADTOCENTRY, &nx) != 0)
            return Q2_ERR_IO;

        tr = &tracks[n++];
        memset(tr, 0, sizeof(*tr));
        tr->number         = t;
        tr->type           = (e.cdte_ctrl & CDROM_DATA_TRACK) ? CD_TRACK_DATA
                                                              : CD_TRACK_AUDIO;
        tr->start_lba      = (u32)e.cdte_addr.lba;
        tr->pregap_lba     = UINT32_MAX;
        tr->length_sectors = nx.cdte_addr.lba > e.cdte_addr.lba
                           ? (u32)(nx.cdte_addr.lba - e.cdte_addr.lba) : 0u;
        tr->sector_size    = CD_SECTOR_RAW;
        tr->file_index     = -1;
        dr->end_lba        = (u32)nx.cdte_addr.lba;
    }

    if (count)
        *count = n;
    return n ? Q2_OK : Q2_ERR_NOT_FOUND;
}

int cd_drive_list(cd_drive_info *out, int max)
{
    int n = 0, i;

    if (!out || max <= 0)
        return 0;

    for (i = 0; i < 16 && n < max; i++) {
        char dev[32], name[16];
        cd_drive_info *info;
        int fd;

        snprintf(dev, sizeof(dev), "/dev/sr%d", i);
        if (!cd_drive_parse(dev, NULL, 0, name, sizeof(name)))
            continue;

        info = &out[n++];
        memset(info, 0, sizeof(*info));
        copy_str(info->path, sizeof(info->path), dev);
        copy_str(info->name, sizeof(info->name), name);
        fd = open(dev, O_RDONLY | O_NONBLOCK);
        if (fd >= 0) {
            info->has_media = ioctl(fd, CDROM_DRIVE_STATUS, CDSL_CURRENT) ==
                              CDS_DISC_OK;
            close(fd);
        }
    }
    return n;
}

/* ========================================================================= */
#else
/* ========================================================================= */

bool cd_drive_parse(const char *path, char *device, size_t device_cap,
                    char *name, size_t name_cap)
{
    return false;
}

static bool dev_read(cd_drive *dr, cd_drive_mode mode, u32 lba, u32 count,
                     u8 *out)
{
    return false;
}

static bool dev_open(cd_drive *dr, const char *device, const char *name)
{
    Q2_ERROR("optical drives are not supported on this platform");
    return false;
}

static void dev_close(cd_drive *dr)
{
    free(dr->buf);
    free(dr->scratch);
}

q2_result cd_drive_read_toc(cd_drive *dr, cd_track *tracks, int max,
                            int *count)
{
    if (count)
        *count = 0;
    return Q2_ERR_UNSUPPORTED;
}

int cd_drive_list(cd_drive_info *out, int max)
{
    return 0;
}

#endif

/* ------------------------------------------------------------------------- */
/* Portable                                                                   */
/* ------------------------------------------------------------------------- */

/* Which strategy this drive answers: the PVD at LBA 16 is on every data disc,
 * and a raw read of it has to come back with the sync pattern in front. */
static bool probe(cd_drive *dr, cd_drive_mode mode)
{
    if (!dev_read(dr, mode, 16, 1, dr->buf))
        return false;
    if (memcmp(dr->buf, k_sync, sizeof(k_sync)) != 0)
        return false;
    return memcmp(dr->buf + (dr->buf[15] == 2 ? 25 : 17), "CD001", 5) == 0;
}

q2_result cd_drive_open(cd_drive **out, const char *path)
{
    char device[64], name[32];
    cd_drive *dr;

    if (!out || !path)
        return Q2_ERR_INVALID_ARG;
    *out = NULL;

    if (!cd_drive_parse(path, device, sizeof(device), name, sizeof(name)))
        return Q2_ERR_UNSUPPORTED;

    dr = (cd_drive *)calloc(1, sizeof(*dr));
    if (!dr)
        return Q2_ERR_NO_MEMORY;
#if defined(__linux__)
    dr->fd = -1;
#endif

    if (!dev_open(dr, device, name)) {
        dev_close(dr);
        free(dr);
        return Q2_ERR_IO;
    }

    if (probe(dr, CD_DRIVE_RAW))
        dr->mode = CD_DRIVE_RAW;
    else if (probe(dr, CD_DRIVE_RAW_SUBCODE))
        dr->mode = CD_DRIVE_RAW_SUBCODE;
    else if (probe(dr, CD_DRIVE_COOKED))
        dr->mode = CD_DRIVE_COOKED;
    else {
        Q2_ERROR("drive %s has no readable data disc in it", name);
        dev_close(dr);
        free(dr);
        return Q2_ERR_NOT_FOUND;
    }

    dr->buf_lba   = UINT32_MAX;
    dr->buf_count = 0;
    Q2_INFO("drive %s reads %s sectors", name, cd_drive_mode_name(dr->mode));
    if (dr->mode == CD_DRIVE_COOKED)
        Q2_WARN("drive %s will not hand over raw sectors, so the music "
                "and the films (Form 2) cannot be read from it", name);

    *out = dr;
    return Q2_OK;
}

void cd_drive_close(cd_drive *dr)
{
    if (!dr)
        return;
    dev_close(dr);
    free(dr);
}

q2_result cd_drive_read_raw(cd_drive *dr, u32 lba, u8 *out)
{
    u32 count;

    if (!dr || !out)
        return Q2_ERR_INVALID_ARG;

    if (dr->buf_count && lba >= dr->buf_lba &&
        lba < dr->buf_lba + dr->buf_count) {
        memcpy(out, dr->buf + (size_t)(lba - dr->buf_lba) * CD_SECTOR_RAW,
               CD_SECTOR_RAW);
        return Q2_OK;
    }

    count = CD_DRIVE_CHUNK;
    if (dr->end_lba) {
        if (lba >= dr->end_lba)
            return Q2_ERR_RANGE;
        if (dr->end_lba - lba < count)
            count = dr->end_lba - lba;
    }

    /* A run that crosses into an audio track or a bad patch fails as a whole;
     * the one sector asked for may still read on its own. */
    if (!dev_read(dr, dr->mode, lba, count, dr->buf)) {
        count = 1;
        if (!dev_read(dr, dr->mode, lba, 1, dr->buf)) {
            dr->buf_count = 0;
            return Q2_ERR_IO;
        }
    }

    dr->buf_lba   = lba;
    dr->buf_count = count;
    memcpy(out, dr->buf, CD_SECTOR_RAW);
    return Q2_OK;
}
