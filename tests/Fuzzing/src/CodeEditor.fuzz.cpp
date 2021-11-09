#include <DLXEmu/CodeEditor.hpp>
#include <DLXEmu/Emulator.hpp>
#include <Phi/Config/FunctionLikeMacro.hpp>
#include <Phi/Config/Warning.hpp>
#include <Phi/Core/Assert.hpp>
#include <Phi/Core/Log.hpp>
#include <Phi/Core/ScopeGuard.hpp>
#include <imgui.h>
#include <imgui_internal.h>
#include <magic_enum.hpp>
#include <spdlog/fmt/bundled/core.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#if defined(FUZZ_VERBOSE_LOG)
#    define FUZZ_LOG(...) PHI_LOG_DEBUG(__VA_ARGS__)
#else
#    define FUZZ_LOG(...) PHI_EMPTY_MACRO()
#endif

static constexpr const std::size_t MaxVectorSize{20u};

[[nodiscard]] bool has_x_more(const std::size_t index, const std::size_t x,
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
[[nodiscard]] std::optional<T> consume_t(const std::uint8_t* data, const std::size_t size,
                                         std::size_t& index) noexcept
{
    if (!has_x_more(index, sizeof(T), size))
    {
        return {};
    }

    PHI_ASSERT(index % sizeof(void*) == 0);

    T value = *reinterpret_cast<const T*>(data + index);
    index += aligned_size<T>();

    return value;
}

[[nodiscard]] std::optional<bool> consume_bool(const std::uint8_t* data, const std::size_t size,
                                               std::size_t& index) noexcept
{
    if (!has_x_more(index, sizeof(bool), size))
    {
        return {};
    }

    PHI_ASSERT(index % sizeof(void*) == 0);

    bool value = static_cast<bool>((data + index));
    index += aligned_size<bool>();

    return value;
}

[[nodiscard]] std::optional<std::uint32_t> consume_uint32(const std::uint8_t* data,
                                                          const std::size_t   size,
                                                          std::size_t&        index) noexcept
{
    return consume_t<std::uint32_t>(data, size, index);
}

[[nodiscard]] std::optional<std::size_t> consume_size_t(const std::uint8_t* data,
                                                        const std::size_t   size,
                                                        std::size_t&        index) noexcept
{
    return consume_t<std::size_t>(data, size, index);
}

[[nodiscard]] std::optional<std::string> consume_string(const std::uint8_t* data,
                                                        const std::size_t   size,
                                                        std::size_t&        index) noexcept
{
    std::string value;

    while (index < size && data[index] != '\0')
    {
        value += static_cast<char>(data[index]);
        index++;
    }

    // Make sure our string is null termianted
    if (!value.empty())
    {
        value.back() = '\0';
    }

    // Move back to proper alignemnt
    index += (sizeof(void*) - (index % sizeof(void*)));

    return value;
}

[[nodiscard]] std::optional<std::string> consume_ascii_string(const std::uint8_t* data,
                                                              const std::size_t   size,
                                                              std::size_t&        index) noexcept
{
    std::string value;

    while (index < size && data[index] != '\0')
    {
        if (data[index] > 127)
        {
            // Reject non ascii characters
            return {};
        }

        value += static_cast<char>(data[index]);
        index++;
    }

    // Make sure our string is null termianted
    if (!value.empty())
    {
        value.append("\0");
    }

    // Move back to proper alignemnt
    index += (sizeof(void*) - (index % sizeof(void*)));

    return value;
}

[[nodiscard]] std::optional<std::vector<std::string>> consume_vector_string(
        const std::uint8_t* data, const std::size_t size, std::size_t& index) noexcept
{
    auto number_of_lines_opt = consume_size_t(data, size, index);
    if (!number_of_lines_opt)
    {
        return {};
    }

    std::size_t              number_of_lines = std::min(number_of_lines_opt.value(), MaxVectorSize);
    std::vector<std::string> result;
    result.reserve(number_of_lines);

    for (std::size_t i{0u}; i < number_of_lines; ++i)
    {
        auto message_opt = consume_ascii_string(data, size, index);
        if (!message_opt)
        {
            return {};
        }

        result.emplace_back(message_opt.value());
    }

    return result;
}

[[nodiscard]] std::optional<dlxemu::CodeEditor::Coordinates> consume_coordinates(
        const std::uint8_t* data, const std::size_t size, std::size_t& index) noexcept
{
    auto column_opt = consume_t<std::int16_t>(data, size, index);
    if (!column_opt)
    {
        return {};
    }
    std::int16_t column = column_opt.value();

    auto line_opt = consume_t<std::int16_t>(data, size, index);
    if (!line_opt)
    {
        return {};
    }
    std::int16_t line = line_opt.value();

    dlxemu::CodeEditor::Coordinates coords;
    coords.m_Column = column;
    coords.m_Line   = line;

    return coords;
}

template <typename T>
[[nodiscard]] std::string print_int(const T val) noexcept
{
    return fmt::format("{0:d} 0x{1:02X}", val, static_cast<std::make_unsigned_t<T>>(val));
}

[[nodiscard]] std::string print_string(const std::string& str) noexcept
{
    std::string hex_str;
    std::string print_str;

    for (char c : str)
    {
        hex_str += fmt::format("\\0x{:02X}, ", static_cast<std::uint8_t>(c));

        // Make some special characters printable
        switch (c)
        {
            case '\n':
                print_str += "\\n";
                break;
            case '\0':
                print_str += "\\0";
                break;

            default:
                print_str += c;
                break;
        }
    }

    return fmt::format("String(\"{:s}\" size: {:d} ({:s}))", print_str, str.size(),
                       hex_str.substr(0, hex_str.size() - 2));
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

    for (const std::uint32_t line_number : breakpoints)
    {
        lines += fmt::format("{:s}, ", print_int(line_number));
    }

    std::string ret = fmt::format("Breakpoints(size: {:d}: {:s})", breakpoints.size(),
                                  lines.substr(0, lines.size() - 2));

    return ret.substr(0, ret.size());
}

[[nodiscard]] const char* print_bool(const bool b) noexcept
{
    return b ? "true" : "false";
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

    // Enforce valid space key mapping
    io.KeyMap[ImGuiKey_Space] = 0;

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
}

// cppcheck-suppress unusedFunction symbolName=LLVMFuzzerTestOneInput
extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
#if defined(FUZZ_VERBOSE_LOG)
    phi::Log::initialize_default_loggers();
#endif

    static bool imgui_init = SetupImGui();

    ImGui::NewFrame();
    auto guard = phi::make_scope_guard(&EndImGui);

    // Clear clipboard content
    ImGui::SetClipboardText("");

    dlxemu::Emulator   emulator;
    dlxemu::CodeEditor editor{&emulator};

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
                auto line_number_opt = consume_uint32(data, size, index);
                if (!line_number_opt)
                {
                    return 0;
                }
                std::uint32_t line_number = line_number_opt.value();

                auto message_opt = consume_string(data, size, index);
                if (!message_opt)
                {
                    return 0;
                }
                std::string message = message_opt.value();

                FUZZ_LOG("AddErrorMarker({:s}, {:s})", print_int(line_number),
                         print_string(message));

                editor.AddErrorMarker(line_number, message);
                break;
            }

            // ClearErrorMarkers
            case 1: {
                FUZZ_LOG("ClearErrorMarkers()");

                editor.ClearErrorMarkers();
                break;
            }

            // SetText
            case 2: {
                auto text_opt = consume_ascii_string(data, size, index);
                if (!text_opt)
                {
                    return 0;
                }
                std::string text = text_opt.value();

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

                std::vector<std::string> lines = lines_opt.value();

                // SetTextLines doesn not allow newlines
                // TODO: Remove this limitation
                for (std::string& line : lines)
                {
                    for (char& c : line)
                    {
                        if (c == '\n')
                        {
                            c = 'x';
                        }
                    }
                }

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
                auto read_only_opt = consume_bool(data, size, index);
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

                FUZZ_LOG("SetCursorPosition(Coordinates({:s}, {:s}))", print_int(coords.m_Line),
                         print_int(coords.m_Column));

                editor.SetCursorPosition(coords);
                break;
            }

            // SetShowWhiteSpaces
            case 11: {
                auto show_whitespaces_opt = consume_bool(data, size, index);
                if (!show_whitespaces_opt)
                {
                    return 0;
                }
                bool show_whitespaces = show_whitespaces_opt.value();

                FUZZ_LOG("SetShowWhitespaces({:s})", print_bool(show_whitespaces));

                editor.SetShowWhitespaces(show_whitespaces);
                break;
            }

            // SetTabSize
            case 12: {
                auto tab_size_opt = consume_t<std::int32_t>(data, size, index);
                if (!tab_size_opt)
                {
                    return 0;
                }
                std::int32_t tab_size = tab_size_opt.value();

                FUZZ_LOG("SetTabSize({:s})", print_int(tab_size));

                editor.SetTabSize(tab_size);
                break;
            }

            // InsertText
            case 13: {
                auto message_opt = consume_ascii_string(data, size, index);
                if (!message_opt)
                {
                    return 0;
                }

                std::string message = message_opt.value();

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

                auto select_opt = consume_bool(data, size, index);
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

                auto select_opt = consume_bool(data, size, index);
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

                auto select_opt = consume_bool(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                auto word_mode_opt = consume_bool(data, size, index);
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

                auto select_opt = consume_bool(data, size, index);
                if (!select_opt)
                {
                    return 0;
                }
                bool select = select_opt.value();

                auto word_mode_opt = consume_bool(data, size, index);
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
                auto select_opt = consume_bool(data, size, index);
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
                auto select_opt = consume_bool(data, size, index);
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
                auto select_opt = consume_bool(data, size, index);
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
                auto select_opt = consume_bool(data, size, index);
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
                auto column_opt = consume_t<std::int32_t>(data, size, index);
                if (!column_opt)
                {
                    return 0;
                }
                std::int32_t column = column_opt.value();

                auto line_opt = consume_t<std::int32_t>(data, size, index);
                if (!line_opt)
                {
                    return 0;
                }
                std::int32_t line = line_opt.value();

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
                auto column_opt = consume_t<std::int32_t>(data, size, index);
                if (!column_opt)
                {
                    return 0;
                }
                std::int32_t column = column_opt.value();

                auto line_opt = consume_t<std::int32_t>(data, size, index);
                if (!line_opt)
                {
                    return 0;
                }
                std::int32_t line = line_opt.value();

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
                auto column_start_opt = consume_t<std::int32_t>(data, size, index);
                if (!column_start_opt)
                {
                    return 0;
                }
                std::int32_t column_start = column_start_opt.value();

                auto line_start_opt = consume_t<std::int32_t>(data, size, index);
                if (!line_start_opt)
                {
                    return 0;
                }
                std::int32_t line_start = line_start_opt.value();

                dlxemu::CodeEditor::Coordinates coord_start;
                coord_start.m_Column = column_start;
                coord_start.m_Line   = line_start;

                auto column_end_opt = consume_t<std::int32_t>(data, size, index);
                if (!column_end_opt)
                {
                    return 0;
                }
                std::int32_t column_end = column_end_opt.value();

                auto line_end_opt = consume_t<std::int32_t>(data, size, index);
                if (!line_end_opt)
                {
                    return 0;
                }
                std::int32_t line_end = line_end_opt.value();

                dlxemu::CodeEditor::Coordinates coord_end;
                coord_end.m_Column = column_end;
                coord_end.m_Line   = line_end;

                auto selection_mode_opt = consume_t<std::uint8_t>(data, size, index);
                if (!selection_mode_opt)
                {
                    return 0;
                }
                dlxemu::CodeEditor::SelectionMode selection_mode =
                        static_cast<dlxemu::CodeEditor::SelectionMode>(std::clamp(
                                selection_mode_opt.value(), (std::uint8_t)0u, (std::uint8_t)2u));

                FUZZ_LOG("SetSelection(Coordinates({:s}, {:s}), Coordiantes({:s}, {:s}), {:s})",
                         print_int(line_start), print_int(column_start), print_int(line_end),
                         print_int(column_end), magic_enum::enum_name(selection_mode));

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

            // Copy
            case 27: {
                FUZZ_LOG("Copy");

                editor.Copy();
                break;
            }

            // Cut
            case 28: {
                FUZZ_LOG("Cut");

                editor.Cut();
                break;
            }

            // Paste
            case 29: {
                FUZZ_LOG("Paste");

                editor.Paste();
                break;
            }

            // Delete
            case 30: {
                FUZZ_LOG("Delete");

                editor.Delete();
                break;
            }

            // Undo
            case 31: {
                auto steps_opt = consume_t<std::uint32_t>(data, size, index);
                if (!steps_opt)
                {
                    return 0;
                }
                std::uint32_t steps = steps_opt.value();

                FUZZ_LOG("Undo({:s})", print_int(steps));

                editor.Undo(steps);
                break;
            }

            // Redo
            case 32: {
                auto steps_opt = consume_t<std::uint32_t>(data, size, index);
                if (!steps_opt)
                {
                    return 0;
                }
                std::uint32_t steps = steps_opt.value();

                FUZZ_LOG("Redo({:s})", print_int(steps));

                editor.Redo(steps);
                break;
            }

            // GetEditorDump
            case 33: {
                FUZZ_LOG("GetEditorDump()");

                volatile std::string text = editor.GetEditorDump();
                PHI_UNUSED_VARIABLE(text);
                break;
            }

            // SetErrorMarkers
            case 34: {
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

                    auto message_opt = consume_ascii_string(data, size, index);
                    if (!message_opt)
                    {
                        return 0;
                    }
                    std::string message = message_opt.value();

                    // Add to error markers
                    markers[line_number] = message;
                }

                FUZZ_LOG("SetErrorMarkers({:s})", print_error_markers(markers));

                editor.SetErrorMarkers(markers);
                break;
            }

            // SetBreakpoints
            case 35: {
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
            case 36: {
                constexpr const static float min_val{-10'000};
                constexpr const static float max_val{+10'000};

                auto x_opt = consume_t<float>(data, size, index);
                if (!x_opt)
                {
                    return 0;
                }
                float x = x_opt.value();

                auto y_opt = consume_t<float>(data, size, index);
                if (!y_opt)
                {
                    return 0;
                }
                float y = y_opt.value();

                // Reject invalid values
                if (x < min_val || x > max_val || y < min_val || y > max_val || std::isnan(x) ||
                    std::isnan(y) || std::isinf(x) || std::isinf(y))
                {
                    return 0;
                }

                ImVec2 size_vec(x, y);

                auto border_opt = consume_bool(data, size, index);
                if (!border_opt)
                {
                    return 0;
                }
                bool border = border_opt.value();

                FUZZ_LOG("Render(ImVec2({:f}, {:f}), {:s})", x, y, border ? "true" : "false");

                editor.Render(size_vec, border);
                break;
            }

            default: {
                return 0;
            }
        }
    }

    FUZZ_LOG("Finished execution");

    return 0;
}