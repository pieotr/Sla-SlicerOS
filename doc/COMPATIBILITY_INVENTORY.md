# Format and operation inventory

This is a source inventory, not a compatibility certificate. Writer presence
means an implementation is registered, not that its bytes have been validated.
All device/firmware validation remains pending. The reference is the local
UVtools source snapshot; no claim about the latest upstream version is made.

## Current export status (2026-10-07)

SL1S is now a distinct printer-persistent GUI export choice using native SL1
schema. 32 source-checked exports/edits, 96 masks and 32 original roundtrips
pass. GUI preset CRUD/restart, sidebar format switching/popup and edited
export pass; two final GUI roundtrips exact. Shared SL1/SL1S publication is
atomic and invalid layer heights preserve the existing output. Klipper ZIP/
RGB.ZIP still need their own schema; legacy SL1 aliases are not support.
SL1S evidence and limits.

The local original UVtools/Emgu/OpenCV oracle fully decodes every mask.
The current audit writes **78 IDs** from **78 registered variants**:
**76 fully decoded**, two known decoder exceptions (SL1SVG and CXDLP v1),
zero entries without export. This is neither every UVtools extension/version
nor certification of hardware or every setting.

Extension-specific version audit corrected all five wrong Anycubic defaults.
PM5/PM3N/PX6S use 517; PM5S/M5SP now have native 518, eleven tables and
two RGB565 previews. 20 files/60 masks and 24 stored color previews source
verified; original Roundtrip and two GUI exports pass. The previous default-only inventory had 14 extension rows
with missing versions. 516/517/518 now share a version-aware native writer with EXTRA/TSMC,
correct table sizes, max version and HEADER pitch. 28 source fixtures,
14 original-setter pairs and GUI 516/517 pass. Printer-persistent version selection now covers all 27 advertised modern
extension/version pairs. Source checks and 27 original re-encode pairs pass;
GUI PWC 518/PM5S Automatic CRUD/restart and PWC 518/DL2P 516 export pass.
Legacy 1/515 now has validated tables/ColorTable and direct atomic mask
streaming: 48 files/144 masks source checked, 48 original roundtrip pairs,
three GUI exports and roundtrips pass. Combined native evidence covers all
51 Anycubic extension/version pairs: zero version-gap rows or unexpected
versions. PWC 1/515 explicitly reports missing physical geometry in GUI.
Anycubic import/per-layer, aliases, full parameters and device evidence
remain open. Klipper ZIP/RGB.ZIP lack archive evidence; SL1S is covered in the newer stage.
Legacy evidence and raw statistics differences.
Version selection evidence.
Modern writer evidence. TSMC evidence.
Full Open does not close E1/E2. See 518 evidence
and version inventory.

Native editor import/save now covers CTB/CBDDLP/Photon v1/v2, including
seeded CTB. Original files: 136 imports / 544 masks and exact byte copies.
Editing: 72 file pairs / 280 masks independently verified; five GUI workflows
and CTest 9/9 pass. This does not cover newer binary readers or compaction.
See Chitubox editor, including retained original
getter/statistics differences for variable Z.

CTB v1/v2 now write both RGB15 previews with correct table/data addresses.
Four native samples and two GUI exports pass original Full Open/Roundtrip;
independent source masks and represented parameters also pass.
See CTB legacy previews.

CBDDLP/Photon now register v1 and v2, two RGB15 previews, and printer AA
1–16 for v2. Independent canonical pixels/metadata/previews and original
Full Open pass for 68 files / 272 masks. Four GUI exports and AA16 preset
CRUD pass. Original Roundtrip is exact for AA1/2/4/8/16; other AA lose pixels
in the original encoder. Raw Convert metadata differences are retained.
See CBDDLP/Photon versions.

NanoDLP now has a controller JSON stored in the printer preset, normal native
export and four registered Mono/RGB v1/v2 variants. Controller scripts remain
explicitly provided by the user. GUI import/save/restart and four export workflows passed. Original UVtools
Roundtrip matches 42 globals and all 6 layers for each variant. See current NanoDLP integration.
Earlier encoder/contour/metadata/API reports remain historical evidence.

Native ZCode now writes XML, grayscale PNG and RSA-encoded absolute SLA
movement with independent two-stage parameters. Complete native masks/actions
and GUI Roundtrip pass; original empty-layer/global getters and raw Convert
have retained failures. See ZCode validation.

