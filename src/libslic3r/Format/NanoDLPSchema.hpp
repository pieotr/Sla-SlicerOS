// NanoDLP schema port from local UVtools.Core/FileFormats/NanoDLPFile.cs (AGPLv3).
// This is a schema reference, not a configured printer profile. A writer must
// replace provenance, timestamps, geometry and hardware-specific motion scripts.
#ifndef SLIC3R_FORMAT_NANODLP_SCHEMA_HPP
#define SLIC3R_FORMAT_NANODLP_SCHEMA_HPP
#include <nlohmann/json.hpp>
#include <charconv>
#include "NanoDLPContourStats.hpp"
namespace Slic3r::nanodlp {
inline nlohmann::json schema_defaults()
{
    // Return independent objects: changing one export cannot leak into another.
    // Clock values are zero so the reference is deterministic across platforms.
    return nlohmann::json::parse(R"NanoSchema(
{
  "meta": {
    "FormatVersion": 2,
    "Distro": "generic",
    "Program": "UVtools",
    "Version": "6.0.1",
    "OS": "linux-x64",
    "Arch": "X64",
    "Profile": false
  },
  "slicer": {
    "Type": "stl",
    "URL": "",
    "PWidth": 0,
    "PHeight": 0,
    "ScaleFactor": 0,
    "StartLayer": 0,
    "SupportDepth": 0.05,
    "SupportLayerNumber": 4,
    "Thickness": 0.05,
    "XOffset": 0,
    "YOffset": 0,
    "ZOffset": 0,
    "XPixelSize": 0,
    "YPixelSize": 0,
    "Mask": null,
    "SliceFromZero": false,
    "DisableValidator": false,
    "AutoCenter": 0,
    "PreviewGenerate": false,
    "Running": false,
    "Debug": false,
    "IsFaulty": false,
    "Corrupted": false,
    "MultiMaterial": false,
    "AdaptExport": "",
    "PreviewColor": "",
    "FaultyLayers": [],
    "OverhangLayers": [],
    "LayerStatus": [],
    "Boundary": {
      "XMin": 0,
      "XMax": 0,
      "YMin": 0,
      "YMax": 0,
      "ZMin": 0,
      "ZMax": 0
    },
    "MC": {
      "StartX": 0,
      "StartY": 0,
      "Width": 0,
      "Height": 0,
      "X": null,
      "Y": null,
      "MultiCureGap": 0,
      "Count": 0
    },
    "MultiThickness": "",
    "ExportPath": "",
    "NetworkSave": "",
    "File": "/plate.stl",
    "FileSize": 0,
    "AdaptSlicing": 0,
    "AdaptSlicingMin": 0,
    "AdaptSlicingMax": 0,
    "SupportOffset": 0,
    "Offset": 0,
    "FillColor": "#ffffff",
    "BlankColor": "#000000",
    "DimAmount": 0,
    "DimWall": 0,
    "DimSkip": 0,
    "PixelDiming": 0,
    "HatchingType": 0,
    "ElephantMidExposure": 1,
    "ElephantType": 0,
    "ElephantAmount": 0,
    "ElephantWall": 0,
    "ElephantLayers": 0,
    "HatchingWall": 20,
    "HatchingGap": 20,
    "HatchingOuterWall": 20,
    "HatchingTopCap": 10,
    "HatchingBottomCap": 10,
    "MultiCureGap": 0,
    "AntiAlias": 0,
    "AntiAlias3D": 0,
    "AntiAlias3DDistance": 0,
    "AntiAlias3DMin": 0,
    "AntiAliasThreshold": 0,
    "ImageRotate": 0,
    "IgnoreMask": 1,
    "XYRes": 0,
    "XRes": 0,
    "YRes": 0,
    "ZResPerc": 0,
    "PreviewWidth": 0,
    "PreviewHeight": 0,
    "BarrelFactor": 0,
    "BarrelX": 0,
    "BarrelY": 0,
    "ImageMirror": 0,
    "DisplayController": 1,
    "LightOutputFormula": "",
    "PlateID": 0,
    "LayerID": 0,
    "LayerCount": 0,
    "UUID": "",
    "DynamicThickness": [],
    "FillColorRGB": {
      "R": 255,
      "G": 255,
      "B": 255,
      "A": 255
    },
    "BlankColorRGB": {
      "R": 0,
      "G": 0,
      "B": 0,
      "A": 255
    },
    "ExportType": 1,
    "OutputPath": "",
    "Suffix": "",
    "SkipEmpty": 0
  },
  "plate": {
    "PlateID": 0,
    "ProfileID": 0,
    "Profile": null,
    "CreatedDate": 0,
    "StopLayers": "",
    "Path": "",
    "LowQualityLayerNumber": 0,
    "AutoCenter": 0,
    "Updated": 0,
    "LastPrint": 0,
    "PrintTime": 0,
    "PrintEst": 0,
    "ImageRotate": 0,
    "MaskEffect": 0,
    "XRes": 0,
    "YRes": 0,
    "ZRes": 0,
    "MultiCure": "",
    "MultiThickness": "",
    "CureTimes": [],
    "DynamicThickness": [],
    "Offset": 0,
    "OverHangs": [],
    "Risky": false,
    "IsFaulty": false,
    "Repaired": false,
    "FaultyLayers": [],
    "Corrupted": false,
    "TotalSolidArea": 0,
    "BlackoutData": null,
    "LayersCount": 0,
    "Feedback": false,
    "ReSliceNeeded": false,
    "MultiMaterial": false,
    "PrintID": 0,
    "MC": {
      "StartX": 0,
      "StartY": 0,
      "Width": 0,
      "Height": 0,
      "X": null,
      "Y": null,
      "MultiCureGap": 0,
      "Count": 0
    },
    "XMin": 0,
    "XMax": 0,
    "YMin": 0,
    "YMax": 0,
    "ZMin": 0,
    "ZMax": 0
  },
  "profile": {
    "ResinID": 0,
    "ProfileID": 0,
    "Title": "",
    "Desc": "",
    "Color": "",
    "ResinPrice": 0,
    "OptimumTemperature": 0,
    "Depth": 0.05,
    "SupportTopWait": 0,
    "SupportWaitHeight": 0,
    "SupportDepth": 0.05,
    "SupportWaitBeforePrint": 0,
    "SupportWaitAfterPrint": 0,
    "TransitionalLayer": 0,
    "Updated": 0,
    "CustomValues": {
      "CdThreshold": "2000",
      "DwFlowEndSlope": "10",
      "FssEnableCrashdetection": "0",
      "FssEnableDynamicwait": "0",
      "FssEnablePeeldetection": "0",
      "FssEnableResinleveldetection": "0",
      "PdPeelEndSlope": "0",
      "PdPeelStartSlope": "0",
      "ResinPreheatTemperature": "25",
      "RlThreshold": "-5",
      "UvPwmValue": "1.0"
    },
    "Type": 0,
    "ZStepWait": 5000,
    "TopWait": 0,
    "WaitHeight": 5,
    "CureTime": 3,
    "WaitBeforePrint": 0,
    "WaitAfterPrint": 0,
    "SupportCureTime": 30,
    "SupportLayerNumber": 4,
    "AdaptSlicing": 0,
    "AdaptSlicingMin": 0,
    "AdaptSlicingMax": 0,
    "SupportOffset": 0,
    "Offset": 0,
    "FillColor": "#ffffff",
    "BlankColor": "#000000",
    "DimAmount": 0,
    "DimWall": 0,
    "DimSkip": 0,
    "PixelDiming": 0,
    "HatchingType": 0,
    "ElephantMidExposure": 1,
    "ElephantType": 0,
    "ElephantAmount": 0,
    "ElephantWall": 0,
    "ElephantLayers": 0,
    "HatchingWall": 20,
    "HatchingGap": 20,
    "HatchingOuterWall": 20,
    "HatchingTopCap": 10,
    "HatchingBottomCap": 10,
    "MultiCureGap": 0,
    "AntiAlias": 0,
    "AntiAlias3D": 0,
    "AntiAlias3DDistance": 0,
    "AntiAlias3DMin": 0,
    "AntiAliasThreshold": 0,
    "ImageRotate": 0,
    "IgnoreMask": 1,
    "XYRes": 0,
    "YRes": 0,
    "ZResPerc": 0,
    "DynamicCureTime": "",
    "DynamicSpeed": "",
    "ShieldBeforeLayer": "G90\\r\\nSET_PROGRESS_BAR CL=[[LayerNumber]] TL=[[TotalNumberOfLayers]]\\r\\nMOVE_PLATE Z=[[LayerPosition]] F=600\\r\\n[[MoveWait 2]]\\r\\n[[PositionSet [[LayerPosition]]]]\\r\\n[JS]\\r\\nif([[_FssEnableDynamicwait]] == 0)output=\\\"[[DynamicWaitStart]]\\\";\\r\\n[/JS]\\r\\nfss_idle",
    "ShieldAfterLayer": "[JS]if ([[LayerNumber]]==1||[[LayerNumber]]%3==0) output=\\\"TRIGGER\\\";[/JS]\\r\\n[JS]if ([[LayerNumber]]==1||[[LayerNumber]]%5==0) output=\\\"M105\\\";[/JS]\\r\\n[[MoveCounterSet 0]]\\r\\nUVLED_ON PWM=[[_UvPwmValue]]\\r\\nDWELL P={[[CureTime]]*1000}\\r\\n[[MoveWait 1]]\\r\\nUVLED_OFF \\r\\n[[GPIOHigh 10]]\\r\\n\\r\\n[JS]\\r\\nvar nano = nanodlpContext();\\r\\nvar resinlevel = nano[\\\"Status\\\"][\\\"ResinLevelMm\\\"];\\r\\nif( [[_FssEnablePeeldetection]]==0 \\u0026\\u0026 [[CurrentPosition]] \\u003e resinlevel) output=\\\"[[SeparationDetectionStart]]\\\";\\r\\n[/JS]\\r\\n\\r\\n[[MoveCounterSet 0]]\\r\\nG91\\r\\nMOVE_PLATE_FSS Z=[[ZLiftDistance]] F={120-([[LayerNumber]]\\u003c5)*70}\\r\\n[[PositionChange [[ZLiftDistance]]]]\\r\\n[[MoveWait 1]]\\r\\n[[SeparationDetectionStop]]\\r\\n[JS]if ([[LayerNumber]]==1||[[LayerNumber]]%100==0) output=\\\"G4 P2000\\\";[/JS]\\r\\n[JS]if ([[LayerNumber]]==1||[[LayerNumber]]%100==0) output=\\\"[[PressureWrite 1]]\\\";[/JS]",
    "ShieldDuringCure": "",
    "ShieldStart": "",
    "ShieldResume": "",
    "ShieldFinish": "",
    "LaserCode": "",
    "ShutterOpenGcode": "",
    "ShutterCloseGcode": "",
    "SeparationDetection": "",
    "ResinLevelDetection": "",
    "CrashDetection": "",
    "DynamicWait": "",
    "SlowSectionHeight": 7,
    "SlowSectionStepWait": 40,
    "JumpPerLayer": 0,
    "DynamicWaitAfterLift": "",
    "DynamicLift": "",
    "JumpHeight": 0,
    "LowQualityCureTime": 0,
    "LowQualitySkipPerLayer": 0,
    "XYResPerc": 0
  },
  "info": {
    "TotalSolidArea": 0,
    "LargestArea": 0,
    "SmallestArea": 0,
    "MinX": 0,
    "MinY": 0,
    "MaxX": 0,
    "MaxY": 0,
    "AreaCount": 1
  }
}
)NanoSchema");
}
// Convert geometric (pixel-centre) area to NanoDLP's mm² info.json fields.
// Preserve the original float product before its four-decimal, away-from-zero
// rounding; occupied-pixel volume is a separate statistic, not this area.
inline nlohmann::json layer_info(const ContourStats &stats, float pixel_area)
{
    if(!std::isfinite(pixel_area) || pixel_area<=0)
        throw ExportError("NanoDLP info requires a finite positive pixel area");
    auto rounded_area = [&](double area) {
        const float scaled=float(area)*pixel_area;
        if(!std::isfinite(scaled)) throw ExportError("NanoDLP contour area overflow");
        // MathF.Round performs both multiplication and division in float;
        // promoting the division to double changes large-area results.
        // Its single-precision rounding limit is 1e8 (already integral there).
        const float rounded=std::abs(scaled)<1e8f ? std::round(scaled*10000.f)/10000.f : scaled;
        // System.Text.Json serializes Single using shortest round-trip digits.
        // nlohmann stores floats as double, so construct the JSON number from
        // that same shortest float spelling instead of emitting binary noise.
        char buffer[64];
        const auto encoded=std::to_chars(buffer,buffer+sizeof(buffer),rounded);
        if(encoded.ec!=std::errc{}) throw ExportError("NanoDLP area serialization failed");
        double result=0;
        const auto decoded=std::from_chars(buffer,encoded.ptr,result);
        if(decoded.ec!=std::errc{}) throw ExportError("NanoDLP area serialization failed");
        return result;
    };
    return {{"TotalSolidArea",rounded_area(stats.total_area)},
            {"SmallestArea",rounded_area(stats.smallest_area)},
            {"LargestArea",rounded_area(stats.largest_area)},
            {"AreaCount",stats.external_count},
            {"MinX",stats.min_x},{"MinY",stats.min_y},
            {"MaxX",stats.max_x},{"MaxY",stats.max_y}};
}

}
#endif
