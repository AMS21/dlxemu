#include "DLXEmu/MemoryViewer.hpp"

#include "DLX/MemoryBlock.hpp"
#include "DLX/Processor.hpp"
#include "DLXEmu/Emulator.hpp"
#include <imgui.h>

namespace dlxemu
{
    MemoryViewer::MemoryViewer(Emulator* emulator)
        : m_Emulator(emulator)
    {}

    void MemoryViewer::Render()
    {
        ImGui::Begin("Memory Viewer");

        dlx::MemoryBlock mem = m_Emulator->GetProcessor().GetMemory();

        auto& values = mem.GetRawMemory();

        for (std::size_t index{0}; index < values.size(); index += 4)
        {
            auto val = mem.LoadWord(mem.GetStartingAddress() + index)->get();

            ImGui::InputInt(std::to_string((mem.GetStartingAddress() + index).get()).c_str(), &val);
        }

        ImGui::End();
    }
} // namespace dlxemu