Native ZCodex now preserves grayscale, variable Z, relative motion and custom
per-layer PWM for Inkspire. Original full Roundtrip matches at PWM255; the
original writer loses custom PWM and raw Convert differs in AA/name/time.
See ZCodex validation for raw differences and limits.

Native JXS now preserves full grayscale and per-layer movement/exposure/rests.
All 58 actions match the original in-memory generator. Original raw JXS writer
and global getters have retained, explicit failures; see
JXS validation. This is not full global equivalence.

Native VDT v1 now preserves grayscale PNG, variable Z and independent two-stage
bottom/normal movement, PWM and explicit rests. Original full decoding and
Roundtrip match; raw SL1 Convert differs only in informational AA (8 versus 16)
in full reports, with additional provenance/statistics differences in raw JSON.
See VDT validation; only FLT previews are implemented.

Native QDT now preserves main laser paths, both layer parities and grayscale-derived
previews; independent PNG/path checks and original Full Open/Encode/Convert pass.
The container has no exposure or motion metadata. See QDT validation
for its fixed printer geometry and interpreted defaults.

Native GR1/MDLP now use the actual Makerbase vertical-line schema, with
separate parameter capabilities, exact thresholded-mask/line checks and
original UVtools Roundtrip comparisons. See
Makerbase validation for the original GR1 Convert
lift/light-off defect; MDLP conversion is also fully compared.

All four native LGS variants now have grayscale nibble-RLE, 4K rotation and
model-specific previews/settings checks against original UVtools, including
models 4000/4500. See LGS validation for the raw
SL1 conversion geometry defect and trailing-byte differences.

Native PHZ v2 now has grayscale/encryption, exact canonical-mask/RLE checks,
original UVtools full decoding and GUI export verification. See
PHZ validation for raw-header/Convert differences.

Native Photon S now has full binary-mask/header checks, exact RLE comparisons
against original Encode/Convert, GUI suffix/export verification and a full
GUI-output decode. Its physical geometry and format limits are validated
before slicing. See Photon S validation.
Raw UVtools volume/pitch/float representation differences are reported there;
matching full-decoder fields does not establish matching raw header values.
CTB/CBDDLP/Photon v2 and SL1/CXDLPv4 have their earlier deeper comparisons:
Chitubox,
real CL-92 files.

Work is active. Separate native CTBEncrypted v4/v5 writers are enabled;
see full validation. Native 3D binary readers and most binary mask
imports remain incomplete (CXDLP v1/v2/v3/v4 mask editing is implemented).
All device/firmware certification, full version coverage, profiles, UI and
remaining main-goal work stay open. Production export uses
`SLAArchiveWriter::create`, with no runtime external conversion adapter.

## Application registry

Source: `src/libslic3r/Format/SLAArchiveFormatRegistry.cpp`.
Readers listed below reflect the removal of placeholder SL1 registrations.

