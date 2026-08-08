#include <Windows.h>
#include <fstream>
#include <iostream>

#include "ContinuousController.h"
#include "Generators/Generator.h"
#include "Safety.h"

namespace
{
	void InitializeGameIdentity()
	{
		if (!Settings::Generator::GameName.empty() || !Settings::Generator::GameVersion.empty())
			return;

		FString Name;
		FString Version;
		UEClass Kismet = ObjectArray::FindClassFast("KismetSystemLibrary");
		UEFunction GetGameName = Kismet.GetFunction("KismetSystemLibrary", "GetGameName");
		UEFunction GetEngineVersion = Kismet.GetFunction("KismetSystemLibrary", "GetEngineVersion");

		Kismet.ProcessEvent(GetGameName, &Name);
		Kismet.ProcessEvent(GetEngineVersion, &Version);

		Settings::Generator::GameName = Name.ToString();
		Settings::Generator::GameVersion = Version.ToString();
	}
}

enum class EFortToastType : uint8
{
    Default                                  = 0,
    Subdued                                  = 1,
    Impactful                                = 2,
    Lock                                     = 3,
    EFortToastType_MAX                       = 4,
};

DWORD MainThreadImpl(HMODULE Module)
{
	FILE* ConsoleInput = nullptr;
	FILE* ConsoleError = nullptr;
	DumperSafety::SetStage("load-config");
	Settings::Config::Load();
	DumperSafety::SetLogDirectory(Settings::Generator::SDKGenerationPath);
	DumperSafety::SetStage("config-loaded");
	const bool HasConsole = !Settings::Config::bContinuous;
	if (HasConsole)
	{
		AllocConsole();
		freopen_s(&ConsoleInput, "CONIN$", "r", stdin);
		freopen_s(&ConsoleError, "CONOUT$", "w", stderr);
		std::cerr.clear();
		std::cerr << std::boolalpha << std::hex;
		std::cerr << "Initializing [Dumper-7]\n";
	}

	DumperSafety::SetStage("startup-delay");
	Settings::Config::DelayDumperStart();
	DumperSafety::SetStage("startup-delay-complete");

	DumperSafety::SetStage("initialize-engine");
	Generator::InitEngineCore();
	DumperSafety::SetStage("initialize-engine-complete");
	DumperSafety::SetStage("initialize-game-identity");
	InitializeGameIdentity();
	DumperSafety::SetStage("initialize-game-identity-complete");

	std::cerr << "GameName: " << Settings::Generator::GameName << "\n";
	std::cerr << "GameVersion: " << Settings::Generator::GameVersion << "\n\n";
	std::cerr << "FolderName: " << (Settings::Generator::GameVersion + '-' + Settings::Generator::GameName) << "\n\n";

	auto UnloadDumper = [&]()
	{
		if (ConsoleInput)
			fclose(ConsoleInput);
		if (ConsoleError)
			fclose(ConsoleError);
		if (HasConsole)
			FreeConsole();
		FreeLibraryAndExitThread(Module, 0);
	};

	if (Settings::Config::bContinuous)
	{
		if (Settings::Config::ControlPipeName.empty())
		{
			std::cerr << "Continuous mode requires ControlPipeName.\n";
			UnloadDumper();
		}

		try
		{
			DumperSafety::SetStage("continuous-controller");
			ContinuousController::Run();
		}
		catch (const std::exception& Error)
		{
			std::cerr << "Continuous controller failed: " << Error.what() << "\n";
		}

		// The controller has detached, but the DLL intentionally remains resident.
		// Unloading after repeated generation needs separate lifecycle validation.
		return 0;
	}

	Generator::GenerateSnapshot();

	if (Settings::Config::bUnloadAfterDump)
		UnloadDumper();

	std::cerr << "\n\nPress F6 to unload\n\n\n";
	while (true)
	{
		if (GetAsyncKeyState(VK_F6) & 1)
			UnloadDumper();
		Sleep(100);
	}

	return 0;
}

DWORD MainThread(HMODULE Module)
{
	__try
	{
		return MainThreadImpl(Module);
	}
	__except (DumperSafety::HandleException(GetExceptionInformation()))
	{
		return 1;
	}
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
	switch (reason)
	{
	case DLL_PROCESS_ATTACH:
		CreateThread(0, 0, (LPTHREAD_START_ROUTINE)MainThread, hModule, 0, 0);
		break;
	}

	return TRUE;
}
