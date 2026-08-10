#include "ContinuousController.h"

#include <Windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <cctype>
#include <format>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "Generators/Generator.h"
#include "OffsetFinder/Offsets.h"
#include "Platform.h"
#include "ReflectionFilter.h"
#include "Safety.h"
#include "Unreal/ObjectArray.h"

#pragma comment(lib, "bcrypt.lib")

namespace
{
	struct ReflectionState
	{
		uint32 TypeCount = 0;
		uint64 Fingerprint = 0;
	};

	struct RuntimeState
	{
		uintptr_t World = 0;
		int32 ObjectCount = 0;
		ReflectionState Reflection;
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

	constexpr bool HasAnyCastFlag(EClassCastFlags Value, EClassCastFlags Mask)
	{
		using UnderlyingType = std::underlying_type_t<EClassCastFlags>;
		return (static_cast<UnderlyingType>(Value) & static_cast<UnderlyingType>(Mask)) != 0;
	}

	ReflectionState GetReflectionState()
	{
		ReflectionState State;
		uint64 Combined = 0xCBF29CE484222325;
		constexpr EClassCastFlags ReflectionTypes = EClassCastFlags::Enum
			| EClassCastFlags::Struct
			| EClassCastFlags::Function
			| EClassCastFlags::Class;
		static_assert(HasAnyCastFlag(EClassCastFlags::Enum, ReflectionTypes));
		static_assert(!HasAnyCastFlag(EClassCastFlags::Actor, ReflectionTypes));
		ObjectArray Objects;
		for (auto Iterator = Objects.begin(); Iterator != Objects.end(); ++Iterator)
		{
			const UEObject Object = *Iterator;
			const EClassCastFlags CastFlags = Object.GetClass().GetCastFlags();
			if (!HasAnyCastFlag(CastFlags, ReflectionTypes))
				continue;

			++State.TypeCount;
			HashValue(Combined, static_cast<uint64>(Iterator.GetIndex()));
			HashValue(Combined, reinterpret_cast<uintptr_t>(Object.GetAddress()));
			HashValue(Combined, static_cast<uint64>(CastFlags));
		}
		HashValue(Combined, State.TypeCount);
		State.Fingerprint = FinalizeHash(Combined);
		return State;
	}

	RuntimeState GetRuntimeState()
	{
		RuntimeState State;
		if (Off::InSDK::World::GWorld != 0x0)
		{
			auto ImageBase = reinterpret_cast<uint8*>(GetModuleHandle(nullptr));
			auto WorldPointer = reinterpret_cast<void**>(ImageBase + Off::InSDK::World::GWorld);
			State.World = reinterpret_cast<uintptr_t>(*WorldPointer);
		}
		State.ObjectCount = ObjectArray::Num();
		State.Reflection = GetReflectionState();
		return State;
	}

	std::string FormatRuntimeState(const char* Prefix, const RuntimeState& State)
	{
		return std::format(
			"{} {:X} {} {} {:016X}",
			Prefix,
			State.World,
			State.ObjectCount,
			State.Reflection.TypeCount,
			State.Reflection.Fingerprint);
	}

	std::string GetRuntimeStatus()
	{
		return FormatRuntimeState("STATUS", GetRuntimeState());
	}

	bool IsStableSnapshot(const RuntimeState& Before, const RuntimeState& After)
	{
		return Before.World == After.World
			&& Before.ObjectCount == After.ObjectCount
			&& Before.Reflection.Fingerprint == After.Reflection.Fingerprint;
	}

