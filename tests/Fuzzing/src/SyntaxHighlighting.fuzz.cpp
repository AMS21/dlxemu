#include <DLXEmu/Emulator.hpp>
#include <phi/compiler_support/warning.hpp>
#include <cstddef>
#include <cstdint>
#include <string_view>

PHI_CLANG_SUPPRESS_WARNING("-Wexit-time-destructors")

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static dlxemu::Emulator emu;

    std::string_view source = std::string_view(reinterpret_cast<const char*>(data), size);

    dlxemu::CodeEditor& editor = emu.GetEditor();

    // Parse it
    editor.SetText(std::string(source.data(), source.size()));
    editor.m_FullText = editor.GetText();

    emu.ParseProgram(editor.m_FullText);

    editor.ColorizeInternal();

    return 0;
}
