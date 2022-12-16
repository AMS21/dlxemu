#include <DLX/Logger.hpp>
#include <DLXEmu/CodeEditor.hpp>
#include <DLXEmu/Emulator.hpp>
#include <fmt/core.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <phi/compiler_support/assume.hpp>
#include <phi/compiler_support/unused.hpp>
#include <phi/compiler_support/warning.hpp>
#include <phi/core/assert.hpp>
#include <phi/core/boolean.hpp>
#include <phi/core/optional.hpp>
#include <phi/core/scope_guard.hpp>
#include <phi/core/sized_types.hpp>
#include <phi/core/types.hpp>
#include <phi/math/abs.hpp>
#include <phi/math/is_nan.hpp>
#include <phi/preprocessor/function_like_macro.hpp>
#include <phi/type_traits/make_unsigned.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(FUZZ_VERBOSE_LOG)
#    define FUZZ_LOG(...)                                                                          \
        fmt::print(stderr, __VA_ARGS__);                                                           \
        std::putc('\n', stderr);                                                                   \
        std::fflush(stderr)
#else
#    define FUZZ_LOG(...) PHI_EMPTY_MACRO()
#endif

PHI_CLANG_SUPPRESS_WARNING("-Wexit-time-destructors")
PHI_CLANG_SUPPRESS_WARNING("-Wglobal-constructors")

// TODO: Use string_view as much as possible so we avoid needless copies

// Limits
static constexpr const phi::size_t MaxVectorSize{8u};
static constexpr const phi::size_t MaxStringLength{16u};
static constexpr const float       MaxSaneFloatValue{1024.0f};

struct Cache
{
    using svec = std::vector<std::string>;

    svec vector_string[MaxVectorSize];

    std::string string;

    static Cache Initialize() noexcept
    {
        Cache c;

        // Resize vector args
        for (phi::usize i{0u}; i < MaxVectorSize; ++i)
        {
            svec& vector = c.vector_string[i.unsafe()];
            vector.resize(i.unsafe());

            // Reserve max size
            for (std::string& str : vector)
            {
                str.reserve(MaxStringLength);
            }
        }

        c.string.reserve(MaxStringLength);

        return c;
    }
};

static Cache cache = Cache::Initialize();

[[nodiscard]] constexpr bool has_x_more(const std::size_t index, const std::size_t x,
                                        const std::size_t size) noexcept
{
    return index + x < size;
}

template <typename T>
[[nodiscard]] constexpr std::size_t aligned_size() noexcept
{
    return sizeof(T) + (sizeof(void*) - sizeof(T));
}

template <typename T>
[[nodiscard]] phi::optional<T> consume_t(const std::uint8_t* data, const std::size_t size,
                                         std::size_t& index) noexcept
{
    if (!has_x_more(index, sizeof(T), size))
    {
        return {};
    }

    PHI_ASSUME(index % sizeof(void*) == 0);

    const phi::size_t old_index = index;
    index += aligned_size<T>();

    if constexpr (phi::is_bool_v<T>)
    {
        phi::int8_t value = *reinterpret_cast<const phi::int8_t*>(data + old_index);
        return static_cast<bool>(value);
    }
    else
    {
        return *reinterpret_cast<const T*>(data + old_index);
    }
}

[[nodiscard]] bool consume_string(const std::uint8_t* data, const std::size_t size,
                                  std::size_t& index) noexcept
{
    // Ensure we're not already past the available data
    if (index >= size)
    {
        return false;
    }

    const char* str_begin = reinterpret_cast<const char*>(data + index);
    phi::size_t str_len   = 0u;

    while (index < size && data[index] != '\0')
    {
        ++index;
        ++str_len;
    }

    // Reject too long strings
    if (str_len > MaxStringLength)
    {
        return false;
    }

    PHI_ASSERT(index <= size);
    // Reject strings that are not null terminated
    if (data[index - 1u] != '\0')
    {
        return false;
    }

    // Move back to proper alignment
    index += (sizeof(void*) - (index % sizeof(void*)));

    // Assign string value to cache
    cache.string = str_begin;

    return true;
}

