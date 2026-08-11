#pragma once

#include "Generators/Generator.h"


class ReflectionIRGenerator
{
    friend class Generator;

public:
    struct CaptureFailure
    {
        unsigned long Code = 0;
        const void* ExceptionAddress = nullptr;
        uintptr_t AccessKind = 0;
        const void* AccessAddress = nullptr;
        char Context[512] = {};
    };

    static inline PredefinedMemberLookupMapType PredefinedMembers;

    static inline std::string MainFolderName = "ReflectionIR";
    static inline std::string SubfolderName = "";

    static inline fs::path MainFolder;
    static inline fs::path Subfolder;

public:
    static void Capture();
    static bool TryCapture(CaptureFailure* Failure);
    static void Generate();

    static void InitPredefinedMembers() { }
    static void InitPredefinedFunctions() { }

private:
    static inline std::string CapturedDocument;
};