| Format ID | Extension | Registered writer | Native reader |
| --- | --- | --- | --- |
| `cbddlp` | `.cbddlp` | `ChituboxCBDDLPArchive` | Not implemented |
| `ctb_v1` (legacy ID `cbt` migrated) | `.v1.ctb` | `ChituboxCTBv1Archive` (native; firmware motion) | No native reader; see the format section of manual.txt |
| `ctb` | `.ctb` | `UVToolsChituboxArchive` | Not implemented |
| `ctb_v3` | `.v3.ctb` | `ChituboxCTBModernArchive` | Native v3, original full decode and GUI roundtrip |
| `ctb_v4` | `.v4.ctb` | `ChituboxCTBModernArchive` | Native v4, two-stage motion; full decode and GUI roundtrip |
| `ctb_v5` | `.v5.ctb` | `ChituboxCTBModernArchive` | Native v5 with resin; full decode and GUI roundtrip |
| `ctb_gktwo` | `.gktwo.ctb` | `ChituboxCTBModernArchive` | Dedicated magic/disclaimer; full decode and GUI roundtrip |
| `ctb_encrypted` | `.encrypted.ctb` | `CTBEncryptedArchive` v5 | Native; full decoder and GUI checked |
| `ctb_encrypted_v4` | `.v4.encrypted.ctb` | `CTBEncryptedArchive` v4 | Native; full decoder and GUI checked |
| `chitubox_zip` | `.zip` | `UVToolsChituboxZIPArchive` | No 3D reader; mask editor supported |
| `cws` | `.cws` | `UVToolsCWSArchive` (XML/grayscale) | No 3D reader; grayscale mask editor supported |
| `cwsnovamaker` | `.cws` | `UVToolsCWSArchive` (NovaMaker slice.conf) | No 3D reader; grayscale mask editor supported |
| `cwsrgb` | `.rgb.cws` | `UVToolsCWSArchive` (packed BGR + slice.conf) | No 3D reader; full LCD mask editor supported |
| `cwsxml` | `.xml.cws` | `UVToolsCWSArchive` (PNG32) | No 3D reader; RGBA mask editor supported |
| `cxdlp` | `.cxdlp` | `UVToolsCXDLPArchive` (v3) | No 3D reader; binary mask editor supported |
| `cxdlpv2` | `.v2.cxdlp` | `UVToolsCXDLPArchive` (v2) | No 3D reader; binary mask editor supported |
| `cxdlpv1` | `.v1.cxdlp` | `CrealityCXDLPv1Archive` | No 3D reader; binary mask editor supported |
| `cxdlpv4` | `.cxdlpv4` | `CrealityCXDLPv4Archive` | No 3D reader; binary mask editor supported |
| `dl2p` | `.dl2p` | `AnycubicSLAArchive / 517` | Not implemented |
| `dlp` | `.dlp` | `AnycubicSLAArchive / 1` | Not implemented |
| `fdg` | `.fdg` | `FDGArchive` (native v2; GUI seed=0) | No native reader; see the format section of manual.txt |
| `genericzip` | `.zip` | `UVToolsGenericZIPArchive` | No 3D reader; mask editor supported |
| `goo` | `.goo` | `UVToolsGOOArchive` | Not implemented |
| `gr1` | `.gr1` | `MakerbaseArchive` (GR1 settings) | Not implemented |
| `jxs` | `.jxs` | `UniformationGKoneArchive` | Not implemented |
| `lgs` | `.lgs` | `LongerOrange10Archive` | Not implemented |
| `lgs120` | `.lgs120` | `LongerOrange120Archive` | Not implemented |
| `lgs30` | `.lgs30` | `LongerOrange30Archive` | Not implemented |
| `lgs4k` | `.lgs4k` | `LongerOrange4KArchive` | Not implemented |
| `m5sp` | `.m5sp` | `AnycubicSLAArchive / 518` | Not implemented |
| `mdlp` | `.mdlp` | `MakerbaseArchive` (MDLP settings) | Not implemented |
| `n4` | `.n4` | `AnetN4Archive` | Not implemented |
| `n7` | `.n7` | `AnetN7Archive` | Not implemented |
| `nanodlp` | `.nanodlp` | `NanoDLPArchive` | Mono v2; explicit controller profile required |
| `nanodlp_v1` | `.v1.nanodlp` | `NanoDLPArchive` | Mono v1; explicit controller profile required |
| `nanodlp_rgb` | `.rgb.nanodlp` | `NanoDLPArchive` | RGB v2; explicit controller profile required |
| `nanodlp_rgb_v1` | `.v1.rgb.nanodlp` | `NanoDLPArchive` | RGB v1; explicit controller profile required |
| `osf` | `.osf` | `UVToolsOSFArchive` | Not implemented |
| `osla` | `.osla` | `OpenSLAArchive` | Not implemented |
| `photon` | `.photon` | `ChituboxPhotonArchive` | Not implemented |
| `photons` | `.photons` | `ChituboxPhotonSArchive` (binary, fixed Photon S geometry) | Not implemented |
| `phz` | `.phz` | `PhrozenPHZArchive` (native v2, optional encryption) | Not implemented |
| `pm3` | `.pm3` | `AnycubicSLAArchive / 515` | Not implemented |
| `pm3m` | `.pm3m` | `AnycubicSLAArchive / 515` | Not implemented |
| `pm3n` | `.pm3n` | `AnycubicSLAArchive / 517` | Not implemented |
| `pm3r` | `.pm3r` | `AnycubicSLAArchive / 515` | Not implemented |
| `pm4n` | `.pm4n` | `AnycubicSLAArchive / 515` | Not implemented |
| `pm4u` | `.pm4u` | `AnycubicZipArchive / ZIP + PW0` | Not implemented |
| `pm5` | `.pm5` | `AnycubicSLAArchive / 517` | Not implemented |
| `pm5s` | `.pm5s` | `AnycubicSLAArchive / 518` | Not implemented |
| `pm7` | `.pm7` | `AnycubicZipArchive / ZIP + PW0` | Not implemented |
| `pm7m` | `.pm7m` | `AnycubicZipArchive / ZIP + PW0` | Not implemented |
| `pmsq` | `.pmsq` | `AnycubicSLAArchive / 1` | Not implemented |
| `pmx2` | `.pmx2` | `AnycubicSLAArchive / 515` | Not implemented |
| `prz` | `.prz` | `UVToolsGOOArchive` | Not implemented |
| `pw0` | `.pw0` | `AnycubicSLAArchive / 1` | Not implemented |
| `pwc` | `.pwc` | `AnycubicSLAArchive / 516 + MACHINE` | Not implemented |
| `pwma` | `.pwma` | `AnycubicSLAArchive / 515` | Not implemented |
| `pwmb` | `.pwmb` | `AnycubicSLAArchive / 515` | Not implemented |
| `pwmo` | `.pwmo` | `AnycubicSLAArchive / 1` | Not implemented |
| `pwms` | `.pwms` | `AnycubicSLAArchive / 1` | Not implemented |
| `pwmx` | `.pwmx` | `AnycubicSLAArchive / 1` | Not implemented |
| `pws` | `.pws` | `AnycubicSLAArchive / 1` | Not implemented |
| `pwsz` | `.pwsz` | `AnycubicZipArchive / ZIP + PW0` | Not implemented |
| `pwx` | `.pwx` | `AnycubicSLAArchive / 1` | Not implemented |
| `px6s` | `.px6s` | `AnycubicSLAArchive / 517` | Not implemented |
| `qdt` | `.qdt` | `Emake3DGalaxyArchive` | Native binary laser paths; tested Full Open/Encode/Convert; fixed geometry and decoder defaults |
| `SL1` | `.sl1` | `SL1Archive` | SL1 |
| `SL1SVG` | `.sl1_svg` | `SL1_SVGArchive` | SL1 SVG |
| `svgx` | `.svgx` | `FlashforgeSVGXArchive` | Native writer; Full Open and GUI checked; see manual.txt for constraints |
| `uvj` | `.uvj` | `UVJArchive` | Not implemented |
| `vda` | `.zip` | `UVToolsVDAArchive` (Voxeldance Additive) | No 3D reader; mask editor supported |
| `vdt` | `.vdt` | `VoxeldanceTangoArchive` | Native v1 JSON/PNG; full grayscale, variable Z, two-stage motion/PWM/rest; Full Open/Roundtrip/GUI tested; orthographic previews pending |
| `zcode` | `.zcode` | `ZCodeArchive` | Not implemented |
| `zcodex` | `.zcodex` | `ZCodexArchive` | Not implemented |

