// Native tests for LedDriver using the mock millis() (g_fake_now) and the
// in-memory NeoPixel mock. Brightness is set to 255 so scale(255)==255 and
// event "on" phases render the exact palette color.

#include "../../../src/drivers/LedDriver.cpp"

#include <unity.h>

namespace {

uint8_t rOf(uint32_t c) { return static_cast<uint8_t>(c >> 16); }
uint8_t bOf(uint32_t c) { return static_cast<uint8_t>(c); }

LedDriver *led = nullptr;

// Begin at t=0, run boot to completion, land in `mode` at t=7000.
void ready(led::Mode mode) {
    g_fake_now = 0;
    led = new LedDriver();
    led->setBrightness(255);
    led->begin();
    led->update();  // t=0, boot line phase start
    g_fake_now = 6600;
    led->update();  // boot finishes on this call
    led->setMode(mode);
    g_fake_now = 7000;
}

}  // namespace

void setUp(void) {}
void tearDown(void) {
    delete led;
    led = nullptr;
}

// --- boot -----------------------------------------------------------------

void test_boot_runs_for_6600ms_then_done(void) {
    g_fake_now = 0;
    led = new LedDriver();
    led->begin();
    led->update();
    TEST_ASSERT_FALSE(led->bootDone());

    g_fake_now = 6599;
    led->update();
    TEST_ASSERT_FALSE(led->bootDone());

    g_fake_now = 6600;
    led->update();
    TEST_ASSERT_TRUE(led->bootDone());
}

void test_boot_mid_is_blue_and_nonzero(void) {
    g_fake_now = 0;
    led = new LedDriver();
    led->begin();
    g_fake_now = 500;  // logo grow phase (420..900)
    led->update();
    const uint32_t c = led->pixelAt(0);
    TEST_ASSERT_NOT_EQUAL(0, c);
    TEST_ASSERT_GREATER_THAN(rOf(c), 0);
    TEST_ASSERT_GREATER_THAN(bOf(c), rOf(c));  // blue dominant
}

void test_boot_text_flash_is_white(void) {
    g_fake_now = 0;
    led = new LedDriver();
    led->begin();
    led->setBrightness(255);
    g_fake_now = 2700;  // "OFFGRID" lock-in flash
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kWhite, led->pixelAt(0));
    TEST_ASSERT_EQUAL_HEX32(led::kWhite, led->pixelAt(7));
}

void test_boot_fill_lighting_order(void) {
    g_fake_now = 0;
    led = new LedDriver();
    led->begin();
    // fill starts at 4450, LED 7 first, one every 150ms
    g_fake_now = 4450 + 30;
    led->update();
    TEST_ASSERT_EQUAL(0, led->pixelAt(6));  // starts at 4600, not lit yet
    TEST_ASSERT_NOT_EQUAL(0, led->pixelAt(7));
}

// --- modes -----------------------------------------------------------------

void test_meshup_chase_pattern(void) {
    ready(led::Mode::MeshUp);
    led->update();
    // step = (7000/45)%8 = 155%8 = 3 -> LED 3 full, 4 dim, rest 0
    TEST_ASSERT_EQUAL_HEX32(led::kBlue, led->pixelAt(3));
    TEST_ASSERT_GREATER_THAN(0, led->pixelAt(4));
    TEST_ASSERT_GREATER_THAN(led->pixelAt(3), led->pixelAt(4));
    TEST_ASSERT_EQUAL(0, led->pixelAt(5));
    TEST_ASSERT_EQUAL(0, led->pixelAt(6));
}

void test_unprovisioned_is_amber_breathing(void) {
    ready(led::Mode::Unprovisioned);
    led->update();
    const uint32_t c = led->pixelAt(0);
    TEST_ASSERT_NOT_EQUAL(0, c);
    TEST_ASSERT_GREATER_THAN(rOf(c), bOf(c));  // amber: red dominates
    // all eight LEDs share the level
    for (uint8_t i = 1; i < LedDriver::RingSize; i++) {
        TEST_ASSERT_EQUAL(c, led->pixelAt(i));
    }
}

void test_no_neighbors_is_dim_blue(void) {
    ready(led::Mode::NoNeighbors);
    led->update();
    const uint32_t c = led->pixelAt(0);
    TEST_ASSERT_NOT_EQUAL(0, c);
    TEST_ASSERT_GREATER_THAN(bOf(c), rOf(c));
    TEST_ASSERT_LESS_THAN(bOf(c), 200);  // dim, not full
}

void test_pairing_mode_is_cyan(void) {
    ready(led::Mode::Pairing);
    g_fake_now = 7050;  // inside the first heartbeat up-slope
    led->update();
    const uint32_t c = led->pixelAt(0);
    TEST_ASSERT_NOT_EQUAL(0, c);
    TEST_ASSERT_GREATER_THAN(bOf(c), rOf(c));
}

// --- events -----------------------------------------------------------------

void test_tx_single_blue_flash(void) {
    ready(led::Mode::MeshUp);
    led->trigger(led::Event::Tx);  // at t=7000, on until 7060
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kBlue, led->pixelAt(0));
    TEST_ASSERT_TRUE(led->eventActive());

    g_fake_now = 7060;
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());
}

