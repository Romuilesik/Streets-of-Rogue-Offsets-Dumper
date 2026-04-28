#include <Windows.h>
#include <ShlObj.h>
#include <fstream>
#include <string>
#include <format>
#include <vector>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <unordered_set>

using MonoDomain = void;
using MonoAssembly = void;
using MonoImage = void;
using MonoClass = void;
using MonoClassField = void;
using MonoType = void;
using MonoObject = void;
using MonoVTable = void;

using fn_mono_get_root_domain = MonoDomain * (*)();
using fn_mono_thread_attach = MonoObject * (*)(MonoDomain*);
using fn_mono_assembly_foreach = void            (*)(void(*)(MonoAssembly*, void*), void*);
using fn_mono_assembly_get_image = MonoImage * (*)(MonoAssembly*);
using fn_mono_image_get_name = const char* (*)(MonoImage*);
using fn_mono_image_get_table_info = void* (*)(MonoImage*, int);
using fn_mono_table_info_get_rows = int             (*)(void*);
using fn_mono_class_get = MonoClass * (*)(MonoImage*, uint32_t);
using fn_mono_class_get_name = const char* (*)(MonoClass*);
using fn_mono_class_get_namespace = const char* (*)(MonoClass*);
using fn_mono_class_get_fields = MonoClassField * (*)(MonoClass*, void**);
using fn_mono_class_get_parent = MonoClass * (*)(MonoClass*);
using fn_mono_class_get_nesting_type = MonoClass * (*)(MonoClass*);
using fn_mono_class_vtable = MonoVTable * (*)(MonoDomain*, MonoClass*);
using fn_mono_vtable_get_static_field_data = void* (*)(MonoVTable*);
using fn_mono_field_get_name = const char* (*)(MonoClassField*);
using fn_mono_field_get_offset = int             (*)(MonoClassField*);
using fn_mono_field_get_type = MonoType * (*)(MonoClassField*);
using fn_mono_field_get_flags = uint32_t(*)(MonoClassField*);
using fn_mono_type_get_name = char* (*)(MonoType*);

constexpr uint32_t FIELD_ATTRIBUTE_STATIC = 0x0010;
constexpr uint32_t FIELD_ATTRIBUTE_LITERAL = 0x0040;

struct MonoFuncs {
    fn_mono_get_root_domain              get_root_domain;
    fn_mono_thread_attach                thread_attach;
    fn_mono_assembly_foreach             assembly_foreach;
    fn_mono_assembly_get_image           assembly_get_image;
    fn_mono_image_get_name               image_get_name;
    fn_mono_image_get_table_info         image_get_table_info;
    fn_mono_table_info_get_rows          table_info_get_rows;
    fn_mono_class_get                    class_get;
    fn_mono_class_get_name               class_get_name;
    fn_mono_class_get_namespace          class_get_namespace;
    fn_mono_class_get_fields             class_get_fields;
    fn_mono_class_get_parent             class_get_parent;
    fn_mono_class_get_nesting_type       class_get_nesting_type;
    fn_mono_class_vtable                 class_vtable;
    fn_mono_vtable_get_static_field_data vtable_get_static_field_data;
    fn_mono_field_get_name               field_get_name;
    fn_mono_field_get_offset             field_get_offset;
    fn_mono_field_get_type               field_get_type;
    fn_mono_field_get_flags              field_get_flags;
    fn_mono_type_get_name                type_get_name;
};

struct FieldEntry {
    std::string name;
    std::string type;
    int         offset;
    bool        is_static;
    bool        from_base;
    std::string base_class_name;
};

struct ClassEntry {
    std::string             ns;
    std::string             name;
    std::string             nesting_parent;
    std::vector<FieldEntry> fields;
};

struct DumpContext {
    MonoFuncs* funcs;
    MonoDomain* domain;
    std::vector<ClassEntry>* classes;
};

std::string GetUtcTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1'000'000;
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_s(&tm, &t);
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S")
        << "." << std::setw(6) << std::setfill('0') << us.count() << " UTC";
    return ss.str();
}

std::string EscapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')  out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else                out += c;
    }
    return out;
}

std::string SafeIdent(const std::string& s, bool upper = false) {
    std::string out;
    for (char c : s) {
        bool special = (c == ' ' || c == '.' || c == '<' || c == '>'
            || c == ',' || c == '`' || c == '[' || c == ']'
            || c == '+' || c == '/');
        out += special ? '_' : (upper ? (char)toupper(c) : c);
    }
    return out;
}

