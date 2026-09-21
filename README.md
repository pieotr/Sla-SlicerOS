# SLA-Slicer

A PrusaSlicer-based SLA application undergoing a major refactor to incorporate
UVtools layer editing, formats, PCB exposure and calibration workflows.
These capabilities are not yet all implemented or verified.

- [User manual (Polish)](manual.txt)
- [SL1 layer editor](doc/LAYER_EDITOR.md)
- [Linux build instructions](install.md)
- [Implementation status and remaining work](doc/REFACTOR_STATUS.md)
- [Format and UVtools operation inventory](doc/COMPATIBILITY_INVENTORY.md)
- [Tests and acceptance checklist](doc/TESTING.md)
- [Validation results](doc/VALIDATION.md)

Build and run the focused checks with:

```sh
bash ./build_linux.sh --jobs 4 --tests --run
bash ./run_prusa_slicer.sh
```
