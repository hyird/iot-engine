# Repository working notes

## Firmware baseline

- Use the canonical `immortalwrt-dtu` checkout for all future TAS-682 firmware
  changes, builds, and releases.
- The designated build checkout is
  `/home/openwrtbuild/immortalwrt-dtu` on `10.10.0.101`; run repository and build
  commands as the `openwrtbuild` user.
- Before changing or building firmware, read and follow any more-specific
  `AGENTS.md` present in the `immortalwrt-dtu` repository.
- Every EdgeNode change must remain backward compatible with deployed firmware,
  including protocol messages, platform APIs, configuration, tasks, and upgrade
  transport. Do not remove an old path until a tested migration and compatibility
  window have been provided and the user has explicitly approved the break.
- Use bounded, resumable WebSocket chunks for firmware that advertises that
  capability, while retaining the tokenized direct-download path for legacy
  firmware that does not advertise it.