	std::string FormatSnapshotResult(
		const char* Result,
		const RuntimeState& Before,
		const RuntimeState& After)
	{
		return std::format(
			"{} {:X} {} {} {:016X} {:X} {} {} {:016X}",
			Result,
			Before.World,
			Before.ObjectCount,
			Before.Reflection.TypeCount,
			Before.Reflection.Fingerprint,
			After.World,
			After.ObjectCount,
			After.Reflection.TypeCount,
			After.Reflection.Fingerprint);
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

	bool ReadPipeBytes(HANDLE Pipe, std::string& Data, const size_t Length)
	{
		Data.resize(Length);
		size_t Offset = 0;
		while (Offset < Length)
		{
			const DWORD Remaining = static_cast<DWORD>(std::min<size_t>(Length - Offset, MAXDWORD));
			DWORD Read = 0;
			if (!ReadFile(Pipe, Data.data() + Offset, Remaining, &Read, nullptr) || Read == 0)
				return false;
			Offset += Read;
		}
		return true;
	}

	std::string Sha256(const std::string& Data)
	{
		BCRYPT_ALG_HANDLE Algorithm = nullptr;
		BCRYPT_HASH_HANDLE Hash = nullptr;
		DWORD ObjectSize = 0;
		DWORD HashSize = 0;
		DWORD ResultSize = 0;

		auto Check = [](const NTSTATUS Status, const char* Operation)
		{
			if (Status < 0)
				throw std::runtime_error(std::string("SHA-256 ") + Operation + " failed");
		};

		Check(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0), "initialization");
		try
		{
			Check(BCryptGetProperty(Algorithm, BCRYPT_OBJECT_LENGTH,
				reinterpret_cast<PUCHAR>(&ObjectSize), sizeof(ObjectSize), &ResultSize, 0), "object-size query");
			Check(BCryptGetProperty(Algorithm, BCRYPT_HASH_LENGTH,
				reinterpret_cast<PUCHAR>(&HashSize), sizeof(HashSize), &ResultSize, 0), "hash-size query");

			std::vector<UCHAR> HashObject(ObjectSize);
			std::vector<UCHAR> Digest(HashSize);
			Check(BCryptCreateHash(Algorithm, &Hash, HashObject.data(), ObjectSize, nullptr, 0, 0), "creation");
			Check(BCryptHashData(Hash, reinterpret_cast<PUCHAR>(const_cast<char*>(Data.data())),
				static_cast<ULONG>(Data.size()), 0), "update");
			Check(BCryptFinishHash(Hash, Digest.data(), HashSize, 0), "finalization");

			std::ostringstream Encoded;
			Encoded << std::hex << std::setfill('0');
			for (const UCHAR Byte : Digest)
				Encoded << std::setw(2) << static_cast<unsigned>(Byte);

			BCryptDestroyHash(Hash);
			BCryptCloseAlgorithmProvider(Algorithm, 0);
			return Encoded.str();
		}
		catch (...)
		{
			if (Hash)
				BCryptDestroyHash(Hash);
			BCryptCloseAlgorithmProvider(Algorithm, 0);
			throw;
		}
	}

	bool IsSha256(const std::string& Value)
	{
		return Value.size() == 64 && std::all_of(Value.begin(), Value.end(), [](const unsigned char Character)
		{
			return std::isxdigit(Character) != 0;
		});
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
		if (Command.starts_with("SET_FILTER "))
		{
			bool bCloseConnection = false;
			try
			{
				constexpr size_t MaximumPayloadSize = 16 * 1024 * 1024;
				std::istringstream Header(Command);
				std::string Operation;
				size_t PayloadSize = 0;
				std::string ExpectedSha256;
				std::string Extra;
				if (!(Header >> Operation >> PayloadSize >> ExpectedSha256)
					|| Operation != "SET_FILTER" || Header >> Extra)
					throw std::invalid_argument("invalid SET_FILTER header");
				if (PayloadSize > MaximumPayloadSize)
				{
					bCloseConnection = true;
					throw std::invalid_argument("SET_FILTER payload exceeds 16 MiB");
				}
				if (!IsSha256(ExpectedSha256))
				{
					bCloseConnection = true;
					throw std::invalid_argument("SET_FILTER requires a SHA-256 digest");
				}

				std::string Payload;
				if (!ReadPipeBytes(Pipe, Payload, PayloadSize))
					break;

				std::transform(ExpectedSha256.begin(), ExpectedSha256.end(), ExpectedSha256.begin(), [](const unsigned char Character)
				{
					return static_cast<char>(std::tolower(Character));
				});
				if (Sha256(Payload) != ExpectedSha256)
					throw std::invalid_argument("SET_FILTER payload SHA-256 mismatch");

				ReflectionFilter::Configure(Payload, ExpectedSha256);
				if (!WritePipeLine(Pipe, ReflectionFilter::GetReportLine()))
					break;
			}
			catch (const std::exception& Error)
			{
				if (!WritePipeLine(Pipe, std::string("ERROR ") + Error.what()))
					break;
				if (bCloseConnection)
					break;
			}
			continue;
		}

		if (Command == "STATUS")
		{
			DumperSafety::SetStage("status");
			if (!WritePipeLine(Pipe, GetRuntimeStatus()))
				break;
			continue;
		}

		const bool bFullSnapshot = Command.starts_with("DUMP_FULL\t");
		if (bFullSnapshot || Command.starts_with("DUMP\t"))
		{
			try
			{
				DumperSafety::SetStage("snapshot");
				const size_t PathOffset = bFullSnapshot ? 10 : 5;
				Settings::Generator::SDKGenerationPath = Command.substr(PathOffset);
				const RuntimeState Before = GetRuntimeState();
				Generator::GenerateSnapshot(bFullSnapshot, false);
				const RuntimeState After = GetRuntimeState();
				const char* Result = IsStableSnapshot(Before, After) ? "DONE" : "UNSTABLE";
				if (!WritePipeLine(Pipe, FormatSnapshotResult(Result, Before, After)))
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
