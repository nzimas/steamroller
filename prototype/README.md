# Offline FM bridge prototype

`fm_staging.py` is the executable reference protocol for a two-slot MCU→DSP FM staging bridge. `fm_copy_block.asm` is a new 32-word DSP payload-copy kernel. Neither is installed on the connected instrument.

Read [the staging specification](../docs/FM-STAGING-PROTOTYPE.md) for ownership, late-block policy, fixtures and limitations. Run `.venv/bin/python tools/fm_staging_probe.py` from the project root after building the local DSP analysis tool.
