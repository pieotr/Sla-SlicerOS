// Development-only oracle. Default mode checks managed schemas; --full-open
// uses original UVtools FileFormat.Open and OpenCV to hash every decoded pixel.
// Neither mode proves firmware acceptance. No network or input file mutation.
using System.IO.Compression;
using System.Reflection;
using System.Runtime.Loader;
using System.Text.Json;
using UVtools.Core;
using UVtools.Core.FileFormats;
using UVtools.Core.Extensions;
using System.Xml.Serialization;
using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

if (args.Length < 2) throw new ArgumentException("Usage: oracle <UVtools DLL directory> <file> [file ...]");
var assemblies = Path.GetFullPath(args[0]);
AssemblyLoadContext.Default.Resolving += (_, name) => {
    var path = Path.Combine(assemblies, name.Name + ".dll");
    return File.Exists(path) ? AssemblyLoadContext.Default.LoadFromAssemblyPath(path) : null;
};
var reports = new List<object>();
if(args[1]=="--exposure-finder-schedules") {
    if(args.Length!=3)throw new ArgumentException("--exposure-finder-schedules requires cases.json");
    using var document=JsonDocument.Parse(File.ReadAllText(args[2]));
    var output=new List<object>();
    foreach(var test in document.RootElement.EnumerateArray()) {
        using var file=new UVJFile();
        file.ResolutionX=2048;file.ResolutionY=1024;file.DisplayWidth=100;file.DisplayHeight=50;
        file.LayerHeight=.05f;file.BottomLayerCount=0;file.ExposureTime=2;file.BottomExposureTime=15;
        file.LiftHeight=file.BottomLiftHeight=5;file.LiftHeight2=file.BottomLiftHeight2=1;
        file.WaitTimeBeforeCure=file.BottomWaitTimeBeforeCure=1;
        var operation=new UVtools.Core.Operations.OperationCalibrateExposureFinder(file) {
            BarsEnabled=true,BarLength=.2m,BarSpacing=.1m,BarThicknessesPx="2",BarFenceThickness=0,
            HolesEnabled=false,TextEnabled=false,BullsEyeEnabled=false,CounterTrianglesEnabled=false,
            StaircaseThicknessPx=0,FeaturesMargin=.1m,PartMargin=0,LeftRightMargin=0,TopBottomMargin=0,
            EnableAntiAliasing=false
        };
        foreach(var setting in test.GetProperty("parameters").EnumerateObject()) {
            var property=operation.GetType().GetProperty(setting.Name)
                ?? throw new ArgumentException("Unknown ExposureFinder property: "+setting.Name);
            object value;
            if(property.PropertyType.IsEnum)value=Enum.ToObject(property.PropertyType,setting.Value.GetInt32());
            else if(property.PropertyType==typeof(bool))value=setting.Value.GetBoolean();
            else if(property.PropertyType==typeof(byte))value=setting.Value.GetByte();
            else if(property.PropertyType==typeof(ushort))value=setting.Value.GetUInt16();
            else if(property.PropertyType==typeof(string))value=setting.Value.GetString()!;
            else if(property.PropertyType==typeof(decimal))value=setting.Value.GetDecimal();
            else throw new ArgumentException("Unsupported ExposureFinder property type: "+property.PropertyType);
            property.SetValue(operation,value);
        }
        operation.ExposureTable.Clear();
        foreach(var row in test.GetProperty("table").EnumerateArray())
            operation.ExposureTable.Add(new UVtools.Core.Objects.ExposureItem(
                row.GetProperty("layer_height").GetDecimal(),row.GetProperty("bottom_exposure").GetDecimal(),
                row.GetProperty("normal_exposure").GetDecimal(),row.GetProperty("brightness").GetByte()));
        var patterns=operation.GetLayers(out _,out _);
        try {
            var capacity=(file.ResolutionX/(uint)patterns[0].Width)*(file.ResolutionY/(uint)patterns[0].Height);
            if(capacity<test.GetProperty("part_count").GetUInt32())throw new InvalidOperationException("Reference fixture cannot fit all requested parts");
        } finally {foreach(var pattern in patterns)pattern.Dispose();}
        if(!operation.Execute())throw new InvalidOperationException("Reference generation failed");
        var rows=new List<object>();
        foreach(var layer in file.Layers) {
            using var mask=layer.LayerMat;
            var samples=new byte[mask.Cols];var values=new SortedSet<byte>();
            for(var y=0;y<mask.Rows;++y) {
                Marshal.Copy(IntPtr.Add(mask.DataPointer,checked(y*(int)mask.Step)),samples,0,mask.Cols);
                foreach(var sample in samples)if(sample!=0)values.Add(sample);
            }
            rows.Add(new {position_z=layer.PositionZ,exposure=layer.ExposureTime,
                brightness=values.Select(value=>(int)value).ToArray(),lift_height=layer.LiftHeightTotal,wait_before=layer.WaitTimeBeforeCure});
        }
        output.Add(new {name=test.GetProperty("name").GetString(),bottom_records=file.BottomLayerCount,
            physical_layers=file.Layers.Select(layer=>layer.PositionZ).Distinct().Count(),exposures=rows});
    }
    Console.WriteLine(JsonSerializer.Serialize(output));return;
}
if(args[1]=="--exposure-finder-tables") {
    if(args.Length!=3)throw new ArgumentException("--exposure-finder-tables requires cases.json");
    using var document=JsonDocument.Parse(File.ReadAllText(args[2]));
    var output=new List<object>();
    foreach(var test in document.RootElement.EnumerateArray()) {
        var operation=new UVtools.Core.Operations.OperationCalibrateExposureFinder();
        IEnumerable<UVtools.Core.Objects.ExposureItem> rows;
        if(test.TryGetProperty("manual",out var manual)) {
            foreach(var row in manual.EnumerateArray())
                operation.ExposureTable.Add(new UVtools.Core.Objects.ExposureItem(
                    row.GetProperty("layer_height").GetDecimal(),
                    row.GetProperty("bottom_exposure").GetDecimal(),
                    row.GetProperty("normal_exposure").GetDecimal(),
                    row.GetProperty("brightness").GetByte()));
            rows=operation.GetSanitizedExposureTable();
        } else {
            foreach(var setting in test.GetProperty("parameters").EnumerateObject()) {
                var property=operation.GetType().GetProperty(setting.Name)
                    ?? throw new ArgumentException("Unknown ExposureFinder property: "+setting.Name);
                object value;
                if(property.PropertyType.IsEnum)value=Enum.ToObject(property.PropertyType,setting.Value.GetInt32());
                else if(property.PropertyType==typeof(bool))value=setting.Value.GetBoolean();
                else if(property.PropertyType==typeof(byte))value=setting.Value.GetByte();
                else if(property.PropertyType==typeof(decimal))value=setting.Value.GetDecimal();
                else throw new ArgumentException("Unsupported ExposureFinder property type: "+property.PropertyType);
                property.SetValue(operation,value);
            }
            operation.GenerateExposureTable();
            rows=operation.ExposureTable;
        }
        output.Add(new {name=test.GetProperty("name").GetString(),rows=rows.Select(row=>new {
            layer_height=row.LayerHeight,bottom_exposure=row.BottomExposure,
            normal_exposure=row.Exposure,brightness=row.Brightness,valid=row.IsValid}).ToArray()});
    }
    Console.WriteLine(JsonSerializer.Serialize(output));return;
}

