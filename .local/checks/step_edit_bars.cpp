// Step Edit as the hands drive it: Shift + the wheel moves the caret a bar at a time, on the same step of the bar,
// also after a knob has been touched (Shift + the wheel is elsewhere the fine edit of the last knob).
#include "melodee/engine/engine_host.h"
#include "melodee/shell/shell.h"
#include "melodee/shell/spare_bar.h"
#include "surface/controls.h"
#include <cstdio>
#include <memory>
using namespace melodee;
using B = SurfaceButton;
int main() {
    auto host = std::make_unique<EngineHost>();
    auto shell = std::make_unique<Shell>(*host);
    host->load(shell->project(), shell->patterns());
    uint32_t now = 1000;
    auto button = [&](B b, bool down) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Button; e.input.index = uint8_t(b); e.input.pressed = down; now += 20; shell->surface(e, now); };
    auto wheel = [&](int d) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Control; e.input.index = uint8_t(SurfaceControl::Wheel); e.input.delta = d; now += 20; shell->surface(e, now); };
    auto knob = [&](SurfaceControl c, int v) { SurfaceEvent e; e.input.kind = SurfaceInput::Kind::Control; e.input.index = uint8_t(c); e.input.value = uint8_t(v); now += 20; shell->surface(e, now); };
    auto caret = [&] { auto& c = shell->context(); return edit_caret(shell->ui(), c.pattern()); };
    int bad = 0;
    auto expect = [&](const char* what, unsigned want) { unsigned got = caret(); bad += got != want; std::printf("%-58s caret on step %2u (bar %u)  %s\n", what, got + 1, got / 16 + 1, got == want ? "ok" : "WRONG"); };
    button(B::Shift, true); button(B::Seq, true); button(B::Seq, false); button(B::Shift, false);
    if (shell->ui().page != PageId::StepEdit) { // an empty project's first track has no sound to edit: give it one, and open the page
        load_sound(shell->project().tracks[0], EngineKind::Sampler, 0);
        shell->context().open(PageId::StepEdit);
    }
    std::printf("page %u (Step Edit is %u); the pattern has %u steps\n", unsigned(shell->ui().page), unsigned(PageId::StepEdit), shell->context().pattern().steps());
    bad += shell->ui().page != PageId::StepEdit;
    // Make it four bars: Shift + K1 on Step Edit is the pattern's length; easier here to set it and tell the shell.
    { auto& c = shell->context(); c.pattern().length = 4 * c.project.bar_ticks(); c.sync_pattern(shell->ui().track, c.track().pattern); }
    expect("at the start", 0);
    wheel(5); expect("the wheel, five steps", 5);
    button(B::Shift, true); wheel(1); expect("Shift + the wheel, one on", 21);
    wheel(2); expect("Shift + the wheel, two on", 53);
    wheel(1); expect("Shift + the wheel, into the spare bar", 69);
    wheel(1); expect("Shift + the wheel, round to the first", 5);
    wheel(-1); expect("Shift + the wheel back, to the spare bar", 69);
    wheel(-1); expect("Shift + the wheel back", 53);
    button(B::Shift, false);
    wheel(-1); expect("the wheel alone, a step back", 52);
    knob(SurfaceControl::K2, 90); // a knob touched: elsewhere Shift + the wheel would now fine-edit it
    button(B::Shift, true); wheel(-1); expect("Shift + the wheel after K2 was turned", 36); button(B::Shift, false);
    shell->context().open(PageId::Scenes);
    knob(SurfaceControl::K1, 80);
    auto& scene = shell->project().scenes[shell->project().scene];
    scene.colour = 2;
    const auto tempo = scene.tempo;
    button(B::Shift, true); wheel(1); button(B::Shift, false);
    const bool colour_changed = scene.colour == 3 && scene.tempo == tempo;
    std::printf("Scenes Shift + wheel after K1 changes colour: %s\n", colour_changed ? "ok" : "WRONG");
    bad += !colour_changed;
    std::printf("%s\n", bad ? "FAULT" : "all as expected");
    return bad != 0;
}
