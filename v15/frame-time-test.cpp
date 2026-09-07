#include "XeFGFrameTime.h"
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <SimpleIni.h>
#endif

static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main()
{
    using namespace MultiGPU;
    Check(ReadFrameTimeSetting("2", "0", true) == 2, "canonical key ignored");
    Check(ReadFrameTimeSetting(nullptr, "1", true) == 1, "legacy key lost");
    Check(!ReadFrameTimeSetting("auto", "1", true), "auto resurrected stale legacy value");
    Check(!ReadFrameTimeSetting("AUTO", "2", true), "uppercase auto not cleared");
    Check(!ReadFrameTimeSetting("", "1", true), "empty canonical fell through");
    Check(!ReadFrameTimeSetting("2junk", "1", true), "partial integer accepted");
    Check(!ReadFrameTimeSetting("-1", nullptr, true), "negative source accepted");
    Check(!ReadFrameTimeSetting("3", nullptr, true), "out of range accepted");
    Check(!ReadFrameTimeSetting("2", nullptr, false), "XeFG-only mode enabled for FSRFG");
    Check(ReadFrameTimeSetting(" 1\t", nullptr, false) == 1, "valid FSRFG source changed");
    Check(!ReadFrameTimeSetting(nullptr, nullptr, true), "missing key not auto");
#ifdef _WIN32
    // Round-trip through the same INI library used by production Config.cpp.
    CSimpleIniA saved;
    saved.SetValue("FrameGen", "FTSource", "0");
    saved.SetValue("FrameGen", "FTInput", "2");
    std::string bytes;
    Check(saved.Save(bytes) >= 0, "INI save");
    CSimpleIniA loaded;
    Check(loaded.LoadData(bytes.data(), bytes.size()) >= 0, "INI load");
    Check(ReadFrameTimeSetting(loaded.GetValue("FrameGen", "FTInput", nullptr),
                              loaded.GetValue("FrameGen", "FTSource", nullptr), true) == 2, "saved source lost at restart");
    loaded.SetValue("FrameGen", "FTInput", "auto");
    Check(!ReadFrameTimeSetting(loaded.GetValue("FrameGen", "FTInput", nullptr),
                               loaded.GetValue("FrameGen", "FTSource", nullptr), true), "saved auto did not clear override");
#endif
    auto v = SelectXeFGFrameTime({}, true, 13.8f, 20.4f);
    Check(v.source == 2 && v.sdkMs == 0 && v.requested == -1, "secondary auto policy");
    v = SelectXeFGFrameTime({}, false, 13.8f, 20.4f);
    Check(v.source == 0 && v.sdkMs == 13.8f, "single GPU or synchronous auto changed");
    v = SelectXeFGFrameTime(0, true, 13.8f, 20.4f);
    Check(v.source == 0 && v.sdkMs == 13.8f && v.requested == 0, "explicit rollback ignored");
    v = SelectXeFGFrameTime(1, true, 13.8f, 20.4f);
    Check(v.source == 1 && v.sdkMs == 20.4f, "explicit Present source ignored");
    v = SelectXeFGFrameTime(2, false, 13.8f, 20.4f);
    Check(v.source == 2 && v.sdkMs == 0, "explicit zero ignored");
    for (float invalid : {-1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
    {
        v = SelectXeFGFrameTime(0, true, invalid, 1.0f);
        Check(v.invalid && v.sdkMs == 0, "invalid input leaked into SDK");
        v = SelectXeFGFrameTime(1, true, 1.0f, invalid);
        Check(v.invalid && v.sdkMs == 0, "invalid Present interval leaked into SDK");
    }
    XeFGFrameTimeTelemetry telemetry;
    auto record = [&] { for (int i = 0; i < 1000; ++i) telemetry.Record(SelectXeFGFrameTime({}, true, 10, 20), 10, 20); };
    std::thread a(record), b(record); a.join(); b.join();
    auto snapshot = telemetry.Take();
    Check(snapshot.frames == 2000 && snapshot.zeroFrames == 2000 && snapshot.SdkMean() == 0 &&
          snapshot.InputMean() == 10 && snapshot.PresentMean() == 20, "telemetry corrupted or double counted");
    Check(telemetry.Take().frames == 0, "telemetry not consumed once");
    std::cout << "PASS: frame-time configuration precedence, restart roundtrip, scoped pacing, invalid inputs and telemetry\n";
}