if(args[1]=="--image-pixels") {
    if(args.Length!=4)throw new ArgumentException("--image-pixels requires input and new raw output");
    if(File.Exists(args[3]))throw new IOException("Refusing to overwrite pixel oracle output");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("UVtools could not identify image input");
    using var mask=(original.FirstLayer ?? throw new InvalidDataException("Image has no layer")).LayerMat;
    var pixels=new byte[checked(mask.Rows*mask.Cols)];
    for(var row=0;row<mask.Rows;++row)
        Marshal.Copy(IntPtr.Add(mask.DataPointer,checked(row*(int)mask.Step)),pixels,row*mask.Cols,mask.Cols);
    File.WriteAllBytes(args[3],pixels);
    Console.WriteLine(JsonSerializer.Serialize(new {status="decoded",width=mask.Cols,height=mask.Rows,
        sha256=Convert.ToHexString(SHA256.HashData(pixels)).ToLowerInvariant()}));return;
}
if(args[1]=="--image-encode" || args[1]=="--image-rgb-encode") {
    if(args.Length!=4)throw new ArgumentException("--image-encode requires an input and a new output");
    if(File.Exists(args[3]))throw new IOException("Refusing to overwrite image oracle output");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("UVtools could not identify image source");
    // This is exactly ImageFile.EncodeInternally's first-mask save operation.
    using var mask=(original.FirstLayer ?? throw new InvalidDataException("Image source has no layers")).LayerMat;
    if(args[1]=="--image-rgb-encode") {
        // Explicit alternative reference for Sun raster: its grayscale writer
        // omits the palette and the original decoder reads an all-black mask.
        // RGB channels all contain the same source samples; no pixel repair.
        using var rgb=new Emgu.CV.Mat();
        Emgu.CV.CvInvoke.CvtColor(mask,rgb,Emgu.CV.CvEnum.ColorConversion.Gray2Bgr);
        rgb.Save(args[3]);
    } else mask.Save(args[3]);
    Console.WriteLine(JsonSerializer.Serialize(new {status="encoded",mode=args[1],source=args[2],output=args[3]}));return;
}
if(args[1]=="--anycubic-enable-tsmc") {
    if(args.Length!=4)throw new ArgumentException("--anycubic-enable-tsmc requires input and new output");
    if(File.Exists(args[3]))throw new IOException("Refusing to overwrite TSMC oracle output");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full) as AnycubicFile
        ?? throw new InvalidDataException("Expected Anycubic source");
    original.LiftHeight2=2.5f;
    original.BottomLiftHeight2=3f;
    original.Encode(args[3]);
    Console.WriteLine(JsonSerializer.Serialize(new {status="encoded_by_original_setters",source=args[2],output=args[3]}));return;
}
if(args[1]=="--anycubic-inspect") {
    if(args.Length!=3)throw new ArgumentException("--anycubic-inspect requires a file");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full) as AnycubicFile
        ?? throw new InvalidDataException("Expected Anycubic input");
    var previews=new List<object>();
    foreach(var image in original.Thumbnails) {
        var rowSize=checked(image.Cols*image.NumberOfChannels);
        var pixels=new byte[checked(rowSize*image.Rows)];
        for(var row=0;row<image.Rows;++row)
            Marshal.Copy(IntPtr.Add(image.DataPointer,checked(row*(int)image.Step)),pixels,row*rowSize,rowSize);
        previews.Add(new {width=image.Cols,height=image.Rows,channels=image.NumberOfChannels,
            sha256=Convert.ToHexString(SHA256.HashData(pixels)).ToLowerInvariant()});
    }
    Console.WriteLine(JsonSerializer.Serialize(new {filename=args[2],status="fully_decoded",original.FileMarkSettings,
        original.HeaderSettings,original.ExtraSettings,original.MachineSettings,original.SoftwareSettings,
        original.ModelSettings,original.SubLayersDefinition,previews}));return;
}
if(args[1]=="--ctb-inspect") {
    if(args.Length!=3)throw new ArgumentException("--ctb-inspect requires a file");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full) as ChituboxFile
        ?? throw new InvalidDataException("Expected ordinary CTB input");
    var previews=new List<object>();
    foreach(var image in original.Thumbnails) {
        var rowSize=checked(image.Cols*image.NumberOfChannels);
        var pixels=new byte[checked(rowSize*image.Rows)];
        for(var row=0;row<image.Rows;++row)
            Marshal.Copy(IntPtr.Add(image.DataPointer,checked(row*(int)image.Step)),pixels,row*rowSize,rowSize);
        previews.Add(new {width=image.Cols,height=image.Rows,channels=image.NumberOfChannels,
            sha256=Convert.ToHexString(SHA256.HashData(pixels)).ToLowerInvariant()});
    }
    Console.WriteLine(JsonSerializer.Serialize(new {filename=args[2],status="fully_decoded",original.HeaderSettings,
        original.PrintParametersSettings,original.SlicerInfoSettings,original.PrintParametersV4Settings,
        original.ResinParametersSettings,previews}));return;
}
if(args[1]=="--ctb-schema") {
    if(args.Length!=2)throw new ArgumentException("--ctb-schema takes no input paths");
    using var original=new ChituboxFile();
    original.SlicerInfoSettings.ModifiedTimestampMinutes=0;
    var tables=new Dictionary<string,object>();
    foreach(var instance in new object[]{original.HeaderSettings,original.PrintParametersSettings,
        original.SlicerInfoSettings,original.PrintParametersV4Settings,original.ResinParametersSettings,
        new ChituboxFile.LayerDef(),new ChituboxFile.LayerDefEx(),new ChituboxFile.Preview()}) {
        var bytes=UVtools.Core.Helpers.Serialize(instance).ToArray();
        var fields=new Dictionary<string,object>();
        foreach(var property in instance.GetType().GetProperties()) {
            if(!property.CustomAttributes.Any(a=>a.AttributeType.Name=="FieldOrderAttribute"))continue;
            object? value=null;byte[]? marker=null;
            if(property.PropertyType==typeof(uint)){value=0x12345678U;marker=BitConverter.GetBytes((uint)value);}
            else if(property.PropertyType==typeof(ushort)){value=(ushort)0x1234;marker=BitConverter.GetBytes((ushort)value);}
            else if(property.PropertyType==typeof(byte)){value=(byte)0x5b;marker=[0x5b];}
            else if(property.PropertyType==typeof(float)){value=BitConverter.UInt32BitsToSingle(0x41234567);marker=BitConverter.GetBytes((float)value);}
            else continue;
            var before=property.GetValue(instance);property.SetValue(instance,value);
            var changed=UVtools.Core.Helpers.Serialize(instance).ToArray();
            var offset=changed.AsSpan().IndexOf(marker);property.SetValue(instance,before);
            if(offset<0) {
                fields.Add(property.Name,new {offset=(int?)null,size=marker.Length,type=property.PropertyType.Name,
                    note="Original serializer did not retain the assigned marker (for example an automatic string length)"});
                continue;
            }
            fields.Add(property.Name,new {offset,size=marker.Length,type=property.PropertyType.Name});
        }
        tables.Add(instance.GetType().Name,new {size=bytes.Length,bytes=Convert.ToHexString(bytes).ToLowerInvariant(),fields});
    }
    Console.WriteLine(JsonSerializer.Serialize(new {scope="original_Chitubox_schema_with_clock_zeroed",tables}));return;
}
if(args[1]=="--ctb-encrypted-crypto") {
    if(args.Length!=3)throw new ArgumentException("--ctb-encrypted-crypto requires native vectors");
    var key=(byte[])typeof(CTBEncryptedFile).GetField("Bigfoot",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    var iv=(byte[])typeof(CTBEncryptedFile).GetField("CookieMonster",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    using var input=JsonDocument.Parse(File.ReadAllText(args[2]));
    var output=new List<object>();
    foreach(var item in input.RootElement.EnumerateArray()) {
        var plain=Convert.FromHexString(item.GetProperty("plaintext").GetString()!);
        var cipher=CryptExtensions.AesCryptBytes(plain,key,CipherMode.CBC,PaddingMode.None,true,iv);
        var decoded=CryptExtensions.AesCryptBytes(cipher,key,CipherMode.CBC,PaddingMode.None,false,iv);
        var checksum=item.GetProperty("checksum").GetUInt64();
        var bytes=BitExtensions.ToBytesLittleEndian(checksum);
        var hash=SHA256.HashData(bytes);
        var signature=CryptExtensions.AesCryptBytes(hash,key,CipherMode.CBC,PaddingMode.None,true,iv);
        var layer=plain.ToArray();var seed=item.GetProperty("seed").GetUInt32();var index=item.GetProperty("index").GetUInt32();
        ChituboxFile.LayerRleCryptBuffer(seed,index,layer);
        var Hex=(byte[] data)=>Convert.ToHexString(data).ToLowerInvariant();
        output.Add(new {name=item.GetProperty("name").GetUInt32(),checksum,seed,index,
            encrypted=Hex(cipher),decrypted=Hex(decoded),digest=Hex(hash),signature=Hex(signature),xor=Hex(layer)});
    }
    Console.WriteLine(JsonSerializer.Serialize(output));return;
}
if(args[1]=="--ctb-encrypted-inspect") {
    if(args.Length!=3)throw new ArgumentException("--ctb-encrypted-inspect requires a file");
    using var original=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full) as CTBEncryptedFile
        ?? throw new InvalidDataException("Expected encrypted CTB input");
    var key=(byte[])typeof(CTBEncryptedFile).GetField("Bigfoot",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    var iv=(byte[])typeof(CTBEncryptedFile).GetField("CookieMonster",BindingFlags.NonPublic|BindingFlags.Static)!.GetValue(null)!;
    var raw=File.ReadAllBytes(args[2]);
    var block=raw.AsSpan((int)original.Header.SettingsOffset,(int)original.Header.SettingsSize).ToArray();
    var plain=CryptExtensions.AesCryptBytes(block,key,CipherMode.CBC,PaddingMode.None,false,iv);
    Console.WriteLine(JsonSerializer.Serialize(new {filename=args[2],status="fully_decoded",original.Header,original.Settings,
        original.ResinParametersSettings,plain_settings=Convert.ToHexString(plain).ToLowerInvariant()}));return;
}
if(args[1]=="--ctb-encrypted-schema") {
    if(args.Length!=2)throw new ArgumentException("--ctb-encrypted-schema takes no input paths");
    using var original=new CTBEncryptedFile();
    var header=UVtools.Core.Helpers.Serialize(original.Header).ToArray();
    var settings=UVtools.Core.Helpers.Serialize(original.Settings).ToArray();
    var resin=UVtools.Core.Helpers.Serialize(original.ResinParametersSettings).ToArray();
    var fields=new Dictionary<string,object>();
    foreach(var property in typeof(CTBEncryptedFile.SlicerSettings).GetProperties()) {
        if(!property.CustomAttributes.Any(a=>a.AttributeType.Name=="FieldOrderAttribute"))continue;
        object? value=null;byte[]? marker=null;
        if(property.PropertyType==typeof(uint)){value=0x12345678U;marker=BitConverter.GetBytes((uint)value);}
        else if(property.PropertyType==typeof(ulong)){value=0x123456789ABCDEF0UL;marker=BitConverter.GetBytes((ulong)value);}
        else if(property.PropertyType==typeof(ushort)){value=(ushort)0x1234;marker=BitConverter.GetBytes((ushort)value);}
        else if(property.PropertyType==typeof(byte)){value=(byte)0x5b;marker=[0x5b];}
        else if(property.PropertyType==typeof(float)){value=BitConverter.UInt32BitsToSingle(0x41234567);marker=BitConverter.GetBytes((float)value);}
        if(value is null||marker is null)throw new InvalidDataException("Unhandled settings property "+property.Name);
        var before=property.GetValue(original.Settings);property.SetValue(original.Settings,value);
        var modified=UVtools.Core.Helpers.Serialize(original.Settings).ToArray();
        var offset=modified.AsSpan().IndexOf(marker);property.SetValue(original.Settings,before);
        if(offset<0)throw new InvalidDataException("Missing serialized marker for "+property.Name);
        fields.Add(property.Name,new {offset,size=marker.Length,type=property.PropertyType.Name});
    }
    Console.WriteLine(JsonSerializer.Serialize(new {headerSize=header.Length,settingsSize=settings.Length,resinSize=resin.Length,
        header=Convert.ToHexString(header).ToLowerInvariant(),settings=Convert.ToHexString(settings).ToLowerInvariant(),
        resin=Convert.ToHexString(resin).ToLowerInvariant(),fields,original.AvailableVersions}));return;
}
if (args[1] == "--nanodlp-volume-set") {
    if(args.Length!=5)throw new ArgumentException("--nanodlp-volume-set requires input, new output, ml");
    if(File.Exists(args[3]))throw new IOException("Refusing to overwrite oracle output");
    using var source=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("Could not open NanoDLP input");
    if(source is not NanoDLPFile nano)throw new InvalidDataException("Expected NanoDLP input");
    var value=float.Parse(args[4],CultureInfo.InvariantCulture);
    if(!float.IsFinite(value)||value<0)throw new ArgumentException("Expected finite nonnegative ml");
    var before=nano.PlateManifest.TotalSolidArea;
    nano.MaterialMilliliters=value;
    var after=nano.PlateManifest.TotalSolidArea;
    nano.Encode(args[3]);
    Console.WriteLine(JsonSerializer.Serialize(new {scope="original_NanoDLP_volume_setter_then_Encode",
        input=args[2],output=args[3],material_ml=value,plate_before=before,plate_after_setter=after,
        plate_after_encode=nano.PlateManifest.TotalSolidArea}));
    return;
}
if (args[1] == "--contour-paths") {
    if(args.Length!=3)throw new ArgumentException("--contour-paths requires native contour fixtures");
    using var input=JsonDocument.Parse(File.ReadAllText(args[2]));
    var output=new List<object>();
    foreach(var fixture in input.RootElement.EnumerateArray()) {
        var width=fixture.GetProperty("width").GetInt32();var height=fixture.GetProperty("height").GetInt32();
        var pixels=Convert.FromHexString(fixture.GetProperty("pixels").GetString()!);
        if(pixels.Length!=checked(width*height))throw new InvalidDataException("Wrong contour mask size");
        using var mat=new Emgu.CV.Mat(height,width,Emgu.CV.CvEnum.DepthType.Cv8U,1);
        for(var y=0;y<height;++y)Marshal.Copy(pixels,y*width,IntPtr.Add(mat.DataPointer,checked(y*(int)mat.Step)),width);
        using var paths=mat.FindContours(out var hierarchy,Emgu.CV.CvEnum.RetrType.Tree);
        var contours=new List<object>();
        for(int i=0;i<paths.Size;++i) {
            int parent=hierarchy[i,3],root=i,depth=0;
            for(int current=parent;current>=0;current=hierarchy[current,3]) {root=current;++depth;}
            contours.Add(new {points=paths[i].ToArray().Select(p=>new[]{p.X,p.Y}).ToArray(),parent,root,
                hole=depth%2!=0,area=Emgu.CV.CvInvoke.ContourArea(paths[i]),
                perimeter=Emgu.CV.CvInvoke.ArcLength(paths[i],true)});
        }
        // Fill the NATIVE paths using the same primitive as SVGX's decoder.
        var native=fixture.GetProperty("contours").EnumerateArray().Select(c=>
            c.GetProperty("points").EnumerateArray().Select(p=>new System.Drawing.Point(p[0].GetInt32(),p[1].GetInt32())).ToArray()).ToArray();
        using var fill=new Emgu.CV.Mat(height,width,Emgu.CV.CvEnum.DepthType.Cv8U,1);
        fill.SetTo(new Emgu.CV.Structure.MCvScalar(0));
        if(native.Length>0) {
            using var vectors=new Emgu.CV.Util.VectorOfVectorOfPoint(native);
            Emgu.CV.CvInvoke.DrawContours(fill,vectors,-1,new Emgu.CV.Structure.MCvScalar(255),-1);
        }
        var actual=new byte[pixels.Length];
        for(var y=0;y<height;++y)Marshal.Copy(IntPtr.Add(fill.DataPointer,checked(y*(int)fill.Step)),actual,y*width,width);
        var binary=pixels.Select(p=>p==0?(byte)0:(byte)255).ToArray();
        output.Add(new {name=fixture.GetProperty("name").GetString(),contours,
            native_fill_sha256=Convert.ToHexString(SHA256.HashData(actual)).ToLowerInvariant(),
            binary_source_sha256=Convert.ToHexString(SHA256.HashData(binary)).ToLowerInvariant()});
    }
    Console.WriteLine(JsonSerializer.Serialize(output));
    return;
}
if (args[1] == "--contour-raw") {
    if(args.Length!=3)throw new ArgumentException("--contour-raw requires a native fixture JSON");
    using var input=JsonDocument.Parse(File.ReadAllText(args[2]));
    var output=new List<object>();
    foreach(var fixture in input.RootElement.EnumerateArray()) {
        var width=fixture.GetProperty("width").GetInt32();var height=fixture.GetProperty("height").GetInt32();
        var pixels=Convert.FromHexString(fixture.GetProperty("pixels").GetString()!);
        if(pixels.Length!=checked(width*height))throw new InvalidDataException("Wrong mask size");
        using var mat=new Emgu.CV.Mat(height,width,Emgu.CV.CvEnum.DepthType.Cv8U,1);
        for(var y=0;y<height;++y)Marshal.Copy(pixels,y*width,IntPtr.Add(mat.DataPointer,checked(y*(int)mat.Step)),width);
        using var contours=new UVtools.Core.EmguCV.EmguContours(mat);
        var pixelArea=fixture.GetProperty("pixel_area").GetSingle();
        var occupied=Enumerable.Range(0,pixels.Length).Where(i=>pixels[i]!=0).ToArray();
        var info=new NanoDLPFile.NanoDLPInfoItem {
            TotalSolidArea=MathF.Round((float)contours.TotalSolidArea*pixelArea,4,MidpointRounding.AwayFromZero),
            SmallestArea=MathF.Round((float)contours.MinSolidArea*pixelArea,4,MidpointRounding.AwayFromZero),
            LargestArea=MathF.Round((float)contours.MaxSolidArea*pixelArea,4,MidpointRounding.AwayFromZero),
            AreaCount=(uint)contours.ExternalContoursCount,
            MinX=occupied.Length>0?occupied.Min(i=>i%width):0,
            MinY=occupied.Length>0?occupied.Min(i=>i/width):0,
            MaxX=occupied.Length>0?occupied.Max(i=>i%width)+1:0,
            MaxY=occupied.Length>0?occupied.Max(i=>i/width)+1:0 };
        output.Add(new {name=fixture.GetProperty("name").GetString(),info,
            total_area=contours.TotalSolidArea,smallest_area=contours.MinSolidArea,
            largest_area=contours.MaxSolidArea,external_count=contours.ExternalContoursCount});
    }
    Console.WriteLine(JsonSerializer.Serialize(output,new JsonSerializerOptions{WriteIndented=true}));
    return;
}
if (args[1] == "--nanodlp-schema") {
    if(args.Length!=2)throw new ArgumentException("--nanodlp-schema takes no file paths");
    using var nano=new NanoDLPFile();
    // Explicitly fixed clock fields make this a deterministic schema/default
    // oracle, not an original encoded file or firmware acceptance certificate.
    nano.PlateManifest.Updated=0; nano.ProfileManifest.Updated=0;
    Console.WriteLine(JsonSerializer.Serialize(new {
        scope="original_NanoDLP_schema_defaults_with_clock_zeroed",
        meta=nano.MetaManifest,slicer=nano.SlicerManifest,plate=nano.PlateManifest,
        profile=nano.ProfileManifest,info=new NanoDLPFile.NanoDLPInfoItem()
    },new JsonSerializerOptions{WriteIndented=true}));
    return;
}
if (args[1] == "--zcode-rsa-lines") {
    if (args.Length != 4) throw new ArgumentException("--zcode-rsa-lines requires plaintext and ciphertext paths");
    var plain = File.ReadAllLines(args[2]);
    var encoded = File.ReadAllLines(args[3]);
    if (plain.Length == 0 || plain.Length != encoded.Length)
        throw new InvalidDataException("RSA line fixtures must be complete and nonempty");
    var cryptoName = typeof(ZCodeFile).Assembly.GetReferencedAssemblies()
        .Single(n => n.Name!.StartsWith("BouncyCastle", StringComparison.Ordinal));
    var crypto = Assembly.Load(cryptoName);
    var pemType = crypto.GetType("Org.BouncyCastle.OpenSsl.PemReader", true)!;
    var engineType = crypto.GetType("Org.BouncyCastle.Crypto.Engines.RsaEngine", true)!;
    object ReadKey(string pem) {
        var reader = Activator.CreateInstance(pemType, new StringReader(pem))!;
        return pemType.GetMethod("ReadObject")!.Invoke(reader, null)!;
    }
    var privateEngine = Activator.CreateInstance(engineType)!;
    var publicEngine = Activator.CreateInstance(engineType)!;
    engineType.GetMethod("Init")!.Invoke(privateEngine, new object[]{true, ReadKey(ZCodeFile.GCodeRSAPrivateKey)});
    engineType.GetMethod("Init")!.Invoke(publicEngine, new object[]{true, ReadKey(ZCodeFile.GCodeRSAPublicKey)});
    var processBlock = engineType.GetMethod("ProcessBlock")!;
    var timer = System.Diagnostics.Stopwatch.StartNew();
    for (int index=0; index<plain.Length; ++index) {
        var data=System.Text.Encoding.UTF8.GetBytes(plain[index]);
        if (data.Length==0 || data.Length>61) throw new InvalidDataException("Invalid fixture line length");
        var pad = new List<byte>{0,1,0}; pad.AddRange(data);
        while(pad.Count<64) pad.Insert(2,255);
        var original=(byte[])processBlock.Invoke(privateEngine,new object[]{pad.ToArray(),0,64})!;
        if(Convert.ToBase64String(original)!=encoded[index])
            throw new InvalidDataException($"Original RSA encryption differs at line {index}");
        var bytes=Convert.FromBase64String(encoded[index]);
        var decoded=(byte[])processBlock.Invoke(publicEngine,new object[]{bytes,0,bytes.Length})!;
        decoded=decoded.Skip(2).SkipWhile(b=>b is 255 or 0).ToArray();
        if(!decoded.SequenceEqual(data)) throw new InvalidDataException($"Original RSA decode differs at line {index}");
    }
    timer.Stop();
    Console.WriteLine(JsonSerializer.Serialize(new {status="passed", scope="original ZCode RSA engine and padding, not archive/firmware",
        lines=plain.Length,original_serial_encrypt_and_decrypt_seconds=timer.Elapsed.TotalSeconds}));
    return;
}
if (args[1] == "--jxs-plan") {
    if (args.Length != 4 || File.Exists(args[3]))
        throw new ArgumentException("--jxs-plan requires input and new output paths");
    using var original = FileFormat.Open(args[2], FileFormat.FileDecodeType.Full) as JXSFile
        ?? throw new InvalidDataException("Expected JXS source");
    // Unmodified original Encode builds ControlFile correctly in memory, but
    // serializes ConfigFile to control.json. Retain its raw broken output and
    // expose the original action plan as a separate, explicitly scoped oracle.
    original.Encode(args[3]);
    Console.WriteLine(JsonSerializer.Serialize(new { scope="original_in_memory_JXS_control_not_raw_file",
        rawOutput=args[3], control=original.ControlFile }));
    return;
}
if (args[1] == "--roundtrip") {
    if (args.Length != 4) throw new ArgumentException("--roundtrip requires input and output paths");
    if (Path.GetFullPath(args[2]) == Path.GetFullPath(args[3]) || File.Exists(args[3]))
        throw new ArgumentException("Roundtrip output must be a new file");
    using var original = FileFormat.Open(args[2], FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("Unknown input format");
    original.Encode(args[3]);
    Console.WriteLine(JsonSerializer.Serialize(new { status="encoded_by_uvtools",input=args[2],output=args[3] }));
    return;
}
if (args[1]=="--convert-aa") {
    if(args.Length!=6)throw new ArgumentException("--convert-aa requires input, new output, version and AA count");
    if(File.Exists(args[3]))throw new IOException("Refusing to replace an AA reference");
    var extension=Path.GetExtension(args[3]).ToLowerInvariant();
    if(extension!=".cbddlp" && extension!=".photon")throw new ArgumentException("AA reference requires CBDDLP or Photon");
    using var source=FileFormat.Open(args[2],FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("Unknown AA source");
    source.AntiAliasing=byte.Parse(args[5],CultureInfo.InvariantCulture);
    using var converted=source.Convert(typeof(ChituboxFile),args[3],uint.Parse(args[4],CultureInfo.InvariantCulture))
        ?? throw new InvalidDataException("Original AA conversion was rejected");
    Console.WriteLine(JsonSerializer.Serialize(new {status="converted_by_uvtools",input=args[2],output=args[3],
        converted.Version,converted.AntiAliasing}));return;
}
if (args[1] == "--convert") {
    if (args.Length != 5 && !(args.Length == 6 && args[5] == "--preserve-layer-rest"))
        throw new ArgumentException("--convert requires input, new output, version and optional --preserve-layer-rest");
    if (Path.GetFullPath(args[2]) == Path.GetFullPath(args[3]) || File.Exists(args[3]))
        throw new ArgumentException("Conversion output must be a new file");
    using var original = FileFormat.Open(args[2], FileFormat.FileDecodeType.Full)
        ?? throw new InvalidDataException("Unknown input format");
    // Ask the unmodified registry for the target family, then call the original
    // conversion API. No hand-written metadata or image transformation here.
    using var target = FileFormat.FindByExtensionOrFilePath(Path.GetExtension(args[3]).TrimStart('.'), true)
        ?? throw new InvalidDataException("Unknown target format");
    using var converted = original.Convert(target.GetType(), args[3], uint.Parse(args[4], CultureInfo.InvariantCulture))
        ?? throw new InvalidDataException("UVtools conversion was rejected");
    var preserveLayerRest = args.Length == 6;
    if (preserveLayerRest) {
        if ((converted is not ChituboxFile && converted is not PHZFile) || converted.Version != 2)
            throw new ArgumentException("Layer rest normalization is only defined for Chitubox/PHZ v2");
        // Keep the ordinary conversion above as a separate reference artifact.
        // UVtools Convert clones SL1 layers whose LightOffDelay is zero; the v2
        // writer has no rest-time fields, so those cloned layers lose their
        // explicit wait. Use the ORIGINAL Layer calculator to represent that
        // same source rest in the legacy target's light-off field before Encode.
        for (uint i = 0; i < converted.LayerCount; ++i)
            converted[i].SetLightOffDelay(original[i].WaitTimeBeforeCure);
        converted.Encode(args[3]);
    }
    Console.WriteLine(JsonSerializer.Serialize(new { status="converted_by_uvtools",input=args[2],output=args[3],
        version=converted.Version,preserveLayerRest }));
    return;
}
if (args[1] == "--fdg-cipher") {
    if(args.Length!=5)throw new ArgumentException("--fdg-cipher requires seed, index, hex bytes");
    var data=Convert.FromHexString(args[4]);
    FDGFile.LayerRleCryptBuffer(uint.Parse(args[2],CultureInfo.InvariantCulture),uint.Parse(args[3],CultureInfo.InvariantCulture),data);
    Console.WriteLine(Convert.ToHexString(data).ToLowerInvariant());
    return;
}
if (args[1] == "--phz-cipher") {
    var data = Convert.FromHexString(args[4]);
    PHZFile.LayerRleCryptBuffer(uint.Parse(args[2],CultureInfo.InvariantCulture),uint.Parse(args[3],CultureInfo.InvariantCulture),data);
    Console.WriteLine(Convert.ToHexString(data).ToLowerInvariant());
    return;
}
var fullOpen = args[1] == "--full-open";
foreach (var filename in args.Skip(fullOpen ? 2 : 1)) {
    if (fullOpen) {
        using var fullFile = FileFormat.Open(filename, FileFormat.FileDecodeType.Full)
            ?? throw new InvalidDataException("UVtools could not identify " + filename);
        var decodedLayers = new List<object>();
        for (uint i = 0; i < fullFile.LayerCount; ++i) {
            var layer = fullFile[i];
            using var mat = layer.LayerMat;
            if (mat.NumberOfChannels != 1 || mat.Depth != Emgu.CV.CvEnum.DepthType.Cv8U)
                throw new InvalidDataException("Expected an 8-bit grayscale layer");
            var pixels = new byte[checked(mat.Rows * mat.Cols)];
            for (var row = 0; row < mat.Rows; ++row)
                Marshal.Copy(IntPtr.Add(mat.DataPointer, checked(row * (int)mat.Step)), pixels, row * mat.Cols, mat.Cols);
            decodedLayers.Add(new { index=i, width=mat.Cols, height=mat.Rows,
                sha256=Convert.ToHexString(SHA256.HashData(pixels)).ToLowerInvariant(),
                layer.PositionZ,layer.ExposureTime,layer.WaitTimeBeforeCure,layer.WaitTimeAfterCure,
                layer.LayerHeight,layer.LightOffDelay,layer.WaitTimeAfterLift,
                layer.NonZeroPixelCount,layer.MaterialMilliliters,
                layer.LiftHeight,layer.LiftHeight2,layer.LiftSpeed,layer.LiftSpeed2,
                layer.RetractHeight2,layer.RetractSpeed,layer.RetractSpeed2,layer.LightPWM });
        }
        // Parallel UVtools decoding updates a cached global total as individual
        // layers arrive. Refresh it after all masks have been read: the last
        // concurrent subtotal is not a stable whole-file comparison value.
        fullFile.MaterialMilliliters = -1;
        reports.Add(new { filename,status="fully_decoded",family=fullFile.GetType().Name,
            fullFile.Version,fullFile.LayerCount,fullFile.ResolutionX,fullFile.ResolutionY,fullFile.DisplayWidth,fullFile.DisplayHeight,
            fullFile.LayerHeight,fullFile.BottomLayerCount,fullFile.TransitionLayerCount,fullFile.ExposureTime,fullFile.BottomExposureTime,
            fullFile.MachineName,fullFile.MachineZ,fullFile.PrintHeight,fullFile.AntiAliasing,fullFile.DisplayMirror,
            fullFile.PrintTime,fullFile.MaterialMilliliters,
            fullFile.WaitTimeBeforeCure,fullFile.WaitTimeAfterCure,fullFile.WaitTimeAfterLift,
            fullFile.BottomWaitTimeBeforeCure,fullFile.BottomWaitTimeAfterCure,fullFile.BottomWaitTimeAfterLift,
            fullFile.LiftHeight,fullFile.LiftHeight2,fullFile.LiftSpeed,fullFile.LiftSpeed2,
            fullFile.BottomLiftHeight,fullFile.BottomLiftHeight2,fullFile.BottomLiftSpeed,fullFile.BottomLiftSpeed2,
            fullFile.RetractHeight2,fullFile.RetractSpeed,fullFile.RetractSpeed2,
            fullFile.BottomRetractHeight2,fullFile.BottomRetractSpeed,fullFile.BottomRetractSpeed2,
            fullFile.LightPWM,fullFile.BottomLightPWM,
            decodedLayers });
        continue;
    }
    if (filename == "--registry") {
        reports.AddRange(FileFormat.AvailableFormats.Select(format => new {
            family=format.GetType().Name, extensions=format.FileExtensions.Select(extension=>extension.Extension).ToArray(),
            extensionVersions=format.FileExtensions.Select(extension=>new { extension=extension.Extension,
                versions=format.GetAvailableVersionsForExtension(extension.Extension) }).ToArray(),
            format.AvailableVersions,format.DefaultVersion }));
        continue;
    }
    if (filename == "--makerbase-schema") {
        using var gr1 = Helpers.Serialize(new GR1File.Header());
        using var mdlp = Helpers.Serialize(new MDLPFile.Header());
        using var line = Helpers.Serialize(new GR1File.LayerLine(1,2,3));
        reports.Add(new { status="schema",gr1HeaderSize=gr1.Length,mdlpHeaderSize=mdlp.Length,
            gr1Header=Convert.ToBase64String(gr1.ToArray()),mdlpHeader=Convert.ToBase64String(mdlp.ToArray()),
            lineSize=line.Length,line=Convert.ToBase64String(line.ToArray()) });
        continue;
    }
    if (filename == "--lgs-schema") {
        using var header = Helpers.Serialize(new LGSFile.Header());
        reports.Add(new { status="schema",headerSize=header.Length,
            defaultHeader=Convert.ToBase64String(header.ToArray()) });
        continue;
    }
    if (filename == "--svgx-schema") {
        using var header = Helpers.Serialize(new FlashForgeSVGXFile.Header());
        using var preview = Helpers.Serialize(new FlashForgeSVGXFile.Preview { BGR=[] });
        reports.Add(new {status="schema",headerSize=header.Length,previewHeaderSize=preview.Length,
            defaultHeader=Convert.ToBase64String(header.ToArray()),
            defaultPreview=Convert.ToBase64String(preview.ToArray()),
            defaultSVG=new FlashForgeSVGXSvg().SerializeToString()});
        continue;
    }
    if (filename == "--chitubox-schema") {
        using var header = Helpers.Serialize(new ChituboxFile.Header());
        using var layer = Helpers.Serialize(new ChituboxFile.LayerDef());
        using var parameters = Helpers.Serialize(new ChituboxFile.PrintParameters());
        using var preview = Helpers.Serialize(new ChituboxFile.Preview());
        reports.Add(new {status="schema",headerSize=header.Length,layerSize=layer.Length,parametersSize=parameters.Length,
            previewSize=preview.Length,defaultParameters=Convert.ToBase64String(parameters.ToArray()),
            extensions=new ChituboxFile().FileExtensions.Select(e=>e.Extension).ToArray()});
        continue;
    }
    if (filename == "--fdg-schema") {
        using var header = Helpers.Serialize(new FDGFile.Header());
        using var layer = Helpers.Serialize(new FDGFile.LayerDef());
        using var preview = Helpers.Serialize(new FDGFile.Preview());
        reports.Add(new {status="schema",headerSize=header.Length,layerSize=layer.Length,previewSize=preview.Length,
            defaultHeader=Convert.ToBase64String(header.ToArray())});
        continue;
    }
    if (filename == "--phz-schema") {
        using var header = Helpers.Serialize(new PHZFile.Header());
        using var layer = Helpers.Serialize(new PHZFile.LayerDef());
        using var preview = Helpers.Serialize(new PHZFile.Preview());
        reports.Add(new {status="schema",headerSize=header.Length,layerSize=layer.Length,previewSize=preview.Length,
            defaultHeader=Convert.ToBase64String(header.ToArray())});
        continue;
    }
    if (filename == "--osf-schema") {
        var osfSchemaSettings = new OSFFile.OSFSettings();
        using var serialized = Helpers.Serialize(osfSchemaSettings);
        using var serializedHeader = Helpers.Serialize(new OSFFile.OSFHeader());
        using var serializedLayer = Helpers.Serialize(new OSFFile.OSFLayerDef());
        reports.Add(new { status="schema", headerSize=serializedHeader.Length,
            settingsSize=serialized.Length, layerSize=serializedLayer.Length,
            defaultSettings=Convert.ToBase64String(serialized.ToArray()) });
        continue;
    }
    if (filename.EndsWith(".osf", StringComparison.OrdinalIgnoreCase)) {
        using var osfStream=File.OpenRead(filename);
        using var osfReader=new BinaryReader(osfStream);
        var osfHeader=Helpers.Deserialize<OSFFile.OSFHeader>(osfStream);
        if(osfHeader.Version!=4 || osfHeader.ImageLog!=2)throw new InvalidDataException("OSF header");
        uint OsfUnsigned(int width) {uint value=0;for(int byteIndex=0;byteIndex<width;byteIndex++)value=(value<<8)|osfReader.ReadByte();return value;}
        var osfPreviewSizes=new uint[]{148*80*2,300*140*2,208*116*2,404*240*2};
        foreach(var expectedSize in osfPreviewSizes){var actualSize=OsfUnsigned(3);if(actualSize!=expectedSize)throw new InvalidDataException("OSF preview size");osfStream.Position+=actualSize;}
        var osfSettings=Helpers.Deserialize<OSFFile.OSFSettings>(osfStream);
        if(osfStream.Position!=osfHeader.HeaderLength || osfSettings.ResolutionX==0 || osfSettings.ResolutionY==0 || osfSettings.LayerCount==0 || osfSettings.LastLayerIndex!=osfSettings.LayerCount-1)
            throw new InvalidDataException("OSF settings/addresses");
        var osf=new OSFFile();
        typeof(OSFFile).GetProperty("Header")!.SetValue(osf,osfHeader);
        typeof(OSFFile).GetProperty("Settings")!.SetValue(osf,osfSettings);
        var osfLayerReports=new List<object>();
        for(uint index=0;index<osfSettings.LayerCount;index++) {
            var osfLayer=Helpers.Deserialize<OSFFile.OSFLayerDef>(osfStream);
            if(osfLayer.Mark!=0x0d0a || osfLayer.StartY>=osfSettings.ResolutionY)throw new InvalidDataException("OSF layer table");
            ulong pixels=(ulong)osfLayer.StartY*osfSettings.ResolutionX,nonzero=0;
            for(uint run=0;run<osfLayer.NumberOfLines;run++) {
                var color=osfReader.ReadByte();uint length=1;
                if((color&1)!=0) {
                    var first=osfReader.ReadByte();
                    if(color==0x0d && (first==0x0a || first==0x0b))throw new InvalidDataException("OSF layer-marker collision");
                    if((first&0x80)==0)length=first;
                    else if((first&0xc0)==0x80)length=((uint)(first&0x3f)<<8)|OsfUnsigned(1);
                    else if((first&0xe0)==0xc0)length=((uint)(first&0x1f)<<16)|OsfUnsigned(2);
                    else if((first&0xf0)==0xe0)length=((uint)(first&0x0f)<<24)|OsfUnsigned(3);
                    else throw new InvalidDataException("OSF run prefix");
                }
                if(length==0)throw new InvalidDataException("OSF empty run");
                pixels+=length;if((color&0xfe)!=0)nonzero+=length;
                if(pixels>(ulong)osfSettings.ResolutionX*osfSettings.ResolutionY)throw new InvalidDataException("OSF raster overflow");
            }
            if(pixels!=(ulong)osfSettings.ResolutionX*osfSettings.ResolutionY)throw new InvalidDataException("OSF incomplete mask");
            osfLayerReports.Add(new{index,osfLayer.NumberOfLines,nonzero});
        }
        if(osfStream.Position!=osfStream.Length)throw new InvalidDataException("OSF trailing bytes");
        reports.Add(new{filename,status="parsed",scope="UVtools original OSF tables and parameter getters; independent RLE checks, no native Mat decode",
            osf.ResolutionX,osf.ResolutionY,osf.LayerHeight,osf.BottomLayerCount,osf.TransitionLayerCount,
            osf.ExposureTime,osf.BottomExposureTime,osf.LiftHeight,osf.LiftHeight2,osf.LiftSpeed,osf.LiftSpeed2,
            osf.RetractHeight2,osf.RetractSpeed,osf.RetractSpeed2,osf.WaitTimeBeforeCure,osf.WaitTimeAfterCure,osf.WaitTimeAfterLift,
            osf.BottomWaitTimeBeforeCure,osf.BottomWaitTimeAfterCure,osf.BottomWaitTimeAfterLift,osf.LightPWM,osf.BottomLightPWM,
            osfSettings.PixelUmMagnified100Times,layers=osfLayerReports});
        continue;
    }
    if (filename.EndsWith(".osla", StringComparison.OrdinalIgnoreCase)) {
        using (var encodingProbe = Helpers.Serialize(new OSLAFile.Header { MachineName = "Żółć Ω" })) {
            var probeBytes = encodingProbe.ToArray();
            var expectedName = System.Text.Encoding.UTF8.GetBytes("Żółć Ω");
            if (!probeBytes.AsSpan(probeBytes.Length-50,expectedName.Length).SequenceEqual(expectedName))
                throw new InvalidDataException("OSLA serializer encoding probe: "+Convert.ToHexString(probeBytes.AsSpan(probeBytes.Length-50)));
        }
        using var oslaStream = File.OpenRead(filename);
        using var oslaReader = new BinaryReader(oslaStream, System.Text.Encoding.ASCII, true);
        var definition = Helpers.Deserialize<OSLAFile.FileDef>(oslaStream); definition.Validate();
        var oslaHeader = Helpers.Deserialize<OSLAFile.Header>(oslaStream);
        if (definition.Version != 0 || oslaHeader.TableSize != 199 || oslaHeader.LayerTableSize != 73 || oslaHeader.PreviewTableSize != 8 ||
            oslaHeader.LayerDataType != "PNG" || oslaHeader.PreviewDataType != "PNG") throw new InvalidDataException("OSLA table schema");
        var custom = Helpers.Deserialize<OSLAFile.CustomTable>(oslaStream);
        if (custom.TableSize != 0) throw new InvalidDataException("OSLA unexpected custom table");
        for (uint i=0;i<oslaHeader.PreviewCount;++i) {
            var preview = Helpers.Deserialize<OSLAFile.Preview>(oslaStream);
            var image = oslaReader.ReadBytes(checked((int)preview.ImageLength));
            if (image.Length != preview.ImageLength || image.Length < 24 || image[0] != 137 || image[1] != 80 ||
                System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(16)) != preview.ResolutionX ||
                System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(20)) != preview.ResolutionY)
                throw new InvalidDataException("OSLA preview PNG");
        }
        if (oslaStream.Position != oslaHeader.LayerDefinitionsAddress) throw new InvalidDataException("OSLA table address");
        var osla = new OSLAFile(); osla.Init(oslaHeader.LayerCount, true);
        var entries = new List<(uint Address, OSLAFile.LayerDef Layer)>();
        for(uint i=0;i<oslaHeader.LayerCount;++i) {
            var address = oslaReader.ReadUInt32();
            var layer = Helpers.Deserialize<OSLAFile.LayerDef>(oslaStream); layer.CopyTo(osla[i]);
            if (osla[i].PositionZ <= 0 || osla[i].ExposureTime <= 0) throw new InvalidDataException("OSLA layer parameters");
            entries.Add((address,layer));
        }
        foreach(var entry in entries) {
            oslaStream.Position=entry.Address; var size=oslaReader.ReadUInt32();var image=oslaReader.ReadBytes(checked((int)size));
            if(image.Length!=size || image.Length<24 || image[0]!=137 || image[1]!=80 ||
                System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(16))!=oslaHeader.ResolutionX ||
                System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(20))!=oslaHeader.ResolutionY)
                throw new InvalidDataException("OSLA layer PNG/address");
        }
        oslaStream.Position=oslaHeader.GCodeAddress;var program=Helpers.Deserialize<OSLAFile.GCodeDef>(oslaStream);
        if(program.GCodeText is null || program.GCodeText.Length!=program.GCodeSize ||
            System.Text.Encoding.ASCII.GetString(oslaReader.ReadBytes(9))!=";OSLATiCo" || oslaStream.Position!=oslaStream.Length)
            throw new InvalidDataException("OSLA motion/footer");
        var fields = new[] {"PositionZ", "ExposureTime", "LiftHeight", "LiftSpeed", "LiftHeight2", "LiftSpeed2", "WaitTimeAfterLift",
            "RetractSpeed", "RetractHeight2", "RetractSpeed2", "WaitTimeBeforeCure", "WaitTimeAfterCure", "LightPWM"};
        double[] Snapshot(UVtools.Core.Layers.Layer layer) => fields.Select(name=>
            Convert.ToDouble(layer.GetType().GetProperty(name)!.GetValue(layer),CultureInfo.InvariantCulture)).ToArray();
        var expected = osla.Layers.Select(Snapshot).ToArray();
        foreach(var layer in osla.Layers) typeof(UVtools.Core.Layers.Layer).GetProperty("NonZeroPixelCount")!.SetValue(layer,(uint)2);
        osla.GCode!.ParseLayersFromGCode(osla,program.GCodeText,false);
        for(var i=0;i<expected.Length;++i) {
            var actual=Snapshot(osla[i]);
            for(var j=0;j<actual.Length;++j) if(Math.Abs(actual[j]-expected[i][j])>.001)
                throw new InvalidDataException($"OSLA motion/table mismatch layer {i}, {fields[j]}: {actual[j]} vs {expected[i][j]}");
        }
        reports.Add(new { filename, status="parsed", scope="UVtools original OSLA tables and LayerDef.CopyTo; PNG IHDR checks, no native Mat decode",
            oslaHeader.ResolutionX, oslaHeader.ResolutionY, oslaHeader.DisplayWidth, oslaHeader.DisplayHeight, oslaHeader.MachineZ,
            oslaHeader.LayerHeight, oslaHeader.BottomLayersCount, oslaHeader.LayerCount, oslaHeader.PrintTime, oslaHeader.MaterialMilliliters,
            oslaHeader.MachineName, layers=entries.Select(entry=>new { entry.Address, definition=entry.Layer }) });
        continue;
    }
    if (filename.EndsWith(".n4", StringComparison.OrdinalIgnoreCase) || filename.EndsWith(".n7", StringComparison.OrdinalIgnoreCase)) {
        using var anetStream = File.OpenRead(filename);
        var header = Helpers.Deserialize<AnetFile.Header>(anetStream);
        if (header.Version != "3" || header.DescriptionLength != 0 || header.PreviewSize != 72866 ||
            header.PreviewResolutionX != 260 || header.PreviewResolutionY != 140 || header.BaseLayersCount != 0 || header.FilledBaseLayersCount != 0)
            throw new InvalidDataException("Anet header");
        var layerReports = new List<object>();
        for (uint i = 0; i < header.LayersCount; ++i) {
            var layer = Helpers.Deserialize<AnetFile.LayerDef>(anetStream);
            var rle = new byte[layer.RleBytesCount]; anetStream.ReadExactly(rle);
            uint position = 0;
            uint Bits(uint count) {
                if (count > 32 || (ulong)position + count > layer.BitsCount) throw new InvalidDataException("Anet bit bounds");
                uint value = 0; for (uint bit = 0; bit < count; ++bit, ++position) value = (value << 1) | (uint)((rle[position / 8] >> (int)(position % 8)) & 1);
                return value;
            }
            var width = Bits(16); var height = Bits(16); bool white = Bits(1) != 0;
            uint pixels = 0, whitePixels = 0;
            while ((ulong)pixels < (ulong)width * height) {
                var key = Bits(5); var run = Bits(key + 1);
                if (run == 0 || (ulong)pixels + run > (ulong)width * height) throw new InvalidDataException("Anet run");
                pixels += run; if (white) whitePixels += run; white = !white;
            }
            if (position != layer.BitsCount || whitePixels != layer.WhitePixelsCount) throw new InvalidDataException("Anet decoded size/count");
            layerReports.Add(new { width, height, whitePixels, layer.XMin, layer.YMin, layer.XMax, layer.YMax, layer.BitsCount });
        }
        var footer = new byte[4]; anetStream.ReadExactly(footer);
        if (footer.AsSpan().IndexOfAnyExcept((byte)0) >= 0 || anetStream.Position != anetStream.Length) throw new InvalidDataException("Anet support count/footer");
        reports.Add(new { filename, status = "parsed", scope = "UVtools original Header/LayerDef deserialization; independent RLE bit parsing, no native Mat decode",
            header.Version, header.Name, header.XYPixelSize, header.LayerHeight, header.ExposureTime, header.BottomExposureTime, header.BottomLayerCount,
            header.LiftSpeed, header.LiftHeight, header.VolumeMicroL, header.EncodedPrintTime, header.LayersCount, layers = layerReports });
        continue;
    }
    var filePrefix = new byte[8];
    using (var prefixStream = File.OpenRead(filename)) prefixStream.ReadExactly(filePrefix);
    if (System.Text.Encoding.ASCII.GetString(filePrefix) == "ANYCUBIC") {
        try {
            using var anyStream = File.OpenRead(filename);
            var anyMark = Helpers.Deserialize<AnycubicFile.FileMark>(anyStream);
            if (anyMark.Mark != AnycubicFile.FileMark.SectionMarkFile) throw new InvalidDataException("Anycubic mark");
            foreach (var address in new[] {anyMark.HeaderAddress, anyMark.PreviewAddress, anyMark.LayerDefinitionAddress, anyMark.LayerImageAddress, anyMark.MachineAddress, anyMark.ModelAddress})
                if (address >= anyStream.Length) throw new InvalidDataException("Anycubic table address outside file: " + address);
            AnycubicFile.Machine? anyMachine = null;
            if (anyMark.MachineAddress > 0) {
                anyStream.Position=anyMark.MachineAddress;
                var machineMark = new byte[12]; anyStream.ReadExactly(machineMark);
                if (System.Text.Encoding.ASCII.GetString(machineMark).TrimEnd('\0') != "MACHINE")
                    throw new InvalidDataException("Anycubic machine address does not point to MACHINE");
                anyStream.Position=anyMark.MachineAddress;
                anyMachine=Helpers.Deserialize<AnycubicFile.Machine>(anyStream); anyMachine.Validate();
            }
            anyStream.Position = anyMark.HeaderAddress;
            var anyHeader = Helpers.Deserialize<AnycubicFile.Header>(anyStream); anyHeader.Validate();
            anyStream.Position = anyMark.LayerDefinitionAddress;
            var anyLayers = Helpers.Deserialize<AnycubicFile.LayerDefinition>(anyStream); anyLayers.Validate();
            var anyFile = new AnycubicFile { FileFullPath=filename };
            typeof(AnycubicFile).GetProperty(nameof(AnycubicFile.FileMarkSettings))!.SetValue(anyFile, anyMark);
            typeof(AnycubicFile).GetProperty(nameof(AnycubicFile.HeaderSettings))!.SetValue(anyFile, anyHeader);
            if (anyMachine is not null) typeof(AnycubicFile).GetProperty(nameof(AnycubicFile.MachineSettings))!.SetValue(anyFile, anyMachine);
            var anyPixels = checked((int)(anyHeader.ResolutionX*anyHeader.ResolutionY));
            if (anyPixels <= 0 || anyPixels > 128*1024*1024) throw new InvalidDataException("Anycubic raster limit");
            var anyLayerReports = new List<object>();
            foreach (var anyLayer in anyLayers.Layers) {
                if ((ulong)anyLayer.DataAddress+anyLayer.DataLength > (ulong)anyStream.Length || anyLayer.DataLength > 1024*1024*1024)
                    throw new InvalidDataException("Anycubic mask address");
                anyStream.Position=anyLayer.DataAddress;
                var rle = new byte[anyLayer.DataLength]; anyStream.ReadExactly(rle);
                long decoded=0, nonzero=0;
                for (var offset=0;offset<rle.Length;++offset) {
                    var value=rle[offset]; int run,color;
                    if (filename.EndsWith(".pws",StringComparison.OrdinalIgnoreCase)) { run=value&127; color=value&128; }
                    else {
                        color=value>>4; run=value&15;
                        if (color==0 || color==15) {
                            if (++offset>=rle.Length) throw new InvalidDataException("Truncated PW0 run");
                            run=(run<<8)|rle[offset];
                        }
                    }
                    decoded+=run; if (color>0) nonzero+=run;
                    if (decoded>anyPixels) throw new InvalidDataException("Anycubic RLE exceeds raster");
                }
                if (decoded!=anyPixels) throw new InvalidDataException("Anycubic RLE does not cover raster: "+decoded+" / "+anyPixels);
                anyLayerReports.Add(new {anyLayer.LayerHeight,anyLayer.ExposureTime,anyLayer.LiftHeight,anyLayer.LiftSpeed,anyLayer.NonZeroPixelCount, decoded,nonzero});
            }
            reports.Add(new { filename,status="parsed",scope="Original UVtools FileMark/Header/LayerDefinition and independent source-derived RLE; no native image decode or firmware validation",
                anyMark.Version,anyHeader.ResolutionX,anyHeader.ResolutionY,anyFile.DisplayWidth,anyFile.DisplayHeight,anyFile.MachineZ,anyHeader.PixelSizeUm,anyHeader.LayerHeight,anyHeader.ExposureTime,anyHeader.BottomExposureTime,
                anyHeader.BottomLayersCount,anyHeader.WaitTimeBeforeCure,anyHeader.LiftHeight,anyHeader.LiftSpeed,anyHeader.RetractSpeed,anyHeader.VolumeMl,
                layers=anyLayerReports });
        } catch (Exception error) { reports.Add(new {filename,status="rejected",error=error.Message}); }
        continue;
    }
    if (filename.EndsWith(".v1.cxdlp",StringComparison.OrdinalIgnoreCase)) {
        using var cxStream=File.OpenRead(filename);
        var cxHeader=Helpers.Deserialize<CrealityCXDLPv1File.Header>(cxStream);cxHeader.Validate();
        if(cxHeader.ResolutionX==0 || cxHeader.ResolutionY==0 || cxHeader.ResolutionX>16384 || cxHeader.ResolutionY>8192)
            throw new InvalidDataException("CXDLPv1 resolution");
        void CxPage() {
            var page=Helpers.Deserialize<CrealityCXDLPv1File.PageBreak>(cxStream);
            if(page.Line!=13 || page.Break!=10) throw new InvalidDataException("CXDLPv1 page break");
        }
        foreach(var size in new[]{116,290,290}) {cxStream.Seek(size*size*2,SeekOrigin.Current);CxPage();}
        var cxInfo=Helpers.Deserialize<CrealityCXDLPv1File.SlicerInfo>(cxStream);
        var cxCounts=new uint[cxHeader.LayerCount];
        for(var i=0;i<cxCounts.Length;i++)cxCounts[i]=Helpers.Deserialize<CrealityCXDLPv1File.PreLayer>(cxStream).Unknown;
        CxPage();var cxLayers=new List<object>();
        for(var i=0;i<cxCounts.Length;i++) {
            var count=Helpers.Deserialize<CrealityCXDLPv1File.PreLayer>(cxStream).Unknown;
            var lines=Helpers.Deserialize<CrealityCXDLPv1File.PreLayer>(cxStream).Unknown;
            if(count!=cxCounts[i] || lines*6UL>ulong.Max(0,(ulong)(cxStream.Length-cxStream.Position)))
                throw new InvalidDataException("CXDLPv1 layer table");
            var decoded=new byte[cxHeader.ResolutionX*cxHeader.ResolutionY];uint nonzero=0;
            for(var j=0;j<lines;j++) {
                var line=Helpers.Deserialize<CrealityCXDLPv1File.LayerLine>(cxStream);
                if(line.StartX>=cxHeader.ResolutionX || line.EndY>=cxHeader.ResolutionY || line.StartY>line.EndY || line.Gray==0)
                    throw new InvalidDataException("CXDLPv1 run coordinates");
                for(var y=(uint)line.StartY;y<=line.EndY;y++) {
                    var index=y*cxHeader.ResolutionX+line.StartX;
                    if(decoded[index]!=0)throw new InvalidDataException("CXDLPv1 overlapping runs");
                    decoded[index]=line.Gray;nonzero++;
                }
            }
            if(nonzero!=count)throw new InvalidDataException("CXDLPv1 nonzero count");CxPage();
            cxLayers.Add(new {nonzero,lines,sha256=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(decoded))});
        }
        var cxFooter=Helpers.Deserialize<CrealityCXDLPv1File.Footer>(cxStream);cxFooter.Validate();
        if(cxStream.Position!=cxStream.Length)throw new InvalidDataException("CXDLPv1 trailing bytes");
        reports.Add(new {filename,status="parsed",scope="Original UVtools Header/SlicerInfo/LayerLine/Footer and independent inclusive-run decode; no native image decode or firmware validation",
            cxHeader.ResolutionX,cxHeader.ResolutionY,cxInfo.DisplayWidth,cxInfo.DisplayHeight,cxInfo.LayerHeight,
            cxInfo.ExposureTime,cxInfo.BottomExposureTime,cxInfo.WaitTimeBeforeCure,cxInfo.BottomLayersCount,
            cxInfo.LiftHeight,cxInfo.BottomLiftHeight,cxInfo.LiftSpeed,cxInfo.BottomLiftSpeed,cxInfo.RetractSpeed,cxInfo.LightPWM,cxInfo.BottomLightPWM,layers=cxLayers});
        continue;
    }
    if(filename.EndsWith(".cxdlpv4",StringComparison.OrdinalIgnoreCase)) {
        try {
        using var c4Stream=File.OpenRead(filename);using var c4Reader=new BinaryReader(c4Stream);
        var c4Header=Helpers.Deserialize<CrealityCXDLPv4File.Header>(c4Stream);c4Header.Validate();
        var c4File=new CrealityCXDLPv4File();
        typeof(CrealityCXDLPv4File).GetProperty("HeaderSettings")!.SetValue(c4File,c4Header);
        var c4Checksum=(uint)typeof(CrealityCXDLPv4File).GetMethod("CalculateCheckSum",BindingFlags.NonPublic|BindingFlags.Instance)!.Invoke(c4File,new object[]{c4Stream,false,-4})!;
        var c4StoredChecksum=System.Buffers.Binary.BinaryPrimitives.ReadUInt32BigEndian(c4Reader.ReadBytes(4));
        if(c4Checksum!=c4StoredChecksum)throw new InvalidDataException("Original UVtools CXDLPv4 checksum mismatch");
        var c4Previews=new List<object>();
        foreach(var address in new[]{c4Header.PreviewSmallOffsetAddress,c4Header.PreviewLargeOffsetAddress}) {
            c4Stream.Position=address;var c4Preview=Helpers.Deserialize<CrealityCXDLPv4File.Preview>(c4Stream);
            if(c4Preview.ImageOffset<address+32 || (ulong)c4Preview.ImageOffset+c4Preview.ImageLength>(ulong)c4Stream.Length-4)throw new InvalidDataException("CXDLPv4 preview addresses");
            c4Stream.Position=c4Preview.ImageOffset;var c4Bytes=c4Reader.ReadBytes(checked((int)c4Preview.ImageLength));
            uint c4Pixels=0;
            for(int j=0;j<c4Bytes.Length;) {
                if(j+2>c4Bytes.Length)throw new InvalidDataException("CXDLPv4 truncated preview pixel");
                var dot=System.Buffers.Binary.BinaryPrimitives.ReadUInt16LittleEndian(c4Bytes.AsSpan(j));j+=2;uint repeat=1;
                if((dot&0x20)!=0){if(j+2>c4Bytes.Length)throw new InvalidDataException("CXDLPv4 truncated preview run");repeat+=(uint)(c4Bytes[j]|((c4Bytes[j+1]&15)<<8));j+=2;}
                c4Pixels+=repeat;if(c4Pixels>c4Preview.ResolutionX*c4Preview.ResolutionY)throw new InvalidDataException("CXDLPv4 preview overflow");
            }
            if(c4Pixels!=c4Preview.ResolutionX*c4Preview.ResolutionY)throw new InvalidDataException("CXDLPv4 preview coverage");
            c4Previews.Add(new {c4Preview.ResolutionX,c4Preview.ResolutionY,c4Preview.ImageOffset,c4Preview.ImageLength,c4Preview.Unknown1,c4Preview.Unknown2,c4Preview.Unknown3,c4Preview.Unknown4,decodedPixels=c4Pixels});
        }
        c4Stream.Position=c4Header.PrintParametersOffsetAddress;
        var c4Params=Helpers.Deserialize<CrealityCXDLPv4File.PrintParameters>(c4Stream);
        typeof(CrealityCXDLPv4File).GetProperty("PrintParametersSettings")!.SetValue(c4File,c4Params);
        c4Stream.Position=c4Header.SlicerAddress;var c4Info=Helpers.Deserialize<CrealityCXDLPv4File.SlicerInfo>(c4Stream);
        typeof(CrealityCXDLPv4File).GetProperty("SlicerInfoSettings")!.SetValue(c4File,c4Info);
        var c4Layers=new List<object>();
        for(uint j=0;j<c4Header.LayerCount;j++) {
            c4Stream.Position=c4Header.LayersDefinitionOffsetAddress+40*j;
            var c4Layer=Helpers.Deserialize<CrealityCXDLPv4File.LayerDef>(c4Stream);
            if(c4Layer.DataSize<44 || (ulong)c4Layer.DataAddress+c4Layer.DataSize>(ulong)c4Stream.Length-4)throw new InvalidDataException("Unsupported encrypted or invalid CXDLPv4 layer");
            c4Stream.Position=c4Layer.DataAddress;var c4Extended=Helpers.Deserialize<CrealityCXDLPv4File.LayerDefEx>(c4Stream);
            var c4Data=c4Reader.ReadBytes(checked((int)c4Layer.DataSize-44));
            if(c4Header.EncryptionKey!=0)CrealityCXDLPv4File.LayerRleCryptBuffer(c4Header.EncryptionKey,j,c4Data);
            var c4Decoded=new byte[c4Header.ResolutionX*c4Header.ResolutionY];uint c4Position=0;
            for(int k=0;k<c4Data.Length;) {
                var code=c4Data[k++];uint run=1;
                if((code&128)!=0) {
                    if(k>=c4Data.Length)throw new InvalidDataException("CXDLPv4 missing run");
                    var first=c4Data[k++];int continuation;
                    if(first<128){run=first;continuation=0;}
                    else if(first<192){run=(uint)(first&63);continuation=1;}
                    else if(first<224){run=(uint)(first&31);continuation=2;}
                    else if(first<240){run=(uint)(first&15);continuation=3;}
                    else throw new InvalidDataException("CXDLPv4 invalid run prefix");
                    while(continuation-->0){if(k>=c4Data.Length)throw new InvalidDataException("CXDLPv4 short run");run=(run<<8)|c4Data[k++];}
                }
                code&=127;var gray=code==0?(byte)0:(byte)((code<<1)|1);
                if(run==0 || run>c4Decoded.Length-c4Position)throw new InvalidDataException("CXDLPv4 run overflow");
                Array.Fill(c4Decoded,gray,(int)c4Position,(int)run);c4Position+=run;
            }
            if(c4Position!=c4Decoded.Length)throw new InvalidDataException("CXDLPv4 mask coverage");
            c4Layers.Add(new {c4Layer.PositionZ,c4Layer.ExposureTime,c4Layer.LightOffSeconds,c4Layer.DataType,c4Layer.LargestArea,
                c4Extended.LiftHeight,c4Extended.LiftSpeed,c4Extended.LiftHeight2,c4Extended.LiftSpeed2,c4Extended.RetractSpeed,c4Extended.RetractHeight2,c4Extended.RetractSpeed2,
                c4Extended.RestTimeBeforeLift,c4Extended.RestTimeAfterLift,c4Extended.RestTimeAfterRetract,c4Extended.LightPWM,sha256=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(c4Decoded))});
        }
        reports.Add(new {filename,status="parsed",scope="Original UVtools CXDLPv4 Header/Preview/PrintParameters/SlicerInfo/LayerDef/LayerDefEx and original checksum and LayerRleCryptBuffer; independent RGB15 and grayscale RLE decode; no native image decode or firmware validation",
            c4Header.Version,c4Header.ResolutionX,c4Header.ResolutionY,c4Header.BedSizeX,c4Header.BedSizeY,c4Header.BedSizeZ,c4Header.PrintHeight,c4Header.LayerHeight,
            c4Params.VolumeMl,c4Params.WeightG,c4Params.CostDollars,c4File.ExposureTime,c4File.BottomExposureTime,previews=c4Previews,layers=c4Layers});
        } catch(Exception error){reports.Add(new {filename,status="rejected",error=error.Message});}
        continue;
    }
    if(filename.EndsWith(".cxdlp",StringComparison.OrdinalIgnoreCase)) {
        try {
        using var c3Stream=File.OpenRead(filename);
        var c3Header=Helpers.Deserialize<CrealityCXDLPFile.Header>(c3Stream);c3Header.Validate();
        if((c3Header.Version!=2 && c3Header.Version!=3) || c3Header.ResolutionX==0 || c3Header.ResolutionY==0)throw new InvalidDataException("CXDLP v3 header");
        var c3File=new CrealityCXDLPFile();
        typeof(CrealityCXDLPFile).GetProperty("HeaderSettings")!.SetValue(c3File,c3Header);
        var c3Position=c3Stream.Position;
        var c3Checksum=(uint)typeof(CrealityCXDLPFile).GetMethod("CalculateCheckSum",BindingFlags.NonPublic|BindingFlags.Instance)!.Invoke(c3File,new object[]{c3Stream,false,-4})!;
        uint C3Unsigned(){uint value=0;for(int j=0;j<4;j++) {int b=c3Stream.ReadByte();if(b<0)throw new EndOfStreamException();value=(value<<8)|(uint)b;}return value;}
        if(C3Unsigned()!=c3Checksum)throw new InvalidDataException("Original UVtools CXDLP checksum mismatch");
        c3Stream.Position=c3Position;
        void C3Page(){var page=Helpers.Deserialize<CrealityCXDLPFile.PageBreak>(c3Stream);if(page.Line!=13 || page.Break!=10)throw new InvalidDataException("CXDLP page break");}
        foreach(var size in new[]{116,290,290}){c3Stream.Seek(size*size*2,SeekOrigin.Current);C3Page();}
        var c3Info=Helpers.Deserialize<CrealityCXDLPFile.SlicerInfo>(c3Stream);
        typeof(CrealityCXDLPFile).GetProperty("SlicerInfoSettings")!.SetValue(c3File,c3Info);
        var c3Areas=new uint[c3Header.LayerCount];for(int j=0;j<c3Areas.Length;j++)c3Areas[j]=Helpers.Deserialize<CrealityCXDLPFile.PreLayer>(c3Stream).LayerArea;
        C3Page();if(c3Header.Version>=3) {var c3Informative=Helpers.Deserialize<CrealityCXDLPFile.SlicerInfoV3>(c3Stream);
        if(c3Informative.PageBreak.Line!=13 || c3Informative.PageBreak.Break!=10)throw new InvalidDataException("CXDLP v3 informative header");}
        var c3Layers=new List<object>();
        for(int j=0;j<c3Areas.Length;j++) {
            var c3Layer=Helpers.Deserialize<CrealityCXDLPFile.LayerDef>(c3Stream);
            if(c3Layer.LayerArea!=c3Areas[j] || c3Layer.PageBreak.Line!=13 || c3Layer.PageBreak.Break!=10)throw new InvalidDataException("CXDLP layer table");
            var c3Decoded=new byte[c3Header.ResolutionX*c3Header.ResolutionY];uint c3Nonzero=0;
            foreach(var line in c3Layer.Lines) {
                if(line.StartX>=c3Header.ResolutionX || line.EndY>=c3Header.ResolutionY || line.StartY>line.EndY || line.Gray==0)throw new InvalidDataException("CXDLP coordinates");
                for(uint y=line.StartY;y<=line.EndY;y++) {
                    var index=y*c3Header.ResolutionX+line.StartX;
                    if(c3Decoded[index]!=0)throw new InvalidDataException("CXDLP overlapping runs");
                    c3Decoded[index]=line.Gray;c3Nonzero++;
                }
            }
            c3Layers.Add(new {c3Layer.LayerArea,c3Layer.LineCount,nonzero=c3Nonzero,sha256=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(c3Decoded))});
        }
        var c3Footer=Helpers.Deserialize<CrealityCXDLPFile.Footer>(c3Stream);c3Footer.Validate();
        if(c3Stream.Position+4!=c3Stream.Length || C3Unsigned()!=c3Checksum)throw new InvalidDataException("CXDLP trailing checksum");
        reports.Add(new {filename,status="parsed",scope="Original UVtools Header/SlicerInfo/SlicerInfoV3/LayerDef/Footer and original checksum calculation; independent inclusive-run decode; no native image decode or firmware validation",
            c3Header.Version,MachineName=c3Header.PrinterModel.ToString(),c3Header.ResolutionX,c3Header.ResolutionY,
            c3File.DisplayWidth,c3File.DisplayHeight,c3File.LayerHeight,c3File.ExposureTime,c3File.BottomExposureTime,c3File.BottomLayerCount,
            c3File.WaitTimeBeforeCure,c3File.LiftHeight,c3File.BottomLiftHeight,c3File.LiftSpeed,c3File.BottomLiftSpeed,c3File.RetractSpeed,
            c3File.LightPWM,c3File.BottomLightPWM,checksum=c3Checksum,layers=c3Layers});
        } catch(Exception error) {reports.Add(new {filename,status="rejected",error=error.Message});}
        continue;
    }
    using var zip = ZipFile.OpenRead(filename);
    if (zip.GetEntry("run.gcode") is { } chituboxEntry) {
        var chitubox = new ChituboxZipFile();
        using var chituboxReader = new StreamReader(chituboxEntry.Open());
        var chituboxCommands = chituboxReader.ReadToEnd();
        foreach (var line in chituboxCommands.Split('\n')) {
            if (!line.StartsWith(';')) continue;
            var pair = line.Trim().TrimStart(';').Split(':',2);
            if (pair.Length != 2) continue;
            foreach (var property in chitubox.HeaderSettings.GetType().GetProperties()) {
                if (property.GetCustomAttribute<System.ComponentModel.DisplayNameAttribute>()?.DisplayName != pair[0].Trim()) continue;
                property.SetValueFromString(chitubox.HeaderSettings,pair[1].Trim());
            }
        }
        var chituboxCount = chitubox.HeaderSettings.LayerCount;
        if (chituboxCount == 0 || chituboxCount > 100000) throw new InvalidDataException("Chitubox ZIP count");
        // Init/parser recalculate material using our synthetic nonzero counts.
        // Capture file metadata before those managed-only placeholders exist.
        var chituboxMaterialHeader = new {chitubox.HeaderSettings.VolumeMl,chitubox.HeaderSettings.WeightG,chitubox.HeaderSettings.Price};
        chitubox.Init(chituboxCount,true);
        foreach (var layer in chitubox.Layers)
            typeof(UVtools.Core.Layers.Layer).GetProperty("NonZeroPixelCount")!.SetValue(layer,(uint)2);
        chitubox.GCode!.ParseLayersFromGCode(chitubox,chituboxCommands,false);
        var chituboxLayers=chitubox.Layers.Select(layer=>new {
            layer.PositionZ,layer.ExposureTime,layer.WaitTimeBeforeCure,layer.WaitTimeAfterCure,
            layer.LiftHeight,layer.LiftHeight2,layer.LiftSpeed,layer.LiftSpeed2,
            layer.WaitTimeAfterLift,layer.RetractSpeed,layer.RetractHeight2,layer.RetractSpeed2,layer.LightPWM
        }).ToArray();
        float chituboxPreviousZ=0;
        for (var i=0;i<chituboxLayers.Length;i++) {
            if(chituboxLayers[i].PositionZ<=chituboxPreviousZ || zip.GetEntry($"{i+1}.png") is null)
                throw new InvalidDataException("Chitubox ZIP layer Z or PNG reference");
            chituboxPreviousZ=chituboxLayers[i].PositionZ;
        }
        reports.Add(new {filename,status="parsed",scope="Original UVtools Chitubox Header/getters and exposure/motion parser; no native image decode or firmware validation",
            chitubox.ResolutionX,chitubox.ResolutionY,chitubox.DisplayWidth,chitubox.DisplayHeight,chitubox.MachineZ,
            headerVolumeMl=chituboxMaterialHeader.VolumeMl,headerWeightG=chituboxMaterialHeader.WeightG,headerPrice=chituboxMaterialHeader.Price,layers=chituboxLayers});
        continue;
    }
    if (filename.EndsWith(".sl1", StringComparison.OrdinalIgnoreCase)) {
        var sl1 = new SL1File();
        foreach (var name in new[] {"config.ini", "prusaslicer.ini"}) {
            using var reader = new StreamReader((zip.GetEntry(name) ?? throw new InvalidDataException(name)).Open());
            while (reader.ReadLine() is { } line) {
                var parts = line.Split('=', 2); if (parts.Length != 2) continue;
                var field = SL1File.IniKeyToMemberName(parts[0].Trim());
                foreach (var config in sl1.Configs) {
                    var property = config.GetType().GetProperty(field);
                    if (property is { CanWrite: true }) property.SetValueFromString(config, parts[1].Trim());
                }
            }
        }
        var parameters = new Dictionary<string, float>();
        foreach (var bottom in new[] {"", "Bottom"})
        foreach (var name in new[] {"WaitTimeBeforeCure", "WaitTimeAfterCure", "LiftHeight", "LiftSpeed", "LiftHeight2", "LiftSpeed2",
            "WaitTimeAfterLift", "RetractSpeed", "RetractHeight2", "RetractSpeed2"})
            parameters[bottom+name] = sl1.LookupCustomValue(bottom+name, -1f);
        var pwm = sl1.LookupCustomValue<byte>("LightPWM", 0);
        var bottomPwm = sl1.LookupCustomValue<byte>("BottomLightPWM", 0);
        var transitions = sl1.LookupCustomValue<ushort>("TransitionLayerCount", 0);
        if (parameters.Values.Any(value => value < 0)) throw new InvalidDataException("Missing SL1 LCD note keyword");
        reports.Add(new { filename, status = "parsed", scope = "UVtools managed SL1 INI fields and LookupCustomValue only; no native image decode",
            sl1.ResolutionX, sl1.ResolutionY, sl1.DisplayWidth, sl1.DisplayHeight, sl1.BottomLayerCount,
            sl1.LayerHeight, sl1.MaterialSettings.InitialLayerHeight, sl1.ExposureTime, sl1.BottomExposureTime,
            transitions, pwm, bottomPwm, parameters });
        continue;
    }
    if (filename.EndsWith(".uvj", StringComparison.OrdinalIgnoreCase)) {
        using var stream = (zip.GetEntry("config.json") ?? throw new InvalidDataException("UVJ config.json")).Open();
        var uvjSettings = JsonSerializer.Deserialize<UVJFile.Settings>(stream) ?? throw new InvalidDataException("UVJ schema");
        var uvj = new UVJFile { JsonSettings = uvjSettings };
        if (uvjSettings.Layers?.Count != uvjSettings.Properties.Size.Layers || uvjSettings.Layers.Count == 0)
            throw new InvalidDataException("UVJ layer count");
        uvj.Init((uint)uvjSettings.Layers.Count, true);
        float previousZ = 0;
        for (var i = 0; i < uvjSettings.Layers.Count; i++) {
            uvjSettings.Layers[i].CopyTo(uvj[i]);
            if (uvj[i].PositionZ <= previousZ || uvj[i].ExposureTime <= 0) throw new InvalidDataException("UVJ Z/exposure");
            if (zip.GetEntry($"slice/{i:D8}.png") is null) throw new InvalidDataException("UVJ missing mask");
            previousZ = uvj[i].PositionZ;
        }
        reports.Add(new { filename, status = "parsed", scope = "UVtools managed UVJ schema and LayerDef.CopyTo; no native image decode",
            uvj.ResolutionX, uvj.ResolutionY, uvj.DisplayWidth, uvj.DisplayHeight, uvj.BottomLayerCount,
            layers = uvj.Layers.Select(layer => new { layer.PositionZ, layer.ExposureTime, layer.WaitTimeBeforeCure, layer.WaitTimeAfterCure,
                layer.LiftHeight, layer.LiftHeight2, layer.LiftSpeed, layer.LiftSpeed2, layer.WaitTimeAfterLift,
                layer.RetractSpeed, layer.RetractHeight2, layer.RetractSpeed2, layer.LightPWM }) });
        continue;
    }
    if (filename.EndsWith(".cws", StringComparison.OrdinalIgnoreCase)) {
        T Xml<T>(string name) {
            using var stream = (zip.GetEntry(name) ?? throw new InvalidDataException(name)).Open();
            return (T)(new XmlSerializer(typeof(T)).Deserialize(stream) ?? throw new InvalidDataException(name));
        }
        var cws = new CWSFile();
        var xmlCws = zip.GetEntry("manifest.xml") is not null;
        string[] slices;
        string commandName;
        if (xmlCws) {
            var manifest = Xml<CWSManifest>("manifest.xml");
            var profile = Xml<CWSSliceBuildConfig>(manifest.SliceProfile.Name!);
            cws.SliceBuildConfig = profile;
            cws.ResolutionX = profile.XResolution; cws.ResolutionY = profile.YResolution;
            slices = manifest.Slices!.Select(slice => slice.Name!).ToArray();
            commandName = manifest.GCode.Name!;
        } else {
            using var settingsReader = new StreamReader((zip.GetEntry("slice.conf") ?? throw new InvalidDataException("slice.conf")).Open());
            foreach (var line in settingsReader.ReadToEnd().Split('\n')) {
                var parts = line.Split('=');
                if (parts.Length != 2) continue;
                foreach (var property in cws.SliceSettings.GetType().GetProperties()) {
                    var attribute = property.GetCustomAttribute<System.ComponentModel.DisplayNameAttribute>();
                    if (attribute?.DisplayName != parts[0].Trim()) continue;
                    property.SetValue(cws.SliceSettings, Convert.ChangeType(parts[1].Trim(), property.PropertyType, CultureInfo.InvariantCulture));
                }
            }
            slices = zip.Entries.Where(entry => System.Text.RegularExpressions.Regex.IsMatch(entry.Name, @"\d+\.png$"))
                .OrderBy(entry => int.Parse(System.Text.RegularExpressions.Regex.Match(entry.Name, @"(\d+)\.png$").Groups[1].Value))
                .Select(entry => entry.FullName).ToArray();
            if (slices.Length != cws.SliceSettings.LayersNum) throw new InvalidDataException("NovaMaker count mismatch");
            commandName = zip.Entries.Single(entry => entry.Name.EndsWith(".gcode")).FullName;
        }
        using var reader = new StreamReader(zip.GetEntry(commandName)!.Open());
        var commands = reader.ReadToEnd();
        // These are the same display-name fields consumed by CWSFile.Decode.
        foreach (var line in commands.Split('\n')) {
            var parts = line.Trim().Trim(';', '(', ')').Split('=');
            if (parts.Length != 2) continue;
            foreach (var property in cws.OutputSettings.GetType().GetProperties()) {
                var attribute = property.GetCustomAttribute<System.ComponentModel.DisplayNameAttribute>();
                if (attribute?.DisplayName != parts[0].Trim()) continue;
                property.SetValue(cws.OutputSettings, Convert.ChangeType(parts[1].Trim(), property.PropertyType, CultureInfo.InvariantCulture));
            }
        }
        cws.Init((uint)slices.Length, true);
        // We check commands here; PNG masks are independently decoded in C++.
        // Mark synthetic layers nonempty to avoid blank-layer motion heuristics.
        foreach (var layer in cws.Layers)
            typeof(UVtools.Core.Layers.Layer).GetProperty("NonZeroPixelCount")!.SetValue(layer, (uint)2);
        cws.GCode!.ParseLayersFromGCode(cws, commands, false);
        if (cws.DisplayWidth <= 0 || cws.DisplayHeight <= 0 || cws.LayerCount == 0)
            throw new InvalidDataException("CWS missing display or layers");
        var parsedLayers = cws.Layers.Select(layer => new {
            layer.PositionZ, layer.ExposureTime, layer.WaitTimeBeforeCure, layer.WaitTimeAfterCure,
            layer.LiftHeight, layer.LiftHeight2, layer.LiftSpeed, layer.LiftSpeed2,
            layer.WaitTimeAfterLift, layer.RetractSpeed, layer.RetractHeight2, layer.RetractSpeed2, layer.LightPWM
        }).ToArray();
        float previousZ = 0;
        for (var i = 0; i < parsedLayers.Length; i++) {
            if (parsedLayers[i].PositionZ <= previousZ || parsedLayers[i].ExposureTime <= 0)
                throw new InvalidDataException("CWS invalid decoded layer Z/exposure");
            if (zip.GetEntry(slices[i]) is null) throw new InvalidDataException("CWS missing PNG");
            previousZ = parsedLayers[i].PositionZ;
        }
        reports.Add(new { filename, status = "parsed", scope = "UVtools managed CWS schema/settings and exposure/motion command parser; no native image decode",
            cws.ResolutionX, cws.ResolutionY, cws.DisplayWidth, cws.DisplayHeight, cws.MachineZ,
            sliceSettings = xmlCws ? null : cws.SliceSettings, layers = parsedLayers });
        continue;
    }
    if (zip.GetEntry("manifest.uvtools") is { } genericEntry) {
        using var genericStream = genericEntry.Open();
        var genericManifest = (GenericZipManifest)new XmlSerializer(typeof(GenericZipManifest)).Deserialize(genericStream)!;
        var generic = new GenericZIPFile { ManifestFile = genericManifest };
        if (generic.ResolutionX == 0 || generic.ResolutionY == 0 || generic.DisplayWidth <= 0 || generic.DisplayHeight <= 0 || generic.LayerHeight <= 0)
            throw new InvalidDataException("Generic ZIP geometry");
        var genericLayers = zip.Entries.Where(entry => entry.FullName.EndsWith(".png", StringComparison.OrdinalIgnoreCase) &&
            uint.TryParse(Path.GetFileNameWithoutExtension(entry.Name), out _)).OrderBy(entry => uint.Parse(Path.GetFileNameWithoutExtension(entry.Name))).ToArray();
        if (genericLayers.Length == 0) throw new InvalidDataException("Generic ZIP masks");
        for (var index = 0; index < genericLayers.Length; ++index)
            if (uint.Parse(Path.GetFileNameWithoutExtension(genericLayers[index].Name)) != index+1) throw new InvalidDataException("Generic ZIP indices");
        reports.Add(new { filename, status="parsed", scope="Original UVtools GenericZipManifest and property getters; no native image decoding or firmware validation",
            generic.ResolutionX, generic.ResolutionY, generic.DisplayWidth, generic.DisplayHeight, generic.MachineZ, generic.LayerHeight, layers=genericLayers.Length });
        continue;
    }
    if (zip.GetEntry("sla_job.xml") is { } vdaEntry) {
        using var vdaStream = vdaEntry.Open();
        var vdaManifest = (VDARoot)new XmlSerializer(typeof(VDARoot)).Deserialize(vdaStream)!;
        var vda = new VDAFile { ManifestFile = vdaManifest };
        if (vda.ResolutionX == 0 || vda.ResolutionY == 0 || vda.DisplayWidth <= 0 || vda.DisplayHeight <= 0 || vda.LayerHeight <= 0 ||
            vdaManifest.Layers.Count != vdaManifest.Slices.LayerCount) throw new InvalidDataException("VDA geometry or count");
        for (var index = 0; index < vdaManifest.Layers.Count; ++index) {
            var vdaLayer = vdaManifest.Layers[index];
            if (vdaLayer.Index != index || vdaLayer.ZPosition <= 0 || zip.GetEntry(vdaLayer.Filename!) is null)
                throw new InvalidDataException("VDA layer table");
            if (Math.Abs(vdaLayer.ZPosition-(index+1)*vda.LayerHeight) > .0001) throw new InvalidDataException("VDA layer Z");
        }
        reports.Add(new { filename, status="parsed", scope="Original UVtools VDARoot and property getters; no native image decoding or firmware validation",
            vda.ResolutionX, vda.ResolutionY, vda.DisplayWidth, vda.DisplayHeight, vda.MachineZ, vda.LayerHeight, layers=vdaManifest.Layers.Count });
        continue;
    }
    T Read<T>(string entryName) {
        using var stream = (zip.GetEntry(entryName) ?? throw new InvalidDataException(entryName)).Open();
        return JsonSerializer.Deserialize<T>(stream) ?? throw new InvalidDataException(entryName);
    }
    var settings = Read<AnycubicZipFile.SettingsManifest>("anycubic_photon_resins.pwsp");
    var layers = Read<AnycubicZipFile.LayersControllerManifest>("layers_controller.conf");
    var info = Read<AnycubicZipFile.PrintInfoManifest>("print_info.json");
    var software = Read<AnycubicZipFile.SoftwareInfoManifest>("software_info.conf");
    using var sceneStream = (zip.GetEntry("scene.slice") ?? throw new InvalidDataException("scene.slice")).Open();
    var scene = Helpers.Deserialize<AnycubicZipFile.SceneManifest>(sceneStream);
    if (scene.Magic != AnycubicZipFile.SceneManifest.MAGIC || scene.Separator != "<---" || scene.EndMarker != "--->")
        throw new InvalidDataException("Scene markers");
    if (scene.LayerCount != layers.Count || scene.LayerDefCount != layers.Count || scene.LayersDef.Length != layers.Count)
        throw new InvalidDataException("Scene layer count");
    var file = new AnycubicZipFile { Settings = settings, PrintInfoSettings = info, SoftwareInfoSettings = software, SceneSettings = scene };
    double z = 0;
    for (var i = 0; i < layers.Count; i++) {
        var layer = layers.Layers[i];
        if (layer.LayerIndex != i || layer.LayerThickness <= 0 || Math.Abs(layer.LayerMinHeight-z) > .0001)
            throw new InvalidDataException("Layer index/thickness/minimum height");
        z += layer.LayerThickness;
        if (Math.Abs(scene.LayersDef[i].Height-z) > .0001) throw new InvalidDataException("Scene layer Z");
        if (zip.GetEntry($"layer_images/layer_{i}.pw0Img") is null) throw new InvalidDataException("Missing PW0 mask");
    }
    if (Math.Abs(scene.ZMax-z) > .0001) throw new InvalidDataException("Scene print height");
    reports.Add(new {
        filename, scope = "UVtools managed metadata and scene only; masks validated separately by C++",
        status = "parsed", file.MachineName, file.ResolutionX, file.ResolutionY,
        file.DisplayWidth, file.DisplayHeight, file.MachineZ, layers = layers.Count,
        file.BottomLayerCount, file.TransitionLayerCount, file.ExposureTime, file.BottomExposureTime,
        file.WaitTimeBeforeCure, file.LiftHeight, file.LiftHeight2, file.LiftSpeed, file.RetractSpeed,
        file.BottomLiftHeight, file.BottomLiftSpeed, info.MaterialMilliliters, info.PrintTime, printHeight = scene.ZMax
    });
}
Console.WriteLine(JsonSerializer.Serialize(reports, new JsonSerializerOptions { WriteIndented = true }));
