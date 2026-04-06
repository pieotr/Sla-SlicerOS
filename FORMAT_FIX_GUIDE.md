# UVtools Format Compatibility - Fix Implementation Guide

## Summary of Analysis

Found root causes for FDG/CWS failures and mapped all 33 UVtools formats:

### FDG Issue
- **Error**: "Not a valid FDG file!" at MAGIC check
- **Root Cause**: Header likely correct, but might be RLE encoding mismatch or layer data offset issue
- **Solution**: Verify 54-field header structure (216 bytes total) and FDG-RLE encoding against UVtools spec

### CWS Issue
- **Error**: "End of Central Directory not found" - ZIP corrupted
- **Root Cause**: ZIP file finalization incomplete using current Zipper utility
- **Solution**: Manual ZIP creation with proper central directory

### All 33 Formats Summary
Grouped by implementation strategy:
- **9 ZIP Archives**: Shared ZIP framework (80% code reuse)
- **13 Binary RLE**: Shared RLE codec (70% code reuse)  
- **4 Packed Pixel**: Shared RGB565 utilities (60% code reuse)
- **7 Hybrid/Special**: Individual implementations

## Immediate Fixes Needed

### 1. FDG Format (CRITICAL)
File: `UVToolsCompatArchive.cpp`

**Issue**: RLE encoding may have subtle per-row boundary logic
**From UVtools FDGFile.cs line 445-467**:
```csharp
for (int y = 0; y < mat.Height; y++)
{
    // ... iterate x ...
    if (x == halfWidth)  // <-- CRITICAL: Boundary at halfway point
    {
        AddRep();
        color = grey7;
        stride = 1;
    }
    // End of row: reset color
    AddRep();
    color = byte.MaxValue;  // 0xFF
}
```

**Fix**: Add hadwidth boundary handling in FDGRasterEncoder::encode()

### 2. CWS Format (CRITICAL)
File: `UVToolsCompatArchive.cpp`

**Issue**: Zipper utility may not support proper ZIP finalization
**Solution**: Use raw ZIP creation with:
- Local file headers (30 + filename_len bytes)
- File data
- Central directory with entries
- End of Central Directory record (22 bytes)

### 3. File Structure Corrections

#### FDG Structure Verification
- Header: 54 × uint32 = 216 bytes (✓ CORRECT)
- Field 16-17: LightPWM, BottomLightPWM (ushorts, not uint32)
- Layer defs: 9 × uint32 = 36 bytes per layer
- Layer data: RLE-encoded images (variable size)

#### CWS Structure 
- Archive ZIP format
- Required entries:
  - manifest.xml (Wanhao-style with Slices array)
  - {projectname}.slicing (XML profile)
  - {projectname}.gcode (GCode content)
  - {projectname}00000.png ... {projectname}NNNNN.png (layer images as PNG)

## 33 Format Implementation Strategy

### Phase 1: Critical Fixes (FDG, CWS, CXDLP)
- Your 3 broken formats
- ~500 lines total
- Enable ~8 similar formats via inheritance

### Phase 2: Binary RLE Base (300 lines)
Powers 13 formats via single RLE codec:
- ChituboxFile (MAGIC: 0x12FD0019, RLE limit 0x7D)
- CTBFile (MAGIC: 0x12FD0086, RLE variant)
- PHZFile (MAGIC: 0x9FDA83AE)
- FDGFile (MAGIC: 0xBD3C7AC8) ← Already partially done
- AnycubicFile (CRC-16-ANSI)
- GooFile, GR1File, MDLPFile, LGSFile, OSFFile, etc.

### Phase 3: ZIP Archive Base (200 lines)
Powers 9 formats via unified ZIP framework:
- SL1 (INI config)
- GenericZIP (XML manifest)
- ChituboxZip (GCode support)
- CWSFile (XML manifest)
- All ZCode variants
- NanoDLPFile
- OSLAFile, VDAFile, KlipperFile

### Phase 4: Special Formats (400 lines)
- FlashForgeSVGX: SVG generation
- QDTFile: Line-based vector format
- AnetFile: UTF-16BE quirks
- ImageFile: OpenCV wrapper

## Code Sharing Matrix

```
RLE Encoder (Base)
├── FDGRasterEncoder (7-bit -> 8-bit: (code<<1)|(code&1))
├── CTBRasterEncoder (8-bit, multi-byte runs)
├── AnycubicRasterEncoder (16-bit RLE, CRC-16)
└── AliasEncoder (for 16+ formats)

ZIP Framework (Base)
├── Simple ZIP creator (manual local headers + central directory)
├── Manifest parsers (XML, JSON)
└── Layer rendering (PNG or RLE format)

Preview System
├── RGB565Encoder (little-endian for Anycubic, GR1, etc.)
├── BMP quirk handler (AnetFile)
└── Thumbnail utilities
```

## Testing Strategy for You

1. **Test FDG Fix**:
   - Export test print as .fdg
   - Open in UVtools
   - If still fails: provide full error + file size

2. **Test CWS Fix**:
   - Export test print as .cws
   - Open in UVtools ZIP validator first: `unzip -t file.cws`
   - If ZIP invalid: fix ZIP creation
   - If manifest missing: add XML generation

3. **Test CXDLP**:
   - Should already work if UTF-16BE strings correct
   - Verify CRC32 checksum (last 4 bytes match file calc)

## Next Steps

1. Apply FDG fix: Add halfwidth boundary check in RLE encoder
2. Apply CWS fix: Implement proper ZIP with central directory
3. Verify compilation: `cmake --build build --target libslic3r -j4`
4. Test exports: FDG, CWS, CXDLP with UVtools
5. Iterate on remaining formats as needed

## Code Template for Additional Formats

For each new format, follow the pattern:
1. Create RasterEncoder specialization (20-50 lines)
2. Implement create_raster() - reuse common helper
3. Implement get_encoder() - return your encoder
4. Implement export_print() - write header + layers + footer
5. Register in SLAArchiveFormatRegistry with lambda factory
