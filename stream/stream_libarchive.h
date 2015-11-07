struct mp_log;

#define MP_ARCHIVE_READ_SIZE 4096
struct mp_archive {
    struct archive *arch;
    char buffer[MP_ARCHIVE_READ_SIZE];
};

void mp_archive_free(struct mp_archive *mpa);

#define MP_ARCHIVE_FLAG_UNSAFE 1
struct mp_archive *mp_archive_new(struct mp_log *log, struct stream *src,
                                  int flags);
