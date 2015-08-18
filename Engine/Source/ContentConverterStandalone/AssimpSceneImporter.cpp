#include "PrecompiledHeader.h"
#include "ContentConverter.h"
#include "ResourceCompiler/StaticMeshGenerator.h"
Log_SetChannel(OBJImporter);

#define CHECK_ARG(str) !Y_strcmp(argv[i], str)
#define CHECK_ARG_PARAM(str) !Y_strcmp(argv[i], str) && ((i + 1) < argc)

static void PrintAssimpSceneImporterSyntax()
{
    Log_InfoPrint("Assimp Scene Importer options:");
    Log_InfoPrint("  -h, -help: Displays this text.");
    Log_InfoPrint("  -i <filename>: Specify source file name.");
    Log_InfoPrint("  -meshdir <path>: Output mesh directory.");
    Log_InfoPrint("  -meshprefix <name>: Output mesh prefix.");
    Log_InfoPrint("  -genmap <path>: Absolute map output directory.");
    Log_InfoPrint("  -[no]materials: Enable/disable material importing");
    Log_InfoPrint("  -materialdir: Directory for writing materials to");
    Log_InfoPrint("  -materialprefix: Prefix for material names");
    Log_InfoPrint("  -defaultmaterialname: Default material name");
    Log_InfoPrint("  -rotate(X|Y|Z) <amount>: Rotate around the axis as a post-transform.");
    Log_InfoPrint("  -translate(X|Y|Z) <degrees>: Translate along the axis as a post-transform.");
    Log_InfoPrint("  -scale[X|Y|Z] <amount>: Uniform or nonuniform scale on axis as a post-transform.");
    Log_InfoPrint("  -flipwinding: Flip the triangle winding order");
    Log_InfoPrint("  -cs <(yup|zup)_(lh|rh)>: File uses specified coordinate system, convert if necessary.");
    Log_InfoPrint("  -center: Center mesh in the center.");
    Log_InfoPrint("  -centerbase: Center mesh in the bottom-center-middle.");
    Log_InfoPrint("  -collision: Defines collision shape type, defaults to none. (trianglemesh, convexhull)");
    Log_InfoPrint("");
}

static uint32 GetCollisionShapeTypeIndex(const char *name)
{
    if (Y_stricmp(name, "none") == 0)
        return StaticMeshGenerator::CollisionShapeType_None;
    else if (Y_stricmp(name, "box") == 0)
        return StaticMeshGenerator::CollisionShapeType_Box;
    else if (Y_stricmp(name, "sphere") == 0)
        return StaticMeshGenerator::CollisionShapeType_Sphere;
    else if (Y_stricmp(name, "trianglemesh") == 0)
        return StaticMeshGenerator::CollisionShapeType_TriangleMesh;
    else if (Y_stricmp(name, "convexhull") == 0)
        return StaticMeshGenerator::CollisionShapeType_ConvexHull;
    else
        return StaticMeshGenerator::CollisionShapeType_None;
}

