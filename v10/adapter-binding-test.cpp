#include "AdapterSelectionBinding.h"
#include <cstdlib>
#include <iostream>

static void require(bool result, const char* message)
{
    if (!result)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main()
{
    using MultiGPU::AdapterSelection;
    using MultiGPU::AdapterSelectionBinding;
    constexpr std::uint64_t rx6600 = 0x140A9;
    constexpr std::uint64_t anotherGpu = 0xFFFFFFFF00001234ULL;
    const AdapterSelection single { false, std::nullopt };
    const AdapterSelection dual { true, rx6600 };
    AdapterSelectionBinding binding;

    require(!binding.IsBound() && !binding.RequiresRestart(dual), "initial GPU selection is unrestricted");
    require(binding.Bind(single), "first SDK context binds single GPU");
    require(binding.RequiresRestart(dual), "reported single-to-6600 switch requires restart");
    require(!binding.Bind(dual), "FG activation must not replace a live SDK adapter binding");
    require(binding.RequiresRestart(dual), "failed rebind must retain the original selection");
    require(!binding.RequiresRestart(single), "reverting the pending change clears the warning");
    require(!binding.RequiresRestart({ false, rx6600 }), "unused saved LUID does not change single GPU");

    binding.Reset(); // Only after the SDK swapchain context is destroyed.
    require(binding.Bind(dual), "a new SDK context can bind the selected secondary GPU");
    require(!binding.RequiresRestart(dual), "cold-start dual selection needs no restart");
    require(binding.RequiresRestart(single), "dual-to-single must not tear down a live secondary runtime");
    require(binding.RequiresRestart({ true, anotherGpu }), "switching secondary GPUs requires restart");
    require(binding.RequiresRestart({ true, std::nullopt }), "clearing an active adapter requires restart");
    for (int i = 0; i < 100; ++i)
    {
        require(!binding.Bind(single), "repeated FG activation cannot migrate the swapchain");
        require(!binding.RequiresRestart(dual), "repeated activation preserves the bound selection");
    }

    binding.Reset();
    require(binding.Bind({ true, std::nullopt }), "missing adapter at startup is recorded");
    require(binding.RequiresRestart(dual), "choosing the first adapter after startup requires restart");
    binding.Reset();
    require(binding.Bind({ true, anotherGpu }), "full 64-bit LUID is supported");
    require(!binding.RequiresRestart({ true, anotherGpu }), "high LUID bits remain intact");
    require(binding.RequiresRestart({ true, 0x1234 }), "different high LUID bits identify a different GPU");
    std::cout << "PASS: XeFG adapter lifetime and pending-change regression cases\n";
}
