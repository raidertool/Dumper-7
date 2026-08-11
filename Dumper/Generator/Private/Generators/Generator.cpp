
#include "Generators/Generator.h"
#include "Generators/CppGenerator.h"
#include "Generators/MappingGenerator.h"
#include "Generators/IDAMappingGenerator.h"
#include "Generators/DumpspaceGenerator.h"
#include "Generators/ReflectionIRGenerator.h"
#include "Managers/StructManager.h"
#include "Managers/EnumManager.h"
#include "Managers/MemberManager.h"
#include "Managers/PackageManager.h"
#include "ReflectionFilter.h"

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

	DumperSafety::SetStage("initialize-engine-object-array");
	ObjectArray::Init();

	DumperSafety::SetStage("initialize-engine-name");
	CALL_PLATFORM_SPECIFIC_FUNCTION(FName::Init);

	DumperSafety::SetStage("initialize-engine-offsets");
	Off::Init();
	DumperSafety::SetStage("initialize-engine-property-sizes");
	PropertySizes::Init();

	DumperSafety::SetStage("initialize-engine-process-event");
	CALL_PLATFORM_SPECIFIC_FUNCTION(Off::InSDK::ProcessEvent::InitPE); // Must be at this position, relies on offsets initialized in Off::Init()

	DumperSafety::SetStage("initialize-engine-world");
	Off::InSDK::World::InitGWorld(); // Must be at this position, relies on offsets initialized in Off::Init()

	DumperSafety::SetStage("initialize-engine-text");
	Off::InSDK::Text::InitTextOffsets(); // Must be at this position, relies on offsets initialized in Off::InitPE()

	DumperSafety::SetStage("initialize-engine-settings");
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

	ReflectionIRGenerator::PredefinedMembers.clear();
	ReflectionIRGenerator::MainFolder.clear();
	ReflectionIRGenerator::Subfolder.clear();
	ReflectionIRGenerator::CapturedDocument.clear();
	ReflectionIRGenerator::CapturedFingerprint = 0;
	DSGen::reset();
}

Generator::SnapshotConsistency Generator::GenerateSnapshot(bool bGenerateCppSdk, bool bWriteObjectDumps)
{
	std::cerr << "Started Generation [Dumper-7]!\n";
	auto DumpStartTime = std::chrono::high_resolution_clock::now();

	DumperSafety::SetStage("snapshot-reset");
	ReflectionFilter::Refresh();
	ResetGenerationState(bWriteObjectDumps);
	DumperSafety::SetStage("snapshot-index-reflection");
	InitInternal();

	auto CaptureReflection = []() -> uint64
	{
		ReflectionIRGenerator::CaptureFailure Failure{};
		if (!ReflectionIRGenerator::TryCapture(&Failure))
		{
			const char* AccessName = Failure.AccessKind == 0
				? "read"
				: Failure.AccessKind == 1 ? "write" : "execute";
			std::ostringstream Message;
			Message << "access violation while capturing " << Failure.Context
				<< " at " << Failure.ExceptionAddress
				<< " (" << AccessName << " access at " << Failure.AccessAddress << ')';
			throw std::runtime_error(Message.str());
		}
		return ReflectionIRGenerator::GetCapturedFingerprint();
	};

	// Capture the deep reflection document once. It owns the exact C++ fragments and
	// semantic data used by the post-capture pipeline; do not traverse reflection a
	// second time after the generators have run.
	DumperSafety::SetStage("snapshot-capture-reflection-ir");
	const uint64 ReflectionSnapshot = CaptureReflection();
	try
	{
		DumperSafety::SetStage("snapshot-write-reflection-ir");
		Generate<ReflectionIRGenerator>();
	}
	catch (const std::exception& Error)
	{
		throw std::runtime_error(std::string("ReflectionIRGenerator failed: ") + Error.what());
	}

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
		std::cerr << "IDAMappingGenerator failed; continuing without IDMAP: "
			<< Error.what() << "\n";
		std::error_code RemoveError;
		fs::remove_all(IDAMappingGenerator::MainFolder, RemoveError);
	}
	try
	{
		DumperSafety::SetStage("snapshot-dumpspace");
		DumperSafety::ProtectedFailure Failure{};
		if (!DumperSafety::TryExecute([]
		{
			Generate<DumpspaceGenerator>();
		}, &Failure))
		{
			const char* AccessName = Failure.AccessKind == 0
				? "read"
				: Failure.AccessKind == 1 ? "write" : "execute";
			std::ostringstream Message;
			Message << "access violation at " << Failure.ExceptionAddress
				<< " (" << AccessName << " access at " << Failure.AccessAddress << ')';
			throw std::runtime_error(Message.str());
		}
	}
	catch (const std::exception& Error)
	{
		throw std::runtime_error(std::string("DumpspaceGenerator failed: ") + Error.what());
	}
	DumperSafety::SetStage("snapshot-reflection-identities");
	const nlohmann::json IdentityManifest{
		{"schema_version", 1},
		{"classes", ReflectionFilter::GetIncludedClassIdentities()},
	};
	std::ofstream IdentityStream(DumperFolder / "ReflectionIdentities.json", std::ios::binary);
	if (!IdentityStream || !(IdentityStream << IdentityManifest.dump(2) << '\n'))
		throw std::runtime_error("Could not write ReflectionIdentities.json");
	DumperSafety::SetStage("snapshot-complete");

	auto DumpFinishTime = std::chrono::high_resolution_clock::now();
	std::chrono::duration<double, std::milli> DumpTime = DumpFinishTime - DumpStartTime;
	std::cerr << "\n\nGenerating SDK took (" << DumpTime.count() << "ms)\n\n\n";

	if (bGenerateCppSdk && Settings::Debug::bExecuteSDKTestScript)
	{
		CppGenerator::ExecuteSDKCompilationTestScript();
	}

	return { ReflectionSnapshot, ReflectionSnapshot };
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
		if (!Obj.IsA(EClassCastFlags::Struct)
			|| ReflectionFilter::ShouldExcludeStruct(Obj.Cast<UEStruct>()))
			continue;

		UEStruct Struct = Obj.Cast<UEStruct>();

		std::vector<UEProperty> ChildProperties = Struct.GetProperties();
		if (ChildProperties.empty()) // Avoids allocating string for GetCppName() and prevents json from auto-creating empty objects for property-less structs
			continue;

		auto& StructMembers = MetadataJson[Struct.GetCppName()];
		for (UEProperty Prop : ChildProperties)
		{
			if (ReflectionFilter::ShouldExcludeProperty(Prop))
				continue;

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
