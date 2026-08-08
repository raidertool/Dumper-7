
#include "Generators/Generator.h"
#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"

#include "HashStringTable.h"
#include "Utils.h"

#include "Platform.h"
#include "Safety.h"
#include "Json/json.hpp"
#include "Dumpspace/DSGen.h"

#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>

inline void InitSettings()
{
	Settings::InitWeakObjectPtrSettings();
	Settings::InitLargeWorldCoordinateSettings();

	Settings::InitObjectPtrPropertySettings();
	Settings::InitArrayDimSizeSettings();
}


void Generator::InitEngineCore()
{
	/* manual override */
	//ObjectArray::Init(/*GObjects*/, /*Layout = Default*/); // FFixedUObjectArray (UEVersion < UE4.21)
	//ObjectArray::Init(/*GObjects*/, /*ChunkSize*/, /*Layout = Default*/); // FChunkedFixedUObjectArray (UEVersion >= UE4.21)

	//FName::Init(/*bForceGNames = false*/);
	//FName::Init(/*AppendString, FName::EOffsetOverrideType::AppendString*/);
	//FName::Init(/*ToString, FName::EOffsetOverrideType::ToString*/);
	//FName::Init(/*GNames, FName::EOffsetOverrideType::GNames, true/false*/);
 
	//Off::InSDK::ProcessEvent::InitPE(/*PEIndex*/);

	/* Back4Blood (requires manual GNames override) */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x8375); });

	/* Multiversus [Unsupported, weird GObjects-struct] */
	//InitObjectArrayDecryption([](void* ObjPtr) -> uint8* { return reinterpret_cast<uint8*>(uint64(ObjPtr) ^ 0x1B5DEAFD6B4068C); });

	ObjectArray::Init();

	CALL_PLATFORM_SPECIFIC_FUNCTION(FName::Init);

	Off::Init();
	PropertySizes::Init();

	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()

	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()

	InitSettings();
}

void Generator::InitInternal()
{
	// Initialize PackageManager with all packages, their names, structs, classes enums, functions and dependencies
	PackageManager::Init();

	// Initialize StructManager with all structs and their names
	StructManager::Init();
	
	// Initialize EnumManager with all enums and their names
	EnumManager::Init();
	
	// Initialized all Member-Name collisions
	MemberManager::Init();

	// Post-Initialize PackageManager after StructManager has been initialized. 'PostInit()' handles Cyclic-Dependencies detection
	PackageManager::PostInit();
}

void Generator::ResetGenerationState(bool bWriteObjectDumps)
{
	DumperFolder.clear();
	bDumpedGObjects = !bWriteObjectDumps;
	bDumepdEditorOnlyMetadata = !bWriteObjectDumps;

	PackageManager::Reset();
	StructManager::Reset();
	EnumManager::Reset();
	MemberManager::Reset();

	CppGenerator::PredefinedMembers.clear();
	CppGenerator::PredefinedStructs.clear();
	CppGenerator::MainFolder.clear();
	CppGenerator::Subfolder.clear();

	MappingGenerator::NameCounter = 0x0;
	MappingGenerator::PredefinedMembers.clear();
	MappingGenerator::MainFolder.clear();
	MappingGenerator::Subfolder.clear();

	IDAMappingGenerator::NameCounter = 0x0;
	IDAMappingGenerator::PredefinedMembers.clear();
	IDAMappingGenerator::MainFolder.clear();
	IDAMappingGenerator::Subfolder.clear();

	DumpspaceGenerator::PredefinedMembers.clear();
	DumpspaceGenerator::MainFolder.clear();
	DumpspaceGenerator::Subfolder.clear();
	DSGen::reset();
}