void CollectFields(MonoFuncs& fn, MonoDomain* domain, MonoClass* klass,
    std::vector<FieldEntry>& out, bool from_base,
    const std::string& base_name,
    std::unordered_set<std::string>& seen) {
    void* iter = nullptr;
    while (MonoClassField* field = fn.class_get_fields(klass, &iter)) {
        uint32_t flags = fn.field_get_flags ? fn.field_get_flags(field) : 0;

        if (flags & FIELD_ATTRIBUTE_LITERAL) continue;

        const char* fname = fn.field_get_name(field);
        MonoType* ftype = fn.field_get_type(field);
        char* tname = ftype ? fn.type_get_name(ftype) : nullptr;

        std::string name_str = fname ? fname : "?";
        std::string type_str = tname ? tname : "?";
        bool        is_static = (flags & FIELD_ATTRIBUTE_STATIC) != 0;

        int offset = fn.field_get_offset(field);
        if (!is_static && offset <= 0) continue;

        std::string dedup_key = (is_static ? "s:" : "i:") + name_str;
        if (seen.contains(dedup_key)) continue;
        seen.insert(dedup_key);

        out.push_back({ name_str, type_str, offset, is_static, from_base,
                        from_base ? base_name : "" });
    }
}

void OnAssembly(MonoAssembly* assembly, void* userdata) {
    auto* ctx = static_cast<DumpContext*>(userdata);
    auto& fn = *ctx->funcs;

    MonoImage* image = fn.assembly_get_image(assembly);
    if (!image) return;

    const char* asmName = fn.image_get_name(image);
    if (!asmName || strcmp(asmName, "Assembly-CSharp") != 0) return;

    constexpr int MONO_TABLE_TYPEDEF = 2;
    void* tableInfo = fn.image_get_table_info(image, MONO_TABLE_TYPEDEF);
    if (!tableInfo) return;

    int rowCount = fn.table_info_get_rows(tableInfo);

    for (int i = 1; i <= rowCount; ++i) {
        uint32_t   token = (MONO_TABLE_TYPEDEF << 24) | i;
        MonoClass* klass = fn.class_get(image, token);
        if (!klass) continue;

        const char* ns = fn.class_get_namespace(klass);
        const char* name = fn.class_get_name(klass);

        ClassEntry cls;
        cls.ns = ns ? ns : "";
        cls.name = name ? name : "?";

        if (fn.class_get_nesting_type) {
            MonoClass* nesting = fn.class_get_nesting_type(klass);
            if (nesting) {
                const char* pname = fn.class_get_name(nesting);
                const char* pns = fn.class_get_namespace(nesting);
                cls.nesting_parent = (pns && pns[0])
                    ? std::string(pns) + "." + (pname ? pname : "?")
                    : (pname ? pname : "?");
            }
        }

        std::unordered_set<std::string> seen;

        CollectFields(fn, ctx->domain, klass, cls.fields, false, "", seen);

        if (fn.class_get_parent) {
            MonoClass* base = fn.class_get_parent(klass);
            while (base) {
                const char* bname = fn.class_get_name(base);
                const char* bns = fn.class_get_namespace(base);
                std::string base_full = (bns && bns[0])
                    ? std::string(bns) + "." + (bname ? bname : "?")
                    : (bname ? bname : "?");
                if (base_full == "System.Object" || base_full == "System.ValueType") break;
                CollectFields(fn, ctx->domain, base, cls.fields, true, base_full, seen);
                base = fn.class_get_parent(base);
            }
        }

        if (!cls.fields.empty())
            ctx->classes->push_back(std::move(cls));
    }
}

void WriteTxt(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\sor_dump.txt");
    out << "// Generated using https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\n";
    out << "// " << ts << "\n\n";
    out << "SoR Mono Full Dump\n==================\n\nAssembly: Assembly-CSharp\n\n";
    for (auto& cls : classes) {
        if (!cls.nesting_parent.empty())
            out << std::format("Class: {}.{} [nested in: {}]\n", cls.ns, cls.name, cls.nesting_parent);
        else
            out << std::format("Class: {}.{}\n", cls.ns, cls.name);
        for (auto& f : cls.fields) {
            std::string tag = f.is_static ? "[static] " : "";
            std::string origin = f.from_base ? std::format(" (from {})", f.base_class_name) : "";
            out << std::format("  [{:#06x}] {}{} : {}{}\n", f.offset, tag, f.name, f.type, origin);
        }
        out << "\n";
    }
}

