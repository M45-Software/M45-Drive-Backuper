#!/bin/bash
# Sanitized, executable fixture of the supported system job. Keep its control
# flow aligned with the job adapter; the harness redirects resources, not logic.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
uuid=aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee
mountpoint=/media/example/Backup
exec 9>/run/lock/local-system-backup.lock
flock -n 9 || exit 0
if [[ $(findmnt -rn -M "$mountpoint" -o UUID) != "$uuid" ]]; then
    echo 'Backup failed: expected backup filesystem is not mounted.' >&2
    exit 1
fi
cd "$mountpoint"
[[ $(findmnt -rn -T . -o UUID) == "$uuid" ]] || exit 1
export BORG_REPO="$mountpoint/repository"
export BORG_CACHE_DIR=/var/cache/local-home-backup
export BORG_SECURITY_DIR=/var/lib/local-home-backup/security
export BORG_UNKNOWN_UNENCRYPTED_REPO_ACCESS_IS_OK=yes
[[ -f "$BORG_REPO/config" ]] || { echo 'Backup repository has not been initialized.' >&2; exit 1; }
# Reclaim abandoned transaction data before pruning needs space for metadata.
borg compact --verbose
cleanup() {
    borg prune --list --glob-archives 'home-*' \
        --keep-hourly 4 --keep-daily 7 --keep-weekly 4 --keep-monthly 6
    borg compact --verbose
}
cleanup
available=$(df -B1 --output=avail . | tail -1 | tr -d ' ')
(( available > 10737418240 )) || { echo 'Less than 10 GiB free after cleanup; backup stopped.' >&2; exit 1; }
backup_rc=0
borg create --stats --show-rc --compression zstd,3 --one-file-system \
    --exclude-from /etc/local-system-backup.exclude --exclude-caches \
    --checkpoint-interval 600 \
    '::home-{now:%Y-%m-%dT%H:%M:%S}' /home/example || backup_rc=$?
# The actual adapter must upgrade this inherited warning policy before running.
if (( backup_rc <= 1 )); then
    cleanup
    if (( backup_rc == 1 )); then
        echo "Backup completed with Borg warnings; see the journal for details." >&2
        backup_rc=0
    fi
fi
exit "$backup_rc"