// Returns an index into cache.vector_string
[[nodiscard]] phi::optional<phi::size_t> consume_vector_string(const std::uint8_t* data,
                                                               const std::size_t   size,
                                                               std::size_t&        index) noexcept
{
    auto number_of_lines_opt = consume_t<phi::size_t>(data, size, index);
    if (!number_of_lines_opt)
    {
        return {};
    }

    const std::size_t number_of_lines = number_of_lines_opt.value();
    if (number_of_lines >= MaxVectorSize)
    {
        return {};
    }

    std::vector<std::string>& res = cache.vector_string[number_of_lines];
    for (std::size_t i{0u}; i < number_of_lines; ++i)
    {
        if (!consume_string(data, size, index))
        {
            return {};
        }

        res[i] = cache.string;
    }

    return number_of_lines;
}

[[nodiscard]] phi::optional<dlxemu::CodeEditor::Coordinates> consume_coordinates(
        const std::uint8_t* data, const std::size_t size, std::size_t& index) noexcept
{
    auto column_opt = consume_t<std::uint32_t>(data, size, index);
    if (!column_opt)
    {
        return {};
    }
    std::uint32_t column = column_opt.value();

    auto line_opt = consume_t<std::uint32_t>(data, size, index);
    if (!line_opt)
    {
        return {};
    }
    std::uint32_t line = line_opt.value();

    return dlxemu::CodeEditor::Coordinates{column, line};
}

template <typename T>
[[nodiscard]] std::string print_int(const T val) noexcept
{
    return fmt::format("{0:d} 0x{1:02X}", val, static_cast<phi::make_unsigned_t<T>>(val));
}

template <typename T>
[[nodiscard]] std::string pretty_char(const T c) noexcept
{
    switch (c)
    {
        case '\n':
            return "\\n";
        case '\0':
            return "\\0";
        case '\t':
            return "\\t";
        case '\r':
            return "\\r";
        case '\a':
            return "\\a";
        case '\v':
            return "\\v";
        case '"':
            return "\\\"";
        case '\b':
            return "\\b";
        case '\f':
            return "\\f";

        default:
            return {1u, static_cast<const char>(c)};
    }
}

[[nodiscard]] std::string print_string(const std::string& str) noexcept
{
    std::string hex_str;
    std::string print_str;

    for (char character : str)
    {
        hex_str += fmt::format("\\0x{:02X}, ", static_cast<std::uint8_t>(character));

        // Make some special characters printable
        print_str += pretty_char(character);
    }

    return fmt::format("String(\"{:s}\" size: {:d} ({:s}))", print_str, str.size(),
                       hex_str.substr(0, hex_str.size() - 2));
}

[[nodiscard]] std::string print_char(const ImWchar character) noexcept
{
    return fmt::format(R"(ImWchar("{:s}" (\0x{:02X})))", pretty_char(character),
                       static_cast<std::uint32_t>(character));
}

[[nodiscard]] std::string print_vector_string(const std::vector<std::string>& vec) noexcept
{
    std::string ret;

    ret += fmt::format("Vector(size: {:d}):\n", vec.size());

    for (const std::string& str : vec)
    {
        ret += print_string(str) + '\n';
    }

    return ret;
}

[[nodiscard]] std::string print_error_markers(
        const dlxemu::CodeEditor::ErrorMarkers& markers) noexcept
{
    std::string ret;

    ret += fmt::format("ErrorMarkers(size: {:d}):\n", markers.size());

    for (const auto& val : markers)
    {
        ret += fmt::format("{:s}: {:s}\n", print_int(val.first), print_string(val.second));
    }

    return ret;
}

[[nodiscard]] std::string print_breakpoints(
        const dlxemu::CodeEditor::Breakpoints& breakpoints) noexcept
{
    std::string lines;

    for (const phi::u32 line_number : breakpoints)
    {
        lines += fmt::format("{:s}, ", print_int(line_number.unsafe()));
    }

    std::string ret = fmt::format("Breakpoints(size: {:d}: {:s})", breakpoints.size(),
                                  lines.substr(0, lines.size() - 2));

    return ret.substr(0, ret.size());
}

[[nodiscard]] const char* print_bool(const phi::boolean boolean) noexcept
{
    return boolean ? "true" : "false";
}