void Generator::GenerateSnapshot(bool bGenerateCppSdk, bool bWriteObjectDumps)
{
	std::cerr << "Started Generation [Dumper-7]!\n";
	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	DumperSafety::SetStage("snapshot-reset");
	ResetGenerationState(bWriteObjectDumps);
	DumperSafety::SetStage("snapshot-index-reflection");
	InitInternal();

	if (bGenerateCppSdk)
	{
		try
		{
			Generate<CppGenerator>();
		}
		catch (const std::exception& Error)
		{
			throw std::runtime_error(std::string("CppGenerator failed: ") + Error.what());
		}
	}
	try
	{
		DumperSafety::SetStage("snapshot-usmap");
		Generate<MappingGenerator>();
	}
	catch (const std::exception& Error)
	{
		throw std::runtime_error(std::string("MappingGenerator failed: ") + Error.what());
	}
	try
	{
		DumperSafety::SetStage("snapshot-idmap");
		Generate<IDAMappingGenerator>();
	}
	catch (const std::exception& Error)
	{
		throw std::runtime_error(std::string("IDAMappingGenerator failed: ") + Error.what());
	}
	try
	{
		DumperSafety::SetStage("snapshot-dumpspace");
		Generate<DumpspaceGenerator>();
	}
	catch (const std::exception& Error)
	{
		throw std::runtime_error(std::string("DumpspaceGenerator failed: ") + Error.what());
	}
	DumperSafety::SetStage("snapshot-complete");

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;
	std::cerr << "\n\nGenerating SDK took (" << DumpTime.count() << "ms)\n\n\n";

	if (bGenerateCppSdk && Settings::Debug::bExecuteSDKTestScript)
	{
		CppGenerator::ExecuteSDKCompilationTestScript();
	}
}

bool Generator::SetupDumperFolder()
{
	try
	{
		std::string FolderName = (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName);
		FileNameHelper::MakeValidFileName(FolderName);

		DumperFolder = fs::path(Settings::Generator::SDKGenerationPath) / FolderName;
		if (fs::exists(DumperFolder))
		{
			fs::path OldFolder = DumperFolder;

			if (Settings::Generator::bCreateUniqueBackups)
			{
				std::time_t Now = std::time(nullptr);
				OldFolder += ("_" + std::to_string(Now));
			}
			else
			{
				OldFolder += "_OLD";
			}

			std::cerr << "Folder already exists. Backing up to: " << OldFolder.generic_string() << "\n";

			fs::remove_all(OldFolder);
			fs::rename(DumperFolder, OldFolder);
		}

		fs::create_directories(DumperFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder)
{
	fs::path Dummy;
	std::string EmptyName = "";
	return SetupFolders(FolderName, OutFolder, EmptyName, Dummy);
}

bool Generator::SetupFolders(std::string& FolderName, fs::path& OutFolder, std::string& SubfolderName, fs::path& OutSubFolder)
{
	FileNameHelper::MakeValidFileName(FolderName);
	FileNameHelper::MakeValidFileName(SubfolderName);

	try
	{
		OutFolder = DumperFolder / FolderName;
		OutSubFolder = OutFolder / SubfolderName;
				
		if (fs::exists(OutFolder))
		{
			fs::path Old = OutFolder.generic_string() + "_OLD";

			fs::remove_all(Old);

			fs::rename(OutFolder, Old);
		}

		fs::create_directories(OutFolder);

		if (!SubfolderName.empty())
			fs::create_directories(OutSubFolder);
	}
	catch (const std::filesystem::filesystem_error& fe)
	{
		std::cerr << "Could not create required folders! Info: \n";
		std::cerr << fe.what() << std::endl;
		return false;
	}

	return true;
}


void DumpEditorOnlyMetadata(const fs::path& DumperFolder)
{
	if (Off::FField::EditorOnlyMetadata == -1)
		return;

	nlohmann::json MetadataJson;
	MetadataJson["GameName"] = Settings::Generator::GameName;
	MetadataJson["GameVersion"] = Settings::Generator::GameVersion;

	for (UEObject Obj : ObjectArray())
	{
		if (!Obj.IsA(EClassCastFlags::Struct))
			continue;

		UEStruct Struct = Obj.Cast<UEStruct>();

		std::vector<UEProperty> ChildProperties = Struct.GetProperties();
		if (ChildProperties.empty()) // Avoids allocating string for GetCppName() and prevents json from auto-creating empty objects for property-less structs
			continue;

		auto& StructMembers = MetadataJson[Struct.GetCppName()];
		for (UEProperty Prop : ChildProperties)
		{
			auto& Entries = StructMembers[Prop.GetValidName()];

			for (const auto& [Key, Value] : Prop.Cast<UEFField>().GetMetaData())
			{
				if (Key.empty() && Value.empty())
					continue;

				Entries[Key] = Value;
			}
		}
	}

	std::ofstream MetadataFile(DumperFolder / "Metadata.json");
	MetadataFile << MetadataJson.dump(4);
}