void WriteJson(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\sor_dump.json");
    out << "{\n";
    out << "  \"generator\": \"https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\",\n";
    out << "  \"generated_at\": \"" << ts << "\",\n";
    out << "  \"assembly\": \"Assembly-CSharp\",\n";
    out << "  \"classes\": [\n";
    for (size_t ci = 0; ci < classes.size(); ++ci) {
        auto& cls = classes[ci];
        out << "    {\n";
        out << "      \"namespace\": \"" << EscapeJson(cls.ns) << "\",\n";
        out << "      \"name\": \"" << EscapeJson(cls.name) << "\"";
        if (!cls.nesting_parent.empty())
            out << ",\n      \"nested_in\": \"" << EscapeJson(cls.nesting_parent) << "\"";
        out << ",\n      \"fields\": [\n";
        for (size_t fi = 0; fi < cls.fields.size(); ++fi) {
            auto& f = cls.fields[fi];
            out << std::format(
                "        {{ \"offset\": {}, \"name\": \"{}\", \"type\": \"{}\","
                " \"static\": {}, \"from_base\": \"{}\" }}",
                f.offset, EscapeJson(f.name), EscapeJson(f.type),
                f.is_static ? "true" : "false",
                EscapeJson(f.base_class_name));
            if (fi + 1 < cls.fields.size()) out << ",";
            out << "\n";
        }
        out << "      ]\n    }";
        if (ci + 1 < classes.size()) out << ",";
        out << "\n";
    }
    out << "  ]\n}\n";
}

void WriteHpp(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\sor_offsets.hpp");
    out << "// Generated using https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\n";
    out << "// " << ts << "\n\n#pragma once\n#include <cstdint>\n\n";
    for (auto& cls : classes) {
        std::string ns_name = cls.ns.empty() ? cls.name : cls.ns + "_" + cls.name;
        out << "namespace " << SafeIdent(ns_name, true) << " {\n";
        if (!cls.nesting_parent.empty())
            out << "    // nested in: " << cls.nesting_parent << "\n";
        for (auto& f : cls.fields) {
            std::string ident = SafeIdent(f.name) + (f.is_static ? "_static" : "");
            std::string comment = f.from_base ? std::format(" // from {}", f.base_class_name) : "";
            out << std::format("    constexpr std::ptrdiff_t {} = {:#x};{}\n", ident, f.offset, comment);
        }
        out << "}\n\n";
    }
}

void WriteZig(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\sor_offsets.zig");
    out << "// Generated using https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\n";
    out << "// " << ts << "\n\n";
    for (auto& cls : classes) {
        std::string ns_name = cls.ns.empty() ? cls.name : cls.ns + "_" + cls.name;
        out << "pub const " << SafeIdent(ns_name) << " = struct {\n";
        if (!cls.nesting_parent.empty())
            out << "    // nested in: " << cls.nesting_parent << "\n";
        for (auto& f : cls.fields) {
            std::string ident = SafeIdent(f.name) + (f.is_static ? "_static" : "");
            std::string comment = f.from_base ? std::format(" // from {}", f.base_class_name) : "";
            out << std::format("    pub const {} = 0x{:x};{}\n", ident, f.offset, comment);
        }
        out << "};\n\n";
    }
}

void WriteRs(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\sor_offsets.rs");
    out << "// Generated using https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\n";
    out << "// " << ts << "\n\n";
    for (auto& cls : classes) {
        std::string ns_name = cls.ns.empty() ? cls.name : cls.ns + "_" + cls.name;
        out << "#[allow(dead_code, non_upper_case_globals)]\npub mod " << SafeIdent(ns_name) << " {\n";
        if (!cls.nesting_parent.empty())
            out << "    // nested in: " << cls.nesting_parent << "\n";
        for (auto& f : cls.fields) {
            std::string ident = SafeIdent(f.name, true) + (f.is_static ? "_STATIC" : "");
            std::string comment = f.from_base ? std::format(" // from {}", f.base_class_name) : "";
            out << std::format("    pub const {}: usize = 0x{:x};{}\n", ident, f.offset, comment);
        }
        out << "}\n\n";
    }
}