bool ParseAssimpSceneImporterOption(AssimpSceneImporter::Options &Options, int &i, int argc, char *argv[])
{
    if (CHECK_ARG_PARAM("-i"))
        Options.SourcePath = argv[++i];
    else if (CHECK_ARG_PARAM("-meshdir"))
        Options.MeshDirectory = argv[++i];
    else if (CHECK_ARG_PARAM("-meshprefix"))
        Options.MeshPrefix = argv[++i];
    else if (CHECK_ARG_PARAM("-genmap"))
        Options.OutputMapName = argv[++i];
    else if (CHECK_ARG("-nomaterials"))
        Options.ImportMaterials = false;
    else if (CHECK_ARG("-materials"))
        Options.ImportMaterials = true;
    else if (CHECK_ARG_PARAM("-materialdir"))
        Options.MaterialDirectory = argv[++i];
    else if (CHECK_ARG_PARAM("-materialprefix"))
        Options.MaterialPrefix = argv[++i];
    else if (CHECK_ARG_PARAM("-defaultmaterialname"))
        Options.DefaultMaterialName = argv[++i];
    else if (CHECK_ARG_PARAM("-collision"))
        Options.BuildCollisionShapeType = GetCollisionShapeTypeIndex(argv[++i]);
    else if (CHECK_ARG_PARAM("-rotateX"))
        Options.TransformMatrix = float4x4::MakeRotationMatrixX(StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-rotateY"))
        Options.TransformMatrix = float4x4::MakeRotationMatrixY(StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-rotateZ"))
        Options.TransformMatrix = float4x4::MakeRotationMatrixZ(StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-translateX"))
        Options.TransformMatrix = float4x4::MakeTranslationMatrix(StringConverter::StringToFloat(argv[++i]), 0.0f, 0.0f) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-translateY"))
        Options.TransformMatrix = float4x4::MakeTranslationMatrix(0.0f, StringConverter::StringToFloat(argv[++i]), 0.0f) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-translateZ"))
        Options.TransformMatrix = float4x4::MakeTranslationMatrix(0.0f, 0.0f, StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-scaleX"))
        Options.TransformMatrix = float4x4::MakeScaleMatrix(StringConverter::StringToFloat(argv[++i]), 0.0f, 0.0f) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-scaleY"))
        Options.TransformMatrix = float4x4::MakeScaleMatrix(0.0f, StringConverter::StringToFloat(argv[++i]), 0.0f) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-scaleZ"))
        Options.TransformMatrix = float4x4::MakeScaleMatrix(0.0f, 0.0f, StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG_PARAM("-scale"))
        Options.TransformMatrix = float4x4::MakeScaleMatrix(StringConverter::StringToFloat(argv[++i])) * Options.TransformMatrix;
    else if (CHECK_ARG("-center"))
        Options.CenterMeshes = StaticMeshGenerator::CenterOrigin_Center;
    else if (CHECK_ARG("-centerbase"))
        Options.CenterMeshes = StaticMeshGenerator::CenterOrigin_CenterBottom;
    else if (CHECK_ARG_PARAM("-cs"))
    {
        ++i;
        if (!Y_stricmp(argv[i], "yup_lh"))
            Options.CoordinateSystem = COORDINATE_SYSTEM_Y_UP_LH;
        else if (!Y_stricmp(argv[i], "yup_rh"))
            Options.CoordinateSystem = COORDINATE_SYSTEM_Y_UP_RH;
        else if (!Y_stricmp(argv[i], "zup_lh"))
            Options.CoordinateSystem = COORDINATE_SYSTEM_Z_UP_LH;
        else if (!Y_stricmp(argv[i], "zup_rh"))
            Options.CoordinateSystem = COORDINATE_SYSTEM_Z_UP_RH;
        else
        {
            Log_ErrorPrintf("Invalid coordinate system specified.");
            return false;
        }
    }
    else if (CHECK_ARG("-h") || CHECK_ARG("-help"))
    {
        i = argc;
        PrintAssimpSceneImporterSyntax();
        return false;
    }
    else
    {
        Log_ErrorPrintf("Unknown option: %s", argv[i]);
        return false;
    }

    return true;
}

int RunAssimpSceneImporter(int argc, char *argv[])
{
    int i;

    if (argc == 0)
    {
        PrintAssimpSceneImporterSyntax();
        return 0;
    }

    AssimpSceneImporter::Options options;
    AssimpSceneImporter::SetDefaultOptions(&options);

    for (i = 0; i < argc; i++)
    {
        if (!ParseAssimpSceneImporterOption(options, i, argc, argv))
            return 1;
    }

    if (options.SourcePath.IsEmpty() || options.MeshDirectory.IsEmpty())
    {
        Log_ErrorPrintf("Missing input file name or output name.");
        return 1;
    }

    {
        ConsoleProgressCallbacks progressCallbacks;
        AssimpSceneImporter importer(&options, &progressCallbacks);

        if (!importer.Execute())
        {
            Log_ErrorPrintf("Import process failed.");
            return 2;
        }
    }
    
    Log_InfoPrintf("Import process successful.");
    return 0;
}