Anycubic ZIP aliases now use native PW0 archives. Other writer rows remain
unverified until independent pixel and parameter comparisons pass. Ambiguous ZIP
aliases need content probing. The 2026-10-03 UVJ implementation replaces its
placeholder. Native N4/N7 and OSLA writers have independent mask/table tests
and managed UVtools schema checks; current export count is 50 generated /
17 explicitly blocked (including the separate encrypted CTB ID). N4/N7 require matching hardware geometry and integer
exposures/common whole-mm/s motion; OSLA supports grayscale PNG and per-layer
two-stage motion. This does not certify native UVtools image decode or firmware.
See current verification.

## Actual extension coverage (2026-10-04)

The managed `--registry` probe of the local UVtools assembly reports 31 families
and 75 distinct extensions. The native registry has 67 IDs; that number does
not establish extension or version parity. `format-coverage.json` in the current
validation artifacts records 13 extensions absent from the printer registry:
BMP, gktwo.ctb, JP2, JPEG/JPG, PBM, PGM, PNG, RAS, rgb.nanodlp, SR,
TIF/TIFF. XML.CWS has a distinct PNG32 writer and mask editor. NovaMaker
`slice.conf` has its own writer and content-probed editor. RGB.CWS now packs
three LCD samples as BGR into each RGB PNG pixel; the oriented LCD width
must be divisible by three. Its editor restores full resolution and grayscale.
Shared `.cws` requires schema identification just like ZIP:
the legacy XML/grayscale choice remains available separately from NovaMaker.
Standalone PNG editing is implemented separately from
printer exporters and is not counted as a printer container.