bool SetupImGui() noexcept
{
    IMGUI_CHECKVERSION();
    if (GImGui != nullptr)
    {
        return true;
    }

    if (ImGui::CreateContext() == nullptr)
    {
        FUZZ_LOG("Failed to create ImGuiContext");
        return false;
    }

    // Set config
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard; // Enable Keyboard Controls
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;  // Enable Gamepad Controls
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;     // Enable Docking
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable; // Enable Multi-Viewport / Platform Windows

    // Enforce valid display size
    io.DisplaySize.x = 1024.0f;
    io.DisplaySize.y = 768.0f;

    // Enfore valid DeltaTime
    io.DeltaTime = 1.0f / 60.0f;

    // Don't save any config
    io.IniFilename = nullptr;

    // SetStyle
    ImGuiStyle& style = ImGui::GetStyle();
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        style.WindowRounding              = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Build atlas
    unsigned char* tex_pixels{nullptr};
    int            tex_w;
    int            tex_h;
    io.Fonts->GetTexDataAsRGBA32(&tex_pixels, &tex_w, &tex_h);

    return true;
}

void EndImGui() noexcept
{
    ImGui::Render();

    volatile ImDrawData* draw_data = ImGui::GetDrawData();
    PHI_UNUSED_VARIABLE(draw_data);

    ImGuiIO& io = ImGui::GetIO();
    // Update and Render additional Platform Windows
    // (Platform functions may change the current OpenGL context, so we save/restore it to make it easier to paste this code elsewhere.
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
    {
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
    }

    ImGui::EndFrame();

    // Ensure frame count doesn't overflow
    GImGui->FrameCount %= 16384;
}

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    static bool imgui_init = SetupImGui();
    (void)imgui_init;

    // Reset some ImGui states
    ImGui::GetIO().ClearInputCharacters();
    ImGui::GetIO().ClearInputKeys();
    ImGui::GetIO().InputQueueSurrogate = 0;
    GImGui->InputEventsQueue.resize(0u);
    ImGui::FocusWindow(nullptr);

    dlxemu::Emulator   emulator;
    dlxemu::CodeEditor editor{&emulator};

    editor.UpdatePalette();

    FUZZ_LOG("Beginning execution");

    for (std::size_t index{0u}; index < size;)
    {
        auto function_index_opt = consume_t<std::uint32_t>(data, size, index);
        if (!function_index_opt)
        {
            return 0;
        }

        const std::uint32_t function_index = function_index_opt.value();

        switch (function_index)
        {
            // AddErrorMarker
            case 0: {
                auto line_number_opt = consume_t<phi::uint32_t>(data, size, index);
                if (!line_number_opt)
                {
                    return 0;
                }
                std::uint32_t line_number = line_number_opt.value();

                if (!consume_string(data, size, index))
                {
                    return 0;
                }
                std::string& message = cache.string;

                FUZZ_LOG("AddErrorMarker({:s}, {:s})", print_int(line_number),
                         print_string(message));

                editor.AddErrorMarker(line_number, message);
                break;
            }

            // ClearErrorMarkers
            case 1: {
                FUZZ_LOG("ClearErrorMarkers");

                editor.ClearErrorMarkers();
                break;
            }

            // SetText
            case 2: {
                if (!consume_string(data, size, index))
                {
                    return 0;
                }
                std::string& text = cache.string;

                FUZZ_LOG("SetText({:s})", print_string(text));

                editor.SetText(text);
                break;
            }

            // GetText
            case 3: {
                FUZZ_LOG("GetText()");

                volatile std::string str = editor.GetText();
                PHI_UNUSED_VARIABLE(str);
                break;
            }

            // SetTextLines
            case 4: {
                auto lines_opt = consume_vector_string(data, size, index);
                if (!lines_opt)
                {
                    return 0;
                }

                const std::vector<std::string>& lines = cache.vector_string[lines_opt.value()];

                FUZZ_LOG("SetTextLines({:s})", print_vector_string(lines));

                editor.SetTextLines(lines);
                break;
            }

            // GetTextLines
            case 5: {
                FUZZ_LOG("GetTextLines()");

                volatile std::vector<std::string> lines = editor.GetTextLines();
                PHI_UNUSED_VARIABLE(lines);
                break;
            }

            // GetSelectedText
            case 6: {
                FUZZ_LOG("GetSelectedText()");

                volatile std::string line = editor.GetSelectedText();
                PHI_UNUSED_VARIABLE(line);
                break;
            }

            // GetCurrentLineText
            case 7: {
                FUZZ_LOG("GetCurrentLineText()");

                volatile std::string line = editor.GetCurrentLineText();
                PHI_UNUSED_VARIABLE(line);
                break;
            }

            // SetReadOnly
            case 8: {
                auto read_only_opt = consume_t<bool>(data, size, index);
                if (!read_only_opt)
                {
                    return 0;
                }
                bool read_only = read_only_opt.value();

                FUZZ_LOG("SetReadOnly({:s})", print_bool(read_only));

                editor.SetReadOnly(read_only);
                break;
            }

            // GetCursorPosition
            case 9: {
                FUZZ_LOG("GetCursorPosition()");

                volatile dlxemu::CodeEditor::Coordinates coords = editor.GetCursorPosition();
                PHI_UNUSED_VARIABLE(coords);
                break;
            }

            // SetCursorPosition
            case 10: {
                auto coords_opt = consume_coordinates(data, size, index);
                if (!coords_opt)
                {
                    return 0;
                }

                auto coords = coords_opt.value();

                FUZZ_LOG("SetCursorPosition(Coordinates({:s}, {:s}))",
                         print_int(coords.m_Line.unsafe()), print_int(coords.m_Column.unsafe()));

                editor.SetCursorPosition(coords);
                break;
            }

            // SetShowWhitespaces
            case 11: {
                auto show_whitespace_opt = consume_t<bool>(data, size, index);
                if (!show_whitespace_opt)
                {
                    return 0;
                }
                bool show_whitespaces = show_whitespace_opt.value();

                FUZZ_LOG("SetShowShitespaces({:s})", print_bool(show_whitespaces));

                editor.SetShowWhitespaces(show_whitespaces);
                break;
            }

            // SetTabSize
            case 12: {
                auto tab_size_opt = consume_t<std::uint_fast8_t>(data, size, index);
                if (!tab_size_opt)
                {
                    return 0;
                }
                std::uint_fast8_t tab_size = tab_size_opt.value();

                FUZZ_LOG("SetTabSize({:s})", print_int(tab_size));

                editor.SetTabSize(tab_size);
                break;
            }

            // InsertText
            case 13: {
                if (!consume_string(data, size, index))
                {
                    return 0;
                }

                std::string& message = cache.string;

                FUZZ_LOG("InsertText({:s})", print_string(message));

                editor.InsertText(message);
                break;
            }

            // MoveUp
            case 14: {
                auto amount_opt = consume_t<std::uint32_t>(data, size, index);
                if (!amount_opt)
                {
                    return 0;
                }
                std::uint32_t amount = amount_opt.value();

                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveUp({:s}, {:s})", print_int(amount), print_bool(select));

                editor.MoveUp(amount, select);
                break;
            }

            // MoveDown
            case 15: {
                auto amount_opt = consume_t<std::uint32_t>(data, size, index);
                if (!amount_opt)
                {
                    return 0;
                }
                std::uint32_t amount = amount_opt.value();

                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveDown({:s}, {:s})", print_int(amount), print_bool(select));

                editor.MoveDown(amount, select);
                break;
            }

            // MoveLeft
            case 16: {
                auto amount_opt = consume_t<std::uint32_t>(data, size, index);
                if (!amount_opt)
                {
                    return 0;
                }
                std::uint32_t amount = amount_opt.value();

                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                auto word_mode_opt = consume_t<bool>(data, size, index);
                if (!word_mode_opt)
                {
                    return 0;
                }
                bool word_mode = word_mode_opt.value();

                FUZZ_LOG("MoveLeft({:s}, {:s}, {:s})", print_int(amount), print_bool(select),
                         print_bool(word_mode));

                editor.MoveLeft(amount, select, word_mode);
                break;
            }

            // MoveRight
            case 17: {
                auto amount_opt = consume_t<std::uint32_t>(data, size, index);
                if (!amount_opt)
                {
                    return 0;
                }
                std::uint32_t amount = amount_opt.value();

                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                auto word_mode_opt = consume_t<bool>(data, size, index);
                if (!word_mode_opt)
                {
                    return 0;
                }
                bool word_mode = word_mode_opt.value();

                FUZZ_LOG("MoveRight({:s}, {:s}, {:s})", print_int(amount), print_bool(select),
                         print_bool(word_mode));

                editor.MoveRight(amount, select, word_mode);
                break;
            }

            // MoveTop
            case 18: {
                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveTop({:s})", print_bool(select));

                editor.MoveTop(select);
                break;
            }

            // MoveBottom
            case 19: {
                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveBottom({:s})", print_bool(select));

                editor.MoveBottom(select);
                break;
            }

            // MoveHome
            case 20: {
                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveHome({:s})", print_bool(select));

                editor.MoveHome(select);
                break;
            }

            // MoveEnd
            case 21: {
                auto select_opt = consume_t<bool>(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                FUZZ_LOG("MoveEnd({:s})", print_bool(select));

                editor.MoveEnd(select);
                break;
            }

            // SetSelectionStart
            case 22: {
                auto column_opt = consume_t<std::uint32_t>(data, size, index);
                if (!column_opt)
                {
                    return 0;
                }
                std::uint32_t column = column_opt.value();

                auto line_opt = consume_t<std::uint32_t>(data, size, index);
                if (!line_opt)
                {
                    return 0;
                }
                std::uint32_t line = line_opt.value();

                dlxemu::CodeEditor::Coordinates coord;
                coord.m_Column = column;
                coord.m_Line   = line;

                FUZZ_LOG("SetSelectionStart(Coordinates({:s}, {:s}))", print_int(line),
                         print_int(column));

                editor.SetSelectionStart(coord);
                break;
            }

            // SetSelectionEnd
            case 23: {
                auto column_opt = consume_t<std::uint32_t>(data, size, index);
                if (!column_opt)
                {
                    return 0;
                }
                std::uint32_t column = column_opt.value();

                auto line_opt = consume_t<std::uint32_t>(data, size, index);
                if (!line_opt)
                {
                    return 0;
                }
                std::uint32_t line = line_opt.value();

                dlxemu::CodeEditor::Coordinates coord;
                coord.m_Column = column;
                coord.m_Line   = line;

                FUZZ_LOG("SetSelectionEnd(Coordinates({:s}, {:s}))", print_int(line),
                         print_int(column));

                editor.SetSelectionEnd(coord);
                break;
            }

            // SetSelection
            case 24: {
                auto column_start_opt = consume_t<std::uint32_t>(data, size, index);
                if (!column_start_opt)
                {
                    return 0;
                }
                std::uint32_t column_start = column_start_opt.value();

                auto line_start_opt = consume_t<std::uint32_t>(data, size, index);
                if (!line_start_opt)
                {
                    return 0;
                }
                std::uint32_t line_start = line_start_opt.value();

                dlxemu::CodeEditor::Coordinates coord_start;
                coord_start.m_Column = column_start;
                coord_start.m_Line   = line_start;

                auto column_end_opt = consume_t<std::uint32_t>(data, size, index);
                if (!column_end_opt)
                {
                    return 0;
                }
                std::uint32_t column_end = column_end_opt.value();

                auto line_end_opt = consume_t<std::uint32_t>(data, size, index);
                if (!line_end_opt)
                {
                    return 0;
                }
                std::uint32_t line_end = line_end_opt.value();

                dlxemu::CodeEditor::Coordinates coord_end;
                coord_end.m_Column = column_end;
                coord_end.m_Line   = line_end;

                auto selection_mode_opt = consume_t<std::uint8_t>(data, size, index);
                if (!selection_mode_opt || selection_mode_opt.value() > 2u)
                {
                    return 0;
                }
                dlxemu::CodeEditor::SelectionMode selection_mode =
                        static_cast<dlxemu::CodeEditor::SelectionMode>(selection_mode_opt.value());

                FUZZ_LOG("SetSelection(Coordinates({:s}, {:s}), Coordinates({:s}, "
                         "{:s}), {:s})",
                         print_int(line_start), print_int(column_start), print_int(line_end),
                         print_int(column_end), dlx::enum_name(selection_mode));

                editor.SetSelection(coord_start, coord_end, selection_mode);
                break;
            }

            // SelectWordUnderCursor
            case 25: {
                FUZZ_LOG("SelectWordUnderCursor");

                editor.SelectWordUnderCursor();
                break;
            }

            // SelectAll
            case 26: {
                FUZZ_LOG("SelectAll");

                editor.SelectAll();
                break;
            }

            // Delete
            case 27: {
                FUZZ_LOG("Delete");

                editor.Delete();
                break;
            }

            // Undo
            case 28: {
                FUZZ_LOG("Undo()");

                editor.Undo();
                break;
            }

            // Redo
            case 29: {
                FUZZ_LOG("Redo()");

                editor.Redo();
                break;
            }

            // SetErrorMarkers
            case 30: {
                auto count_opt = consume_t<std::size_t>(data, size, index);
                if (!count_opt)
                {
                    return 0;
                }
                std::size_t count = std::min(count_opt.value(), MaxVectorSize);

                dlxemu::CodeEditor::ErrorMarkers markers;
                for (std::size_t i{0u}; i < count; ++i)
                {
                    auto line_number_opt = consume_t<std::uint32_t>(data, size, index);
                    if (!line_number_opt)
                    {
                        return 0;
                    }
                    std::uint32_t line_number = line_number_opt.value();

                    if (!consume_string(data, size, index))
                    {
                        return 0;
                    }
                    std::string& message = cache.string;

                    // Add to error markers
                    markers[line_number] = message;
                }

                FUZZ_LOG("SetErrorMarkers({:s})", print_error_markers(markers));

                editor.SetErrorMarkers(markers);
                break;
            }

            // SetBreakpoints
            case 31: {
                auto count_opt = consume_t<std::size_t>(data, size, index);
                if (!count_opt)
                {
                    return 0;
                }
                std::size_t count = std::min(count_opt.value(), MaxVectorSize);

                dlxemu::CodeEditor::Breakpoints breakpoints;

                for (std::size_t i{0u}; i < count; ++i)
                {
                    auto line_number_opt = consume_t<std::uint32_t>(data, size, index);
                    if (!line_number_opt)
                    {
                        return 0;
                    }
                    std::uint32_t line_number = line_number_opt.value();

                    breakpoints.insert(line_number);
                }

                FUZZ_LOG("SetBreakpoints({:s})", print_breakpoints(breakpoints));

                editor.SetBreakpoints(breakpoints);
                break;
            }

            // Render
            case 32: {
                auto x_opt = consume_t<float>(data, size, index);
                if (!x_opt)
                {
                    return 0;
                }
                const float x = x_opt.value();

                if (x >= MaxSaneFloatValue || x < 0.0f || phi::is_nan(x))
                {
                    return 0;
                }

                auto y_opt = consume_t<float>(data, size, index);
                if (!y_opt)
                {
                    return 0;
                }
                const float y = y_opt.value();

                if (y >= MaxSaneFloatValue || y < 0.0f || phi::is_nan(y))
                {
                    return 0;
                }

                ImVec2 size_vec(x, y);

                auto border_opt = consume_t<bool>(data, size, index);
                if (!border_opt)
                {
                    return 0;
                }
                const bool border = border_opt.value();

                FUZZ_LOG("Render(ImVec2({:f}, {:f}), {:s})", x, y, border ? "true" : "false");

                ImGui::NewFrame();
                editor.Render(size_vec, border);
                EndImGui();

                break;
            }

            // EnterCharacter
            case 33: {
                auto character_opt = consume_t<ImWchar>(data, size, index);
                if (!character_opt)
                {
                    return 0;
                }
                ImWchar character = character_opt.value();

                auto shift_opt = consume_t<bool>(data, size, index);
                if (!shift_opt)
                {
                    return 0;
                }
                bool shift = shift_opt.value();

                FUZZ_LOG("EnterCharacter({:s}, {:s})", print_char(character), print_bool(shift));
                editor.EnterCharacter(character, shift);
                break;
            }

            // ClearText
            case 34: {
                FUZZ_LOG("ClearText()");

                editor.ClearText();
                break;
            }

            // ClearSelection
            case 35: {
                FUZZ_LOG("ClearSelection");

                editor.ClearSelection();
                break;
            }

            // Backspace
            case 36: {
                FUZZ_LOG("Backspace");

                editor.Backspace();
                break;
            }

            // ImGui::AddKeyEvent
            case 37: {
                auto key_opt = consume_t<ImGuiKey>(data, size, index);
                if (!key_opt)
                {
                    return 0;
                }
                ImGuiKey key = key_opt.value();

                if (!ImGui::IsNamedKey(key) || ImGui::IsAliasKey(key))
                {
                    return 0;
                }

                auto down_opt = consume_t<bool>(data, size, index);
                if (!down_opt)
                {
                    return 0;
                }
                bool down = down_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddKeyEvent({}, {:s})", key, print_bool(down));
                ImGui::GetIO().AddKeyEvent(key, down);

                break;
            }

            // ImGui::AddKeyAnalogEvent
            case 38: {
                auto key_opt = consume_t<ImGuiKey>(data, size, index);
                if (!key_opt)
                {
                    return 0;
                }
                ImGuiKey key = key_opt.value();

                if (!ImGui::IsNamedKey(key) || ImGui::IsAliasKey(key))
                {
                    return 0;
                }

                auto down_opt = consume_t<bool>(data, size, index);
                if (!down_opt)
                {
                    return 0;
                }
                bool down = down_opt.value();

                auto value_opt = consume_t<float>(data, size, index);
                if (!value_opt)
                {
                    return 0;
                }
                float value = value_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddKeyAnalogEvent({}, {:s}, {:f})", key, print_bool(down),
                         value);
                ImGui::GetIO().AddKeyAnalogEvent(key, down, value);

                break;
            }

            // ImGui::AddMousePosEvent
            case 39: {
                auto x_opt = consume_t<float>(data, size, index);
                if (!x_opt)
                {
                    return 0;
                }
                const float x = x_opt.value();

                if (phi::abs(x) >= MaxSaneFloatValue)
                {
                    return 0;
                }

                auto y_opt = consume_t<float>(data, size, index);
                if (!y_opt)
                {
                    return 0;
                }
                const float y = y_opt.value();

                if (phi::abs(y) >= MaxSaneFloatValue)
                {
                    return 0;
                }

                FUZZ_LOG("ImGui::GetIO().AddMousePosEvent({:f}, {:f})", x, y);
                ImGui::GetIO().AddMousePosEvent(x, y);

                break;
            }

            // ImGui::AddMouseButtonEvent
            case 40: {
                auto button_opt = consume_t<int>(data, size, index);
                if (!button_opt)
                {
                    return 0;
                }
                int button = button_opt.value();

                if (button < 0 || button >= ImGuiMouseButton_COUNT)
                {
                    return 0;
                }

                auto down_opt = consume_t<bool>(data, size, index);
                if (!down_opt)
                {
                    return 0;
                }
                bool down = down_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddMouseButtonEvent({}, {:s})", button, print_bool(down));
                ImGui::GetIO().AddMouseButtonEvent(button, down);

                break;
            }

            // ImGui::AddMouseWheelEvent
            case 41: {
                auto wh_x_opt = consume_t<float>(data, size, index);
                if (!wh_x_opt)
                {
                    return 0;
                }
                const float wh_x = wh_x_opt.value();

                if (phi::abs(wh_x) >= MaxSaneFloatValue)
                {
                    return 0;
                }

                auto wh_y_opt = consume_t<float>(data, size, index);
                if (!wh_y_opt)
                {
                    return 0;
                }
                const float wh_y = wh_y_opt.value();

                if (phi::abs(wh_y) >= MaxSaneFloatValue)
                {
                    return 0;
                }

                FUZZ_LOG("ImGui::GetIO().AddMouseWheelEvent({:f}, {:f})", wh_x, wh_y);
                ImGui::GetIO().AddMouseWheelEvent(wh_x, wh_y);

                break;
            }

            // ImGui::AddFocusEvent
            case 42: {
                auto focused_opt = consume_t<bool>(data, size, index);
                if (!focused_opt)
                {
                    return 0;
                }
                bool focused = focused_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddFocusEvent({:s})", print_bool(focused));
                ImGui::GetIO().AddFocusEvent(focused);

                break;
            }

            // ImGui::AddInputCharacter
            case 43: {
                auto c_opt = consume_t<unsigned int>(data, size, index);
                if (!c_opt)
                {
                    return 0;
                }
                unsigned int c = c_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddInputCharacter({})", c);
                ImGui::GetIO().AddInputCharacter(c);

                break;
            }

            // ImGui::AddInputCharacterUTF16
            case 44: {
                auto c_opt = consume_t<ImWchar16>(data, size, index);
                if (!c_opt)
                {
                    return 0;
                }
                ImWchar16 c = c_opt.value();

                FUZZ_LOG("ImGui::GetIO().AddInputCharacterUTF16({})", c);
                ImGui::GetIO().AddInputCharacterUTF16(c);

                break;
            }

            // ImGui::AddInputCharactersUTF8
            case 45: {
                if (!consume_string(data, size, index))
                {
                    return 0;
                }
                std::string& str = cache.string;

                FUZZ_LOG("ImGui::GetIO().AddInputCharactersUTF8({:s})", print_string(str));
                ImGui::GetIO().AddInputCharactersUTF8(str.c_str());

                break;
            }

            default: {
                return 0;
            }
        }
    }

    FUZZ_LOG("VerifyInternalState()");
    editor.VerifyInternalState();

    FUZZ_LOG("Finished execution");

    return 0;
}
