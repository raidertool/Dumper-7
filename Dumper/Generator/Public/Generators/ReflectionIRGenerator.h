#pragma once

#include "Generators/Generator.h"


class ReflectionIRGenerator
{
    friend class Generator;

public:
    static inline PredefinedMemberLookupMapType PredefinedMembers;

    static inline std::string MainFolderName = "ReflectionIR";
    static inline std::string SubfolderName = "";

    static inline fs::path MainFolder;
    static inline fs::path Subfolder;

public:
    static void Capture();
    static void Generate();

    static void InitPredefinedMembers() { }
    static void InitPredefinedFunctions() { }

private:
    static inline std::string CapturedDocument;
};