CTB versions and the four independent ZIP families require schema-specific
handling. Chitubox ZIP has its own native writer and mask editor; Klipper ZIP
remains pending. SL1/SL1SVG ZIP aliases do not implement those schemas.
Generic ZIP and Voxeldance Additive ZIP now have distinct native writers and
content-probed mask editors; its schema stores geometry and images without
exposure or movement settings. Encrypted CTB remains unavailable.
OSF v4 now has independent RLE/table tests and managed original UVtools table
checks; full UVtools image decoding and device acceptance remain unverified.

## UVtools reference families

Each row requires a separate extension/version/capability inventory before it
can be marked complete. The families include image/container formats, so a
family is not necessarily a printer format.

Reference registry SHA-256: `6d8685e11f1697f59329093d079cdbb5b08e93775fcd6a9e1924b5d521f5c323`.

| Reference class | Coverage work |
| --- | --- |
| `SL1File` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `ChituboxZipFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `ChituboxFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `CTBEncryptedFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `AnycubicPhotonSFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `PHZFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `AnycubicFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `AnycubicZipFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `CWSFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `AnetFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `LGSFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `VDAFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `VDTFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `CrealityCXDLPFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `CrealityCXDLPv4File` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `NanoDLPFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `KlipperFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `FDGFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `GooFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `ZCodeFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `JXSFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `ZCodexFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `MDLPFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `GR1File` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `FlashForgeSVGXFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `QDTFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `OSLAFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `OSFFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `UVJFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `GenericZIPFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |
| `ImageFile` | Audit extensions, versions, read/write, per-layer parameters and fixtures |

## UVtools operations to audit and port

Source: `UVtools.Core/Operations/Operation*.cs`. This inventory covers concrete
operation classes, not the separate pixel editor, scripting system, UI or file
codecs. Each operation needs an implementation mapping, parameter documentation,
UI entry, cancellation/undo behavior where applicable, and a reference test.
None is marked complete solely because similar code may exist in the slicer.

- [x] `OperationBlur` — Fala 41: all six variants (`LayerBlur.cpp`, recipe `blur`); byte parity with UVtools 7.0.0 for Gaussian, Box, Median and StackBlur size 3 and >= 11 (StackBlur 5..9: UVtools' OpenCV 4.12 bug, native = exact filter); Pyramid is a no-op in UVtools 7.0.0, native runs the described down+up pyramid; Filter2D = OpenCV filter2D (no UVtools headless parity: nested kernel property). GUI catalog entry pending (next wave)
- [x] `OperationCalculator` — `LayerCalculatorDialog.cpp` (Operations > Calculator...), test case `calculator: pixels, light-off delay, tilt and print time`
- [x] `OperationCalibrateBloomingEffect` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`; uvj / cxdlpv4 only (several exposures at one Z, no representation in other formats)
- [x] `OperationCalibrateElephantFoot` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`
- [x] `OperationCalibrateExposureFinder` — `SLA/ExposureFinder*.cpp`, `ExposureFinderDialog.cpp`, GUI `exposure_finder_{form,manual,validation}_smoke`; Fala 43: accessible row list in AT-SPI
- [x] `OperationCalibrateExternalTests` — offline by design: only the reference text list (test `External test list is offline reference text`)
- [x] `OperationCalibrateGrayscale` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke` ("Grayscale patches" + best-level calculator)
- [x] `OperationCalibrateLiftHeight` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`
- [x] `OperationCalibrateStressTower` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`
- [x] `OperationCalibrateTolerance` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`
- [x] `OperationCalibrateXYZAccuracy` — `SLA/CalibrationRaster.*`, `[sla_calibration]` (`sla_calibration_generators_tests.cpp`), GUI `SlaRasterToolsDialog.cpp` + `calibration_entries_smoke`
- [x] `OperationChangeResolution` — recipe `change_resolution` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationDoubleExposure` — recipe `double_exposure` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationDynamicLayerHeight` — recipe `dynamic_layer_height` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationDynamicLifts` — recipe `dynamic_lifts` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationEditParameters` — recipe `edit_parameters` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationFadeExposureTime` — recipe `fade_exposure` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationFlip` — recipe `flip` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationIPrintedThisFile` — Fala 43: `SLA/ResinStock.{hpp,cpp}` (`resin_stock.json`, UVtools `Consume` arithmetic), `ResinStockDialog.cpp` (File > "I printed this file..." and "Resin stock..."), `[sla_resin_stock]`, GUI `resin_stock_smoke`; no Undo and no automatic booking after export (by scope)
- [x] `OperationInfill` — recipe `infill` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLayerArithmetic` — recipe `layer_arithmetic` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLayerClone` — recipe `clone` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLayerExportGif` — recipe `export_gif`, `SLA/LayerExports.*`, GUI Operations > Export
- [x] `OperationLayerExportHeatMap` — recipe `export_heat_map`, `SLA/LayerExports.*`, test `export heat map: exact sums, merged Z and ROI`
- [x] `OperationLayerExportHtml` — recipe `export_html`, `SLA/LayerExports.*`, GUI Operations > Export
- [x] `OperationLayerExportImage` — recipe `export_images`, `SLA/LayerExports.*`, GUI Operations > Export
- [x] `OperationLayerExportMesh` — recipe `export_mesh`, `SLA/LayerExports.*`, test `export mesh: 2x2x2 voxel cube is a closed outward oriented surface`
- [x] `OperationLayerExportSkeleton` — recipe `export_skeleton`, `SLA/LayerExports.*`, test `export skeleton: a thick bar thins to its centre line`
- [x] `OperationLayerImport` — Fala 41 engine + recipe `layer_import`: tier 1 (replace / stack / merge / bitwise on every document), tier 2 (insert / extend / discard on sliced and generated documents); PNG pictures and printer files, BMP / JPEG / GIF out of scope.
- [x] `OperationLayerReHeight` — recipe `reheight` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLayerRemove` — recipe `remove` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLightBleedCompensation` — recipe `light_bleed` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationLithophane` — recipe `lithophane` (`doc/AUTOMATION.md`), GUI catalog `LayerOperationCatalog_Batch2.cpp`; the thumbnail text overlay is intentionally not ported (a layer document has no thumbnail slot, not applicable)
- [x] `OperationMask` — Fala 41 `mask_image_layers` / recipe `mask_image` (size mismatch is an error, optional bilinear fit).
- [x] `OperationMorph` — GUI "Range operation" in `LayerEditorContent.cpp` (Erode, Dilate, open, close, gradient, top/black hat), `LayerOperations`; no separate recipe op
- [x] `OperationMove` — recipe `move` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationPCBExposure` — `SLA/PCBExposure.*`, GUI `SlaRasterToolsDialog.cpp`, `[sla_pcb]` (435 assertions), GUI `pcb_expose_smoke`, `pcb_pm7_generation_smoke`
- [x] `OperationPattern` — recipe `pattern` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationPhasedExposure` — recipe `phased_exposure` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationPixelArithmetic` — recipe `pixel_arithmetic` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationPixelDimming` — recipe `pixel_dimming` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationProgress` — UVtools' internal progress helper, not a user operation; equivalent `SLA/LayerProgress.hpp` (progress + cancellation in every operation)
- [x] `OperationRaftRelief` — all five types since Fala 41 (`LayerLinkedLines.cpp`, byte parity with UVtools in 4 oracle cases)
- [x] `OperationRaiseOnPrintFinish` — recipe `raise_on_print_finish` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationRedrawModel` — Fala 41 `redraw_model_layers` / recipe `redraw_model`, needs a second model-only file (no model / support pair in a document); strict contour branch included. No headless UVtools oracle (second file)
- [x] `OperationRepairLayers` — recipe `repair_layers` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationResize` — recipe `resize` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationRotate` — recipe `rotate` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [ ] `OperationScripting` — out of scope by design: arbitrary C# scripts are not supported (`doc/AUTOMATION.md`); replaced by declarative JSON recipes (`--sla-run-recipe`)
- [x] `OperationSolidify` — recipe `solidify` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationStirResin` — Fala 41 `stir_resin_plan` / recipe `stir_resin` (StirOnly, StirAndPrint; sliced and generated documents; no thumbnail).
- [x] `OperationThreshold` — recipe `threshold` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`
- [x] `OperationTimelapse` — recipe `timelapse` (`LayerRecipe.cpp`, `doc/AUTOMATION.md`), GUI `LayerOperationCatalog*.cpp`, tests `sla_layer_operations*_tests.cpp` / `sla_layer_ops_document*_tests.cpp` / `sla_layer_recipe_tests.cpp`

