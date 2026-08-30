#include "ReflectionFilter.h"

#include "Json/json.hpp"
#include "Unreal/ObjectArray.h"

#include <exception>
#include <stdexcept>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>


namespace
{
	struct FilterReport
	{
		size_t Configured = 0;
		size_t Matched = 0;
		size_t Unmatched = 0;
		size_t ExcludedObjects = 0;
		size_t ExcludedMembers = 0;
	};

	std::unordered_set<std::string> ExactIdentities;
	std::unordered_set<int32> ExcludedObjectIndices;
	std::vector<std::string> IncludedClassIdentities;
	std::vector<std::string> IncludedStructIdentities;
	std::vector<std::string> IncludedEnumIdentities;
	std::string ConfigurationSha256;
	FilterReport Report;

	bool IsReflectionObject(const UEObject Object)
	{
		return Object && Object.IsA(
			EClassCastFlags::Class
			| EClassCastFlags::Struct
			| EClassCastFlags::Function
			| EClassCastFlags::Enum);
	}

	std::string GetIdentity(const UEObject Object)
	{
		std::string Identity = Object.GetPathName();
		const size_t TypeSeparator = Identity.find(' ');
		return TypeSeparator == std::string::npos
			? Identity
			: Identity.substr(TypeSeparator + 1);
	}

	bool IsDirectMatch(const UEObject Object)
	{
		return Object && ExactIdentities.contains(GetIdentity(Object));
	}

	bool ResolveObjectExclusion(const UEObject Object, std::unordered_set<int32>& Visiting)
	{
		if (!Object)
			return false;

		const int32 Index = Object.GetIndex();
		if (ExcludedObjectIndices.contains(Index))
			return true;
		if (!IsReflectionObject(Object))
			return false;
		if (!Visiting.insert(Index).second)
			return false;

		bool bExcluded = IsDirectMatch(Object);
		for (UEObject Outer = Object.GetOuter(); !bExcluded && Outer; Outer = Outer.GetOuter())
		{
			if (IsReflectionObject(Outer) && ResolveObjectExclusion(Outer, Visiting))
				bExcluded = true;
		}

		if (!bExcluded && Object.IsA(EClassCastFlags::Struct))
		{
			const UEStruct Super = Object.Cast<UEStruct>().GetSuper();
			bExcluded = Super && ResolveObjectExclusion(Super, Visiting);
		}

		Visiting.erase(Index);
		if (bExcluded)
			ExcludedObjectIndices.insert(Index);
		return bExcluded;
	}

