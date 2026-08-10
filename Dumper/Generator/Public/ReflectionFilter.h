#pragma once

#include <string>

#include "Unreal/UnrealObjects.h"


namespace ReflectionFilter
{
	void Configure(const std::string& Payload, const std::string& PayloadSha256);
	void Refresh();

	bool ShouldExclude(UEObject Object);
	bool ShouldExcludeStruct(UEStruct Struct);
	bool ShouldExcludeFunction(UEFunction Function);
	bool ShouldExcludeEnum(UEEnum Enum);
	bool ShouldExcludeProperty(UEProperty Property);

	std::string GetReportLine();
}