## Pixel editor (Fala 41 silnik, Fala 42 GUI)

- [x] Text tool — engine `SLA/HersheyText.{hpp,cpp}` (shared with the calibration generators) + `LayerDocument::ShapeTool::Text` / `draw_text`; recipe `draw_text`; GUI tool since Fala 42 (`sliced_canvas_tools_f42_smoke`); Hershey faces only (basic Latin)
- [x] Supports / Drain holes — engine `LayerDocument::dig_column` (one undo step), `support_layer_step`, `drain_layer_step`; recipes `draw_support`, `drill_drain_hole`; GUI tools since Fala 42 (`sliced_canvas_tools_f42_smoke`)

## Aktualizacja CBDDLP/Photon — 2026-10-04

CBDDLP/Photon v2, AA=8: natywny eksport w ChituboxArchive.cpp, GTK GUI,
pełny UVtools, niezależne maski i porównanie z oryginalnym encoderem zaliczone.
Brak natywnego importu tych formatów; pozostałe wersje/AA jeszcze otwarte.
Audyt wszystkich 66 ID: 49 zapisanych, 47 Full Open, 2 wyjątki poza aktywnym
rejestrem UVtools (SL1SVG i starszy CXDLPv1), 17 blocked. To nie jest odbiór
parametrów wszystkich wersji ani firmware wszystkich drukarek.
Surowe i semantyczne różnice.