	bool ReferencesExcludedObject(const UEProperty Property)
	{
		if (!Property)
			return false;

		if (Property.IsA(EClassCastFlags::OptionalProperty))
			return ReferencesExcludedObject(Property.Cast<UEOptionalProperty>().GetValueProperty());
		if (Property.IsA(EClassCastFlags::MapProperty))
		{
			const UEMapProperty Map = Property.Cast<UEMapProperty>();
			return ReferencesExcludedObject(Map.GetKeyProperty())
				|| ReferencesExcludedObject(Map.GetValueProperty());
		}
		if (Property.IsA(EClassCastFlags::SetProperty))
			return ReferencesExcludedObject(Property.Cast<UESetProperty>().GetElementProperty());
		if (Property.IsA(EClassCastFlags::ArrayProperty))
			return ReferencesExcludedObject(Property.Cast<UEArrayProperty>().GetInnerProperty());
		if (Property.IsA(EClassCastFlags::EnumProperty))
		{
			const UEEnumProperty EnumProperty = Property.Cast<UEEnumProperty>();
			return ReflectionFilter::ShouldExclude(EnumProperty.GetEnum())
				|| ReferencesExcludedObject(EnumProperty.GetUnderlayingProperty());
		}
		if (Property.IsA(EClassCastFlags::ByteProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEByteProperty>().GetEnum());
		if (Property.IsA(EClassCastFlags::StructProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEStructProperty>().GetUnderlayingStruct());
		if (Property.IsA(EClassCastFlags::ClassProperty))
		{
			const UEClassProperty ClassProperty = Property.Cast<UEClassProperty>();
			return ReflectionFilter::ShouldExclude(ClassProperty.GetPropertyClass())
				|| ReflectionFilter::ShouldExclude(ClassProperty.GetMetaClass());
		}
		if (Property.IsA(EClassCastFlags::InterfaceProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEInterfaceProperty>().GetPropertyClass());
		if (Property.IsA(EClassCastFlags::ObjectProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEObjectProperty>().GetPropertyClass());
		if (Property.IsA(EClassCastFlags::DelegateProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEDelegateProperty>().GetSignatureFunction());
		if (Property.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
			return ReflectionFilter::ShouldExclude(Property.Cast<UEMulticastInlineDelegateProperty>().GetSignatureFunction());

		return false;
	}
}


void ReflectionFilter::Configure(const std::string& Payload, const std::string& PayloadSha256)
{
	nlohmann::json Document;
	try
	{
		Document = nlohmann::json::parse(Payload);
	}
	catch (const nlohmann::json::exception& Error)
	{
		throw std::invalid_argument(std::string("invalid reflection-filter JSON: ") + Error.what());
	}

	if (!Document.is_object() || Document.value("schema_version", 0) != 1)
		throw std::invalid_argument("reflection-filter schema_version must be 1");
	if (Document.size() != 2 || !Document.contains("schema_version") || !Document.contains("identities"))
		throw std::invalid_argument("reflection-filter contains unknown or missing fields");
	if (!Document.contains("identities") || !Document["identities"].is_array())
		throw std::invalid_argument("reflection-filter identities must be an array");
	if (Document.dump() != Payload)
		throw std::invalid_argument("reflection-filter payload is not canonical JSON");

	std::unordered_set<std::string> NewIdentities;
	std::string PreviousIdentity;
	for (const nlohmann::json& Identity : Document["identities"])
	{
		if (!Identity.is_string() || Identity.get_ref<const std::string&>().empty())
			throw std::invalid_argument("reflection-filter identities must be non-empty strings");
		const std::string& Value = Identity.get_ref<const std::string&>();
		if (!PreviousIdentity.empty() && Value <= PreviousIdentity)
			throw std::invalid_argument("reflection-filter identities must be sorted and unique");
		NewIdentities.insert(Value);
		PreviousIdentity = Value;
	}

	const std::unordered_set<std::string> OldIdentities = ExactIdentities;
	const std::string OldConfigurationSha256 = ConfigurationSha256;
	ExactIdentities = std::move(NewIdentities);
	ConfigurationSha256 = PayloadSha256;
	try
	{
		Refresh();
	}
	catch (...)
	{
		const std::exception_ptr ConfigurationError = std::current_exception();
		ExactIdentities = OldIdentities;
		ConfigurationSha256 = OldConfigurationSha256;
		try
		{
			Refresh();
		}
		catch (...)
		{
		}
		std::rethrow_exception(ConfigurationError);
	}
}

void ReflectionFilter::Refresh()
{
	ExcludedObjectIndices.clear();
	IncludedClassIdentities.clear();
	IncludedStructIdentities.clear();
	IncludedEnumIdentities.clear();
	Report = {};
	Report.Configured = ExactIdentities.size();

	std::unordered_map<std::string, UEObject> ObservedClasses;
	std::unordered_map<std::string, UEObject> ObservedStructs;
	std::unordered_map<std::string, UEObject> ObservedEnums;
	const auto ObserveClass = [&](const UEObject Object)
	{
		if (Object && Object.IsA(EClassCastFlags::Class))
			ObservedClasses.try_emplace(GetIdentity(Object), Object);
	};
	const auto ObserveStandaloneReflection = [&](const UEObject Object)
	{
		if (!Object)
			return;
		if (Object.IsA(EClassCastFlags::Enum))
			ObservedEnums.try_emplace(GetIdentity(Object), Object);
		else if (Object.IsA(EClassCastFlags::Struct)
			&& !Object.IsA(EClassCastFlags::Class)
			&& !Object.IsA(EClassCastFlags::Function))
			ObservedStructs.try_emplace(GetIdentity(Object), Object);
	};
	for (const UEObject Object : ObjectArray())
	{
		ObserveClass(Object);
		ObserveClass(Object.GetClass());
		ObserveStandaloneReflection(Object);
	}

	if (ExactIdentities.empty())
	{
		IncludedClassIdentities.reserve(ObservedClasses.size());
		for (const auto& [Identity, _] : ObservedClasses)
			IncludedClassIdentities.push_back(Identity);
		std::ranges::sort(IncludedClassIdentities);
		IncludedStructIdentities.reserve(ObservedStructs.size());
		for (const auto& [Identity, _] : ObservedStructs)
			IncludedStructIdentities.push_back(Identity);
		std::ranges::sort(IncludedStructIdentities);
		IncludedEnumIdentities.reserve(ObservedEnums.size());
		for (const auto& [Identity, _] : ObservedEnums)
			IncludedEnumIdentities.push_back(Identity);
		std::ranges::sort(IncludedEnumIdentities);
		return;
	}

	std::unordered_set<std::string> MatchedIdentities;
	std::unordered_set<int32> Visiting;
	std::unordered_set<int32> VisitedReflectionObjects;
	const auto VisitReflectionObject = [&](const UEObject Object)
	{
		if (!Object || !VisitedReflectionObjects.insert(Object.GetIndex()).second)
			return;
		if (IsDirectMatch(Object))
		{
			MatchedIdentities.insert(GetIdentity(Object));
			ExcludedObjectIndices.insert(Object.GetIndex());
		}
		if (!IsReflectionObject(Object))
			return;
		ResolveObjectExclusion(Object, Visiting);
	};

	for (const UEObject Object : ObjectArray())
	{
		VisitReflectionObject(Object);
		VisitReflectionObject(Object.GetClass());
	}

	for (const UEObject Object : ObjectArray())
	{
		if (!Object.IsA(EClassCastFlags::Function)
			|| ExcludedObjectIndices.contains(Object.GetIndex()))
			continue;

		for (const UEProperty Property : Object.Cast<UEFunction>().GetProperties())
		{
			if (ReferencesExcludedObject(Property))
			{
				ExcludedObjectIndices.insert(Object.GetIndex());
				break;
			}
		}
	}

	size_t ExcludedMembers = 0;
	for (const UEObject Object : ObjectArray())
	{
		if (!Object.IsA(EClassCastFlags::Struct))
			continue;

		const bool bOwnerExcluded = ExcludedObjectIndices.contains(Object.GetIndex());
		for (const UEProperty Property : Object.Cast<UEStruct>().GetProperties())
		{
			if (bOwnerExcluded || ReferencesExcludedObject(Property))
				ExcludedMembers++;
		}
	}

	Report.Matched = MatchedIdentities.size();
	Report.Unmatched = Report.Configured - Report.Matched;
	Report.ExcludedObjects = ExcludedObjectIndices.size();
	Report.ExcludedMembers = ExcludedMembers;
	IncludedClassIdentities.reserve(ObservedClasses.size());
	for (const auto& [Identity, Object] : ObservedClasses)
	{
		if (!ExcludedObjectIndices.contains(Object.GetIndex()))
			IncludedClassIdentities.push_back(Identity);
	}
	std::ranges::sort(IncludedClassIdentities);
	IncludedStructIdentities.reserve(ObservedStructs.size());
	for (const auto& [Identity, Object] : ObservedStructs)
	{
		if (!ExcludedObjectIndices.contains(Object.GetIndex()))
			IncludedStructIdentities.push_back(Identity);
	}
	std::ranges::sort(IncludedStructIdentities);
	IncludedEnumIdentities.reserve(ObservedEnums.size());
	for (const auto& [Identity, Object] : ObservedEnums)
	{
		if (!ExcludedObjectIndices.contains(Object.GetIndex()))
			IncludedEnumIdentities.push_back(Identity);
	}
	std::ranges::sort(IncludedEnumIdentities);

	if (Report.Unmatched != 0)
	{
		std::string FirstUnmatched;
		for (const std::string& Identity : ExactIdentities)
		{
			if (!MatchedIdentities.contains(Identity))
			{
				FirstUnmatched = Identity;
				break;
			}
		}
		throw std::invalid_argument(
			std::to_string(Report.Unmatched) + " unmatched reflection identities; first: " + FirstUnmatched);
	}
}

bool ReflectionFilter::ShouldExclude(const UEObject Object)
{
	if (!Object)
		return false;
	if (ExcludedObjectIndices.contains(Object.GetIndex()))
		return true;
	if (!IsReflectionObject(Object))
		return false;

	std::unordered_set<int32> Visiting;
	return ResolveObjectExclusion(Object, Visiting);
}

bool ReflectionFilter::ShouldExcludeStruct(const UEStruct Struct)
{
	return ShouldExclude(Struct);
}

bool ReflectionFilter::ShouldExcludeFunction(const UEFunction Function)
{
	if (ShouldExclude(Function))
		return true;
	for (const UEProperty Property : Function.GetProperties())
	{
		if (ShouldExcludeProperty(Property))
			return true;
	}
	return false;
}

bool ReflectionFilter::ShouldExcludeEnum(const UEEnum Enum)
{
	return ShouldExclude(Enum);
}

bool ReflectionFilter::ShouldExcludeProperty(const UEProperty Property)
{
	return ReferencesExcludedObject(Property);
}

const std::vector<std::string>& ReflectionFilter::GetIncludedClassIdentities()
{
	return IncludedClassIdentities;
}

const std::vector<std::string>& ReflectionFilter::GetIncludedStructIdentities()
{
	return IncludedStructIdentities;
}

const std::vector<std::string>& ReflectionFilter::GetIncludedEnumIdentities()
{
	return IncludedEnumIdentities;
}

std::string ReflectionFilter::GetReportLine()
{
	return "FILTER " + ConfigurationSha256
		+ " " + std::to_string(Report.Configured)
		+ " " + std::to_string(Report.Matched)
		+ " " + std::to_string(Report.ExcludedObjects + Report.ExcludedMembers);
}
