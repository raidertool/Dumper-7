#include "Generators/ReflectionIRGenerator.h"
#include "Generators/CppGenerator.h"

#include "Json/json.hpp"
#include "Managers/PackageManager.h"
#include "Managers/StructManager.h"
#include "Wrappers/EnumWrapper.h"
#include "Wrappers/MemberWrappers.h"
#include "Wrappers/StructWrapper.h"

#include "Platform.h"
#include "Settings.h"

#include <fstream>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sstream>


namespace
{
    constexpr int32 ReflectionIRFormatVersion = 2;
    thread_local char ReflectionIRCaptureContext[512] = "initializing";

    void SetCaptureContext(const char* Kind, const int32 Index) noexcept
    {
        sprintf_s(ReflectionIRCaptureContext, "%s index %d", Kind, Index);
    }

    LONG HandleCaptureException(
        EXCEPTION_POINTERS* Exception,
        ReflectionIRGenerator::CaptureFailure* Failure) noexcept
    {
        if (!Exception || !Exception->ExceptionRecord
            || Exception->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
        {
            return EXCEPTION_CONTINUE_SEARCH;
        }

        if (Failure)
        {
            Failure->Code = Exception->ExceptionRecord->ExceptionCode;
            Failure->ExceptionAddress = Exception->ExceptionRecord->ExceptionAddress;
            if (Exception->ExceptionRecord->NumberParameters >= 2)
            {
                Failure->AccessKind = Exception->ExceptionRecord->ExceptionInformation[0];
                Failure->AccessAddress = reinterpret_cast<const void*>(
                    Exception->ExceptionRecord->ExceptionInformation[1]);
            }
            strcpy_s(Failure->Context, ReflectionIRCaptureContext);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }

    void SortByIdentity(nlohmann::json& Records)
    {
        std::sort(Records.begin(), Records.end(), [](const nlohmann::json& Left, const nlohmann::json& Right)
        {
            return Left.at("identity").get_ref<const std::string&>() < Right.at("identity").get_ref<const std::string&>();
        });
    }

    nlohmann::json ObjectReference(const UEObject Object)
    {
        if (!Object)
            return nullptr;

        return {
            { "identity", Object.GetPathName() },
            { "path_name", Object.GetPathName() },
            { "raw_name", Object.GetName() },
            { "valid_name", Object.GetValidName() },
            { "cpp_name", Object.GetCppName() },
        };
    }

    nlohmann::json FieldClassReference(const UEFFieldClass FieldClass)
    {
        if (!FieldClass)
            return nullptr;

        return {
            { "id", static_cast<uint64>(FieldClass.GetId()) },
            { "name", FieldClass.GetName() },
            { "valid_name", FieldClass.GetValidName() },
            { "cpp_name", FieldClass.GetCppName() },
            { "cast_flags", static_cast<uint64>(FieldClass.GetCastFlags()) },
            { "class_flags", static_cast<uint64>(FieldClass.GetClassFlags()) },
        };
    }

    nlohmann::json PropertyType(const UEProperty Property)
    {
        if (!Property)
            return nullptr;

        const auto [Class, FieldClass] = Property.GetClass();
        nlohmann::json Type = {
            { "property_class", Property.GetPropClassName() },
            { "cast_flags", static_cast<uint64>(Property.GetCastFlags()) },
            { "cpp_type", Property.GetCppType() },
            { "size", Property.GetSize() },
            { "alignment", Property.GetAlignment() },
            { "class", ObjectReference(Class) },
            { "field_class", FieldClassReference(FieldClass) },
        };

        if (Property.IsA(EClassCastFlags::OptionalProperty))
        {
            Type["value"] = PropertyType(Property.Cast<UEOptionalProperty>().GetValueProperty());
        }
        else if (Property.IsA(EClassCastFlags::MapProperty))
        {
            const UEMapProperty Map = Property.Cast<UEMapProperty>();
            Type["key"] = PropertyType(Map.GetKeyProperty());
            Type["value"] = PropertyType(Map.GetValueProperty());
        }
        else if (Property.IsA(EClassCastFlags::SetProperty))
        {
            Type["element"] = PropertyType(Property.Cast<UESetProperty>().GetElementProperty());
        }
        else if (Property.IsA(EClassCastFlags::ArrayProperty))
        {
            Type["inner"] = PropertyType(Property.Cast<UEArrayProperty>().GetInnerProperty());
        }
        else if (Property.IsA(EClassCastFlags::EnumProperty))
        {
            const UEEnumProperty EnumProperty = Property.Cast<UEEnumProperty>();
            Type["enum"] = ObjectReference(EnumProperty.GetEnum());
            Type["underlying"] = PropertyType(EnumProperty.GetUnderlayingProperty());
        }
        else if (Property.IsA(EClassCastFlags::ByteProperty))
        {
            Type["enum"] = ObjectReference(Property.Cast<UEByteProperty>().GetEnum());
        }
        else if (Property.IsA(EClassCastFlags::StructProperty))
        {
            Type["struct"] = ObjectReference(Property.Cast<UEStructProperty>().GetUnderlayingStruct());
        }
        else if (Property.IsA(EClassCastFlags::ClassProperty))
        {
            const UEClassProperty ClassProperty = Property.Cast<UEClassProperty>();
            Type["property_class_target"] = ObjectReference(ClassProperty.GetPropertyClass());
            Type["meta_class"] = ObjectReference(ClassProperty.GetMetaClass());
        }
        else if (Property.IsA(EClassCastFlags::InterfaceProperty))
        {
            Type["interface_class"] = ObjectReference(Property.Cast<UEInterfaceProperty>().GetPropertyClass());
        }
        else if (Property.IsA(EClassCastFlags::ObjectProperty))
        {
            Type["property_class_target"] = ObjectReference(Property.Cast<UEObjectProperty>().GetPropertyClass());
        }
        else if (Property.IsA(EClassCastFlags::DelegateProperty))
        {
            Type["signature"] = ObjectReference(Property.Cast<UEDelegateProperty>().GetSignatureFunction());
        }
        else if (Property.IsA(EClassCastFlags::MulticastInlineDelegateProperty))
        {
            Type["signature"] = ObjectReference(Property.Cast<UEMulticastInlineDelegateProperty>().GetSignatureFunction());
        }
        else if (Property.IsA(EClassCastFlags::FieldPathProperty))
        {
            Type["target_field_class"] = FieldClassReference(Property.Cast<UEFieldPathProperty>().GetFieldClass());
        }

        if (Property.IsA(EClassCastFlags::BoolProperty))
        {
            const UEBoolProperty Bool = Property.Cast<UEBoolProperty>();
            Type["bool"] = {
                { "native", Bool.IsNativeBool() },
                { "byte_offset", Bool.GetByteOffset() },
                { "bit_index", Bool.GetBitIndex() },
                { "field_mask", Bool.GetFieldMask() },
            };
        }

        return Type;
    }

    nlohmann::json PropertyRecord(
        const PropertyWrapper& WrappedProperty,
        const std::string& OwnerIdentity,
        const int32 Ordinal)
    {
        const UEProperty Property = WrappedProperty.GetUnrealProperty();
        const std::string Identity = OwnerIdentity
            + "." + Property.GetName()
            + "@" + std::to_string(Property.GetOffset())
            + ":" + std::to_string(Ordinal);
        return {
            { "identity", Identity },
            { "owner_identity", OwnerIdentity },
            { "ordinal", Ordinal },
            { "raw_name", Property.GetName() },
            { "valid_name", Property.GetValidName() },
            { "generated_name", WrappedProperty.GetName() },
            { "offset", Property.GetOffset() },
            { "size", Property.GetSize() },
            { "effective_size", WrappedProperty.GetSize() },
            { "alignment", Property.GetAlignment() },
            { "array_dim", Property.GetArrayDim() },
            { "property_flags", static_cast<uint64>(Property.GetPropertyFlags()) },
            { "property_flags_text", Property.StringifyFlags() },
            { "type", PropertyType(Property) },
        };
    }

    nlohmann::json FunctionRecord(const FunctionWrapper& WrappedFunction, const int32 Ordinal)
    {
        const UEFunction Function = WrappedFunction.GetUnrealFunction();
        nlohmann::json Parameters = nlohmann::json::array();
        int32 ParameterOrdinal = 0;
        for (const PropertyWrapper& Parameter : WrappedFunction.GetMembers().IterateMembers())
        {
            if (!Parameter.IsUnrealProperty())
                continue;

            Parameters.push_back(PropertyRecord(Parameter, Function.GetPathName(), ParameterOrdinal++));
        }

        return {
            { "ordinal", Ordinal },
            { "identity", Function.GetPathName() },
            { "path_name", Function.GetPathName() },
            { "raw_name", Function.GetName() },
            { "valid_name", Function.GetValidName() },
            { "generated_name", WrappedFunction.GetName() },
            { "full_name", Function.GetFullName() },
            { "outer", ObjectReference(Function.GetOuter()) },
            { "package", ObjectReference(Function.GetOutermost()) },
            { "object_flags", static_cast<uint64>(Function.GetFlags()) },
            { "object_flags_text", Function.StringifyObjFlags() },
            { "function_flags", static_cast<uint64>(Function.GetFunctionFlags()) },
            { "function_flags_text", Function.StringifyFlags("|") },
            { "parameter_struct_size", Function.GetStructSize() },
            { "exec_offset", Platform::GetOffset(Function.GetExecFunction()) },
            { "parameters", std::move(Parameters) },
        };
    }

    nlohmann::json StructRecord(const StructWrapper& WrappedStruct)
    {
        const UEStruct Struct = WrappedStruct.GetUnrealStruct();
        const auto [GeneratedName, bIsUniqueName] = WrappedStruct.GetUniqueName();

        nlohmann::json Properties = nlohmann::json::array();
        nlohmann::json Functions = nlohmann::json::array();
        MemberManager Members = WrappedStruct.GetMembers();
        int32 PropertyOrdinal = 0;
        for (const PropertyWrapper& Property : Members.IterateMembers())
        {
            if (!Property.IsUnrealProperty())
                continue;

            Properties.push_back(PropertyRecord(Property, Struct.GetPathName(), PropertyOrdinal++));
        }
        int32 FunctionOrdinal = 0;
        for (const FunctionWrapper& Function : Members.IterateFunctions())
        {
            if (Function.IsPredefined())
                continue;

            Functions.push_back(FunctionRecord(Function, FunctionOrdinal++));
        }
        SortByIdentity(Functions);
        for (int32 Ordinal = 0; Ordinal < static_cast<int32>(Functions.size()); ++Ordinal)
            Functions[Ordinal]["ordinal"] = Ordinal;

        nlohmann::json CyclicPackages = nlohmann::json::array();
        for (const int32 PackageIndex : StructManager::GetCyclicPackages(Struct.GetIndex()))
        {
            CyclicPackages.push_back(ObjectReference(ObjectArray::GetByIndex(PackageIndex)));
        }
        SortByIdentity(CyclicPackages);

        nlohmann::json Record = {
            { "identity", Struct.GetPathName() },
            { "path_name", Struct.GetPathName() },
            { "raw_name", Struct.GetName() },
            { "valid_name", Struct.GetValidName() },
            { "cpp_name", Struct.GetCppName() },
            { "generated_name", GeneratedName },
            { "cpp_qualified_name", CppGenerator::GetStructPrefixedName(WrappedStruct) },
            { "generated_name_is_unique", bIsUniqueName },
            { "full_name", Struct.GetFullName() },
            { "package", ObjectReference(Struct.GetOutermost()) },
            { "outer", ObjectReference(Struct.GetOuter()) },
            { "meta_class", ObjectReference(Struct.GetClass()) },
            { "super", ObjectReference(Struct.GetSuper()) },
            { "object_flags", static_cast<uint64>(Struct.GetFlags()) },
            { "object_flags_text", Struct.StringifyObjFlags() },
            { "kind", WrappedStruct.IsClass() ? "class" : WrappedStruct.IsFunction() ? "function" : "struct" },
            { "is_class", WrappedStruct.IsClass() },
            { "is_function", WrappedStruct.IsFunction() },
            { "is_interface", WrappedStruct.IsInterface() },
            { "is_final", WrappedStruct.IsFinal() },
            { "is_exact_uobject", WrappedStruct.IsExactClassUObject() },
            { "size", WrappedStruct.GetSize() },
            { "reflected_size", Struct.GetStructSize() },
            { "unaligned_size", WrappedStruct.GetUnalignedSize() },
            { "alignment", WrappedStruct.GetAlignment() },
            { "reflected_min_alignment", Struct.GetMinAlignment() },
            { "last_member_end", WrappedStruct.GetLastMemberEnd() },
            { "uses_explicit_alignment", WrappedStruct.ShouldUseExplicitAlignment() },
            { "reuses_trailing_padding", WrappedStruct.HasReusedTrailingPadding() },
            { "cyclic_packages", std::move(CyclicPackages) },
            { "properties", std::move(Properties) },
            { "functions", std::move(Functions) },
        };

        if (WrappedStruct.IsClass())
        {
            const UEClass Class = Struct.Cast<UEClass>();
            Record["class_cast_flags"] = static_cast<uint64>(Class.GetCastFlags());
            Record["class_cast_flags_text"] = Class.StringifyCastFlags();

            nlohmann::json Interfaces = nlohmann::json::array();
            const TArray<FImplementedInterface> ImplementedInterfaces =
                Class.GetImplementedInterfaces();
            if (ImplementedInterfaces.IsValid()
                && !Platform::IsBadReadPtr(ImplementedInterfaces.GetDataPtr()))
            {
                for (const FImplementedInterface& Interface : ImplementedInterfaces)
                {
                    if (!Interface.InterfaceClass)
                        continue;

                    Interfaces.push_back({
                        { "class", ObjectReference(Interface.InterfaceClass) },
                        { "pointer_offset", Interface.PointerOffset },
                        { "implemented_by_blueprint", Interface.bImplementedByK2 },
                    });
                }
            }
            std::sort(Interfaces.begin(), Interfaces.end(), [](const nlohmann::json& Left, const nlohmann::json& Right)
            {
                return Left.at("class").at("identity").get_ref<const std::string&>()
                    < Right.at("class").at("identity").get_ref<const std::string&>();
            });
            Record["implemented_interfaces"] = std::move(Interfaces);
        }

        std::ostringstream Declaration;
        std::ostringstream CppFunctions;
        std::ostringstream Parameters;
        std::ostringstream Assertions;
        CppGenerator::GenerateStruct(
            WrappedStruct,
            Declaration,
            CppFunctions,
            Parameters,
            Assertions,
            Struct.GetPackageIndex());
        Record["cpp"] = {
            { "declaration", Declaration.str() },
            { "functions", CppFunctions.str() },
            { "parameters", Parameters.str() },
            { "assertions", Assertions.str() },
        };

        return Record;
    }

    nlohmann::json EnumRecord(const EnumWrapper& WrappedEnum)
    {
        const UEEnum Enum = WrappedEnum.GetUnrealEnum();
        const auto [GeneratedName, bIsUniqueName] = WrappedEnum.GetUniqueName();
        const std::vector<std::pair<FName, int64>> ReflectedMembers = Enum.GetNameValuePairs();
        nlohmann::json Members = nlohmann::json::array();
        int32 Ordinal = 0;
        for (const EnumCollisionInfo& Member : WrappedEnum.GetMembers())
        {
            const int64 ReflectedValue = static_cast<size_t>(Ordinal) < ReflectedMembers.size()
                ? ReflectedMembers[Ordinal].second
                : static_cast<int64>(Member.GetValue());
            Members.push_back({
                { "ordinal", Ordinal++ },
                { "raw_name", Member.GetRawName() },
                { "generated_name", Member.GetUniqueName() },
                { "collision_count", Member.GetCollisionCount() },
                { "value", ReflectedValue },
                { "value_bits", Member.GetValue() },
            });
        }

        nlohmann::json Record = {
            { "identity", Enum.GetPathName() },
            { "path_name", Enum.GetPathName() },
            { "raw_name", Enum.GetName() },
            { "valid_name", Enum.GetValidName() },
            { "cpp_name", Enum.GetCppName() },
            { "generated_name", GeneratedName },
            { "cpp_qualified_name", CppGenerator::GetEnumPrefixedName(WrappedEnum) },
            { "generated_name_is_unique", bIsUniqueName },
            { "full_name", Enum.GetFullName() },
            { "package", ObjectReference(Enum.GetOutermost()) },
            { "outer", ObjectReference(Enum.GetOuter()) },
            { "object_flags", static_cast<uint64>(Enum.GetFlags()) },
            { "object_flags_text", Enum.StringifyObjFlags() },
            { "underlying_size", WrappedEnum.GetUnderlyingTypeSize() },
            { "underlying_signed", WrappedEnum.IsUnderlyingTypeSigned() },
            { "members", std::move(Members) },
        };

        std::ostringstream Declaration;
        CppGenerator::GenerateEnum(WrappedEnum, Declaration);
        Record["cpp"] = {
            { "declaration", Declaration.str() },
        };
        return Record;
    }

    nlohmann::json DependencyRecord(const RequirementInfo& Requirement)
    {
        return {
            { "package", ObjectReference(ObjectArray::GetByIndex(Requirement.PackageIdx)) },
            { "requires_structs", Requirement.bShouldIncludeStructs },
            { "requires_classes", Requirement.bShouldIncludeClasses },
        };
    }

    nlohmann::json DependencyRecords(const DependencyListType& Dependencies)
    {
        nlohmann::json Records = nlohmann::json::array();
        for (const auto& [_, Requirement] : Dependencies)
            Records.push_back(DependencyRecord(Requirement));
        std::sort(Records.begin(), Records.end(), [](const nlohmann::json& Left, const nlohmann::json& Right)
        {
            return Left.at("package").at("identity").get_ref<const std::string&>()
                < Right.at("package").at("identity").get_ref<const std::string&>();
        });
        return Records;
    }
}


void ReflectionIRGenerator::Capture()
{
    CapturedDocument.clear();
    strcpy_s(ReflectionIRCaptureContext, "initializing");

    nlohmann::json Packages = nlohmann::json::array();
    nlohmann::json Types = nlohmann::json::array();
    nlohmann::json Enums = nlohmann::json::array();

    for (const PackageInfoHandle Package : PackageManager::IterateOverPackageInfos())
    {
        if (Package.IsEmpty())
            continue;

        SetCaptureContext("package", Package.GetIndex());
        const UEObject PackageObject = ObjectArray::GetByIndex(Package.GetIndex());
        const auto [PackageName, PackageCollisionCount] = Package.GetNameCollisionPair();
        const DependencyInfo& Dependencies = Package.GetPackageDependencies();
        Packages.push_back({
            { "identity", PackageObject.GetPathName() },
            { "path_name", PackageObject.GetPathName() },
            { "raw_name", PackageObject.GetName() },
            { "valid_name", PackageObject.GetValidName() },
            { "generated_name", Package.GetName() },
            { "generated_name_base", PackageName },
            { "generated_name_collision_count", PackageCollisionCount },
            { "dependencies", {
                { "structs", DependencyRecords(Dependencies.StructsDependencies) },
                { "classes", DependencyRecords(Dependencies.ClassesDependencies) },
                { "parameters", DependencyRecords(Dependencies.ParametersDependencies) },
            } },
        });

        for (const int32 EnumIndex : Package.GetEnums())
        {
            SetCaptureContext("enum", EnumIndex);
            Enums.push_back(EnumRecord(EnumWrapper(ObjectArray::GetByIndex<UEEnum>(EnumIndex))));
        }

        const auto GenerateType = [&](const int32 Index)
        {
            SetCaptureContext("type", Index);
            Types.push_back(StructRecord(StructWrapper(ObjectArray::GetByIndex<UEStruct>(Index))));
        };

        if (Package.HasStructs())
            Package.GetSortedStructs().VisitAllNodesWithCallback(GenerateType);
        if (Package.HasClasses())
            Package.GetSortedClasses().VisitAllNodesWithCallback(GenerateType);
    }

    SortByIdentity(Packages);
    SortByIdentity(Types);
    SortByIdentity(Enums);

    const nlohmann::json Document = {
        { "format", "dumper-7-reflection-ir" },
        { "format_version", ReflectionIRFormatVersion },
        { "game", {
            { "name", Settings::Generator::GameName },
            { "version", Settings::Generator::GameVersion },
            { "pointer_size", sizeof(void*) },
        } },
        { "packages", std::move(Packages) },
        { "types", std::move(Types) },
        { "enums", std::move(Enums) },
    };

    CapturedDocument = Document.dump(-1, ' ', false, nlohmann::detail::error_handler_t::replace);
    strcpy_s(ReflectionIRCaptureContext, "complete");
}

bool ReflectionIRGenerator::TryCapture(CaptureFailure* Failure)
{
    __try
    {
        Capture();
        return true;
    }
    __except (HandleCaptureException(GetExceptionInformation(), Failure))
    {
        return false;
    }
}

void ReflectionIRGenerator::Generate()
{
    if (CapturedDocument.empty())
        throw std::runtime_error("Reflection IR was not captured before generation");

    std::ofstream Output(MainFolder / "ReflectionIR.json", std::ios::binary);
    if (!Output)
        throw std::runtime_error("Could not open ReflectionIR.json for writing");

    Output << CapturedDocument;
    if (!Output)
        throw std::runtime_error("Could not write ReflectionIR.json");
}