## CTB v1 / migracja błędnego CBT — 2026-10-05

`.cbt` nie jest rozszerzeniem rejestrowanym przez UVtools. Wpis zastąpiono
jawnym ctb_v1 / .v1.ctb, z migracją starych ID w presetach i fabryce.
Writer zapisuje prawdziwy Version=1, gray7/RLE, geometrię, variable Z,
przejścia, PWM i light-off; nie podmienia pliku na CTB v2. V1 pomija blok
PrintParameters. Niestandardowe ruchy są jawnie odrzucane zamiast tracone;
domyślne wartości dekodera nie są certyfikatem firmware drukarki.
112 asercji, dwie orientacje, maski i wszystkie odszyfrowane RLE zgodne
z oryginałem. Full Open/Roundtrip: 42 pola globalne/warstwy bez różnic;
również GTK eksport/sufiksy/popup i sześć niepustych masek GUI pass.
Surowy Convert nadal failed dla light-off/MachineZ/czasu; raport zachowano.
Końcowy CTest 9/9 pass, 77.88 s; libseqarrange_tests niezmieniony, pominięty.
Świeży audyt: 67 ID / 64 written / 62 Full Open / dwa wyjątki / 3 blocked:
encrypted CTB v5, SVGX i NanoDLP. Brakujące wersje/rozszerzenia/importy,
firmware, profile i UI/UX oraz E3–E6 pozostają otwarte.
Dowody i ograniczenia.
Checkpoint: build/ctb-v1-2026-10-05/checkpoint/manifest.json. Cel aktywny.

## SVGX — wspólne ścieżki konturów, 2026-10-05

Skorygowano założenie ZIP: SVGX ma binarny header, BMP i jeden SVG.
Wyodrębniono PixelContourTree z NanoDLP, z opcjonalnymi CHAIN_APPROX_SIMPLE
points, rodzicami/root, otworami/wyspami oraz area/perimeter. Stats-only
nie alokuje path storage. Oryginalny UVtools/OpenCV: 68 056 masek,
127 792 kontury i 641 152 wierzchołki zgodne; DrawContours z natywnych
punktów odtwarza każdy piksel. Dotychczasowe 12 pól NanoDLP nadal zgodne.
604 213 asercji / dwa przypadki pass; 224 asercje geometrii siedmiu
rodzin obejmują także SVGX z prostokątnymi pikselami/orientacją/odbiciami.
Usunięto błędną, nieużywaną klasę ZIP; bezpośredni SVGX export_print
przestał zwracać pusty sukces. Registry i codec/writer nadal blocked.
Original serializer potwierdza header=28/BMP=54 i niespójność deklaracji
UTF-16 z zapisem UTF-8. Header/BMP/XML, konwersja px↔mm, parametry,
pełny plik/UVtools/GUI i limity worker pamięci pozostają do wdrożenia.
Końcowy CTest: 9/9 pass, 66.08 s; libseqarrange_tests niezmieniony,
pominięty (wcześniej pass w PHZ). Nie zaliczamy konturów jako działającego
eksportu SVGX. Ostatni pełny audyt bez zmiany: 67 ID / 64 written /
62 Full Open / dwa wyjątki / 3 blocked. E1/E2 oraz E3–E6 nadal otwarte.
Dowody i następna partia.
Checkpoint: build/svgx-2026-10-05/contours-checkpoint/manifest.json.
Główny cel pozostaje aktywny.