void WriteCs(const std::wstring& dir, const std::vector<ClassEntry>& classes, const std::string& ts) {
    std::ofstream out(dir + L"\\SorOffsets.cs");
    out << "// Generated using https://github.com/Romuilesik/Streets-of-Rogue-Offsets-Dumper\n";
    out << "// " << ts << "\n\nnamespace SorOffsets\n{\n";
    for (auto& cls : classes) {
        std::string ns_name = cls.ns.empty() ? cls.name : cls.ns + "_" + cls.name;
        out << "    public static class " << SafeIdent(ns_name) << "\n    {\n";
        if (!cls.nesting_parent.empty())
            out << "        // nested in: " << cls.nesting_parent << "\n";
        for (auto& f : cls.fields) {
            std::string ident = SafeIdent(f.name) + (f.is_static ? "_Static" : "");
            std::string comment = f.from_base ? std::format(" // from {}", f.base_class_name) : "";
            out << std::format("        public const int {} = 0x{:x};{}\n", ident, f.offset, comment);
        }
        out << "    }\n\n";
    }
    out << "}\n";
}

DWORD WINAPI DumpThread(LPVOID) {
    HMODULE hMono = nullptr;
    for (int i = 0; i < 100 && !hMono; ++i) {
        hMono = GetModuleHandleA("mono-2.0-bdwgc.dll");
        if (!hMono) hMono = GetModuleHandleA("mono.dll");
        if (!hMono) Sleep(100);
    }
    if (!hMono) return 1;

    auto getfn = [&]<typename T>(T & dst, const char* sym) {
        dst = reinterpret_cast<T>(GetProcAddress(hMono, sym));
    };

    MonoFuncs fn{};
    getfn(fn.get_root_domain, "mono_get_root_domain");
    getfn(fn.thread_attach, "mono_thread_attach");
    getfn(fn.assembly_foreach, "mono_assembly_foreach");
    getfn(fn.assembly_get_image, "mono_assembly_get_image");
    getfn(fn.image_get_name, "mono_image_get_name");
    getfn(fn.image_get_table_info, "mono_image_get_table_info");
    getfn(fn.table_info_get_rows, "mono_table_info_get_rows");
    getfn(fn.class_get, "mono_class_get");
    getfn(fn.class_get_name, "mono_class_get_name");
    getfn(fn.class_get_namespace, "mono_class_get_namespace");
    getfn(fn.class_get_fields, "mono_class_get_fields");
    getfn(fn.class_get_parent, "mono_class_get_parent");
    getfn(fn.class_get_nesting_type, "mono_class_get_nesting_type");
    getfn(fn.class_vtable, "mono_class_vtable");
    getfn(fn.vtable_get_static_field_data, "mono_vtable_get_static_field_data");
    getfn(fn.field_get_name, "mono_field_get_name");
    getfn(fn.field_get_offset, "mono_field_get_offset");
    getfn(fn.field_get_type, "mono_field_get_type");
    getfn(fn.field_get_flags, "mono_field_get_flags");
    getfn(fn.type_get_name, "mono_type_get_name");

    if (!fn.get_root_domain || !fn.thread_attach || !fn.assembly_foreach) return 2;

    MonoDomain* domain = fn.get_root_domain();
    fn.thread_attach(domain);

    wchar_t downloadsPath[MAX_PATH];
    ExpandEnvironmentStringsW(L"%USERPROFILE%\\Downloads\\SOR Output", downloadsPath, MAX_PATH);
    CreateDirectoryW(downloadsPath, nullptr);
    std::wstring outDir = downloadsPath;

    std::string             ts = GetUtcTimestamp();
    std::vector<ClassEntry> classes;

    DumpContext ctx{ &fn, domain, &classes };
    fn.assembly_foreach(OnAssembly, &ctx);

    WriteTxt(outDir, classes, ts);
    WriteJson(outDir, classes, ts);
    WriteHpp(outDir, classes, ts);
    WriteZig(outDir, classes, ts);
    WriteRs(outDir, classes, ts);
    WriteCs(outDir, classes, ts);

    MessageBoxW(nullptr,
        (L"Done! Saved to:\n" + outDir + L"\n\n"
            L"sor_dump.txt\n"
            L"sor_dump.json\n"
            L"sor_offsets.hpp\n"
            L"sor_offsets.zig\n"
            L"sor_offsets.rs\n"
            L"SorOffsets.cs").c_str(),
        L"SoR Dumper", MB_OK);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hModule);
        CreateThread(nullptr, 0, DumpThread, nullptr, 0, nullptr);
    }
    return TRUE;
}