void test_msg_received_double_flash_then_mode_resumes(void) {
    ready(led::Mode::MeshUp);
    led->trigger(led::Event::MsgReceived);  // {80,80,80,760} white, ends 8000
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kWhite, led->pixelAt(0));  // on 7000-7080
    TEST_ASSERT_EQUAL_HEX32(led::kWhite, led->pixelAt(7));

    g_fake_now = 7080;  // off -> mode shows through
    led->update();
    TEST_ASSERT_NOT_EQUAL(0, led->pixelAt(4));  // chase tail visible
    TEST_ASSERT_NOT_EQUAL(led::kWhite, led->pixelAt(0));

    g_fake_now = 7160;  // second on
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kWhite, led->pixelAt(3));

    g_fake_now = 8000;  // pattern finished
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());
}

void test_failed_triple_red_blink(void) {
    ready(led::Mode::MeshUp);
    led->trigger(led::Event::Failed);  // {100,100} x3 -> on 7000/7200/7400, ends 7600
    g_fake_now = 7050;
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kRed, led->pixelAt(0));

    g_fake_now = 7150;  // gap
    led->update();
    TEST_ASSERT_NOT_EQUAL(led::kRed, led->pixelAt(0));

    g_fake_now = 7450;  // third on window
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kRed, led->pixelAt(7));

    g_fake_now = 7600;
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());
}

void test_event_drift_stays_on_schedule(void) {
    // update() calls land late (slow loop); the pattern must not stretch.
    ready(led::Mode::MeshUp);
    g_fake_now = 7000;
    led->trigger(led::Event::Tx);
    g_fake_now = 7100;  // 40ms late
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());  // ended at 7060, not 7100+60

    g_fake_now = 7000;
    led->trigger(led::Event::MsgReceived);
    g_fake_now = 8100;  // way late
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());  // ended at 8000
}

// --- reset progress ----------------------------------------------------------

void test_reset_progress_fills_ring(void) {
    ready(led::Mode::MeshUp);
    led->setResetProgress(50);
    led->update();
    TEST_ASSERT_NOT_EQUAL(0, led->pixelAt(3));  // 4 of 8 lit
    TEST_ASSERT_EQUAL(0, led->pixelAt(4));

    led->setResetProgress(100);
    led->update();
    TEST_ASSERT_NOT_EQUAL(0, led->pixelAt(7));

    led->setResetProgress(0);  // ignored, stays at 100
    led->update();
    TEST_ASSERT_NOT_EQUAL(0, led->pixelAt(7));

    led->clearResetProgress();
    led->update();
    TEST_ASSERT_EQUAL(0, led->pixelAt(0));  // back to mode (LED 0 off at step 3)
}

// --- config ------------------------------------------------------------------

void test_quiet_disables_all_leds(void) {
    ready(led::Mode::MeshUp);
    led->setQuiet(true);
    led->update();
    for (uint8_t i = 0; i < LedDriver::RingSize; i++) {
        TEST_ASSERT_EQUAL(0, led->pixelAt(i));
    }
}

void test_disabled_disables_all_leds(void) {
    ready(led::Mode::MeshUp);
    led->setEnabled(false);
    led->update();
    for (uint8_t i = 0; i < LedDriver::RingSize; i++) {
        TEST_ASSERT_EQUAL(0, led->pixelAt(i));
    }
}

void test_zero_brightness_disables_all_leds(void) {
    ready(led::Mode::MeshUp);
    led->setBrightness(0);
    led->update();
    for (uint8_t i = 0; i < LedDriver::RingSize; i++) {
        TEST_ASSERT_EQUAL(0, led->pixelAt(i));
    }
}

void test_preview_plays_custom_pattern(void) {
    ready(led::Mode::MeshUp);
    const uint16_t steps[] = {200, 150, 200};
    led->preview(steps, 3, 2);  // 550ms per repeat, x2 = 1100, ends 8100
    g_fake_now = 7100;
    led->update();
    TEST_ASSERT_EQUAL_HEX32(led::kBlue, led->pixelAt(0));  // on
    g_fake_now = 7250;  // in the 150ms off gap
    led->update();
    TEST_ASSERT_NOT_EQUAL(led::kBlue, led->pixelAt(0));
    g_fake_now = 8100;
    led->update();
    TEST_ASSERT_FALSE(led->eventActive());
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    UnityBegin("LedDriver");
    RUN_TEST(test_boot_runs_for_6600ms_then_done);
    RUN_TEST(test_boot_mid_is_blue_and_nonzero);
    RUN_TEST(test_boot_text_flash_is_white);
    RUN_TEST(test_boot_fill_lighting_order);
    RUN_TEST(test_meshup_chase_pattern);
    RUN_TEST(test_unprovisioned_is_amber_breathing);
    RUN_TEST(test_no_neighbors_is_dim_blue);
    RUN_TEST(test_pairing_mode_is_cyan);
    RUN_TEST(test_tx_single_blue_flash);
    RUN_TEST(test_msg_received_double_flash_then_mode_resumes);
    RUN_TEST(test_failed_triple_red_blink);
    RUN_TEST(test_event_drift_stays_on_schedule);
    RUN_TEST(test_reset_progress_fills_ring);
    RUN_TEST(test_quiet_disables_all_leds);
    RUN_TEST(test_disabled_disables_all_leds);
    RUN_TEST(test_zero_brightness_disables_all_leds);
    RUN_TEST(test_preview_plays_custom_pattern);
    return UnityEnd();
}