## SVGX writer, 2026-10-05

Native binary/BMP/SVG writer enabled; full mask, parameter and GUI checks pass.
The original encoder loses edges at some pitches; its raw failed reports are retained.
Current audit: 67 IDs / 65 written / 63 Full Open / two exceptions / two blocked.
Evidence and profile constraints. E1–E6 remain active.

## CTBEncrypted v4/v5 — 2026-10-05

Odblokowano samodzielny native writer dla obu wersji, z AES/podpisem,
XOR-RLE, dwufazowym ruchem, zmiennym Z i UTF-8. Osiem plików i dwa
rzeczywiste eksporty GUI przeszły pełny odczyt oraz weryfikację pikseli
i parametrów. Popup i końcówki obu wersji przechodzą test GUI.
Audyt: 68 ID / 67 written / 65 Full Open / dwa wyjątki / jeden blocked
(NanoDLP). Surowe różnice i błędy ponownego zapisu UVtools zachowano.
Dowody i ograniczenia.
Brakujące wersje/importy oraz profile, UI/UX i E3–E6 nadal otwarte.

Końcowe regresje: 1904 asercje / 10 przypadków pass; CTest 9/9 pass
(32.15 s). Niezmieniony libseqarrange_tests pominięto; przeszedł wcześniej.
Checkpoint: `build/ctb-encrypted-2026-10-05/checkpoint/manifest.json`.
Główny cel pozostaje aktywny.

## NanoDLP — API z jawnym profilem sterownika, 2026-10-05

Dodano kompletny natywny kontener v1/v2 Mono/RGB, manifesty, podgląd
i szablony przekazanego profilu. 16 plików / 48 masek przechodzi
oryginalny Full Open, niezależną weryfikację i ścisły Roundtrip.
Eksport z GUI pozostaje blocked: profile sterownika i parametry ruchu
wymagają powiązania. API nie wykonuje i nie certyfikuje skryptów.
Dowody, surowe różnice i kolejka.
Pełny refactor i niezmieniony audyt rejestru pozostają otwarte.

Końcowe testy: 960 asercji / 2 przypadki pass; CTest 9/9 pass (32.35 s).
Niezmieniony libseqarrange_tests pominięty, z wcześniejszym wynikiem pass.
Końcowe fixtures: `profile-verified-fixtures`, verifier: `profile-verified-independent.json`.
Checkpoint: `build/nanodlp-2026-10-05/profile-checkpoint/manifest.json`.
Feedback i trzy pliki testexports użytkownika zachowane; cel pozostaje aktywny.

## Ordinary CTB v3–v5 and GKtwo — 2026-10-06

Four native writers/preset IDs, separate from encrypted CTB. Final regressions:
8988 assertions/12 cases; CTest9/9. Sixteen fixture files/64 masks checked by
original Full Open and independent PNG/parameter/statistics validation. Four
GUI exports and original Roundtrip42globals+6layers each pass; eight previews
retain identical decoded pixels. Raw UTF-8 re-encode and UVJ Convert failures
are retained and explained rather than excluded from comparison.

Current audit75written/73Full/two exceptions/zero unavailable. Every UVtools
version, extension, native importer and device setting is still not covered.
The remaining11 ImageFile extensions represent a single-image format and
need appropriate image import/export UI rather than silently discarding a
multi-layer print. See CTB modern evidence.


### ImageFile native masks — 2026-10-06

PNG/JPG/JPEG/JP2/TIF/TIFF/BMP/PBM/PGM/SR/RAS are separate native mask I/O,
with explicit current/range exports in the layer editor. They are not printer
job containers or printer-registry IDs. Nine original ImageFile writer pairs
match; SR/RAS match separate original RGB Mat.Save references. Original
unpaletted grayscale SR/RAS decoding loses all pixels; raw failed comparisons
are retained. Native output uses an explicit linear palette. TIFF stacks,
advanced color/CMYK/JP2 variants and clean-system packaging remain open.
Fresh printer audit is unchanged: 75 written, 73 Full Open, two known original
decoder exceptions. See image report.
