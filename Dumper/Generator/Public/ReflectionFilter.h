#pragma once

#include <string>
#include <vector>

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

	const std::vector<std::string>& GetIncludedClassIdentities();
	const std::vector<std::string>& GetIncludedStructIdentities();
	const std::vector<std::string>& GetIncludedEnumIdentities();
	std::string GetReportLine();
}
