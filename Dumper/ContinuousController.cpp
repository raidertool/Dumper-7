#include "ContinuousController.h"

#include <Windows.h>
#include <format>
#include <stdexcept>
#include <string>

#include "Generators/Generator.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "Safety.h"
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

	ReflectionState GetReflectionState()
	{
		ReflectionState State;
		const uint64 ObjectCount = static_cast<uint64>(max(ObjectArray::Num(), 0));
		const uint64 ChunkCount = static_cast<uint64>(max(ObjectArray::NumChunks(), 0));

		uint64 Combined = 0xCBF29CE484222325;
		HashValue(Combined, ObjectCount);
		HashValue(Combined, ChunkCount);
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
	DumperSafety::SetStage("controller-create-pipe");
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
	DumperSafety::SetControlPipe(Pipe);

	DumperSafety::SetStage("controller-wait-client");
	const bool Connected = ConnectNamedPipe(Pipe, nullptr)
		|| GetLastError() == ERROR_PIPE_CONNECTED;
	if (!Connected)
	{
		CloseHandle(Pipe);
		throw std::runtime_error("Could not connect the continuous-control pipe");
	}

	DumperSafety::SetStage("controller-ready");
	WritePipeLine(Pipe, "READY");
	std::string Command;
	while (ReadPipeLine(Pipe, Command))
	{
		if (Command == "STATUS")
		{
			DumperSafety::SetStage("status");
			if (!WritePipeLine(Pipe, GetRuntimeStatus()))
				break;
			continue;
		}

		if (Command.starts_with("DUMP\t"))
		{
			try
			{
				DumperSafety::SetStage("snapshot");
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
	DumperSafety::ClearControlPipe();
	CloseHandle(Pipe);
}
