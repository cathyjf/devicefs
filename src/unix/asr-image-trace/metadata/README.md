# Native signing

`DEVICEFS_ASR_SIGNING_IDENTITY` selects the identity used to sign the whole
bundle. CMake generates `Info.plist` and `entitlements.plist` in the build tree.
The bundle identifier is `com.cathyjf.devicefs.asr-image-trace`.

`DEVICEFS_ASR_SNAPSHOT_ENTITLEMENT` controls the generated entitlements:

- `OFF` (the default) produces an empty entitlement dictionary for supplied-snapshot
  and synthetic-producer runs on a Mac with AMFI enabled.
- `ON` claims `com.apple.developer.vfs.snapshot` for automatic fixture creation
  in an AMFI-disabled macOS VM.
