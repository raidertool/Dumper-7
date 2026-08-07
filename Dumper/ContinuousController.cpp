#include "ContinuousController.h"

#include <Windows.h>
#include <format>
#include <stdexcept>
#include <string>

#include "Generators/Generator.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "Unreal/ObjectArray.h"

namespace
{
	struct ReflectionState
	{
		uint32 TypeCount = 0;
		uint64 Fingerprint = 0;
	};

	void HashValue(uint64& Hash, uint64 Value)
	{
		for (int32 Byte = 0; Byte < sizeof(Value); ++Byte)
		{
			Hash ^= (Value >> (Byte * 8)) & 0xFF;
			Hash *= 0x100000001B3;
		}
	}

	uint64 FinalizeHash(uint64 Hash)
	{
		Hash ^= Hash >> 33;
		Hash *= 0xFF51AFD7ED558CCD;
		Hash ^= Hash >> 33;
		Hash *= 0xC4CEB9FE1A85EC53;
		return Hash ^ (Hash >> 33);
	}

	uint64 GetTypeFingerprint(const UEObject Object, uint64 TypeFlags)
	{
		uint64 Hash = 0xCBF29CE484222325;
		HashValue(Hash, TypeFlags);

		uint32 Depth = 0;
		for (UEObject Current = Object; Current && Depth < 64; Current = Current.GetOuter())
		{
			const FName Name = Current.GetFName();
			HashValue(Hash, static_cast<uint32>(Name.GetCompIdx()));
			HashValue(Hash, Name.GetNumber());
			++Depth;
		}
		HashValue(Hash, Depth);
		return FinalizeHash(Hash);
	}

	ReflectionState GetReflectionState()
	{
		constexpr uint64 ReflectionMask =
			static_cast<uint64>(EClassCastFlags::Enum)
			| static_cast<uint64>(EClassCastFlags::Struct)
			| static_cast<uint64>(EClassCastFlags::Function);

		ReflectionState State;
		uint64 FingerprintXor = 0;
		uint64 FingerprintSum = 0;
		for (const UEObject Object : ObjectArray())
		{
			if (!Object)
				continue;

			const UEClass Class = Object.GetClass();
			if (!Class)
				continue;

			const uint64 TypeFlags = static_cast<uint64>(Class.GetCastFlags()) & ReflectionMask;
			if (TypeFlags == 0)
				continue;

			const uint64 TypeFingerprint = GetTypeFingerprint(Object, TypeFlags);
			FingerprintXor ^= TypeFingerprint;
			FingerprintSum += TypeFingerprint;
			++State.TypeCount;
		}

		uint64 Combined = 0xCBF29CE484222325;
		HashValue(Combined, State.TypeCount);
		HashValue(Combined, FingerprintXor);
		HashValue(Combined, FingerprintSum);
		State.Fingerprint = FinalizeHash(Combined);
		return State;
	}

	std::string GetRuntimeStatus()
	{
		uintptr_t World = 0x0;
		if (Off::InSDK::World::GWorld != 0x0)
		{
			auto ImageBase = reinterpret_cast<uint8*>(GetModuleHandle(nullptr));
			auto WorldPointer = reinterpret_cast<void**>(ImageBase + Off::InSDK::World::GWorld);
			World = reinterpret_cast<uintptr_t>(*WorldPointer);
		}

		const ReflectionState State = GetReflectionState();
		return std::format(
			"STATUS {:X} {} {} {:016X}",
			World,
			ObjectArray::Num(),
			State.TypeCount,
			State.Fingerprint);
	}

	bool ReadPipeLine(HANDLE Pipe, std::string& Line)
	{
		Line.clear();
		char Character = 0;
		DWORD Read = 0;
		while (ReadFile(Pipe, &Character, 1, &Read, nullptr) && Read == 1)
		{
			if (Character == '\n')
				return true;
			if (Character != '\r')
				Line.push_back(Character);
		}
		return false;
	}

	bool WritePipeLine(HANDLE Pipe, const std::string& Line)
	{
		const std::string Message = Line + "\n";
		DWORD Written = 0;
		return WriteFile(
			Pipe,
			Message.data(),
			static_cast<DWORD>(Message.size()),
			&Written,
			nullptr)
			&& Written == Message.size();
	}
}

void ContinuousController::Run()
{
	const std::string PipePath = "\\\\.\\pipe\\" + Settings::Config::ControlPipeName;
	HANDLE Pipe = CreateNamedPipeA(
		PipePath.c_str(),
		PIPE_ACCESS_DUPLEX,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
		1,
		4096,
		4096,
		0,
		nullptr);
	if (Pipe == INVALID_HANDLE_VALUE)
		throw std::runtime_error("Could not create the continuous-control pipe");

	const bool Connected = ConnectNamedPipe(Pipe, nullptr)
		|| GetLastError() == ERROR_PIPE_CONNECTED;
	if (!Connected)
	{
		CloseHandle(Pipe);
		throw std::runtime_error("Could not connect the continuous-control pipe");
	}

	WritePipeLine(Pipe, "READY");
	std::string Command;
	while (ReadPipeLine(Pipe, Command))
	{
		if (Command == "STATUS")
		{
			if (!WritePipeLine(Pipe, GetRuntimeStatus()))
				break;
			continue;
		}

		if (Command.starts_with("DUMP\t"))
		{
			try
			{
				Settings::Generator::SDKGenerationPath = Command.substr(5);
				Generator::GenerateSnapshot(false, false);
				if (!WritePipeLine(Pipe, "DONE"))
					break;
			}
			catch (const std::exception& Error)
			{
				if (!WritePipeLine(Pipe, std::string("ERROR ") + Error.what()))
					break;
			}
			continue;
		}

		if (Command == "STOP")
		{
			WritePipeLine(Pipe, "BYE");
			break;
		}

		if (!WritePipeLine(Pipe, "ERROR unknown command"))
			break;
	}

	FlushFileBuffers(Pipe);
	DisconnectNamedPipe(Pipe);
	CloseHandle(Pipe);
}
