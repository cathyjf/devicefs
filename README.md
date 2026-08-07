# devicefs

Expose Windows block devices as ordinary files on a virtual filesystem.

This allows an unmodified `proxmox-backup-client` running under WSL1
to back up Windows block devices to a Proxmox Backup Server datastore.
