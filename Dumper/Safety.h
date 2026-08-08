#pragma once

#include <Windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace DumperSafety
{
	inline thread_local const char* CurrentStage = "startup";
	inline HANDLE ControlPipe = INVALID_HANDLE_VALUE;
	inline char CrashLogPath[MAX_PATH] = "C:\\Dumper-7\\dumper-crash.log";
	inline char StageLogPath[MAX_PATH] = "C:\\Dumper-7\\dumper-stage.log";
	inline bool HasLogDirectory = false;

	inline void WriteStage() noexcept
	{
		if (!HasLogDirectory)
			return;

		char Message[256] = {};
		sprintf_s(
			Message,
			"pid=%lu\r\nstage=%s\r\n",
			GetCurrentProcessId(),
			CurrentStage);
		const HANDLE File = CreateFileA(
			StageLogPath,
			GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			CREATE_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (File == INVALID_HANDLE_VALUE)
			return;

		DWORD Written = 0;
		WriteFile(File, Message, static_cast<DWORD>(strlen(Message)), &Written, nullptr);
		CloseHandle(File);
	}

	inline void SetStage(const char* Stage) noexcept
	{
		if (strcmp(CurrentStage, Stage) == 0)
			return;
		CurrentStage = Stage;
		WriteStage();
	}

	inline void SetLogDirectory(const std::string& Directory) noexcept
	{
		sprintf_s(CrashLogPath, "%s\\dumper-crash.log", Directory.c_str());
		sprintf_s(StageLogPath, "%s\\dumper-stage.log", Directory.c_str());
		HasLogDirectory = true;
		WriteStage();
	}

	inline void SetControlPipe(HANDLE Pipe) noexcept
	{
		ControlPipe = Pipe;
	}

	inline void ClearControlPipe() noexcept
	{
		ControlPipe = INVALID_HANDLE_VALUE;
	}

	inline LONG HandleException(EXCEPTION_POINTERS* Exception) noexcept
	{
		const DWORD Code = Exception && Exception->ExceptionRecord
			? Exception->ExceptionRecord->ExceptionCode
			: 0;
		const void* Address = Exception && Exception->ExceptionRecord
			? Exception->ExceptionRecord->ExceptionAddress
			: nullptr;
		const bool IsAccessViolation = Exception && Exception->ExceptionRecord
			&& Code == EXCEPTION_ACCESS_VIOLATION
			&& Exception->ExceptionRecord->NumberParameters >= 2;
		const ULONG_PTR AccessKind = IsAccessViolation
			? Exception->ExceptionRecord->ExceptionInformation[0]
			: 0;
		const void* AccessAddress = IsAccessViolation
			? reinterpret_cast<const void*>(Exception->ExceptionRecord->ExceptionInformation[1])
			: nullptr;
		const char* AccessName = AccessKind == 0 ? "read" : AccessKind == 1 ? "write" : "execute";

		char Message[768] = {};
		if (IsAccessViolation)
		{
			sprintf_s(
				Message,
				"ERROR Dumper-7 aborted stage '%s' after exception 0x%08lX at %p "
				"(%s access at %p).\r\n",
				CurrentStage,
				Code,
				Address,
				AccessName,
				AccessAddress);
		}
		else
		{
			sprintf_s(
				Message,
				"ERROR Dumper-7 aborted stage '%s' after exception 0x%08lX at %p.\r\n",
				CurrentStage,
				Code,
				Address);
		}
		OutputDebugStringA(Message);

		const HANDLE File = CreateFileA(
			CrashLogPath,
			FILE_APPEND_DATA,
			FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr,
			OPEN_ALWAYS,
			FILE_ATTRIBUTE_NORMAL,
			nullptr);
		if (File != INVALID_HANDLE_VALUE)
		{
			DWORD Written = 0;
			WriteFile(File, Message, static_cast<DWORD>(strlen(Message)), &Written, nullptr);
			CloseHandle(File);
		}

		const HANDLE Pipe = ControlPipe;
		ControlPipe = INVALID_HANDLE_VALUE;
		if (Pipe != INVALID_HANDLE_VALUE)
		{
			DWORD Written = 0;
			WriteFile(Pipe, Message, static_cast<DWORD>(strlen(Message)), &Written, nullptr);
			CloseHandle(Pipe);
		}

		return EXCEPTION_EXECUTE_HANDLER;
	}
}
