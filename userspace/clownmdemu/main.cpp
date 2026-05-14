#include "frontend.h"

int main(int argc, char** argv) {
    using namespace clownmdemu_frontend;

    (void)argc;
    (void)argv;

    ClownMDEmu_Constant_Initialise();

    auto* app = new AppState{};

    if (!init_framebuffer(app)) {
        delete app;
        return 1;
    }
    if (!init_emulator(app)) {
        destroy_app(app);
        delete app;
        return 1;
    }
    if (!browse_and_load_rom(app)) {
        const int exit_code = app->quit_requested ? 0 : 1;
        destroy_app(app);
        delete app;
        return exit_code;
    }

    reset_emulator(app);

    printf("ClownMDEmu\n");
    if (!app->loaded_rom_path.empty())
        printf("ROM file: %s\n", path_basename(app->loaded_rom_path.c_str()));
    if (!app->rom_title.empty())
        printf("Loaded: %s\n", app->rom_title.c_str());
    printf("Controls: arrows, A/S/D, Q/W/E, Enter, Backspace, Tab, Escape\n");

    while (!app->quit_requested) {
        pump_input(app);

        // tab for reset, shift+tab for load ROM, escape for quit
        if (app->reset_requested) {
            app->reset_requested = false;
            reset_emulator(app);
            continue;
        }

        if (app->loadrom_requested) {
            app->loadrom_requested = false;
            if (browse_and_load_rom(app)) {
                reset_emulator(app);
            }
            continue;
        }

        audio_try_submit(app);

        vk_u64 now = VK_CALL(tick_count);
        vk_u32 catchup_frames = 0;
        while (now >= app->timing.next_frame_tick && catchup_frames < kMaxCatchupFrames) {
            emulate_frame(app);
            audio_try_submit(app);
            schedule_next_frame(app);
            now = VK_CALL(tick_count);
            ++catchup_frames;
        }

        idle_until_next_work(app);
    }

    destroy_app(app);
    delete app;
    return 0;
}